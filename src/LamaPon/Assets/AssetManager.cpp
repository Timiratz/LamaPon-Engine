#include "LamaPon/Assets/AssetManager.h"

#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Assets/AssetArchive.h"
#include "LamaPon/Assets/CmoImporter.h"
#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Assets/FbxImporter.h"
#include "LamaPon/Assets/GltfImporter.h"
#include "LamaPon/Assets/SdkmeshImporter.h"
#include "LamaPon/Assets/TextureCache.h"
#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Assets/VboImporter.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <Model.h>
#include <DirectXCollision.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // ボーン変換後の全メッシュ外接範囲を合成する(model: 対象モデル, bounds: 範囲の出力)。
    bool CalculateModelBounds(
        const DirectX::Model& model,
        LamaPon::Bounds3D& bounds)
    {
        // ボーンの絶対変換行列
        std::vector<DirectX::XMMATRIX> boneTransforms(
            model.bones.size());
        if (!boneTransforms.empty())
        {
            model.CopyAbsoluteBoneTransformsTo(
                boneTransforms.size(),
                boneTransforms.data());
        }

        // 外接範囲の設定済み状態
        bool initialized{};
        // 合成対象のメッシュ
        for (const auto& mesh : model.meshes)
        {
            if (!mesh)
            {
                continue;
            }
            // 変換後のメッシュ外接範囲
            DirectX::BoundingBox transformed =
                mesh->boundingBox;
            if (mesh->boneIndex < boneTransforms.size())
            {
                mesh->boundingBox.Transform(
                    transformed,
                    boneTransforms[mesh->boneIndex]);
            }
            // 外接範囲の最小座標
            const DirectX::XMFLOAT3 minimum{
                transformed.Center.x - transformed.Extents.x,
                transformed.Center.y - transformed.Extents.y,
                transformed.Center.z - transformed.Extents.z
            };
            // 外接範囲の最大座標
            const DirectX::XMFLOAT3 maximum{
                transformed.Center.x + transformed.Extents.x,
                transformed.Center.y + transformed.Extents.y,
                transformed.Center.z + transformed.Extents.z
            };
            if (!initialized)
            {
                bounds = { minimum, maximum };
                initialized = true;
                continue;
            }
            bounds.minimum.x = std::min(bounds.minimum.x, minimum.x);
            bounds.minimum.y = std::min(bounds.minimum.y, minimum.y);
            bounds.minimum.z = std::min(bounds.minimum.z, minimum.z);
            bounds.maximum.x = std::max(bounds.maximum.x, maximum.x);
            bounds.maximum.y = std::max(bounds.maximum.y, maximum.y);
            bounds.maximum.z = std::max(bounds.maximum.z, maximum.z);
        }
        return initialized;
    }

    // 効果生成を委譲し、効果の寿命内で元の材質色を記録する。
    class MaterialCapturingEffectFactory final
        : public DirectX::IEffectFactory
    {
    public:
        // 材質色を記録する生成器を作る(device: 借用D3D11デバイス)。
        explicit MaterialCapturingEffectFactory(ID3D11Device* device)
            : m_factory(device)
        {
        }

        // 外部画像の検索先を指定する(path: 画像フォルダー)。
        void SetDirectory(const wchar_t* path) noexcept
        {
            m_factory.SetDirectory(path);
        }

        // 描画効果を生成して元材質色を控える(info: 材質設定, context: 任意の即時コンテキスト)。
        std::shared_ptr<DirectX::IEffect> __cdecl CreateEffect(
            const EffectInfo& info,
            ID3D11DeviceContext* context) override
        {
            // 生成した描画効果
            auto effect = m_factory.CreateEffect(info, context);
            if (effect != nullptr)
            {
                m_diffuseColors.insert_or_assign(
                    effect.get(),
                    DirectX::XMFLOAT4{
                        info.diffuseColor.x,
                        info.diffuseColor.y,
                        info.diffuseColor.z,
                        info.alpha
                    });
            }
            return effect;
        }

        // DirectXTKへ画像生成を委譲する(name: 画像名, context: 任意の即時コンテキスト, textureView: ビュー出力)。
        void __cdecl CreateTexture(
            const wchar_t* name,
            ID3D11DeviceContext* context,
            ID3D11ShaderResourceView** textureView) override
        {
            m_factory.CreateTexture(name, context, textureView);
        }

        // 記録した材質色を移動して取り出す。
        [[nodiscard]] std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> TakeDiffuseColors() noexcept
        {
            return std::move(m_diffuseColors);
        }

    private:
        // 元の描画効果生成器
        DirectX::EffectFactory m_factory;
        // 効果別の元材質色
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> m_diffuseColors;
    };

    // 上位8階層から物理ModelTextureフォルダーを探す(modelPath: 対象モデル)。
    std::filesystem::path FindCmoTextureDirectory(
        const std::filesystem::path& modelPath)
    {
        // 画像検索中のフォルダー
        auto directory = modelPath.parent_path();
        // 階層またはミップの深度
        for (std::size_t depth = 0;
             depth < 8 && !directory.empty();
             ++depth)
        {
            // 共通画像フォルダー
            const auto sharedTextures =
                directory / L"ModelTexture";
            if (std::filesystem::is_directory(
                    sharedTextures))
            {
                return sharedTextures;
            }

            // 検索先の親フォルダー
            const auto parent = directory.parent_path();
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return modelPath.parent_path();
    }

    // 失敗したHRESULTを操作名付き例外にする(result: 実行結果, operation: 操作名)。
    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    // 失敗したHRESULTを画像パス付き例外にする(result: 実行結果, path: 対象画像)。
    void ThrowIfFailed(const HRESULT result, const std::filesystem::path& path)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                "Failed to load texture: " + LamaPon::PathToUtf8(path)
                + " (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
        }
    }

    // D3D11のBackendなら借用ポインターを返す(backend: 任意の描画Backend)。
    [[nodiscard]] LamaPon::D3D11Backend* AsD3D11Backend(
        LamaPon::GraphicsBackend* const backend) noexcept
    {
        return dynamic_cast<LamaPon::D3D11Backend*>(backend);
    }

    // 材質のD3D11ビューを共有登録する(model: 更新対象, backend: D3D11なら取り込むBackend)。
    void ImportSkeletalTextureViews(
        LamaPon::SkeletalModel& model,
        LamaPon::GraphicsBackend* const backend)
    {
        // 借用D3D11Backend
        auto* const d3d11 = AsD3D11Backend(backend);
        if (d3d11 == nullptr)
        {
            return;
        }

        // 同じビューは一度だけ登録し、強所有ハンドルを共有する。
        // 元ビュー別の共通ハンドル
        std::unordered_map<
            ID3D11ShaderResourceView*,
            LamaPon::GraphicsViewHandle> importedViews;
        // 元ビューを共有登録する(native: 任意のD3D11ビュー)。
        const auto importView =
            [d3d11, &importedViews](
                ID3D11ShaderResourceView* const native)
            -> LamaPon::GraphicsViewHandle
        {
            if (native == nullptr)
            {
                return {};
            }
            // 登録済みビューの検索結果
            if (const auto found = importedViews.find(native);
                found != importedViews.end())
            {
                return found->second;
            }
            // 取り込んだ資源とビュー
            auto imported = d3d11->ImportShaderResourceView(native);
            // 画像を参照する描画ビュー
            auto view = std::move(imported.second);
            importedViews.emplace(native, view);
            return view;
        };

        // 材質ビューの更新対象
        for (auto& primitive : model.primitives)
        {
            // 共通形式の材質画像
            auto& textures = primitive.embeddedTextures;
            textures.albedo = importView(primitive.texture.Get());
            textures.normal = importView(primitive.normalTexture.Get());
            textures.roughness = importView(
                primitive.roughnessTexture.Get());
            textures.metallic = importView(
                primitive.metallicTexture.Get());
            textures.occlusion = importView(
                primitive.occlusionTexture.Get());
            textures.emissive = importView(
                primitive.emissiveTexture.Get());
            textures.occlusionStrength =
                primitive.occlusionStrength;
            textures.emissiveFactor = primitive.emissiveFactor;
        }
    }

    // 対応DXGI形式を共通形式へ変換し未対応なら例外にする(format: 元の形式)。
    [[nodiscard]] LamaPon::GraphicsTextureFormat ToGraphicsTextureFormat(
        const DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return LamaPon::GraphicsTextureFormat::Rgba8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return LamaPon::GraphicsTextureFormat::Bgra8Unorm;
        case DXGI_FORMAT_BC1_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc1Unorm;
        case DXGI_FORMAT_BC3_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc3Unorm;
        case DXGI_FORMAT_BC5_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc5Unorm;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return LamaPon::GraphicsTextureFormat::Rgba16Float;
        case DXGI_FORMAT_R8_UNORM:
            return LamaPon::GraphicsTextureFormat::R8Unorm;
        case DXGI_FORMAT_R8G8_UNORM:
            return LamaPon::GraphicsTextureFormat::Rg8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Rgba8UnormSrgb;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bgra8UnormSrgb;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return LamaPon::GraphicsTextureFormat::Bgrx8Unorm;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bgrx8UnormSrgb;
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bc1UnormSrgb;
        case DXGI_FORMAT_BC2_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc2Unorm;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bc2UnormSrgb;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bc3UnormSrgb;
        case DXGI_FORMAT_BC4_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc4Unorm;
        case DXGI_FORMAT_BC4_SNORM:
            return LamaPon::GraphicsTextureFormat::Bc4Snorm;
        case DXGI_FORMAT_BC5_SNORM:
            return LamaPon::GraphicsTextureFormat::Bc5Snorm;
        case DXGI_FORMAT_BC6H_UF16:
            return LamaPon::GraphicsTextureFormat::Bc6hUf16;
        case DXGI_FORMAT_BC6H_SF16:
            return LamaPon::GraphicsTextureFormat::Bc6hSf16;
        case DXGI_FORMAT_BC7_UNORM:
            return LamaPon::GraphicsTextureFormat::Bc7Unorm;
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return LamaPon::GraphicsTextureFormat::Bc7UnormSrgb;
        case DXGI_FORMAT_R16_FLOAT:
            return LamaPon::GraphicsTextureFormat::R16Float;
        case DXGI_FORMAT_R16G16_FLOAT:
            return LamaPon::GraphicsTextureFormat::Rg16Float;
        case DXGI_FORMAT_R32_FLOAT:
            return LamaPon::GraphicsTextureFormat::R32Float;
        case DXGI_FORMAT_R32G32_FLOAT:
            return LamaPon::GraphicsTextureFormat::Rg32Float;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return LamaPon::GraphicsTextureFormat::Rgba32Float;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return LamaPon::GraphicsTextureFormat::R10g10b10a2Unorm;
        case DXGI_FORMAT_R16G16_UNORM:
            return LamaPon::GraphicsTextureFormat::Rg16Unorm;
        case DXGI_FORMAT_B5G5R5A1_UNORM:
            return LamaPon::GraphicsTextureFormat::B5g5r5a1Unorm;
        case DXGI_FORMAT_B5G6R5_UNORM:
            return LamaPon::GraphicsTextureFormat::B5g6r5Unorm;
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return LamaPon::GraphicsTextureFormat::B4g4r4a4Unorm;
        case DXGI_FORMAT_R16_UNORM:
            return LamaPon::GraphicsTextureFormat::R16Unorm;
        case DXGI_FORMAT_A8_UNORM:
            return LamaPon::GraphicsTextureFormat::A8Unorm;
        case DXGI_FORMAT_R8G8_SNORM:
            return LamaPon::GraphicsTextureFormat::Rg8Snorm;
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return LamaPon::GraphicsTextureFormat::Rgba8Snorm;
        case DXGI_FORMAT_R16G16_SNORM:
            return LamaPon::GraphicsTextureFormat::Rg16Snorm;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return LamaPon::GraphicsTextureFormat::Rgba16Unorm;
        case DXGI_FORMAT_R16G16B16A16_SNORM:
            return LamaPon::GraphicsTextureFormat::Rgba16Snorm;
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
            return LamaPon::GraphicsTextureFormat::R8g8B8g8Unorm;
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
            return LamaPon::GraphicsTextureFormat::G8r8G8b8Unorm;
        case DXGI_FORMAT_R8_SNORM:
            return LamaPon::GraphicsTextureFormat::R8Snorm;
        case DXGI_FORMAT_R16_SNORM:
            return LamaPon::GraphicsTextureFormat::R16Snorm;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return LamaPon::GraphicsTextureFormat::R11g11b10Float;
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return LamaPon::GraphicsTextureFormat::R9g9b9e5SharedExp;
        default:
            throw std::invalid_argument(
                "The prepared texture format has no graphics backend mapping.");
        }
    }

    // 準備済み画像から2D生成設定を作る(data: 空でないミップ列)。
    [[nodiscard]] LamaPon::GraphicsTexture2DDescription
        MakeTextureDescription(
            const LamaPon::TextureLoader::PreparedTextureData& data)
    {
        if (data.levels.empty())
        {
            throw std::invalid_argument(
                "A prepared texture requires at least one mip level.");
        }
        if (data.levels.size()
            > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::overflow_error(
                "The prepared texture has too many mip levels.");
        }
        return {
            data.levels.front().width,
            data.levels.front().height,
            static_cast<std::uint32_t>(data.levels.size()),
            ToGraphicsTextureFormat(data.format)
        };
    }

    // 準備済みバイト列を借用して転送記述を作る(data: 転送API完了まで保持するミップ列)。
    [[nodiscard]] std::vector<LamaPon::GraphicsTextureSubresourceData>
        MakeTextureSubresources(
            const LamaPon::TextureLoader::PreparedTextureData& data)
    {
        // 借用する転送バイト列
        std::vector<LamaPon::GraphicsTextureSubresourceData> subresources;
        subresources.reserve(data.levels.size());
        // 処理対象のミップ
        for (const auto& level : data.levels)
        {
            if (level.bytes.size()
                > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::overflow_error(
                    "A prepared texture mip is too large.");
            }
            subresources.push_back({
                std::as_bytes(std::span{ level.bytes }),
                level.rowPitch,
                static_cast<std::uint32_t>(level.bytes.size())
            });
        }
        return subresources;
    }

    // DDSの面単位で2D生成設定を作る(data: 準備済みDDS)。
    [[nodiscard]] LamaPon::GraphicsTexture2DDescription
        MakeDdsFaceDescription(
            const LamaPon::TextureLoader::PreparedDdsTextureData& data)
    {
        return {
            data.width,
            data.height,
            data.mipLevels,
            ToGraphicsTextureFormat(data.format)
        };
    }

    // 深度と容量を検証してDDSバイト列の転送記述を作る(data: 転送API完了まで保持するDDS)。
    [[nodiscard]] std::vector<LamaPon::GraphicsTextureSubresourceData>
        MakeDdsSubresources(
            const LamaPon::TextureLoader::PreparedDdsTextureData& data)
    {
        // 借用する転送バイト列
        std::vector<LamaPon::GraphicsTextureSubresourceData> subresources;
        subresources.reserve(data.subresources.size());
        // DDS転送データの番号
        for (std::size_t index{}; index < data.subresources.size(); ++index)
        {
            // 元のDDS転送データ
            const auto& source = data.subresources[index];
            if (source.bytes.size()
                > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::overflow_error(
                    "A prepared DDS subresource is too large.");
            }
            // 深度1枚分の転送容量
            std::size_t slicePitch = source.bytes.size();
            if (data.dimension
                == LamaPon::TextureLoader::
                    PreparedDdsTextureDimension::Texture3D)
            {
                // 階層またはミップの深度
                const auto depth = std::max(
                    data.depth >> static_cast<std::uint32_t>(index),
                    1u);
                if (source.bytes.size() % depth != 0u)
                {
                    throw std::invalid_argument(
                        "A prepared DDS volume has an invalid slice pitch.");
                }
                slicePitch = source.bytes.size() / depth;
            }
            if (slicePitch > std::numeric_limits<std::uint32_t>::max())
            {
                throw std::overflow_error(
                    "A prepared DDS slice is too large.");
            }
            subresources.push_back({
                std::as_bytes(std::span{ source.bytes }),
                source.rowPitch,
                static_cast<std::uint32_t>(slicePitch)
            });
        }
        return subresources;
    }

    struct D3D12DdsResource final
    {
        // 生成したDDS画像資源
        LamaPon::GraphicsTextureHandle texture;
        // DDSの対応ビュー
        LamaPon::GraphicsViewHandle view;
        // キューブ次元の識別
        bool cube{};
    };

    // DDSの配列・キューブ・深度を維持して生成する(backend: 借用D3D12Backend, data: 準備済みDDS)。
    [[nodiscard]] D3D12DdsResource CreateD3D12DdsResource(
        LamaPon::D3D12Backend& backend,
        const LamaPon::TextureLoader::PreparedDdsTextureData& data)
    {
        // 借用する転送バイト列
        const auto subresources = MakeDdsSubresources(data);
        // DDSの面単位の生成設定
        const auto faceDescription = MakeDdsFaceDescription(data);
        using Dimension =
            LamaPon::TextureLoader::PreparedDdsTextureDimension;
        switch (data.dimension)
        {
        case Dimension::Texture2D:
        {
            // 生成する画像アセット資源
            auto texture = backend.CreateTexture2D(
                faceDescription,
                subresources);
            // 画像を参照する描画ビュー
            auto view = backend.CreateShaderResourceView(
                texture,
                { 0u, data.mipLevels });
            return { std::move(texture), std::move(view), false };
        }
        case Dimension::Texture2DArray:
        {
            // 配列画像と対応する描画ビュー
            auto [texture, view] = backend.CreateTextureArray(
                faceDescription,
                data.arraySize,
                false,
                subresources);
            return { std::move(texture), std::move(view), false };
        }
        case Dimension::TextureCube:
        case Dimension::TextureCubeArray:
        {
            // 配列画像と対応する描画ビュー
            auto [texture, view] = backend.CreateTextureArray(
                faceDescription,
                data.arraySize,
                true,
                subresources);
            return { std::move(texture), std::move(view), true };
        }
        case Dimension::Texture3D:
        {
            // 画像資源の生成設定
            LamaPon::GraphicsTexture3DDescription description{
                data.width,
                data.height,
                data.depth,
                data.mipLevels,
                ToGraphicsTextureFormat(data.format)
            };
            // 生成する画像アセット資源
            auto texture = backend.CreateTexture3D(
                description,
                subresources);
            // 画像を参照する描画ビュー
            auto view = backend.CreateShaderResourceView(
                texture,
                { 0u, data.mipLevels });
            return { std::move(texture), std::move(view), false };
        }
        }
        throw std::invalid_argument("The DDS resource dimension is invalid.");
    }

    // D3D11のみ互換ビューを解決する(backend: 任意の描画Backend, view: 同世代の共通ビュー)。
    [[nodiscard]] Microsoft::WRL::ComPtr<
        ID3D11ShaderResourceView> ResolveCompatibilityView(
            LamaPon::GraphicsBackend* const backend,
            const LamaPon::GraphicsViewHandle& view)
    {
        // 解決したD3D11ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resolved;
        // 借用D3D11Backend
        if (auto* const d3d11 = AsD3D11Backend(backend))
        {
            resolved = d3d11->ResolveShaderResourceView(view);
        }
        return resolved;
    }

    // 互換ビューの解決後に完成した資源組を公開する(asset: 公開先, backend: 任意の描画Backend, texture: 同世代の画像, view: 同世代のビュー)。
    template <typename Asset>
    void PublishTextureResources(
        Asset& asset,
        LamaPon::GraphicsBackend* const backend,
        LamaPon::GraphicsTextureHandle texture,
        LamaPon::GraphicsViewHandle view)
    {

        // 解決した互換ビュー
        auto compatibility = ResolveCompatibilityView(backend, view);
        asset.resources.Publish(
            LamaPon::TextureResourceSnapshot{
                std::move(texture),
                std::move(view),
                std::move(compatibility)
            });
    }

    // 共通ハンドルが空のD3D11互換ビューを公開する(asset: 公開先, view: 所有する互換ビュー)。
    template <typename Asset>
    void PublishLegacyTextureView(
        Asset& asset,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view)
    {
        asset.resources.Publish(
            LamaPon::TextureResourceSnapshot{
                {},
                {},
                std::move(view)
            });
    }

    // 先読みキャッシュの復号済みバイトを消す(entry: 対象の共有バイト列)。
    // 使用中のバイトを壊さないよう、ほかに保持者がいない唯一保持のときだけ消す。
    void SecureErasePrefetchEntry(
        const std::shared_ptr<const std::vector<std::uint8_t>>& entry)
    {
        if (entry && entry.use_count() == 1)
        {
            LamaPon::Crypto::SecureErase(
                const_cast<std::vector<std::uint8_t>&>(*entry));
        }
    }

    // 準備済み画像の全ミップを生成して公開する(asset: 公開先, backend: 描画Backend, data: 転送用ミップ列)。
    template <typename Asset>
    void CreatePreparedTextureResources(
        Asset& asset,
        LamaPon::GraphicsBackend& backend,
        LamaPon::TextureLoader::PreparedTextureData& data)
    {
        // 画像資源の生成設定
        const auto description = MakeTextureDescription(data);
        // 借用する転送バイト列
        const auto subresources = MakeTextureSubresources(data);
        // 生成する画像アセット資源
        auto texture = backend.CreateTexture2D(
            description,
            subresources);
        // GPUへ転送し終えた復号済みピクセルをメモリから消す。
        for (auto& level : data.levels)
        {
            LamaPon::Crypto::SecureErase(level.bytes);
        }
        // 画像を参照する描画ビュー
        auto view = backend.CreateShaderResourceView(
            texture,
            LamaPon::GraphicsTextureViewDescription{
                0,
                description.mipLevels
            });
        PublishTextureResources(
            asset,
            &backend,
            std::move(texture),
            std::move(view));
    }

    // パスの拡張子を小文字化する(path: 対象パス)。
    [[nodiscard]] std::wstring LoweredExtension(
        const std::filesystem::path& path)
    {
        // 小文字化したファイル拡張子
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            std::towlower);
        return extension;
    }

    // 画像生成も先読みする拡張子か調べる(extension: 小文字の拡張子)。
    [[nodiscard]] bool IsTextureExtension(
        const std::wstring& extension) noexcept
    {
        return extension == L".dds"
            || extension == L".png"
            || extension == L".jpg"
            || extension == L".jpeg"
            || extension == L".bmp"
            || extension == L".gif"
            || extension == L".tif"
            || extension == L".tiff";
    }

    // 組み込み図形の種類を返す(path: 組み込み指定パス)。
    [[nodiscard]] std::wstring BuiltInTextureKind(
        const std::filesystem::path& path)
    {
        // 正規化する組み込み指定
        auto name = path.generic_wstring();
        std::ranges::transform(
            name,
            name.begin(),
            std::towlower);
        if (name == L"builtin/circle"
            || name == L"builtin/triangle"
            || name == L"builtin/ring")
        {
            return name.substr(8);
        }
        return {};
    }

    // 退化線分も含め点から線分への距離を求める(pointX: 点X, pointY: 点Y, ax: 始点X, ay: 始点Y, bx: 終点X, by: 終点Y)。
    [[nodiscard]] float SegmentDistance(
        const float pointX,
        const float pointY,
        const float ax,
        const float ay,
        const float bx,
        const float by) noexcept
    {
        // 線分の横方向成分
        const float edgeX = bx - ax;
        // 線分の縦方向成分
        const float edgeY = by - ay;
        // 線分長の二乗
        const float lengthSquared = edgeX * edgeX + edgeY * edgeY;
        // 線分上の最近点の割合
        const float projection = lengthSquared > 0.0f
            ? std::clamp(
                ((pointX - ax) * edgeX
                    + (pointY - ay) * edgeY)
                    / lengthSquared,
                0.0f,
                1.0f)
            : 0.0f;
        // 最近点からの横距離
        const float deltaX = pointX - (ax + edgeX * projection);
        // 最近点からの縦距離
        const float deltaY = pointY - (ay + edgeY * projection);
        return std::sqrt(deltaX * deltaX + deltaY * deltaY);
    }

    // 白い図形をRGBA画像として生成する(backend: 任意の描画Backend, device: 互換デバイス, sourcePath: 組み込み指定, kind: 図形種類)。
    [[nodiscard]] std::shared_ptr<LamaPon::TextureAsset>
        CreateBuiltInTexture(
            LamaPon::GraphicsBackend* const backend,
            ID3D11Device* device,
            const std::filesystem::path& sourcePath,
            const std::wstring& kind)
    {
        // 組み込み画像の一辺
        constexpr std::uint32_t Size = 256;
        // 図形の白色RGBA画素列
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(Size) * Size * 4u,
            0u);
        // 境界距離を被覆率に変換する(signedDistance: 図形内側が正の距離)。
        const auto edge = [](
                const float signedDistance) noexcept
        {
            return std::clamp(
                signedDistance + 0.5f,
                0.0f,
                1.0f);
        };
        // 有向辺と点の外積を求める(ax: 始点X, ay: 始点Y, bx: 終点X, by: 終点Y, px: 点X, py: 点Y)。
        const auto cross = [](
                const float ax,
                const float ay,
                const float bx,
                const float by,
                const float px,
                const float py) noexcept
        {
            return (bx - ax) * (py - ay)
                - (by - ay) * (px - ax);
        };
        // 図形画像の行番号
        for (std::uint32_t y{}; y < Size; ++y)
        {
            // 図形画像の列番号
            for (std::uint32_t x{}; x < Size; ++x)
            {
                // 画素中心のX座標
                const float pointX = static_cast<float>(x) + 0.5f;
                // 画素中心のY座標
                const float pointY = static_cast<float>(y) + 0.5f;
                // 図形内側が正の境界距離
                float signedDistance = -1000.0f;
                if (kind == L"circle")
                {
                    // 図形中心からの横距離
                    const float dx = pointX - 128.0f;
                    // 図形中心からの縦距離
                    const float dy = pointY - 128.0f;
                    signedDistance =
                        112.0f - std::sqrt(dx * dx + dy * dy);
                }
                else if (kind == L"ring")
                {
                    // 図形中心からの横距離
                    const float dx = pointX - 128.0f;
                    // 図形中心からの縦距離
                    const float dy = pointY - 128.0f;
                    // 中心または辺からの距離
                    const float distance =
                        std::sqrt(dx * dx + dy * dy);
                    signedDistance =
                        8.0f - std::abs(distance - 96.0f);
                }
                else
                {
                    // 三角形の頂点AX
                    constexpr float ax = 128.0f;
                    // 三角形の頂点AY
                    constexpr float ay = 16.0f;
                    // 三角形の頂点BX
                    constexpr float bx = 240.0f;
                    // 三角形の頂点BY
                    constexpr float by = 238.0f;
                    // 三角形の頂点CX
                    constexpr float cx = 16.0f;
                    // 三角形の頂点CY
                    constexpr float cy = 238.0f;
                    // 三角形内側の判定
                    const bool inside =
                        cross(ax, ay, bx, by, pointX, pointY) >= 0.0f
                        && cross(bx, by, cx, cy, pointX, pointY)
                            >= 0.0f
                        && cross(cx, cy, ax, ay, pointX, pointY)
                            >= 0.0f;
                    // 中心または辺からの距離
                    const float distance = std::min({
                        SegmentDistance(
                            pointX, pointY, ax, ay, bx, by),
                        SegmentDistance(
                            pointX, pointY, bx, by, cx, cy),
                        SegmentDistance(
                            pointX, pointY, cx, cy, ax, ay) });
                    signedDistance = inside ? distance : -distance;
                }
                // 図形の境界被覆率
                const auto alpha = static_cast<std::uint8_t>(
                    std::lround(edge(signedDistance) * 255.0f));
                // 画素RGBAの先頭位置
                const auto pixelIndex =
                    (static_cast<std::size_t>(y) * Size + x) * 4u;
                pixels[pixelIndex] = 255u;
                pixels[pixelIndex + 1] = 255u;
                pixels[pixelIndex + 2] = 255u;
                pixels[pixelIndex + 3] = alpha;
            }
        }
        // 生成または回収したアセット
        auto asset = std::make_shared<LamaPon::TextureAsset>();
        asset->width = Size;
        asset->height = Size;
        asset->gpuBytes = static_cast<std::uint64_t>(Size) * Size * 4u;
        asset->sourcePath = sourcePath.lexically_normal();
        if (backend != nullptr)
        {
            // 転送用の画像データ
            LamaPon::TextureLoader::PreparedTextureData prepared;
            prepared.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            prepared.levels.push_back(
                LamaPon::TextureLoader::PreparedTextureLevel{
                    Size,
                    Size,
                    Size * 4u,
                    std::move(pixels)
                });
            CreatePreparedTextureResources(
                *asset,
                *backend,
                prepared);
            return asset;
        }
        if (device == nullptr)
        {
            throw std::runtime_error(
                "Cannot create built-in texture without a D3D11 device.");
        }
        // D3D11画像の生成設定
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = Size;
        textureDescription.Height = Size;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // 画像転送の初期データ
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = pixels.data();
        initialData.SysMemPitch = Size * 4u;
        // 生成する画像アセット資源
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                &initialData,
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(built-in)");
        // D3D11ビューの生成設定
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = textureDescription.Format;
        viewDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipLevels = 1;
        // 画像を参照する描画ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                texture.Get(),
                &viewDescription,
                view.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(built-in)");
        PublishLegacyTextureView(*asset, std::move(view));
        return asset;
    }
}

namespace LamaPon
{
    namespace Detail
    {
        class TextureResourceSlot final
        {
        public:
            // 空の資源組を持つ共有公開先を作る。
            TextureResourceSlot()
                : current(
                    std::make_shared<const TextureResourceSnapshot>())
            {
            }

            // 同世代の公開資源組
            std::atomic<std::shared_ptr<const TextureResourceSnapshot>>
                current;
        };
    }

    // 空の共有公開先を作る。
    TextureResourceBinding::TextureResourceBinding()
        : m_slot(std::make_shared<Detail::TextureResourceSlot>())
    {
    }

    TextureResourceBinding::~TextureResourceBinding() = default;

    TextureResourceBinding::TextureResourceBinding(
        const TextureResourceBinding&) noexcept = default;

    // 移動元を空にせず公開先を共有する(other: 元の公開窓口)。
    TextureResourceBinding::TextureResourceBinding(
        TextureResourceBinding&& other) noexcept
        : m_slot(other.m_slot)
    {
    }

    TextureResourceBinding& TextureResourceBinding::operator=(
        const TextureResourceBinding&) noexcept = default;

    // 移動元を空にせず公開先を共有する(other: 元の公開窓口)。
    TextureResourceBinding& TextureResourceBinding::operator=(
        TextureResourceBinding&& other) noexcept
    {
        m_slot = other.m_slot;
        return *this;
    }

    std::shared_ptr<const TextureResourceSnapshot>
        TextureResourceBinding::Acquire() const noexcept
    {
        return m_slot != nullptr
            ? m_slot->current.load(std::memory_order_acquire)
            : nullptr;
    }

    void TextureResourceBinding::Publish(
        TextureResourceSnapshot snapshot)
    {
        // 一括公開する完成資源組
        auto published = std::make_shared<const TextureResourceSnapshot>(
            std::move(snapshot));
        if (m_slot == nullptr)
        {
            m_slot = std::make_shared<Detail::TextureResourceSlot>();
        }
        m_slot->current.store(
            std::move(published),
            std::memory_order_release);
    }

    AssetManager::AssetManager(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context)
        : AssetManager(device, context, nullptr)
    {
    }

    AssetManager::AssetManager(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        GraphicsBackend& backend)
        : AssetManager(device, context, &backend)
    {
    }

    AssetManager::AssetManager(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        GraphicsBackend* backend)
        : m_device(device)
        , m_context(context)
        , m_backend(backend)
    {
        if (m_backend != nullptr && !m_backend->IsInitialized())
        {
            throw std::invalid_argument(
                "AssetManager requires an initialized graphics backend.");
        }
        // 借用D3D11Backend
        if (auto* const d3d11 = AsD3D11Backend(m_backend))
        {
            if (m_device == nullptr)
            {
                m_device = d3d11->Device();
            }
            if (m_context == nullptr)
            {
                m_context = d3d11->Context();
            }
            if (m_device != d3d11->Device()
                || m_context != d3d11->Context())
            {
                throw std::invalid_argument(
                    "AssetManager device/context do not match the graphics "
                    "backend.");
            }
        }
        else if (m_backend != nullptr
            && (m_device != nullptr || m_context != nullptr))
        {
            throw std::invalid_argument(
                "A non-D3D11 backend cannot borrow DirectX 11 objects.");
        }

        ThrowIfFailed(
            D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                m_d2dFactory.ReleaseAndGetAddressOf()),
            "D2D1CreateFactory");
        ThrowIfFailed(
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(
                    m_dwriteFactory.ReleaseAndGetAddressOf())),
            "DWriteCreateFactory");
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(m_wicFactory.ReleaseAndGetAddressOf())),
            "CoCreateInstance(WIC)");
    }

    AssetManager::~AssetManager()
    {
        QuiesceGraphicsWork();
    }

    void AssetManager::QuiesceGraphicsWork() noexcept
    {
        try
        {
            {
                // 実行中モデル処理の排他
                std::scoped_lock lock(m_graphicsWorkMutex);
                m_acceptingGraphicsWork = false;
            }

            // 終了待ちより先に予算待ちを解除し、次フレームがない終了時のデッドロックを防ぐ。
            DisableModelUploadThrottle();

            // 実行中モデル処理の排他
            std::unique_lock lock(m_graphicsWorkMutex);
            // 実行中モデル処理がなくなるまで待つ。
            m_graphicsWorkCondition.wait(
                lock,
                [this]
                {
                    return m_activeGraphicsWork == 0;
                });
        }
        catch (...)
        {

        }
        WaitForModelPreparation();
    }

    bool AssetManager::TryBeginGraphicsWork() noexcept
    {
        try
        {
            // 実行中モデル処理の排他
            std::scoped_lock lock(m_graphicsWorkMutex);
            if (!m_acceptingGraphicsWork)
            {
                return false;
            }
            ++m_activeGraphicsWork;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void AssetManager::EndGraphicsWork() noexcept
    {
        try
        {
            {
                // 実行中モデル処理の排他
                std::scoped_lock lock(m_graphicsWorkMutex);
                if (m_activeGraphicsWork > 0)
                {
                    --m_activeGraphicsWork;
                }
            }
            m_graphicsWorkCondition.notify_all();
        }
        catch (...)
        {
        }
    }

    void AssetManager::SetAssetRoot(std::filesystem::path assetRoot)
    {
        SetAssetRoot(std::move(assetRoot), true);
    }

    void AssetManager::SetAssetRoot(
        std::filesystem::path assetRoot,
        const bool createMissingMeta)
    {
        // ルートとアーカイブを参照するモデル準備を切替前に完了させる。
        WaitForModelPreparation();
        m_assetRoot = std::filesystem::absolute(std::move(assetRoot)).lexically_normal();
        m_archive.reset();
        if (!std::filesystem::is_directory(m_assetRoot))
        {

            // 暗号化アーカイブのパス
            auto archivePath = m_assetRoot;
            archivePath += L".tpak";
            if (std::filesystem::is_regular_file(archivePath))
            {
                m_archive = AssetArchive::Open(archivePath);
            }
        }
        m_database.SetAssetRoot(m_assetRoot);
        static_cast<void>(
            m_database.Refresh(createMissingMeta));
        Clear();
    }

    std::filesystem::path AssetManager::ResolvePath(const std::filesystem::path& path) const
    {
        if (path.is_absolute())
        {
            return path.lexically_normal();
        }

        return (m_assetRoot / path).lexically_normal();
    }

    std::vector<std::uint8_t> AssetManager::ReadFileBytes(
        const std::filesystem::path& path) const
    {
        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey = MakeCacheKey(resolvedPath);
        // 先読み済みの共有バイト列
        std::shared_ptr<
            const std::vector<std::uint8_t>>
                cachedBytes;
        {
            // 先読みキャッシュの排他
            std::scoped_lock lock(m_prefetchMutex);
            // ディスクまたは先読みの結果
            if (const auto cached =
                    m_prefetchedBytes.find(cacheKey);
                cached != m_prefetchedBytes.end())
            {
                cachedBytes = cached->second;
            }
        }
        if (cachedBytes)
        {
            return *cachedBytes;
        }
        return ReadFileBytesUncached(resolvedPath);
    }

    std::vector<std::uint8_t> AssetManager::ReadFileBytesFresh(
        const std::filesystem::path& path) const
    {
        return ReadFileBytesUncached(ResolvePath(path));
    }

    std::vector<std::uint8_t>
        AssetManager::ReadFileBytesUncached(
            const std::filesystem::path& resolvedPath) const
    {
        if (m_archive)
        {
            // アーカイブ内の相対パス
            const auto relative =
                resolvedPath.lexically_relative(m_assetRoot);
            // 読み込むアセットのバイト列
            if (auto bytes = m_archive->TryRead(relative))
            {
                return std::move(*bytes);
            }
            throw std::runtime_error(
                "Asset not found in archive: "
                + PathToUtf8(relative));
        }

        // 通常ファイルの読み取り
        std::ifstream input(
            resolvedPath,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open asset file: "
                + PathToUtf8(resolvedPath));
        }
        // ファイル終端の位置
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine asset size: "
                + PathToUtf8(resolvedPath));
        }
        // ファイル容量分の読込先
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!input)
            {
                throw std::runtime_error(
                    "Failed to read asset file: "
                    + PathToUtf8(resolvedPath));
            }
        }
        return bytes;
    }

    bool AssetManager::FileExists(
        const std::filesystem::path& path) const
    {
        if (!BuiltInTextureKind(path).empty())
        {
            return true;
        }
        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        if (m_archive)
        {
            return m_archive->Contains(
                resolvedPath.lexically_relative(m_assetRoot));
        }
        return std::filesystem::is_regular_file(resolvedPath);
    }

    AssetPrefetchReport AssetManager::PrefetchFiles(
        const std::vector<std::filesystem::path>& paths,
        const std::function<bool(
            const std::size_t,
            const std::size_t)>& progress)
    {
        // 先読み処理の集計
        AssetPrefetchReport report;
        report.requestedFiles = paths.size();
        if (progress && !progress(0, paths.size()))
        {
            report.cancelled = true;
            return report;
        }

        // 先読みの処理済み件数
        std::size_t completed{};
        // 先読み対象のパス
        for (const auto& path : paths)
        {
            if (!BuiltInTextureKind(path).empty())
            {
                try
                {
                    if (m_backend != nullptr || m_device != nullptr)
                    {
                        static_cast<void>(LoadTexture(path));
                        ++report.preparedTextures;
                    }
                    ++report.loadedFiles;
                }
                catch (const std::exception&)
                {
                    ++report.failedFiles;
                }
                ++completed;
                if (progress
                    && !progress(completed, paths.size()))
                {
                    report.cancelled = true;
                    break;
                }
                continue;
            }
            // 解決済みのアセットパス
            const auto resolvedPath = ResolvePath(path);
            // パスの再利用キー
            const auto key = MakeCacheKey(resolvedPath);

            try
            {
                // 先読みの再利用済み判定
                bool foundExisting{};
                for (;;)
                {
                    // 読み取り開始時の画像世代
                    std::uint64_t textureEpoch{};
                    // 読み取り開始時のパス世代
                    std::uint64_t pathGeneration{};
                    // 読み取り開始時の先読み世代
                    std::uint64_t prefetchEpoch{};
                    {
                        // 世代更新と公開の一括排他
                        std::scoped_lock lock(
                            m_textureMutex,
                            m_prefetchMutex);
                        if (m_prefetchedBytes.contains(key))
                        {
                            ++report.cachedFiles;
                            foundExisting = true;
                            break;
                        }
                        textureEpoch = m_textureEpoch;
                        // パス世代の検索結果
                        const auto generation =
                            m_texturePathGenerations.find(key);
                        pathGeneration = generation
                                != m_texturePathGenerations.end()
                            ? generation->second
                            : 0;
                        prefetchEpoch = m_prefetchEpoch;
                    }

                    // 読み込むアセットのバイト列
                    auto bytes =
                        std::make_shared<
                            const std::vector<std::uint8_t>>(
                                ReadFileBytesUncached(
                                    resolvedPath));
                    // 読み取り中の無効化判定
                    bool generationChanged{};
                    {
                        // 無効化と同じ排他区間で世代を再確認し、変更済みなら読み直す。
                        // 世代更新と公開の一括排他
                        std::scoped_lock lock(
                            m_textureMutex,
                            m_prefetchMutex);
                        // パス世代の検索結果
                        const auto generation =
                            m_texturePathGenerations.find(key);
                        // 確定時の画像パス世代
                        const auto currentPathGeneration = generation
                                != m_texturePathGenerations.end()
                            ? generation->second
                            : 0;
                        generationChanged =
                            textureEpoch != m_textureEpoch
                            || pathGeneration
                                != currentPathGeneration
                            || prefetchEpoch != m_prefetchEpoch;
                        if (!generationChanged)
                        {
                            // 採用先の位置と新規採用の有無
                            const auto [iterator, inserted] =
                                m_prefetchedBytes.emplace(
                                    key,
                                    bytes);
                            static_cast<void>(iterator);
                            if (inserted)
                            {
                                ++report.loadedFiles;
                                report.loadedBytes +=
                                    bytes->size();
                                m_prefetchedByteCount +=
                                    bytes->size();
                            }
                            else
                            {
                                ++report.cachedFiles;
                            }
                        }
                    }
                    if (!generationChanged)
                    {
                        break;
                    }
                }

                // 画像生成に失敗しても先読みバイト列を残し、本読み込み側へエラー処理を委ねる。
                if (!foundExisting
                    && (m_backend != nullptr || m_device != nullptr)
                    && IsTextureExtension(
                        LoweredExtension(resolvedPath)))
                {
                    try
                    {
                        static_cast<void>(
                            LoadTexture(resolvedPath));
                        ++report.preparedTextures;
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
            catch (const std::exception&)
            {
                ++report.failedFiles;
            }

            ++completed;
            if (progress
                && !progress(completed, paths.size()))
            {
                report.cancelled = true;
                break;
            }
        }
        return report;
    }

    void AssetManager::ClearPrefetchedFiles() noexcept
    {
        try
        {
            // 先読みキャッシュの排他
            std::scoped_lock lock(m_prefetchMutex);
            ++m_prefetchEpoch;
            // 破棄前に復号済みバイトを消す。
            for (auto& [key, entry] : m_prefetchedBytes)
            {
                SecureErasePrefetchEntry(entry);
            }
            m_prefetchedBytes.clear();
            m_prefetchedByteCount = 0;
        }
        catch (...)
        {
        }
    }

    std::size_t AssetManager::PrefetchedFileCount()
        const noexcept
    {
        try
        {
            // 先読みキャッシュの排他
            std::scoped_lock lock(m_prefetchMutex);
            return m_prefetchedBytes.size();
        }
        catch (...)
        {
            return 0;
        }
    }

    std::size_t AssetManager::PrefetchedByteCount()
        const noexcept
    {
        try
        {
            // 先読みキャッシュの排他
            std::scoped_lock lock(m_prefetchMutex);
            return m_prefetchedByteCount;
        }
        catch (...)
        {
            return 0;
        }
    }

    std::shared_ptr<const TextureAsset> AssetManager::LoadTexture(
        const std::filesystem::path& path,
        const TextureLoader::TextureUsage usage)
    {
        // 組み込み図形の識別
        const auto builtInKind = BuiltInTextureKind(path);
        // 解決済みのアセットパス
        const auto resolvedPath = builtInKind.empty()
            ? ResolvePath(path)
            : path.lexically_normal();
        // 用途を含まないパスキー
        const auto baseCacheKey = MakeCacheKey(resolvedPath);
        // 用途で圧縮形式が異なるため、同じパスでもキャッシュを分ける。
        // アセットの再利用キー
        auto cacheKey = baseCacheKey;
        cacheKey += L"|u";
        cacheKey += static_cast<wchar_t>(
            L'0' + static_cast<int>(usage));

        for (;;)
        {
            // 画像読み込み開始時の世代
            std::uint64_t epoch{};
            // 読み取り開始時のパス世代
            std::uint64_t pathGeneration{};
            {
                // 画像キャッシュの排他
                std::scoped_lock lock(m_textureMutex);
                // 共有キャッシュの検索結果
                if (const auto existing =
                        m_textureCache.find(cacheKey);
                    existing != m_textureCache.end())
                {
                    return existing->second;
                }
                epoch = m_textureEpoch;
                pathGeneration =
                    m_texturePathGenerations[baseCacheKey];
            }

            // ロック外で生成し、無効化された旧結果は公開せず読み直す。
            // 生成後に必要な段階転送
            std::optional<PendingTextureUpload> pendingUpload;
            // 生成する画像アセット資源
            auto texture = builtInKind.empty()
                ? LoadTextureUncached(
                    resolvedPath,
                    usage,
                    pendingUpload)
                : CreateBuiltInTexture(
                    m_backend,
                    m_device,
                    resolvedPath,
                    builtInKind);

            {
                // 世代確認・キャッシュ採用・段階転送登録を一括で確定する。
                // 画像公開と転送登録の排他
                std::scoped_lock lock(m_textureMutex, m_uploadMutex);
                // 確定時のパス世代検索
                const auto currentGeneration =
                    m_texturePathGenerations.find(baseCacheKey);
                // 確定時の画像パス世代
                const auto currentPathGeneration =
                    currentGeneration !=
                            m_texturePathGenerations.end()
                        ? currentGeneration->second
                        : 0;
                if (epoch == m_textureEpoch
                    && pathGeneration == currentPathGeneration)
                {
                    // 採用先の位置と新規採用の有無
                    const auto [iterator, inserted] =
                        m_textureCache.try_emplace(
                            cacheKey,
                            texture);
                    if (inserted && pendingUpload)
                    {
                        try
                        {
                            m_pendingUploads.push_back(
                                std::move(*pendingUpload));
                        }
                        catch (...)
                        {
                            // 仮表示だけが残らないようキャッシュ採用も取り消す。
                            m_textureCache.erase(iterator);
                            throw;
                        }
                    }
                    return iterator->second;
                }
            }
        }
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        AssetManager::CreateTextureViewFromMemory(
            const std::span<const std::uint8_t> bytes,
            const bool isDds,
            const TextureLoader::TextureUsage usage)
    {
        // 画像を参照する描画ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (bytes.empty())
        {
            return view;
        }
        if (isDds)
        {

            ThrowIfFailed(
                DirectX::CreateDDSTextureFromMemory(
                    m_device,
                    bytes.data(),
                    bytes.size(),
                    nullptr,
                    view.ReleaseAndGetAddressOf()),
                "Creating a DDS texture from memory");
            return view;
        }

        // WIC画像のBC圧縮許可
        const bool compress = RuntimeTextureCompressionEnabled();
        // 画像内容と用途の保存キー
        const std::uint64_t diskCacheKey =
            TextureCache::ComputeKey(bytes, compress, usage);
        // 転送用の画像データ
        TextureLoader::PreparedTextureData prepared;
        // ディスクまたは先読みの結果
        if (auto cached = TextureCache::TryLoad(diskCacheKey))
        {
            prepared = std::move(cached->data);
        }
        else
        {
            // 生成したRGBAミップ列
            auto mips = TextureLoader::GenerateMipChain(
                TextureLoader::DecodeImageBytes(bytes));
            // 保存する画像キャッシュ
            TextureCache::CachedTexture entry;
            std::copy_n(
                mips.back().pixels.begin(),
                entry.placeholderPixel.size(),
                entry.placeholderPixel.begin());
            entry.data = TextureLoader::PrepareTextureData(
                std::move(mips),
                compress,
                usage);
            TextureCache::Store(diskCacheKey, entry);
            prepared = std::move(entry.data);
        }
        // モデル画像は法線の仮表示による陰影崩れを避け、全ミップを一度に転送する。
        return TextureLoader::CreateTexture(m_device, prepared);
    }

    GraphicsViewHandle
        AssetManager::CreateTextureViewHandleFromMemory(
            const std::span<const std::uint8_t> bytes,
            const bool isDds,
            const TextureLoader::TextureUsage usage)
    {
        if (bytes.empty() || m_backend == nullptr)
        {
            return {};
        }
        if (isDds)
        {
            if (auto* const d3d12 = dynamic_cast<D3D12Backend*>(m_backend))
            {
                return CreateD3D12DdsResource(
                    *d3d12,
                    TextureLoader::PrepareDdsResourceData(bytes)).view;
            }
        }
        if (isDds && TextureLoader::IsDdsCubeTexture(bytes))
        {
            // 借用D3D11Backend
            if (auto* const d3d11 = AsD3D11Backend(m_backend))
            {
                // 画像のD3D11ビュー
                auto nativeView = CreateTextureViewFromMemory(
                    bytes,
                    true,
                    usage);
                return d3d11->ImportShaderResourceView(nativeView.Get())
                    .second;
            }
            return {};
        }
        // 転送用の画像データ
        auto prepared = isDds
            ? TextureLoader::PrepareDdsTextureData(bytes)
            : TextureLoader::PrepareTextureData(
                TextureLoader::GenerateMipChain(
                    TextureLoader::DecodeImageBytes(bytes)),
                RuntimeTextureCompressionEnabled(),
                usage);
        // 画像資源の生成設定
        const auto description = MakeTextureDescription(prepared);
        // 借用する転送バイト列
        const auto subresources = MakeTextureSubresources(prepared);
        // 生成する画像アセット資源
        auto texture = m_backend->CreateTexture2D(
            description,
            subresources);
        // GPUへ転送し終えた復号済みピクセルをメモリから消す。
        for (auto& level : prepared.levels)
        {
            Crypto::SecureErase(level.bytes);
        }
        return m_backend->CreateShaderResourceView(
            texture,
            GraphicsTextureViewDescription{
                0,
                description.mipLevels });
    }

    std::shared_ptr<TextureAsset>
        AssetManager::LoadTextureUncached(
            const std::filesystem::path& resolvedPath,
            const TextureLoader::TextureUsage usage,
            std::optional<PendingTextureUpload>& pendingUpload)
    {
        pendingUpload.reset();
        if (!FileExists(resolvedPath))
        {
            throw std::runtime_error("Texture file does not exist: " + LamaPon::PathToUtf8(resolvedPath));
        }

        // 生成する画像アセット資源
        auto texture = std::make_shared<TextureAsset>();
        texture->sourcePath = resolvedPath;

        // 小文字化したファイル拡張子
        const auto extension =
            LoweredExtension(resolvedPath);
        // 読み込むアセットのバイト列
        const auto bytes = ReadFileBytes(resolvedPath);
        if (extension == L".dds")
        {
            // 借用D3D11Backend
            if (auto* const d3d11 = AsD3D11Backend(m_backend))
            {

                // 読み込んだDDSビュー
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
                    loadedView;
                ThrowIfFailed(
                    DirectX::CreateDDSTextureFromMemory(
                        m_device,
                        bytes.data(),
                        bytes.size(),
                        nullptr,
                        loadedView.ReleaseAndGetAddressOf()),
                    resolvedPath);
                // 同世代の画像と描画ビュー
                auto [textureHandle, viewHandle] =
                    d3d11->ImportShaderResourceView(
                        loadedView.Get());
                PublishTextureResources(
                    *texture,
                    m_backend,
                    std::move(textureHandle),
                    std::move(viewHandle));
            }
            else if (auto* const d3d12 = dynamic_cast<D3D12Backend*>(
                    m_backend))
            {

                // 転送用の画像データ
                const auto prepared =
                    TextureLoader::PrepareDdsResourceData(bytes);
                // 画像のGPU資源
                auto resource = CreateD3D12DdsResource(*d3d12, prepared);
                texture->width = prepared.width;
                texture->height = prepared.height;
                texture->isCube = resource.cube;
                // DDSの転送データ
                for (const auto& subresource : prepared.subresources)
                {
                    texture->gpuBytes += subresource.bytes.size();
                }
                PublishTextureResources(
                    *texture,
                    m_backend,
                    std::move(resource.texture),
                    std::move(resource.view));
            }
            else
            {
                // 読み込んだDDSビュー
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
                    loadedView;
                ThrowIfFailed(
                    DirectX::CreateDDSTextureFromMemory(
                        m_device,
                        bytes.data(),
                        bytes.size(),
                        nullptr,
                        loadedView.ReleaseAndGetAddressOf()),
                    resolvedPath);
                PublishLegacyTextureView(
                    *texture,
                    std::move(loadedView));
            }
        }
        else
        {

            // WIC画像のBC圧縮許可
            const bool compress =
                RuntimeTextureCompressionEnabled();
            // 画像内容と用途の保存キー
            const std::uint64_t diskCacheKey =
                TextureCache::ComputeKey(bytes, compress, usage);
            // 転送用の画像データ
            TextureLoader::PreparedTextureData prepared;

            // 仮表示の1×1平均色画像
            TextureLoader::CpuImage placeholder;
            placeholder.width = 1;
            placeholder.height = 1;
            // ディスクまたは先読みの結果
            if (auto cached =
                    TextureCache::TryLoad(diskCacheKey))
            {
                prepared = std::move(cached->data);
                placeholder.pixels.assign(
                    cached->placeholderPixel.begin(),
                    cached->placeholderPixel.end());
            }
            else
            {
                // 生成したRGBAミップ列
                auto mips =
                    TextureLoader::GenerateMipChain(
                        TextureLoader::DecodeImageBytes(
                            bytes));
                placeholder = mips.back();
                prepared =
                    TextureLoader::PrepareTextureData(
                        std::move(mips),
                        compress,
                        usage);
                // 保存する画像キャッシュ
                TextureCache::CachedTexture entry;

                std::copy_n(
                    placeholder.pixels.begin(),
                    entry.placeholderPixel.size(),
                    entry.placeholderPixel.begin());
                entry.data = std::move(prepared);
                TextureCache::Store(diskCacheKey, entry);
                prepared = std::move(entry.data);
            }

            texture->gpuBytes = prepared.TotalBytes();
            if ((m_backend != nullptr || m_context != nullptr)
                && prepared.TotalBytes()
                    >= ProgressiveUploadThreshold())
            {
                // 転送完了までは1×1の平均色を公開し、粗いミップから差し替える。
                texture->width = prepared.levels[0].width;
                texture->height = prepared.levels[0].height;
                // 段階転送先のD3D11画像
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    gpuTexture;
                // 段階転送先の共通画像
                GraphicsTextureHandle gpuTextureHandle;
                if (m_backend != nullptr)
                {
                    // ミップ更新可能な生成設定
                    auto uploadDescription =
                        MakeTextureDescription(prepared);
                    uploadDescription.updateMode =
                        GraphicsTextureUpdateMode::PerMipUpdate;
                    gpuTextureHandle = m_backend->CreateTexture2D(
                        uploadDescription,
                        {});
                    // 仮表示のRGBA転送データ
                    TextureLoader::PreparedTextureData
                        placeholderData;
                    placeholderData.format =
                        DXGI_FORMAT_R8G8B8A8_UNORM;
                    placeholderData.levels.push_back(
                        TextureLoader::PreparedTextureLevel{
                            placeholder.width,
                            placeholder.height,
                            placeholder.width * 4u,
                            std::move(placeholder.pixels)
                        });
                    CreatePreparedTextureResources(
                        *texture,
                        *m_backend,
                        placeholderData);
                }
                else
                {
                    gpuTexture =
                        TextureLoader::CreateUploadableTexture(
                            m_device,
                            prepared);
                    PublishLegacyTextureView(
                        *texture,
                        TextureLoader::CreateTexture(
                            m_device,
                            std::vector<TextureLoader::CpuImage>{
                                std::move(placeholder)
                            },
                            false));
                }

                // 最小ミップの番号
                const auto lastLevel =
                    static_cast<std::ptrdiff_t>(
                        prepared.levels.size()) - 1;
                pendingUpload.emplace(
                    PendingTextureUpload{
                        texture,
                        std::move(gpuTexture),
                        std::move(gpuTextureHandle),
                        std::move(prepared),
                        lastLevel
                    });
                return texture;
            }


            texture->width = prepared.levels.front().width;
            texture->height = prepared.levels.front().height;
            texture->isCube = false;

            if (m_backend != nullptr)
            {
                CreatePreparedTextureResources(
                    *texture,
                    *m_backend,
                    prepared);
            }
            else
            {
                PublishLegacyTextureView(
                    *texture,
                    TextureLoader::CreateTexture(
                        m_device,
                        prepared));
            }
        }


        // 公開中の画像資源組
        const auto resources = texture->resources.Acquire();
        // 公開中のD3D11ビュー
        const auto compatibilityView = resources != nullptr
            ? resources->d3d11ShaderResourceView
            : nullptr;
        if (compatibilityView)
        {
            // 画像のGPU資源
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            compatibilityView->GetResource(
                resource.ReleaseAndGetAddressOf());
            // D3D11資源の次元
            D3D11_RESOURCE_DIMENSION dimension{};
            resource->GetType(&dimension);
            if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
            {

                // 深度を持つD3D11画像
                Microsoft::WRL::ComPtr<ID3D11Texture3D> texture3D;
                ThrowIfFailed(resource.As(&texture3D), resolvedPath);

                // 画像資源の生成設定
                D3D11_TEXTURE3D_DESC description{};
                texture3D->GetDesc(&description);
                texture->width = description.Width;
                texture->height = description.Height;
                texture->isCube = false;
            }
            else
            {
                // D3D11の2D画像
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
                ThrowIfFailed(resource.As(&texture2D), resolvedPath);

                // 画像資源の生成設定
                D3D11_TEXTURE2D_DESC description{};
                texture2D->GetDesc(&description);
                texture->width = description.Width;
                texture->height = description.Height;
                texture->isCube =
                    (description.MiscFlags
                        & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
            }
        }
        if (texture->gpuBytes == 0 && extension == L".dds")
        {
            // D3D11経路の容量は基本ヘッダーを除くファイルサイズによる概算とする。
            // 基本DDSヘッダー容量
            constexpr std::size_t DdsHeaderBytes = 128;
            texture->gpuBytes =
                bytes.size() > DdsHeaderBytes
                    ? bytes.size() - DdsHeaderBytes
                    : bytes.size();
        }
        return texture;
    }

    void AssetManager::PumpTextureUploads(
        const std::size_t byteBudget)
    {
        if (m_backend == nullptr && m_context == nullptr)
        {
            return;
        }

        // 段階転送キューの排他
        std::scoped_lock lock(m_uploadMutex);
        // 今回の画像転送容量
        std::size_t uploadedBytes = 0;
        while (!m_pendingUploads.empty())
        {
            // 転送待ち画像の先頭
            auto& pending = m_pendingUploads.front();

            if (pending.asset.use_count() == 1)
            {
                // 転送せず捨てる未転送の復号済みピクセルを消す。
                for (auto& level : pending.data.levels)
                {
                    Crypto::SecureErase(level.bytes);
                }
                m_pendingUploads.pop_front();
                continue;
            }

            // ビュー生成の失敗時に同じ範囲を再試行できるよう、進捗の確定を遅らせる。
            // 今回の転送後ミップ番号
            auto nextLevelAfterBatch = pending.nextLevel;
            // 今回の画像転送量
            std::size_t batchBytes = 0;
            while (nextLevelAfterBatch >= 0)
            {
                // 最初の1ミップを必ず進めるため、指定予算の超過を許容する。
                if ((uploadedBytes != 0 || batchBytes != 0)
                    && (uploadedBytes >= byteBudget
                        || batchBytes
                            >= byteBudget - uploadedBytes))
                {
                    break;
                }
                // 処理対象のミップ
                const auto& level = pending.data.levels[
                    static_cast<std::size_t>(
                        nextLevelAfterBatch)];
                if (batchBytes
                    > std::numeric_limits<std::size_t>::max()
                        - level.bytes.size())
                {
                    throw std::overflow_error(
                        "Progressive texture upload size overflowed.");
                }
                batchBytes += level.bytes.size();
                --nextLevelAfterBatch;
            }

            if (nextLevelAfterBatch == pending.nextLevel)
            {

                break;
            }

            // 公開範囲の最大解像度
            const auto mostDetailed =
                static_cast<std::uint32_t>(
                    nextLevelAfterBatch + 1);
            // 画像の全ミップ数
            const auto totalMipLevels =
                static_cast<std::uint32_t>(
                    pending.data.levels.size());
            // 転送後に公開するビュー
            GraphicsViewHandle nextView;
            // 転送後の互換ビュー
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
                nextCompatibilityView;
            if (pending.textureHandle)
            {
                if (m_backend == nullptr)
                {
                    throw std::logic_error(
                        "A progressive texture handle lost its backend.");
                }
                nextView = m_backend->CreateShaderResourceView(
                    pending.textureHandle,
                    GraphicsTextureViewDescription{
                        mostDetailed,
                        totalMipLevels - mostDetailed
                    });
                nextCompatibilityView = ResolveCompatibilityView(
                    m_backend,
                    nextView);
            }
            else
            {
                nextCompatibilityView =
                    TextureLoader::CreateTextureView(
                        m_device,
                        pending.texture.Get(),
                        pending.data.format,
                        mostDetailed,
                        totalMipLevels);
            }

            // 転送失敗時も公開中の資源組と進捗・CPUバイト列を維持する。
            // 今回転送するミップ番号
            for (auto levelIndex = pending.nextLevel;
                // 今回の転送後ミップ番号
                levelIndex > nextLevelAfterBatch;
                --levelIndex)
            {
                // 処理対象のミップ
                auto& level = pending.data.levels[
                    static_cast<std::size_t>(levelIndex)];
                if (pending.textureHandle)
                {
                    if (level.bytes.size()
                        > std::numeric_limits<std::uint32_t>::max())
                    {
                        throw std::overflow_error(
                            "A progressive texture mip is too large.");
                    }
                    m_backend->UpdateTexture2D(
                        pending.textureHandle,
                        static_cast<std::uint32_t>(levelIndex),
                        GraphicsTextureSubresourceData{
                            std::as_bytes(std::span{ level.bytes }),
                            level.rowPitch,
                            static_cast<std::uint32_t>(
                                level.bytes.size())
                        });
                }
                else
                {
                    m_context->UpdateSubresource(
                        pending.texture.Get(),
                        static_cast<UINT>(levelIndex),
                        nullptr,
                        level.bytes.data(),
                        level.rowPitch,
                        0);
                }
            }

            pending.asset->resources.Publish(
                TextureResourceSnapshot{
                    pending.textureHandle,
                    std::move(nextView),
                    std::move(nextCompatibilityView)
                });
            // 今回転送するミップ番号
            for (auto levelIndex = pending.nextLevel;
                // 今回の転送後ミップ番号
                levelIndex > nextLevelAfterBatch;
                --levelIndex)
            {
                // GPUへ転送し終えた復号済みピクセルをメモリから消す。
                Crypto::SecureErase(
                    pending.data.levels[
                        static_cast<std::size_t>(levelIndex)]
                            .bytes);
            }
            pending.nextLevel = nextLevelAfterBatch;
            uploadedBytes += batchBytes;

            if (pending.nextLevel < 0)
            {
                m_pendingUploads.pop_front();
                continue;
            }

            break;
        }
    }

    std::size_t
        AssetManager::PendingTextureUploadCount()
            const noexcept
    {
        // 段階転送キューの排他
        std::scoped_lock lock(m_uploadMutex);
        return m_pendingUploads.size();
    }

    void AssetManager::PumpModelUploads(
        const std::size_t byteBudget) noexcept
    {
        try
        {
            // 予算0でも進捗を止めないよう最低1バイトの予算を補充する。
            // 最低1バイトの補充予算
            const std::size_t effectiveBudget = std::max<std::size_t>(
                byteBudget,
                1u);
            {
                // モデル転送予算の排他
                std::scoped_lock lock(m_modelUploadMutex);
                m_modelUploadBytesLastFrame.store(
                    m_modelUploadBytesCurrentFrame,
                    std::memory_order_relaxed);
                m_modelUploadBytesCurrentFrame = 0;
                if (m_modelUploadThrottled)
                {
                    m_modelUploadFrameBudget = effectiveBudget;
                    m_modelUploadBudgetRemaining = effectiveBudget;
                }
            }
            m_modelUploadCondition.notify_all();
        }
        catch (...)
        {
        }
    }

    void AssetManager::WaitForModelUploadBudget(
        const std::size_t byteCount)
    {
        if (byteCount == 0)
        {
            return;
        }
        // モデル転送予算の排他
        std::unique_lock lock(m_modelUploadMutex);
        if (!m_modelUploadThrottled
            || std::this_thread::get_id()
                != m_modelPreparationThread)
        {
            return;
        }
        // 予算補充または終了による待機解除を確認する。
        m_modelUploadCondition.wait(
            lock,
            [this, byteCount]
            {
                // 待機解除に必要な予算
                const std::size_t required = std::min(
                    byteCount,
                    m_modelUploadFrameBudget);
                return !m_modelUploadThrottled
                    || (m_modelUploadFrameBudget > 0
                        && m_modelUploadBudgetRemaining
                            >= required);
            });
        if (!m_modelUploadThrottled)
        {
            return;
        }
        // 待機解除に必要な予算
        const std::size_t required = std::min(
            byteCount,
            m_modelUploadFrameBudget);
        m_modelUploadBudgetRemaining -= required;
        m_modelUploadBytesCurrentFrame += byteCount;
    }

    std::size_t AssetManager::PendingModelUploadCount()
        const noexcept
    {
        try
        {
            // モデル転送予算の排他
            std::scoped_lock lock(m_modelUploadMutex);
            return m_modelUploadThrottled ? 1u : 0u;
        }
        catch (...)
        {
            return 0;
        }
    }

    void AssetManager::DisableModelUploadThrottle() noexcept
    {
        try
        {
            {
                // モデル転送予算の排他
                std::scoped_lock lock(m_modelUploadMutex);
                m_modelUploadThrottled = false;
                m_modelUploadBudgetRemaining = 0;
            }
            m_modelUploadCondition.notify_all();
        }
        catch (...)
        {
        }
    }

    void AssetManager::EndModelUploadPreparation() noexcept
    {
        try
        {
            {
                // モデル転送予算の排他
                std::scoped_lock lock(m_modelUploadMutex);
                m_modelUploadThrottled = false;
                m_modelPreparationThread = {};
                m_modelUploadBudgetRemaining = 0;
            }
            m_modelUploadCondition.notify_all();
        }
        catch (...)
        {
        }
    }

    std::shared_ptr<const ModelAsset> AssetManager::LoadModel(
        const std::filesystem::path& path)
    {
        if (!TryBeginGraphicsWork())
        {
            throw std::logic_error(
                "AssetManager is stopping graphics work.");
        }
        // 所有者を破棄せず処理数を減らす(owner: 処理を借用した管理器)。
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        // 所有者を破棄せず処理数を戻す終了処理
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        return LoadModelImpl(path);
    }

    std::shared_ptr<const ModelAsset> AssetManager::LoadModelImpl(
        const std::filesystem::path& path)
    {

        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey = MakeCacheKey(resolvedPath);
        // 同じパスの準備結果
        std::future<std::shared_ptr<ModelAsset>> preparedFuture;
        // 回収した準備結果の世代
        std::uint64_t preparedGeneration{};
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            // 共有キャッシュの検索結果
            if (const auto existing = m_modelCache.find(cacheKey);
                existing != m_modelCache.end())
            {
                return existing->second;
            }
            if (m_pendingModelPreparation
                && m_pendingModelPreparation->cacheKey == cacheKey)
            {
                preparedFuture = std::move(
                    m_pendingModelPreparation->future);
                preparedGeneration =
                    m_pendingModelPreparation->generation;
                m_pendingModelPreparation.reset();
            }
        }

        // 準備結果を待つかの判定
        const bool usedPreparedFuture = preparedFuture.valid();
        if (usedPreparedFuture)
        {
            // 同期要求では予算待ちを解除して準備完了まで進める。
            DisableModelUploadThrottle();
        }
        // 生成または回収したアセット
        auto asset = usedPreparedFuture
            ? preparedFuture.get()
            : LoadModelUncached(resolvedPath, m_context);
        // 回収した準備結果の無効化
        bool preparedResultIsStale{};
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            preparedResultIsStale = usedPreparedFuture
                && preparedGeneration != m_modelGeneration;
        }
        if (preparedResultIsStale)
        {
            // 無効化済みの準備結果を採用せず同期経路で読み直す。
            asset = LoadModelUncached(resolvedPath, m_context);
        }
        // モデル準備と共有の排他
        std::scoped_lock lock(m_modelMutex);
        // 採用先の位置と新規採用の有無
        const auto [iterator, inserted] =
            m_modelCache.try_emplace(cacheKey, std::move(asset));
        static_cast<void>(inserted);
        return iterator->second;
    }

    bool AssetManager::PrepareModelAsync(
        const std::filesystem::path& path)
    {
        if (!TryBeginGraphicsWork())
        {
            return false;
        }
        // 所有者を破棄せず処理数を減らす(owner: 処理を借用した管理器)。
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        // 所有者を破棄せず処理数を戻す終了処理
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey = MakeCacheKey(resolvedPath);
        // 別モデルの完了済み結果
        std::future<std::shared_ptr<ModelAsset>> completedFuture;
        // 別モデルの再利用キー
        std::wstring completedKey;
        // 別モデルの準備開始世代
        std::uint64_t completedGeneration{};
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            if (m_modelCache.contains(cacheKey))
            {
                return false;
            }
            if (m_pendingModelPreparation)
            {
                if (m_pendingModelPreparation->cacheKey
                    == cacheKey)
                {
                    return true;
                }
                if (m_pendingModelPreparation->future.wait_for(
                        std::chrono::seconds(0))
                    != std::future_status::ready)
                {
                    return false;
                }
                // 別モデルの完了済みジョブも回収し、次の準備枠を空ける。
                completedKey =
                    m_pendingModelPreparation->cacheKey;
                completedGeneration =
                    m_pendingModelPreparation->generation;
                completedFuture = std::move(
                    m_pendingModelPreparation->future);
                m_pendingModelPreparation.reset();
            }
        }

        if (completedFuture.valid())
        {
            try
            {
                // 回収した別モデル
                auto completedAsset = completedFuture.get();
                // モデル準備と共有の排他
                std::scoped_lock lock(m_modelMutex);
                if (completedGeneration == m_modelGeneration)
                {
                    m_modelCache.try_emplace(
                        std::move(completedKey),
                        std::move(completedAsset));
                }
            }
            catch (...)
            {

            }
        }

        // モデル準備と共有の排他
        std::scoped_lock lock(m_modelMutex);
        if (m_modelCache.contains(cacheKey))
        {
            return false;
        }
        if (m_pendingModelPreparation)
        {
            return m_pendingModelPreparation->cacheKey
                == cacheKey;
        }
        // 読み込み時のモデル世代
        const auto generation = m_modelGeneration;
        {
            // モデル転送予算の排他
            std::scoped_lock uploadLock(m_modelUploadMutex);
            m_modelUploadThrottled = true;
            m_modelPreparationThread = {};
            m_modelUploadBudgetRemaining =
                DefaultModelUploadBudgetPerFrame;
            m_modelUploadFrameBudget =
                DefaultModelUploadBudgetPerFrame;
            m_modelUploadBytesCurrentFrame = 0;
        }
        // 回収する非同期準備結果
        std::future<std::shared_ptr<ModelAsset>> future;
        if (!TryBeginGraphicsWork())
        {
            EndModelUploadPreparation();
            return false;
        }
        try
        {
            // 登録済みワーカーでモデルを準備する。
            future = std::async(
                std::launch::async,
                [this, resolvedPath]()
            {
                // 所有者を破棄せず処理数を減らす(owner: 処理を借用した管理器)。
                const auto finishGraphicsWork = [](
                    AssetManager* owner) noexcept
                {
                    owner->EndGraphicsWork();
                };
                // 所有者を破棄せず処理数を戻す終了処理
                const std::unique_ptr<
                    AssetManager,
                    decltype(finishGraphicsWork)> graphicsWorkScope{
                        this,
                        finishGraphicsWork
                    };
                {
                    // モデル転送予算の排他
                    std::scoped_lock lock(m_modelUploadMutex);
                    m_modelPreparationThread =
                        std::this_thread::get_id();
                }
                // 管理器を破棄せず準備登録を解除する。
                const auto finishUpload = [this](
                    AssetManager*) noexcept
                {
                    EndModelUploadPreparation();
                };
                // 準備登録を解除する終了処理
                const std::unique_ptr<
                    AssetManager,
                    decltype(finishUpload)> uploadScope{
                        this,
                        finishUpload
                    };
                // ワーカーのCOM初期化結果
                const HRESULT comResult = CoInitializeEx(
                    nullptr,
                    COINIT_MULTITHREADED);
                if (FAILED(comResult)
                    && comResult != RPC_E_CHANGED_MODE)
                {
                    throw std::runtime_error(
                        "Could not initialize COM for model import.");
                }
                // comScope: 初期化成功時のCOM寿命管理
                struct ComScope final
                {
                    // COM初期化の成功状態
                    bool initialized{};
                    // このスレッドで初期化したCOMのみを終了する。
                    ~ComScope()
                    {
                        if (initialized)
                        {
                            CoUninitialize();
                        }
                    }
                } comScope{ SUCCEEDED(comResult) };

                // ワーカーへ即時コンテキストを渡さず、デバイス操作だけでモデルを準備する。
                return LoadModelUncached(
                    resolvedPath,
                    nullptr);
            });
        }
        catch (...)
        {
            EndGraphicsWork();
            EndModelUploadPreparation();
            throw;
        }
        m_pendingModelPreparation.emplace(
            PendingModelPreparation{
                resolvedPath,
                cacheKey,
                std::move(future),
                generation
            });
        return true;
    }

    ModelPreparationState
        AssetManager::PollModelPreparation(
            const std::filesystem::path& path,
            std::string* error)
    {
        if (!TryBeginGraphicsWork())
        {
            if (error != nullptr)
            {
                *error = "AssetManager is stopping graphics work.";
            }
            return ModelPreparationState::Failed;
        }
        // 所有者を破棄せず処理数を減らす(owner: 処理を借用した管理器)。
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        // 所有者を破棄せず処理数を戻す終了処理
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        if (error != nullptr)
        {
            error->clear();
        }
        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey = MakeCacheKey(resolvedPath);
        // 回収する非同期準備結果
        std::future<std::shared_ptr<ModelAsset>> future;
        // 読み込み時のモデル世代
        std::uint64_t generation{};
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            if (m_modelCache.contains(cacheKey))
            {
                return ModelPreparationState::Ready;
            }
            if (!m_pendingModelPreparation
                || m_pendingModelPreparation->cacheKey
                    != cacheKey)
            {
                return ModelPreparationState::NotQueued;
            }
            if (m_pendingModelPreparation->future.wait_for(
                    std::chrono::seconds(0))
                != std::future_status::ready)
            {
                return ModelPreparationState::Pending;
            }
            generation =
                m_pendingModelPreparation->generation;
            future = std::move(
                m_pendingModelPreparation->future);
            m_pendingModelPreparation.reset();
        }

        try
        {
            // 生成または回収したアセット
            auto asset = future.get();
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            if (generation != m_modelGeneration)
            {
                return ModelPreparationState::NotQueued;
            }
            m_modelCache.try_emplace(
                cacheKey,
                std::move(asset));
            return ModelPreparationState::Ready;
        }
        // 準備の失敗理由を出力する(exception: 回収時の例外)。
        catch (const std::exception& exception)
        {
            if (error != nullptr)
            {
                *error = exception.what();
            }
            return ModelPreparationState::Failed;
        }
        catch (...)
        {
            if (error != nullptr)
            {
                *error = "Unknown model preparation failure.";
            }
            return ModelPreparationState::Failed;
        }
    }

    std::shared_ptr<const ModelAsset> AssetManager::CreateModelInstance(
        const std::filesystem::path& path)
    {
        if (!TryBeginGraphicsWork())
        {
            throw std::logic_error(
                "AssetManager is stopping graphics work.");
        }
        // 所有者を破棄せず処理数を減らす(owner: 処理を借用した管理器)。
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        // 所有者を破棄せず処理数を戻す終了処理
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        // 解決済みのアセットパス
        const auto resolvedPath = ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey = MakeCacheKey(resolvedPath);
        // 同じモデルの準備中判定
        bool preparationPending{};
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            preparationPending = m_pendingModelPreparation
                && m_pendingModelPreparation->cacheKey == cacheKey;
        }
        if (preparationPending)
        {
            // 準備の終了とディスクキャッシュ保存を待ってから独立資源を生成する。
            static_cast<void>(LoadModelImpl(resolvedPath));
        }
        return LoadModelUncached(resolvedPath, m_context);
    }

    std::shared_ptr<const AnimationClip>
        AssetManager::LoadAnimationClip(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
        // 共有キャッシュの検索結果
        if (const auto existing =
                m_animationCache.find(cacheKey);
            existing != m_animationCache.end())
        {
            return existing->second;
        }

        if (!FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Could not open animation clip: "
                + LamaPon::PathToUtf8(resolvedPath));
        }
        // 読み込むアセットのバイト列
        const auto bytes = ReadFileBytes(resolvedPath);
        // 解析したアニメーション
        auto clip = std::make_shared<AnimationClip>(
            AnimationClip::FromJson(
                std::string_view(
                    reinterpret_cast<const char*>(bytes.data()),
                    bytes.size())));
        m_animationCache.emplace(
            cacheKey,
            clip);
        return clip;
    }

    std::shared_ptr<const AnimationClip>
        AssetManager::ReloadAnimationClip(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        m_animationCache.erase(
            MakeCacheKey(resolvedPath));
        return LoadAnimationClip(
            resolvedPath);
    }

    std::shared_ptr<const DataAsset>
        AssetManager::LoadDataAsset(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
        // 共有キャッシュの検索結果
        if (const auto existing =
                m_dataAssetCache.find(cacheKey);
            existing != m_dataAssetCache.end())
        {
            return existing->second;
        }

        if (!FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Could not open data asset: "
                + LamaPon::PathToUtf8(resolvedPath));
        }
        // 読み込むアセットのバイト列
        const auto bytes = ReadFileBytes(resolvedPath);
        // 生成または回収したアセット
        auto asset = std::make_shared<const DataAsset>(
            DataAsset::FromJson(
                std::string_view(
                    reinterpret_cast<const char*>(
                        bytes.data()),
                    bytes.size()),
                LamaPon::PathToUtf8(
                    resolvedPath.filename())));
        m_dataAssetCache.emplace(
            cacheKey,
            asset);
        return asset;
    }

    std::shared_ptr<const DataAsset>
        AssetManager::ReloadDataAsset(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        m_dataAssetCache.erase(
            MakeCacheKey(resolvedPath));
        return LoadDataAsset(resolvedPath);
    }

    std::shared_ptr<const AnimatorController>
        AssetManager::LoadAnimatorController(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        // アセットの再利用キー
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
        // 共有キャッシュの検索結果
        if (const auto existing =
                m_animatorControllerCache.find(cacheKey);
            existing
                != m_animatorControllerCache.end())
        {
            return existing->second;
        }
        if (!FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Could not open Animator Controller: "
                + LamaPon::PathToUtf8(resolvedPath));
        }
        // 読み込むアセットのバイト列
        const auto bytes = ReadFileBytes(resolvedPath);
        // 解析した制御設定
        auto controller =
            std::make_shared<AnimatorController>(
                AnimatorController::FromJson(
                    std::string_view(
                        reinterpret_cast<const char*>(bytes.data()),
                        bytes.size())));
        controller->ResolveAssetReferences(
            m_database);
        m_animatorControllerCache.emplace(
            cacheKey,
            controller);
        return controller;
    }

    std::shared_ptr<const AnimatorController>
        AssetManager::ReloadAnimatorController(
            const std::filesystem::path& path)
    {
        // 解決済みのアセットパス
        const auto resolvedPath =
            ResolvePath(path);
        m_animatorControllerCache.erase(
            MakeCacheKey(resolvedPath));
        return LoadAnimatorController(
            resolvedPath);
    }

    std::shared_ptr<ModelAsset> AssetManager::LoadModelUncached(
        const std::filesystem::path& resolvedPath,
        ID3D11DeviceContext* context)
    {
        if (!FileExists(resolvedPath))
        {
            throw std::runtime_error("Model file does not exist: " + LamaPon::PathToUtf8(resolvedPath));
        }

        // 小文字化したファイル拡張子
        auto extension = resolvedPath.extension().wstring();
        std::ranges::transform(extension, extension.begin(), std::towlower);

        // DirectXTK形式のモデル
        std::unique_ptr<DirectX::Model> loadedModel;
        // 共通形式のモデル
        std::shared_ptr<SkeletalModel> skeletalModel;
        // 元のパーツ別材質色
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> embeddedDiffuseColors;
        if (extension == L".cmo")
        {
            if (m_device != nullptr)
            {

                // CMOの外部画像はEffectFactoryが物理ファイルから読むためアーカイブ経由に対応しない。
                // 読み込むアセットのバイト列
                const auto bytes = ReadFileBytes(resolvedPath);
                // 元の材質色を記録する生成器
                MaterialCapturingEffectFactory effectFactory(m_device);
                // 外部画像の検索フォルダー
                const auto modelDirectory =
                    FindCmoTextureDirectory(resolvedPath).wstring();
                effectFactory.SetDirectory(modelDirectory.c_str());
                loadedModel = DirectX::Model::CreateFromCMO(
                    m_device,
                    bytes.data(),
                    bytes.size(),
                    effectFactory);
                embeddedDiffuseColors =
                    effectFactory.TakeDiffuseColors();
            }
            else
            {

                skeletalModel = CmoImporter::Load(*this, resolvedPath);
            }
        }
        else if (extension == L".sdkmesh")
        {
            if (m_device != nullptr)
            {

                // 読み込むアセットのバイト列
                const auto bytes = ReadFileBytes(resolvedPath);
                // 元の材質色を記録する生成器
                MaterialCapturingEffectFactory effectFactory(m_device);
                // 外部画像の検索フォルダー
                const auto modelDirectory =
                    resolvedPath.parent_path().wstring();
                effectFactory.SetDirectory(modelDirectory.c_str());
                loadedModel = DirectX::Model::CreateFromSDKMESH(
                    m_device,
                    bytes.data(),
                    bytes.size(),
                    effectFactory);
                embeddedDiffuseColors =
                    effectFactory.TakeDiffuseColors();
            }
            else
            {

                skeletalModel = SdkmeshImporter::Load(*this, resolvedPath);
            }
        }
        else if (extension == L".vbo")
        {
            if (m_device != nullptr)
            {

                // 読み込むアセットのバイト列
                const auto bytes = ReadFileBytes(resolvedPath);
                loadedModel = DirectX::Model::CreateFromVBO(
                    m_device,
                    bytes.data(),
                    bytes.size());
            }
            else
            {
                skeletalModel = VboImporter::Load(*this, resolvedPath);
            }
        }
        else if (extension == L".gltf"
            || extension == L".glb")
        {
            skeletalModel = GltfImporter::Load(
                m_device,
                context,
                *this,
                resolvedPath);
        }
        else if (extension == L".fbx")
        {
            skeletalModel = FbxImporter::Load(
                m_device,
                context,
                *this,
                resolvedPath);
        }
        else
        {
            throw std::runtime_error(
                "Unsupported model format: " + LamaPon::PathToUtf8(resolvedPath.extension()));
        }

        if (skeletalModel)
        {
            ImportSkeletalTextureViews(
                *skeletalModel,
                m_backend);
        }

        // 生成または回収したアセット
        auto asset = std::make_shared<ModelAsset>();
        asset->model =
            std::shared_ptr<DirectX::Model>(std::move(loadedModel));
        asset->skeletalModel = std::move(skeletalModel);
        asset->embeddedDiffuseColors =
            std::move(embeddedDiffuseColors);
        if (asset->skeletalModel
            && asset->skeletalModel->hasLocalBounds)
        {
            asset->localBounds =
                asset->skeletalModel->localBounds;
            asset->hasLocalBounds = true;
        }
        else if (asset->model)
        {
            asset->hasLocalBounds = CalculateModelBounds(
                *asset->model,
                asset->localBounds);
        }
        asset->sourcePath = resolvedPath;
        return asset;
    }

    std::shared_ptr<const TextTextureAsset> AssetManager::LoadTextTexture(
        const std::string_view text,
        const std::string_view fontFamily,
        const float fontSize,
        const TextLayoutOptions& layout)
    {
        // UTF16の表示文字列
        const std::wstring wideText = Utf8ToWide(text);
        // UTF16の書体名
        const std::wstring wideFontFamily = Utf8ToWide(fontFamily);
        // 文字配置の制約横幅
        const float layoutWidth = std::clamp(layout.size.x, 0.0f, 4096.0f);
        // 文字配置の制約高さ
        const float layoutHeight = std::clamp(layout.size.y, 0.0f, 4096.0f);
        // 横幅制約の有効性
        const bool constrainedWidth = layoutWidth > 0.0f;
        // 高さ制約の有効性
        const bool constrainedHeight = layoutHeight > 0.0f;
        // 文字配置の最大横幅
        const float maximumWidth = constrainedWidth ? layoutWidth : 4096.0f;
        // 文字配置の最大高さ
        const float maximumHeight = constrainedHeight ? layoutHeight : 4096.0f;


        // 文字と書体・配置の再利用キー
        std::wstring cacheKey = wideFontFamily;
        cacheKey += L'\x1f';
        cacheKey += std::to_wstring(fontSize);
        cacheKey += L'\x1f';
        cacheKey += std::to_wstring(layoutWidth);
        cacheKey += L',';
        cacheKey += std::to_wstring(layoutHeight);
        cacheKey += L',';
        cacheKey += std::to_wstring(
            static_cast<int>(layout.horizontalAlignment));
        cacheKey += L',';
        cacheKey += std::to_wstring(
            static_cast<int>(layout.verticalAlignment));
        cacheKey += L',';
        cacheKey += layout.wordWrap ? L'1' : L'0';
        cacheKey += L'\x1f';
        cacheKey += wideText;

        // 共有キャッシュの検索結果
        if (const auto existing = m_textCache.find(cacheKey);
            existing != m_textCache.end())
        {
            existing->second.lastUsed = ++m_textCacheClock;
            return existing->second.asset;
        }

        // 文字の書式設定
        Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat;
        ThrowIfFailed(
            m_dwriteFactory->CreateTextFormat(
                wideFontFamily.empty() ? L"Yu Gothic UI" : wideFontFamily.c_str(),
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                std::max(fontSize, 1.0f),
                L"ja-jp",
                textFormat.ReleaseAndGetAddressOf()),
            "IDWriteFactory::CreateTextFormat");

        // 文字の横方向揃え
        DWRITE_TEXT_ALIGNMENT textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        if (constrainedWidth)
        {
            switch (layout.horizontalAlignment)
            {
            case TextHorizontalAlignment::Center:
                textAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
                break;
            case TextHorizontalAlignment::Right:
                textAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
                break;
            default:
                break;
            }
        }

        // 文字の縦方向揃え
        DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment =
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        if (constrainedHeight)
        {
            switch (layout.verticalAlignment)
            {
            case TextVerticalAlignment::Center:
                paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
                break;
            case TextVerticalAlignment::Bottom:
                paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
                break;
            default:
                break;
            }
        }

        ThrowIfFailed(
            textFormat->SetTextAlignment(textAlignment),
            "IDWriteTextFormat::SetTextAlignment");
        ThrowIfFailed(
            textFormat->SetParagraphAlignment(paragraphAlignment),
            "IDWriteTextFormat::SetParagraphAlignment");
        ThrowIfFailed(
            textFormat->SetWordWrapping(
                layout.wordWrap && constrainedWidth
                    ? DWRITE_WORD_WRAPPING_WRAP
                    : DWRITE_WORD_WRAPPING_NO_WRAP),
            "IDWriteTextFormat::SetWordWrapping");

        // 描画用の文字配置
        Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
        ThrowIfFailed(
            m_dwriteFactory->CreateTextLayout(
                wideText.c_str(),
                static_cast<UINT32>(wideText.size()),
                textFormat.Get(),
                maximumWidth,
                maximumHeight,
                textLayout.ReleaseAndGetAddressOf()),
            "IDWriteFactory::CreateTextLayout");

        // 配置後の文字寸法
        DWRITE_TEXT_METRICS metrics{};
        ThrowIfFailed(
            textLayout->GetMetrics(&metrics),
            "IDWriteTextLayout::GetMetrics");

        // 余白追加前の描画横幅
        const float renderedWidth =
            constrainedWidth
                ? maximumWidth
                : metrics.widthIncludingTrailingWhitespace;
        // 余白追加前の描画高さ
        const float renderedHeight =
            constrainedHeight ? maximumHeight : metrics.height;
        // 文字画像の横幅
        const std::uint32_t width = std::max(
            static_cast<std::uint32_t>(
                std::ceil(renderedWidth)) + 4u,
            1u);
        // 文字画像の高さ
        const std::uint32_t height = std::max(
            static_cast<std::uint32_t>(std::ceil(renderedHeight)) + 4u,
            1u);

        // 文字描画先のCPU画像
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        ThrowIfFailed(
            m_wicFactory->CreateBitmap(
                width,
                height,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnLoad,
                bitmap.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateBitmap(text)");

        // 文字描画先の形式
        const D2D1_RENDER_TARGET_PROPERTIES renderTargetProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));

        // 文字描画のD2D対象
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> renderTarget;
        ThrowIfFailed(
            m_d2dFactory->CreateWicBitmapRenderTarget(
                bitmap.Get(),
                renderTargetProperties,
                renderTarget.ReleaseAndGetAddressOf()),
            "ID2D1Factory::CreateWicBitmapRenderTarget");
        renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        // 描画時に色を乗算するため、文字画像は白で生成する。
        // 白い文字の描画ブラシ
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        ThrowIfFailed(
            renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                brush.ReleaseAndGetAddressOf()),
            "ID2D1RenderTarget::CreateSolidColorBrush");

        renderTarget->BeginDraw();
        renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        renderTarget->DrawTextLayout(
            D2D1::Point2F(2.0f, 2.0f),
            textLayout.Get(),
            brush.Get());
        ThrowIfFailed(
            renderTarget->EndDraw(),
            "ID2D1RenderTarget::EndDraw");

        // 読み取る画像の範囲
        WICRect lockRectangle{
            0,
            0,
            static_cast<INT>(width),
            static_cast<INT>(height)
        };
        // 画像バイト列の借用固定
        Microsoft::WRL::ComPtr<IWICBitmapLock> bitmapLock;
        ThrowIfFailed(
            bitmap->Lock(
                &lockRectangle,
                WICBitmapLockRead,
                bitmapLock.ReleaseAndGetAddressOf()),
            "IWICBitmap::Lock");

        // 画像1行のバイト数
        UINT stride{};
        // 固定した画像の容量
        UINT dataSize{};
        // 固定した画像のバイト先頭
        BYTE* data{};
        ThrowIfFailed(bitmapLock->GetStride(&stride), "IWICBitmapLock::GetStride");
        ThrowIfFailed(
            bitmapLock->GetDataPointer(&dataSize, &data),
            "IWICBitmapLock::GetDataPointer");
        static_cast<void>(dataSize);

        // D3D11画像の生成設定
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = width;
        textureDescription.Height = height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        // 画像転送の初期データ
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        initialData.SysMemPitch = stride;

        // 生成または回収したアセット
        auto asset = std::make_shared<TextTextureAsset>();
        asset->width = width;
        asset->height = height;
        if (m_backend != nullptr)
        {
            // 画像資源の生成設定
            const GraphicsTexture2DDescription description{
                width,
                height,
                1,
                GraphicsTextureFormat::Bgra8Unorm
            };
            // 借用する転送バイト列
            const std::array subresources{
                GraphicsTextureSubresourceData{
                    std::span<const std::byte>{
                        reinterpret_cast<const std::byte*>(data),
                        dataSize
                    },
                    stride,
                    dataSize
                }
            };
            // 生成する画像アセット資源
            auto texture = m_backend->CreateTexture2D(
                description,
                subresources);
            // 画像を参照する描画ビュー
            auto view = m_backend->CreateShaderResourceView(
                texture,
                GraphicsTextureViewDescription{ 0, 1 });
            PublishTextureResources(
                *asset,
                m_backend,
                std::move(texture),
                std::move(view));
        }
        else
        {
            // 生成する画像アセット資源
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(
                m_device->CreateTexture2D(
                    &textureDescription,
                    &initialData,
                    texture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(text)");
            // 画像を参照する描画ビュー
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(
                    texture.Get(),
                    nullptr,
                    view.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(text)");
            PublishLegacyTextureView(*asset, std::move(view));
        }


        // 文字画像の推定キャッシュ容量
        const std::size_t bytes =
            static_cast<std::size_t>(width)
            * static_cast<std::size_t>(height)
            * 4u;
        m_textCache.emplace(
            std::move(cacheKey),
            TextCacheEntry{
                asset,
                ++m_textCacheClock,
                bytes });
        m_textCacheBytes += bytes;
        TrimTextCache();
        return asset;
    }


    void AssetManager::TrimTextCache() noexcept
    {
        if (m_textCacheBytes <= m_textCacheBudgetBytes)
        {
            m_textCacheBudgetWarningIssued = false;
            return;
        }
        try
        {
            while (m_textCacheBytes > m_textCacheBudgetBytes)
            {
                // 未使用で最も古い文字画像
                auto oldest = m_textCache.end();
                // キャッシュの検索位置
                for (auto iterator = m_textCache.begin();
                     iterator != m_textCache.end();
                     ++iterator)
                {
                    if (iterator->second.asset.use_count() > 1)
                    {
                        continue;
                    }
                    if (oldest == m_textCache.end()
                        || iterator->second.lastUsed
                            < oldest->second.lastUsed)
                    {
                        oldest = iterator;
                    }
                }

                if (oldest == m_textCache.end())
                {
                    if (!m_textCacheBudgetWarningIssued)
                    {
                        m_textCacheBudgetWarningIssued = true;
                        Logger::Instance().Warning(
                            "文字テクスチャキャッシュが予算を超えていますが、"
                            "使用中の項目しか残っていないため追い出せません。"
                            "表示中のテクスチャを解放すると掃除できます。"
                        );
                    }
                    return;
                }

                m_textCacheBytes -= std::min(
                    m_textCacheBytes,
                    oldest->second.bytes);
                m_textCache.erase(oldest);
            }
            m_textCacheBudgetWarningIssued = false;
        }
        catch (...)
        {

        }
    }

    void AssetManager::Clear() noexcept
    {
        WaitForModelPreparation();
        try
        {
            // 世代更新と転送待ちの破棄を一括で行い、新世代の転送を誤って消さない。
            // 世代更新と公開の一括排他
            std::scoped_lock lock(
                m_textureMutex,
                m_uploadMutex,
                m_prefetchMutex);
            ++m_textureEpoch;
            ++m_prefetchEpoch;
            m_texturePathGenerations.clear();
            m_textureCache.clear();
            // 未転送のミップに残る復号済みピクセルを消してから破棄する。
            for (auto& pending : m_pendingUploads)
            {
                for (auto& level : pending.data.levels)
                {
                    Crypto::SecureErase(level.bytes);
                }
            }
            m_pendingUploads.clear();
            // 破棄前に復号済みバイトを消す。
            for (auto& [key, entry] : m_prefetchedBytes)
            {
                SecureErasePrefetchEntry(entry);
            }
            m_prefetchedBytes.clear();
            m_prefetchedByteCount = 0;
        }
        catch (...)
        {
        }
        try
        {
            // モデル準備と共有の排他
            std::scoped_lock lock(m_modelMutex);
            ++m_modelGeneration;
            m_modelCache.clear();
        }
        catch (...)
        {
        }
        m_textCache.clear();
        m_textCacheBytes = 0;
        m_textCacheBudgetWarningIssued = false;
        m_animationCache.clear();
        m_animatorControllerCache.clear();
        m_dataAssetCache.clear();
    }

    void AssetManager::Invalidate(
        const std::filesystem::path& path) noexcept
    {
        try
        {
            // 組み込み図形の識別
            const auto builtInKind = BuiltInTextureKind(path);
            // 無効化対象の解決済みパス
            const auto cachePath = builtInKind.empty()
                ? ResolvePath(path)
                : path.lexically_normal();
            // アセットの再利用キー
            const auto cacheKey =
                MakeCacheKey(cachePath);
            {
                // パス世代更新と転送待ちの除去を一括で行い、新世代の転送を維持する。
                // 世代更新と公開の一括排他
                std::scoped_lock lock(
                    m_textureMutex,
                    m_uploadMutex,
                    m_prefetchMutex);
                ++m_texturePathGenerations[cacheKey];
                // 無効化する画像用途
                for (int usage =
                        static_cast<int>(
                            TextureLoader::TextureUsage::Color);
                    usage <= static_cast<int>(
                        TextureLoader::TextureUsage::DataMap);
                    ++usage)
                {
                    // 用途を含む画像キー
                    auto textureKey = cacheKey;
                    textureKey += L"|u";
                    textureKey += static_cast<wchar_t>(L'0' + usage);
                    m_textureCache.erase(textureKey);
                }
                // 同じパスの転送待ちを除く(pending: 確認対象の待機画像)。
                std::erase_if(
                    m_pendingUploads,
                    [&cacheKey](
                        PendingTextureUpload& pending)
                    {
                        // このパスの待機画像か
                        const bool match = pending.asset != nullptr
                            && MakeCacheKey(
                                pending.asset->sourcePath)
                                == cacheKey;
                        if (match)
                        {
                            // 除く前に未転送の復号済みピクセルを消す。
                            for (auto& level : pending.data.levels)
                            {
                                Crypto::SecureErase(level.bytes);
                            }
                        }
                        return match;
                    });
                // ディスクまたは先読みの結果
                if (const auto cached =
                        m_prefetchedBytes.find(cacheKey);
                    cached != m_prefetchedBytes.end())
                {
                    if (cached->second)
                    {
                        m_prefetchedByteCount -=
                            std::min(
                                m_prefetchedByteCount,
                                cached->second->size());
                    }
                    // 破棄前に復号済みバイトを消す。
                    SecureErasePrefetchEntry(cached->second);
                    m_prefetchedBytes.erase(cached);
                }
            }
            {
                // モデル準備と共有の排他
                std::scoped_lock lock(m_modelMutex);
                ++m_modelGeneration;
                m_modelCache.erase(cacheKey);
            }
            m_animationCache.erase(cacheKey);
            m_animatorControllerCache.erase(cacheKey);
            m_dataAssetCache.erase(cacheKey);
        }
        catch (...)
        {
        }
    }

    std::wstring AssetManager::MakeCacheKey(const std::filesystem::path& path)
    {
        // パスの再利用キー
        auto key = path.wstring();
        std::ranges::transform(key, key.begin(), std::towlower);
        return key;
    }

    void AssetManager::WaitForModelPreparation() noexcept
    {
        try
        {
            // 回収する非同期準備結果
            std::future<std::shared_ptr<ModelAsset>> future;
            {
                // モデル準備と共有の排他
                std::scoped_lock lock(m_modelMutex);
                if (!m_pendingModelPreparation)
                {
                    return;
                }
                future = std::move(
                    m_pendingModelPreparation->future);
                m_pendingModelPreparation.reset();
            }
            if (future.valid())
            {
                DisableModelUploadThrottle();
                static_cast<void>(future.get());
            }
        }
        catch (...)
        {
        }
    }
}
