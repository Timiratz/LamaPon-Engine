#include "LamaPon/Assets/FbxImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/ModelCache.h"
#include "LamaPon/Assets/ModelLod.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>
#include <nlohmann/json.hpp>
#include <ufbx.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Vertex =
        DirectX::VertexPositionNormalTangentColorTextureSkinning;

    // 変換後の頂点位置でモデル全体の境界を広げる(model: 更新するモデル, vertices: 元の局所頂点列, transform: モデル座標への変換)。
    void ExpandModelBounds(
        LamaPon::SkeletalModel& model,
        const std::span<const Vertex> vertices,
        DirectX::FXMMATRIX transform) noexcept
    {
        // 処理する元の頂点
        for (const auto& vertex : vertices)
        {
            // 局所または変換後の頂点位置
            DirectX::XMFLOAT3 position{};
            DirectX::XMStoreFloat3(
                &position,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&vertex.position),
                    transform));
            if (!model.hasLocalBounds)
            {
                model.localBounds = { position, position };
                model.hasLocalBounds = true;
                continue;
            }
            model.localBounds.minimum.x = std::min(
                model.localBounds.minimum.x, position.x);
            model.localBounds.minimum.y = std::min(
                model.localBounds.minimum.y, position.y);
            model.localBounds.minimum.z = std::min(
                model.localBounds.minimum.z, position.z);
            model.localBounds.maximum.x = std::max(
                model.localBounds.maximum.x, position.x);
            model.localBounds.maximum.y = std::max(
                model.localBounds.maximum.y, position.y);
            model.localBounds.maximum.z = std::max(
                model.localBounds.maximum.z, position.z);
        }
    }

    // 局所頂点の境界を計算し、頂点の有無を返す(vertices: 境界を求める頂点列, bounds: 境界の返却先)。
    bool CalculateLocalBounds(
        const std::span<const Vertex> vertices,
        LamaPon::Bounds3D& bounds) noexcept
    {
        // 最初の頂点の取り込み済み状態
        bool initialized{};
        // 処理する元の頂点
        for (const auto& vertex : vertices)
        {
            // 局所または変換後の頂点位置
            const auto& position = vertex.position;
            if (!initialized)
            {
                bounds = { position, position };
                initialized = true;
                continue;
            }
            bounds.minimum.x = std::min(
                bounds.minimum.x, position.x);
            bounds.minimum.y = std::min(
                bounds.minimum.y, position.y);
            bounds.minimum.z = std::min(
                bounds.minimum.z, position.z);
            bounds.maximum.x = std::max(
                bounds.maximum.x, position.x);
            bounds.maximum.y = std::max(
                bounds.maximum.y, position.y);
            bounds.maximum.z = std::max(
                bounds.maximum.z, position.z);
        }
        return initialized;
    }

    struct SceneDeleter final
    {
        // 所有するufbx解析結果を解放する(scene: 解放する解析結果)。
        void operator()(ufbx_scene* scene) const noexcept
        {
            ufbx_free_scene(scene);
        }
    };

    struct BakedAnimationDeleter final
    {
        // 焼き込み結果を解放する(animation: 解放するアニメーション)。
        void operator()(ufbx_baked_anim* animation) const noexcept
        {
            ufbx_free_baked_anim(animation);
        }
    };

    // ufbxの長さ付き文字列をコピーする(value: 元の文字列)。
    std::string ToString(const ufbx_string value)
    {
        return value.data != nullptr
            ? std::string(value.data, value.length)
            : std::string{};
    }

    // ufbxの解析エラーを診断文へ変換する(error: 解析のエラー情報)。
    std::string FormatError(const ufbx_error& error)
    {
        // ufbx診断文の格納先
        std::array<char, 4096> buffer{};
        ufbx_format_error(
            buffer.data(),
            buffer.size(),
            &error);
        return buffer.data();
    }

    // 右手系・Y上・メートル単位のFBX解析設定を作る(utf8Path: 解析中に保持するUTF-8パス)。
    ufbx_load_opts MakeLoadOptions(
        const std::string_view utf8Path)
    {
        // FBXの解析設定
        ufbx_load_opts options{};
        options.filename = {
            utf8Path.data(),
            utf8Path.size()
        };
        options.target_axes =
            ufbx_axes_right_handed_y_up;
        options.target_unit_meters = 1.0f;
        options.space_conversion =
            UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        options.geometry_transform_handling =
            UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        options.inherit_mode_handling =
            UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
        options.pivot_handling =
            UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
        options.clean_skin_weights = true;
        options.generate_missing_normals = true;
        options.normalize_normals = true;
        options.normalize_tangents = true;
        options.use_blender_pbr_material = true;
        options.node_depth_limit = 512;
        return options;
    }

    // HRESULTが失敗なら操作名付き例外を送出する(result: APIの結果, operation: 失敗した操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const std::string& operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                operation
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    struct LoadedTexture final
    {
        // D3D11画像の所有ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        // 他APIの画像ハンドル
        LamaPon::GraphicsViewHandle graphicsView;
        // DDS以外で検出した透過
        bool hasTransparency{};
    };

    // 画像の先頭フレームをRGBAへ変換し、透過の有無を調べる(decoder: 有効なWICデコーダー)。
    bool WicImageHasTransparency(IWICBitmapDecoder* decoder)
    {
        // 透過検査する先頭フレーム
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(
            decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()),
            "Reading the FBX texture frame");

        // 画像の幅（画素）
        UINT width{};
        // 画像の高さ（画素）
        UINT height{};
        ThrowIfFailed(
            frame->GetSize(&width, &height),
            "Reading the FBX texture size");
        if (width == 0 || height == 0)
        {
            return false;
        }

        // RGBA1行のバイト数
        const std::uint64_t stride64 =
            static_cast<std::uint64_t>(width) * 4u;
        // RGBA全体のバイト数
        const std::uint64_t byteCount64 =
            stride64 * static_cast<std::uint64_t>(height);
        if (stride64 > std::numeric_limits<UINT>::max()
            || byteCount64 > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "FBX texture is too large to inspect its alpha channel.");
        }

        // WIC画像処理の生成元
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
            "Creating WIC for FBX texture inspection");
        // RGBA画素への変換器
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(
            factory->CreateFormatConverter(
                converter.ReleaseAndGetAddressOf()),
            "Creating the FBX texture alpha converter");
        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "Converting the FBX texture alpha channel");

        // RGBA1行のバイト数
        const auto stride = static_cast<UINT>(stride64);
        // RGBA全体のバイト数
        const auto byteCount = static_cast<UINT>(byteCount64);
        // 透過検査用のRGBA画素列
        std::vector<std::uint8_t> pixels(byteCount);
        ThrowIfFailed(
            converter->CopyPixels(
                nullptr,
                stride,
                byteCount,
                pixels.data()),
            "Reading the FBX texture alpha channel");
        // 透過を検査するアルファの位置
        for (std::size_t index = 3;
            index < pixels.size();
            index += 4)
        {
            if (pixels[index] < 255u)
            {
                return true;
            }
        }
        return false;
    }

    // メモリ上のWIC画像の透過を調べる(bytes: 元画像の先頭, byteCount: 元画像のバイト数)。
    bool WicMemoryHasTransparency(
        const std::uint8_t* bytes,
        const std::size_t byteCount)
    {
        if (byteCount > std::numeric_limits<DWORD>::max())
        {
            throw std::runtime_error(
                "Embedded FBX texture is too large to inspect.");
        }

        // WIC画像処理の生成元
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
            "Creating WIC for embedded FBX texture inspection");
        // 内蔵画像のWIC読み取り元
        Microsoft::WRL::ComPtr<IWICStream> stream;
        ThrowIfFailed(
            factory->CreateStream(stream.ReleaseAndGetAddressOf()),
            "Creating the embedded FBX texture stream");
        ThrowIfFailed(
            stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes),
                static_cast<DWORD>(byteCount)),
            "Opening the embedded FBX texture stream");
        // 内蔵画像のWICデコーダー
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromStream(
                stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnLoad,
                decoder.ReleaseAndGetAddressOf()),
            "Opening the embedded FBX texture alpha channel");
        return WicImageHasTransparency(decoder.Get());
    }

    // ufbxの3成分をDirectXの実数3成分へ変換する(value: 元の3成分)。
    DirectX::XMFLOAT3 ToFloat3(
        const ufbx_vec3 value) noexcept
    {
        return {
            static_cast<float>(value.x),
            static_cast<float>(value.y),
            static_cast<float>(value.z)
        };
    }

    // ufbxの回転をDirectXの4成分へ変換する(value: 元の四元数)。
    DirectX::XMFLOAT4 ToFloat4(
        const ufbx_quat value) noexcept
    {
        return {
            static_cast<float>(value.x),
            static_cast<float>(value.y),
            static_cast<float>(value.z),
            static_cast<float>(value.w)
        };
    }

    // ufbxの行列をDirectXの行ベクトル用配置へ変換する(value: 元の変換行列)。
    DirectX::XMFLOAT4X4 ToMatrix(
        const ufbx_matrix& value) noexcept
    {
        return {
            value.m00,
            value.m10,
            value.m20,
            0.0f,
            value.m01,
            value.m11,
            value.m21,
            0.0f,
            value.m02,
            value.m12,
            value.m22,
            0.0f,
            value.m03,
            value.m13,
            value.m23,
            1.0f
        };
    }

    // ufbxの位置・回転・倍率を共通の局所姿勢へ変換する(transform: 元の局所変換)。
    LamaPon::SkeletalPoseTransform ToPose(
        const ufbx_transform& transform) noexcept
    {
        // 変換先の共通局所姿勢
        LamaPon::SkeletalPoseTransform result;
        result.translation = ToFloat3(transform.translation);
        result.rotation = ToFloat4(transform.rotation);
        result.scale = ToFloat3(transform.scale);
        return result;
    }

    // 任意のD3D11デバイスへ不変バッファーを作る(device: 空なら作成を省略, assets: 転送予算の管理元, data: 初期データの先頭, byteCount: 初期データのバイト数, bindFlags: バインド用途, output: COM参照の返却先)。
    void CreateBuffer(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const void* data,
        const std::size_t byteCount,
        const UINT bindFlags,
        ID3D11Buffer** output)
    {
        if (device == nullptr)
        {
            return;
        }
        if (byteCount == 0
            || byteCount > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "FBX mesh buffer has an invalid size.");
        }
        // 不変バッファーの設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byteCount);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = bindFlags;
        // バッファーの初期データ
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        assets.WaitForModelUploadBudget(byteCount);
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                &initialData,
                output),
            "Creating FBX mesh buffer");
    }

    // アルファ破棄エフェクトと入力配置を未作成なら追加する(primitive: 更新する描画部分, device: 作成先デバイス)。
    void EnableAlphaCutout(
        LamaPon::SkeletalPrimitive& primitive,
        ID3D11Device* device)
    {
        if (primitive.cutoutEffect != nullptr)
        {
            return;
        }

        primitive.cutoutEffect =
            std::make_shared<DirectX::SkinnedDGSLEffect>(device);
        primitive.cutoutEffect->SetWeightsPerVertex(4);
        primitive.cutoutEffect->SetTextureEnabled(true);
        primitive.cutoutEffect->SetAlphaDiscardEnable(true);

        // 入力配置用の頂点シェーダー
        const void* shaderBytecode{};
        // シェーダーのバイト数
        std::size_t shaderBytecodeSize{};
        primitive.cutoutEffect->GetVertexShaderBytecode(
            &shaderBytecode,
            &shaderBytecodeSize);
        ThrowIfFailed(
            device->CreateInputLayout(
                Vertex::InputElements,
                Vertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.cutoutInputLayout.ReleaseAndGetAddressOf()),
            "Creating FBX alpha-cutout input layout");
    }

    // 拡張子の大文字小文字を無視してDDSかを調べる(path: 画像のパス)。
    bool IsDdsPath(const std::filesystem::path& path)
    {
        // 小文字化する画像拡張子
        auto extension = path.extension().wstring();
        // 拡張子を小文字へ揃える(character: 拡張子の1文字)。
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        return extension == L".dds";
    }

    // 内蔵・外部画像を取り込み、DDS以外は透過を検査する(device: D3D11デバイス、空は共通経路, assets: 画像の取得元, modelPath: 元モデルのパス, source: 元の画像記述, usage: 画像用途, recorder: D3D11復元情報の記録先)。
    LoadedTexture
        LoadTexture(
            ID3D11Device* const device,
            LamaPon::AssetManager& assets,
            const std::filesystem::path& modelPath,
            const ufbx_texture& source,
            const LamaPon::TextureLoader::TextureUsage usage,
            LamaPon::ModelCache::Recorder& recorder)
    {
        // 読み込んだ画像と透過情報
        LoadedTexture texture;
        if (source.content.data != nullptr
            && source.content.size > 0)
        {
            // 元ファイルまたは頂点のバイト列
            const auto* bytes =
                static_cast<const std::uint8_t*>(
                    source.content.data);
            // 内蔵画像の形式判定用パス
            const std::filesystem::path hint =
                LamaPon::PathFromUtf8(
                    !ToString(source.relative_filename).empty()
                        ? ToString(source.relative_filename)
                        : ToString(source.filename));
            // 内蔵画像の元バイト列
            const std::span<const std::uint8_t> imageBytes(
                bytes,
                source.content.size);
            if (device != nullptr)
            {
                texture.view = assets.CreateTextureViewFromMemory(
                    imageBytes,
                    IsDdsPath(hint),
                    usage);
            }
            else
            {
                texture.graphicsView =
                    assets.CreateTextureViewHandleFromMemory(
                        imageBytes,
                        IsDdsPath(hint),
                        usage);
            }
            if (!IsDdsPath(hint))
            {
                texture.hasTransparency =
                    WicMemoryHasTransparency(
                        bytes,
                        source.content.size);
            }
            // 内蔵画像はD3D11の復元用キャッシュへバイト列をコピーする。
            if (device != nullptr)
            {
                recorder.RegisterEmbeddedImage(
                    texture.view.Get(),
                    imageBytes,
                    IsDdsPath(hint),
                    texture.hasTransparency,
                    usage);
            }
            return texture;
        }

        // 画像記述の相対または元パス
        std::string filename =
            ToString(source.relative_filename);
        if (filename.empty())
        {
            filename = ToString(source.filename);
        }
        if (filename.empty())
        {
            return {};
        }
        std::ranges::replace(filename, '\\', '/');
        // 外部画像の解決先パス
        auto texturePath =
            LamaPon::PathFromUtf8(filename);
        if (!texturePath.is_absolute())
        {
            texturePath =
                modelPath.parent_path() / texturePath;
        }
        texturePath = texturePath.lexically_normal();
        if (!assets.FileExists(texturePath))
        {
            // 元画像の絶対パス候補
            const auto absolute =
                LamaPon::PathFromUtf8(
                    ToString(source.absolute_filename));
            if (!absolute.empty()
                && assets.FileExists(absolute))
            {
                texturePath = absolute;
            }
            else
            {
                return {};
            }
        }

        // 外部画像の元バイト列
        const auto fileBytes = assets.ReadFileBytes(texturePath);
        if (device != nullptr)
        {
            texture.view = assets.CreateTextureViewFromMemory(
                fileBytes,
                IsDdsPath(texturePath),
                usage);
        }
        else
        {
            texture.graphicsView =
                assets.CreateTextureViewHandleFromMemory(
                    fileBytes,
                    IsDdsPath(texturePath),
                    usage);
        }
        if (!IsDdsPath(texturePath))
        {
            texture.hasTransparency =
                WicMemoryHasTransparency(
                    fileBytes.data(),
                    fileBytes.size());
        }
        // 外部画像はD3D11の復元用キャッシュへパスと内容ハッシュを記録する。
        if (device != nullptr)
        {
            recorder.RegisterExternalImage(
                texture.view.Get(),
                texturePath,
                fileBytes,
                IsDdsPath(texturePath),
                texture.hasTransparency,
                usage);
        }
        return texture;
    }

    // ノード、無ければメッシュから描画部分の材質を探す(node: 所属ノード, mesh: 元メッシュ, part: 材質別の描画部分)。
    const ufbx_material* MaterialForPart(
        const ufbx_node& node,
        const ufbx_mesh& mesh,
        const ufbx_mesh_part& part)
    {
        if (part.index < node.materials.count)
        {
            return node.materials.data[part.index];
        }
        if (part.index < mesh.materials.count)
        {
            return mesh.materials.data[part.index];
        }
        return nullptr;
    }

    // 材質と画像を描画部分へ適用する(primitive: 更新する描画部分, material: 任意の元材質, device: D3D11デバイス、空は共通経路, assets: 画像の取得元, modelPath: 元モデルのパス, textureCache: 画像と用途別の再利用先, recorder: 復元情報の記録先)。
    void ApplyMaterial(
        LamaPon::SkeletalPrimitive& primitive,
        const ufbx_material* material,
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        std::map<
            std::pair<
                const ufbx_texture*,
                LamaPon::TextureLoader::TextureUsage>,
            LoadedTexture>& textureCache,
        LamaPon::ModelCache::Recorder& recorder)
    {
        if (material == nullptr)
        {
            primitive.baseColor = {
                0.8f,
                0.8f,
                0.8f,
                1.0f
            };
            return;
        }

        // 元の基本色と画像記述
        const auto& baseColor = material->pbr.base_color;
        primitive.baseColor = {
            static_cast<float>(baseColor.value_vec4.x),
            static_cast<float>(baseColor.value_vec4.y),
            static_cast<float>(baseColor.value_vec4.z),
            static_cast<float>(baseColor.value_vec4.w)
        };
        if (!baseColor.has_value)
        {
            primitive.baseColor = {
                0.8f,
                0.8f,
                0.8f,
                1.0f
            };
        }

        // 元の不透明度
        const auto& opacity = material->pbr.opacity;
        if (opacity.has_value)
        {
            primitive.baseColor.w *=
                static_cast<float>(opacity.value_real);
        }
        // 元の粗さ
        const auto& roughness = material->pbr.roughness;
        primitive.roughness = roughness.has_value
            ? std::clamp(
                static_cast<float>(roughness.value_real),
                0.0f,
                1.0f)
            : 0.5f;
        // 元の金属度
        const auto& metalness = material->pbr.metalness;
        primitive.metallic = metalness.has_value
            ? std::clamp(
                static_cast<float>(metalness.value_real),
                0.0f,
                1.0f)
            : 0.0f;
        primitive.alpha = primitive.baseColor.w < 0.999f;
        primitive.doubleSided =
            material->features.double_sided.enabled;


        // 画像と用途の組で読み込み結果を再利用する(source: 元の画像記述, usage: 画像用途)。
        const auto resolveTexture =
            [&](const ufbx_texture* source,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> LoadedTexture
            {
                if (source == nullptr)
                {
                    return {};
                }
                // 画像と用途の再利用キー
                const auto key = std::make_pair(source, usage);
                // 同じ画像と用途の既存結果
                if (const auto existing =
                        textureCache.find(key);
                    existing != textureCache.end())
                {
                    return existing->second;
                }
                // 生成した画像ビューと透過情報
                auto loaded = LoadTexture(
                    device,
                    assets,
                    modelPath,
                    *source,
                    usage,
                    recorder);
                textureCache.emplace(key, loaded);
                return loaded;
            };

        // 接続された材質画像を用途付きで取得する(map: 元の材質画像記述, usage: 画像用途)。
        const auto resolveMap =
            [&](const ufbx_material_map& map,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> LoadedTexture
            {
                if (!map.texture_enabled
                    || map.texture == nullptr)
                {
                    return {};
                }
                return resolveTexture(map.texture, usage);
            };

        using Usage = LamaPon::TextureLoader::TextureUsage;
        // 取り込んだ法線画像
        const auto normalMap = resolveMap(
            material->pbr.normal_map,
            Usage::NormalMap);
        primitive.normalTexture = normalMap.view;
        primitive.embeddedTextures.normal = normalMap.graphicsView;
        // 取り込んだ粗さ画像
        const auto roughnessMap = resolveMap(
            material->pbr.roughness,
            Usage::DataMap);
        primitive.roughnessTexture = roughnessMap.view;
        primitive.embeddedTextures.roughness =
            roughnessMap.graphicsView;
        // 取り込んだ金属度画像
        const auto metallicMap = resolveMap(
            material->pbr.metalness,
            Usage::DataMap);
        primitive.metallicTexture = metallicMap.view;
        primitive.embeddedTextures.metallic =
            metallicMap.graphicsView;
        // 取り込んだ遮蔽画像
        const auto occlusionMap = resolveMap(
            material->pbr.ambient_occlusion,
            Usage::DataMap);
        primitive.occlusionTexture = occlusionMap.view;
        primitive.embeddedTextures.occlusion =
            occlusionMap.graphicsView;
        // 取り込んだ発光画像
        const auto emissiveMap = resolveMap(
            material->pbr.emission_color,
            Usage::Color);
        primitive.emissiveTexture = emissiveMap.view;
        primitive.embeddedTextures.emissive =
            emissiveMap.graphicsView;

        // 発光色の有無で発光を判定し、色と非負の係数の積を使う。
        // 元の発光色
        const auto& emissionColor = material->pbr.emission_color;
        if (emissionColor.has_value)
        {
            // 元の発光係数
            const auto& emissionFactor =
                material->pbr.emission_factor;
            // 非負の発光係数
            const float strength = emissionFactor.has_value
                ? std::max(
                    static_cast<float>(
                        emissionFactor.value_real),
                    0.0f)
                : 1.0f;
            primitive.emissiveFactor = {
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.x),
                    0.0f) * strength,
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.y),
                    0.0f) * strength,
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.z),
                    0.0f) * strength
            };
        }
        primitive.embeddedTextures.occlusionStrength =
            primitive.occlusionStrength;
        primitive.embeddedTextures.emissiveFactor =
            primitive.emissiveFactor;

        // 基本色に接続された画像
        const auto* sourceTexture =
            baseColor.texture_enabled
                ? baseColor.texture
                : nullptr;
        if (sourceTexture == nullptr)
        {
            return;
        }

        // 画像付きFBXの二重着色を防ぐため拡散RGBを白にし、求めた不透明度は残す。
        primitive.baseColor.x = 1.0f;
        primitive.baseColor.y = 1.0f;
        primitive.baseColor.z = 1.0f;

        // 基本色画像のビューと透過情報
        const auto loadedTexture =
            resolveTexture(sourceTexture, Usage::Color);
        primitive.texture = loadedTexture.view;
        primitive.embeddedTextures.albedo =
            loadedTexture.graphicsView;
        primitive.textureHasTransparency =
            loadedTexture.hasTransparency;
        if (primitive.textureHasTransparency
            && device != nullptr)
        {
            EnableAlphaCutout(primitive, device);
        }
    }

    // 72ボーン以内のスキンを追加し、番号を返す(destination: 更新するモデル, source: 元のスキン情報)。
    std::ptrdiff_t AddSkin(
        LamaPon::SkeletalModel& destination,
        const ufbx_skin_deformer& source)
    {
        if (source.clusters.count
            > static_cast<std::size_t>(
                DirectX::IEffectSkinning::MaxBones))
        {
            throw std::runtime_error(
                "FBX skin exceeds the DirectXTK limit of 72 joints.");
        }

        // 最初のスキンまたは生成先
        LamaPon::SkeletalSkin skin;
        skin.name = ToString(source.name);
        skin.joints.reserve(source.clusters.count);
        skin.inverseBindMatrices.reserve(
            source.clusters.count);
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index < source.clusters.count;
            ++index)
        {
            // スキンのボーンと逆変換
            const auto* cluster = source.clusters.data[index];
            if (cluster == nullptr
                || cluster->bone_node == nullptr)
            {
                throw std::runtime_error(
                    "FBX skin contains a missing bone.");
            }
            skin.joints.push_back(
                cluster->bone_node->typed_id);
            skin.inverseBindMatrices.push_back(
                ToMatrix(cluster->geometry_to_bone));
        }
        destination.skins.emplace_back(std::move(skin));
        return static_cast<std::ptrdiff_t>(
            destination.skins.size() - 1);
    }

    // 面の角から共通頂点と最大4ボーンの影響度を取り込む(mesh: 元メッシュ, skin: 任意の元スキン, cornerIndex: 面の角の番号)。
    Vertex MakeVertex(
        const ufbx_mesh& mesh,
        const ufbx_skin_deformer* skin,
        const std::uint32_t cornerIndex)
    {
        // 局所または変換後の頂点位置
        const auto position =
            ufbx_get_vertex_vec3(
                &mesh.vertex_position,
                cornerIndex);
        // 元頂点の法線
        const auto normal =
            ufbx_get_vertex_vec3(
                &mesh.vertex_normal,
                cornerIndex);
        // 元頂点のUV座標
        const ufbx_vec2 uv = mesh.vertex_uv.exists
            ? ufbx_get_vertex_vec2(
                &mesh.vertex_uv,
                cornerIndex)
            : ufbx_vec2{};
        // 元頂点の接線方向
        const ufbx_vec3 tangent =
            mesh.vertex_tangent.exists
                ? ufbx_get_vertex_vec3(
                    &mesh.vertex_tangent,
                    cornerIndex)
                : ufbx_vec3{ 1.0f, 0.0f, 0.0f };
        // 元頂点のRGBA色
        const ufbx_vec4 color =
            mesh.vertex_color.exists
                ? ufbx_get_vertex_vec4(
                    &mesh.vertex_color,
                    cornerIndex)
                : ufbx_vec4{
                    1.0f,
                    1.0f,
                    1.0f,
                    1.0f
                };

        // 頂点が参照する4ボーン番号
        DirectX::XMUINT4 jointIndices{};
        // 頂点の4ボーンの影響度
        DirectX::XMFLOAT4 blendWeights{
            1.0f,
            0.0f,
            0.0f,
            0.0f
        };
        // 整理済みの先頭4影響だけを取り込み、合計が正なら正規化する。
        if (skin != nullptr)
        {
            // 元の頂点または共有頂点の番号
            const auto vertexIndex =
                mesh.vertex_indices.data[cornerIndex];
            if (vertexIndex >= skin->vertices.count)
            {
                throw std::runtime_error(
                    "FBX skin vertex index is invalid.");
            }
            // 頂点に対応するスキン情報
            const auto& skinVertex =
                skin->vertices.data[vertexIndex];
            // 最大4枠の影響数
            const std::size_t influenceCount =
                std::min<std::size_t>(
                    skinVertex.num_weights,
                    4);
            // 参照する4ボーンの番号
            std::array<std::uint32_t, 4> joints{};
            // 取り込む4ボーンの影響度
            std::array<float, 4> weights{};
            // 取り込んだ影響度の合計
            float total{};
            // 取り込むボーンの影響枠
            for (std::size_t influence = 0;
                // 最大4枠の影響数
                influence < influenceCount;
                ++influence)
            {
                // スキン重み列の要素番号
                const std::size_t weightIndex =
                    skinVertex.weight_begin + influence;
                if (weightIndex >= skin->weights.count)
                {
                    throw std::runtime_error(
                        "FBX skin weight index is invalid.");
                }
                // ボーンの影響度の記録
                const auto& weight =
                    skin->weights.data[weightIndex];
                if (weight.cluster_index
                    >= skin->clusters.count)
                {
                    throw std::runtime_error(
                        "FBX skin joint index is invalid.");
                }
                joints[influence] = weight.cluster_index;
                weights[influence] =
                    static_cast<float>(weight.weight);
                total += weights[influence];
            }
            if (total > 0.000001f)
            {
                // 正規化するボーンの影響度
                for (auto& weight : weights)
                {
                    weight /= total;
                }
                blendWeights = {
                    weights[0],
                    weights[1],
                    weights[2],
                    weights[3]
                };
            }
            jointIndices = {
                joints[0],
                joints[1],
                joints[2],
                joints[3]
            };
        }

        // 画像の原点に合わせ、UVのVを反転する。
        return Vertex{
            ToFloat3(position),
            ToFloat3(normal),
            DirectX::XMFLOAT4{
                tangent.x,
                tangent.y,
                tangent.z,
                1.0f
            },
            DirectX::XMFLOAT4{
                color.x,
                color.y,
                color.z,
                color.w
            },
            DirectX::XMFLOAT2{ uv.x, 1.0f - uv.y },
            jointIndices,
            blendWeights
        };
    }

    // 材質ごとの面を三角形へ分割して頂点を展開する(mesh: 元メッシュ, part: 材質別の描画部分, skin: 任意の元スキン)。
    std::vector<Vertex> BuildPartVertices(
        const ufbx_mesh& mesh,
        const ufbx_mesh_part& part,
        const ufbx_skin_deformer* skin)
    {
        // 三角形分割した面の角番号
        std::vector<std::uint32_t> triangleCorners(
            std::max<std::size_t>(
                mesh.max_face_triangles * 3,
                3));
        // 取り込む共通CPU頂点列
        std::vector<Vertex> vertices;
        vertices.reserve(part.num_triangles * 3);
        // 材質内の面番号
        for (std::size_t faceIndex = 0;
            faceIndex < part.face_indices.count;
            ++faceIndex)
        {
            // メッシュ全体での元の面番号
            const auto sourceFace =
                part.face_indices.data[faceIndex];
            if (sourceFace >= mesh.faces.count)
            {
                throw std::runtime_error(
                    "FBX material part references a missing face.");
            }
            // 三角形へ分割する面
            const auto face = mesh.faces.data[sourceFace];
            // 面の分割後の三角形数
            const std::uint32_t triangleCount =
                ufbx_triangulate_face(
                    triangleCorners.data(),
                    triangleCorners.size(),
                    &mesh,
                    face);
            // 分割した三角形の角番号
            for (std::size_t corner = 0;
                corner < triangleCount * 3;
                ++corner)
            {
                // 元メッシュの面の角番号
                const auto cornerIndex =
                    triangleCorners[corner];
                if (cornerIndex >= mesh.num_indices)
                {
                    throw std::runtime_error(
                        "FBX triangle references a missing vertex.");
                }
                vertices.push_back(
                    MakeVertex(
                        mesh,
                        skin,
                        cornerIndex));
            }
        }
        if (vertices.empty()
            || vertices.size()
                > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "FBX mesh part contains no supported triangles.");
        }
        return vertices;
    }


    struct IndexedGeometry final
    {
        // 重複を共有化した頂点列
        std::vector<Vertex> vertices;
        // 共有頂点を参照する索引列
        std::vector<std::uint32_t> indices;
    };

    // 頂点のバイト配置からFNV-1a識別値を作る(vertex: 比較する頂点)。
    std::uint64_t HashVertex(const Vertex& vertex) noexcept
    {
        // FNV-1aの初期値
        constexpr std::uint64_t offset = 14695981039346656037ull;
        // FNV-1aの乗算値
        constexpr std::uint64_t prime = 1099511628211ull;
        // 頂点バイト列の識別値
        std::uint64_t hash = offset;
        // 元ファイルまたは頂点のバイト列
        const auto* bytes = reinterpret_cast<
            const std::uint8_t*>(&vertex);
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index < sizeof(Vertex);
            ++index)
        {
            hash ^= bytes[index];
            hash *= prime;
        }
        return hash;
    }

    // バイト列が等しい頂点を共有し、索引付き幾何を作る(expanded: 三角形ごとの展開済み頂点)。
    IndexedGeometry BuildIndexedGeometry(
        const std::vector<Vertex>& expanded)
    {
        // 共有頂点と索引の生成先
        IndexedGeometry result;
        result.vertices.reserve(expanded.size());
        result.indices.reserve(expanded.size());
        // 同じ頂点ハッシュの候補番号
        std::unordered_multimap<
            std::uint64_t,
            std::uint32_t> verticesByHash;
        verticesByHash.reserve(expanded.size());

        // 処理する元の頂点
        for (const auto& vertex : expanded)
        {
            // 頂点バイト列の識別値
            const auto hash = HashVertex(vertex);
            // 元の頂点または共有頂点の番号
            std::uint32_t vertexIndex =
                std::numeric_limits<std::uint32_t>::max();
            // begin: 同一ハッシュ候補の先頭, end: 候補範囲の終端
            const auto [begin, end] =
                verticesByHash.equal_range(hash);
            // 同じハッシュの頂点候補
            for (auto candidate = begin;
                candidate != end;
                ++candidate)
            {
                if (std::memcmp(
                        &result.vertices[candidate->second],
                        &vertex,
                        sizeof(Vertex)) == 0)
                {
                    vertexIndex = candidate->second;
                    break;
                }
            }
            if (vertexIndex
                == std::numeric_limits<std::uint32_t>::max())
            {
                vertexIndex = static_cast<std::uint32_t>(
                    result.vertices.size());
                result.vertices.push_back(vertex);
                verticesByHash.emplace(hash, vertexIndex);
            }
            result.indices.push_back(vertexIndex);
        }
        return result;
    }

    // 材質設定済みの描画部分へCPU幾何と任意のGPUリソースを追加する(device: D3D11デバイス、空はCPU経路, assets: 転送予算の管理元, vertices: 展開済みの頂点列, material: 設定済み材質の移動元, meshNode: 所属ノード番号, skin: スキン番号、無しは−1, recorder: 描画部分順の記録先)。
    LamaPon::SkeletalPrimitive FinalizePrimitive(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const std::vector<Vertex>& vertices,
        LamaPon::SkeletalPrimitive material,
        const std::size_t meshNode,
        const std::ptrdiff_t skin,
        LamaPon::ModelCache::Recorder& recorder)
    {
        // 共有化した頂点と索引
        const auto geometry = BuildIndexedGeometry(vertices);
        // キャッシュ幾何はmodel->primitivesへの追加と同じ順で記録する。
        recorder.AddGeometry(
            geometry.vertices.data(),
            geometry.vertices.size(),
            sizeof(Vertex),
            geometry.indices.data(),
            geometry.indices.size());

        // 作成する描画部分
        LamaPon::SkeletalPrimitive primitive =
            std::move(material);
        primitive.meshNode = meshNode;
        primitive.skin = skin;
        primitive.indexCount =
            static_cast<std::uint32_t>(geometry.indices.size());
        // 共通CPU頂点列のバイト先頭
        const auto* vertexBytes =
            reinterpret_cast<const std::uint8_t*>(
                geometry.vertices.data());
        primitive.cpuVertexData.assign(
            vertexBytes,
            vertexBytes
                + geometry.vertices.size() * sizeof(Vertex));
        primitive.cpuVertexStride = sizeof(Vertex);
        primitive.cpuIndices = geometry.indices;
        primitive.hasLocalBounds =
            CalculateLocalBounds(
                geometry.vertices,
                primitive.localBounds);
        CreateBuffer(
            device,
            assets,
            geometry.vertices.data(),
            geometry.vertices.size() * sizeof(Vertex),
            D3D11_BIND_VERTEX_BUFFER,
            primitive.vertexBuffer.ReleaseAndGetAddressOf());
        CreateBuffer(
            device,
            assets,
            geometry.indices.data(),
            geometry.indices.size() * sizeof(std::uint32_t),
            D3D11_BIND_INDEX_BUFFER,
            primitive.indexBuffer.ReleaseAndGetAddressOf());
        if (primitive.skin < 0 && primitive.hasLocalBounds)
        {
            // 生成した詳細度別の索引列
            const auto lodLevels =
                LamaPon::ModelLod::BuildLevels<Vertex>(
                    geometry.vertices,
                    geometry.indices,
                    primitive.localBounds);
            // 詳細度の段階番号
            for (std::size_t level = 0;
                level < lodLevels.size();
                ++level)
            {
                if (lodLevels[level].empty())
                {
                    continue;
                }
                primitive.cpuLodIndices[level] =
                    lodLevels[level];
                CreateBuffer(
                    device,
                    assets,
                    lodLevels[level].data(),
                    lodLevels[level].size()
                        * sizeof(std::uint32_t),
                    D3D11_BIND_INDEX_BUFFER,
                    primitive.lodIndexBuffers[level]
                        .ReleaseAndGetAddressOf());
                primitive.lodIndexCounts[level] =
                    static_cast<std::uint32_t>(
                        lodLevels[level].size());
            }
        }

        if (device != nullptr)
        {
            primitive.effect =
                std::make_shared<DirectX::SkinnedEffect>(device);
            primitive.effect->SetWeightsPerVertex(4);
            // 入力配置用の頂点シェーダー
            const void* shaderBytecode{};
            // シェーダーのバイト数
            std::size_t shaderBytecodeSize{};
            primitive.effect->GetVertexShaderBytecode(
                &shaderBytecode,
                &shaderBytecodeSize);
            ThrowIfFailed(
                device->CreateInputLayout(
                    Vertex::InputElements,
                    Vertex::InputElementCount,
                    shaderBytecode,
                    shaderBytecodeSize,
                    primitive.inputLayout.ReleaseAndGetAddressOf()),
                "Creating FBX input layout");
        }
        return primitive;
    }

    // 静的頂点へ位置・接線と法線の変換を焼き込む(vertex: 元の局所頂点, transform: 位置・接線用の変換, normalTransform: 法線用の逆転置行列)。
    Vertex TransformStaticVertex(
        const Vertex& vertex,
        DirectX::FXMMATRIX transform,
        DirectX::CXMMATRIX normalTransform)
    {
        using namespace DirectX;
        // 変換を焼き込む頂点のコピー
        Vertex result = vertex;
        XMStoreFloat3(
            &result.position,
            XMVector3Transform(
                XMLoadFloat3(&vertex.position),
                transform));
        XMStoreFloat3(
            &result.normal,
            XMVector3Normalize(
                XMVector3TransformNormal(
                    XMLoadFloat3(&vertex.normal),
                    normalTransform)));
        // 変換前の接線方向
        const XMFLOAT3 tangentDirection{
            vertex.tangent.x,
            vertex.tangent.y,
            vertex.tangent.z
        };
        // 変換し正規化した接線方向
        XMFLOAT3 transformedTangent{};
        XMStoreFloat3(
            &transformedTangent,
            XMVector3Normalize(
                XMVector3TransformNormal(
                    XMLoadFloat3(&tangentDirection),
                    transform)));
        result.tangent = {
            transformedTangent.x,
            transformedTangent.y,
            transformedTangent.z,
            vertex.tangent.w
        };
        return result;
    }

    // 材質別の頂点をCPUで準備し、静的結合とリソース作成へ渡す。
    struct StagingPart final
    {
        // 材質別に展開した頂点列
        std::vector<Vertex> vertices;
        // 所属するモデルノード番号
        std::size_t meshNode{};
        // スキン番号、無しは−1
        std::ptrdiff_t skin{ -1 };
        // ufbx解析結果に属する元材質
        const ufbx_material* material{};
    };

    // 開始を0秒へ揃えてTRSを焼き込み、線形キーを作る(scene: 元の解析結果, stack: 取り込むアニメーション)。
    LamaPon::SkeletalAnimationClip ImportAnimation(
        const ufbx_scene& scene,
        const ufbx_anim_stack& stack)
    {
        // 開始補正とキー焼き込みの設定
        ufbx_bake_opts options{};
        options.trim_start_time = true;
        options.resample_rate = 30.0;
        options.maximum_sample_rate = 60.0;
        options.key_reduction_enabled = true;
        options.key_reduction_rotation = true;

        // ufbx解析・焼込のエラー情報
        ufbx_error error{};
        // 所有するアニメーション焼込結果
        std::unique_ptr<
            ufbx_baked_anim,
            BakedAnimationDeleter> baked(
                ufbx_bake_anim(
                    &scene,
                    stack.anim,
                    &options,
                    &error));
        if (!baked)
        {
            throw std::runtime_error(
                "Failed to bake FBX animation: "
                + FormatError(error));
        }

        // 取り込むアニメーションクリップ
        LamaPon::SkeletalAnimationClip clip;
        clip.name = ToString(stack.name);
        if (clip.name.empty())
        {
            clip.name = "Animation";
        }
        clip.duration = static_cast<float>(
            std::max(baked->playback_duration, 0.0));
        clip.tracks.reserve(baked->nodes.count);
        // 元ノードの番号
        for (std::size_t nodeIndex = 0;
            nodeIndex < baked->nodes.count;
            ++nodeIndex)
        {
            // 取り込む元ノードの記述
            const auto& source = baked->nodes.data[nodeIndex];
            if (source.typed_id >= scene.nodes.count)
            {
                continue;
            }
            // 取り込むノードの変換トラック
            LamaPon::SkeletalNodeTrack track;
            track.node = source.typed_id;
            track.translation.interpolation =
                LamaPon::SkeletalInterpolation::Linear;
            track.rotation.interpolation =
                LamaPon::SkeletalInterpolation::Linear;
            track.scale.interpolation =
                LamaPon::SkeletalInterpolation::Linear;

            track.translation.keys.reserve(
                source.translation_keys.count);
            // 元の変換キーの番号
            for (std::size_t keyIndex = 0;
                keyIndex < source.translation_keys.count;
                ++keyIndex)
            {
                // 元のアニメーション変換キー
                const auto& key =
                    source.translation_keys.data[keyIndex];
                track.translation.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat3(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            track.rotation.keys.reserve(
                source.rotation_keys.count);
            // 元の変換キーの番号
            for (std::size_t keyIndex = 0;
                keyIndex < source.rotation_keys.count;
                ++keyIndex)
            {
                // 元のアニメーション変換キー
                const auto& key =
                    source.rotation_keys.data[keyIndex];
                track.rotation.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat4(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            track.scale.keys.reserve(
                source.scale_keys.count);
            // 元の変換キーの番号
            for (std::size_t keyIndex = 0;
                keyIndex < source.scale_keys.count;
                ++keyIndex)
            {
                // 元のアニメーション変換キー
                const auto& key =
                    source.scale_keys.data[keyIndex];
                track.scale.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat3(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            if (!track.translation.keys.empty()
                || !track.rotation.keys.empty()
                || !track.scale.keys.empty())
            {
                clip.tracks.emplace_back(std::move(track));
            }
        }
        return clip;
    }

    // metaの正のimportScaleを読み、未指定・不正なら1を返す(assets: ファイルの取得元, modelPath: 元モデルのパス)。
    float ReadModelImportScale(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath)
    {
        // 倍率を読むmetaファイルのパス
        const std::filesystem::path metaPath(
            modelPath.wstring() + L".meta");
        if (!assets.FileExists(metaPath))
        {
            return 1.0f;
        }
        try
        {
            // 元ファイルまたは頂点のバイト列
            const auto bytes = assets.ReadFileBytes(metaPath);
            if (bytes.empty())
            {
                return 1.0f;
            }
            // 倍率指定を読むmetaのJSON
            const auto document = nlohmann::json::parse(
                bytes.begin(),
                bytes.end());
            // metaの補正倍率
            const double scale = document.value(
                "importScale",
                1.0);
            return scale > 0.0
                ? static_cast<float>(scale)
                : 1.0f;
        }
        catch (const std::exception&)
        {
            return 1.0f;
        }
    }
}

namespace LamaPon
{
    bool FbxImporter::RequiresSkinning(
        AssetManager& assets,
        const std::filesystem::path& path,
        bool* const requiresForwardRole)
    {
        if (requiresForwardRole != nullptr)
        {
            *requiresForwardRole = false;
        }
        if (!assets.FileExists(path))
        {
            throw std::runtime_error(
                "Unable to open FBX file: "
                + PathToUtf8(path));
        }
        // 元ファイルまたは頂点のバイト列
        const auto bytes = assets.ReadFileBytes(path);
        if (bytes.empty())
        {
            throw std::runtime_error("FBX file is empty.");
        }

        // モデルのUTF-8パス
        const auto utf8Path = PathToUtf8(path);
        // FBXの解析設定
        const auto options = MakeLoadOptions(utf8Path);
        // ufbx解析・焼込のエラー情報
        ufbx_error error{};
        // 所有するFBX解析結果
        const std::unique_ptr<ufbx_scene, SceneDeleter> scene(
            ufbx_load_memory(
                bytes.data(),
                bytes.size(),
                &options,
                &error));
        if (!scene)
        {
            throw std::runtime_error(
                "Failed to inspect FBX skinning: "
                + FormatError(error));
        }

        // Loadと同じく、表示中で三角形を持つ材質パーツから描画役割を判定する。
        // スキン描画を要する部分の有無
        bool requiresSkinnedRole{};
        // 通常描画を要する部分の有無
        bool foundForwardRole{};
        // 元ノードの番号
        for (std::size_t nodeIndex = 0;
            nodeIndex < scene->nodes.count;
            ++nodeIndex)
        {
            // 元または生成するモデルノード
            const auto* node = scene->nodes.data[nodeIndex];
            // 表示中の元メッシュ
            const auto* mesh = node->mesh;
            if (mesh == nullptr
                || !node->visible)
            {
                continue;
            }
            // 材質別の描画部分番号
            for (std::size_t partIndex = 0;
                partIndex < mesh->material_parts.count;
                ++partIndex)
            {
                if (mesh->material_parts.data[partIndex]
                        .num_triangles == 0)
                {
                    continue;
                }
                if (mesh->skin_deformers.count != 0)
                {
                    requiresSkinnedRole = true;
                }
                else
                {
                    foundForwardRole = true;
                }
            }
        }
        if (requiresForwardRole != nullptr)
        {
            *requiresForwardRole = foundForwardRole;
        }
        return requiresSkinnedRole;
    }

    std::shared_ptr<SkeletalModel> FbxImporter::Load(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        if (!assets.FileExists(path))
        {
            throw std::runtime_error(
                "Unable to open FBX file: "
                + PathToUtf8(path));
        }
        // 元ファイルまたは頂点のバイト列
        std::vector<std::uint8_t> bytes = assets.ReadFileBytes(path);
        if (bytes.empty())
        {
            throw std::runtime_error("FBX file is empty.");
        }


        // モデル内容から作るキャッシュキー
        const std::uint64_t cacheKey =
            ModelCache::ComputeKey(bytes, 1);
        if (device != nullptr)
        {
            // 復元できたモデルキャッシュ
            if (auto cached = ModelCache::TryLoad(
                    device,
                    context,
                    assets,
                    cacheKey))
            {
                Logger::Instance().Info(
                    "FBX cache: " + PathToUtf8(path)
                    + " nodes="
                    + std::to_string(cached->nodes.size())
                    + " primitives="
                    + std::to_string(cached->primitives.size()));
                return cached;
            }
        }
        // キャッシュ復元情報の記録先
        ModelCache::Recorder recorder;

        // モデルのUTF-8パス
        const std::string utf8Path = PathToUtf8(path);
        // FBXの解析設定
        const auto options = MakeLoadOptions(utf8Path);

        // ufbx解析・焼込のエラー情報
        ufbx_error error{};
        // 所有するFBX解析結果
        std::unique_ptr<ufbx_scene, SceneDeleter> scene(
            ufbx_load_memory(
                bytes.data(),
                bytes.size(),
                &options,
                &error));
        if (!scene)
        {
            throw std::runtime_error(
                "Failed to load FBX: "
                + FormatError(error));
        }

        // 読み込み先のCPUモデル
        auto model = std::make_shared<SkeletalModel>();
        model->nodes.reserve(scene->nodes.count);
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index < scene->nodes.count;
            ++index)
        {
            // 取り込む元ノードの記述
            const auto* source = scene->nodes.data[index];
            // 元または生成するモデルノード
            SkeletalNode node;
            node.name = ToString(source->name);
            if (node.name.empty())
            {
                node.name =
                    "Node " + std::to_string(index);
            }
            node.parent = source->parent != nullptr
                ? static_cast<std::ptrdiff_t>(
                    source->parent->typed_id)
                : -1;
            node.bindPose = ToPose(source->local_transform);
            model->nodes.emplace_back(std::move(node));
        }

        // metaの存在・不在も依存として記録し、補正倍率は根ノードだけへ適用する。
        {
            // 依存として照合するmetaパス
            const std::filesystem::path metaPath(
                path.wstring() + L".meta");
            // metaファイルの存在状態
            const bool metaExists =
                assets.FileExists(metaPath);
            // 依存として記録するmeta内容
            std::vector<std::uint8_t> metaBytes;
            if (metaExists)
            {
                metaBytes = assets.ReadFileBytes(metaPath);
            }
            recorder.RegisterDependency(
                metaPath,
                metaExists,
                metaBytes);
        }
        // 根ノードへ適用する補正倍率
        const float importScale =
            ReadModelImportScale(assets, path);
        if (importScale != 1.0f)
        {
            // 元または生成するモデルノード
            for (auto& node : model->nodes)
            {
                if (node.parent < 0)
                {
                    node.bindPose.translation.x *= importScale;
                    node.bindPose.translation.y *= importScale;
                    node.bindPose.translation.z *= importScale;
                    node.bindPose.scale.x *= importScale;
                    node.bindPose.scale.y *= importScale;
                    node.bindPose.scale.z *= importScale;
                }
            }
        }


        // 画像と用途別の読込結果
        std::map<
            std::pair<
                const ufbx_texture*,
                LamaPon::TextureLoader::TextureUsage>,
            LoadedTexture> textureCache;
        // 材質別の展開済み頂点一覧
        std::vector<StagingPart> stagingParts;
        // 元ノードの番号
        for (std::size_t nodeIndex = 0;
            nodeIndex < scene->nodes.count;
            ++nodeIndex)
        {
            // 元または生成するモデルノード
            const auto* node = scene->nodes.data[nodeIndex];
            // 表示中の元メッシュ
            const auto* mesh = node->mesh;
            if (mesh == nullptr || !node->visible)
            {
                continue;
            }

            // 最初のスキンまたは生成先
            const ufbx_skin_deformer* skin{};
            // スキン番号、無しは−1
            std::ptrdiff_t skinIndex = -1;
            if (mesh->skin_deformers.count > 0)
            {
                skin = mesh->skin_deformers.data[0];
                skinIndex = AddSkin(*model, *skin);
            }

            // 材質別の描画部分番号
            for (std::size_t partIndex = 0;
                partIndex < mesh->material_parts.count;
                ++partIndex)
            {
                // 材質別の元の描画部分
                const auto& part =
                    mesh->material_parts.data[partIndex];
                if (part.num_triangles == 0)
                {
                    continue;
                }
                // 結合前の頂点と所属情報
                StagingPart staging;
                staging.vertices =
                    BuildPartVertices(*mesh, part, skin);
                staging.meshNode = node->typed_id;
                staging.skin = skinIndex;
                staging.material =
                    MaterialForPart(*node, *mesh, part);
                stagingParts.push_back(std::move(staging));
            }
        }

        model->animations.reserve(
            scene->anim_stacks.count);
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index < scene->anim_stacks.count;
            ++index)
        {
            // 取り込むアニメーションクリップ
            auto clip = ImportAnimation(
                *scene,
                *scene->anim_stacks.data[index]);
            if (!clip.tracks.empty())
            {
                model->animations.emplace_back(
                    std::move(clip));
            }
        }

        // ノードごとの局所バインド姿勢
        std::vector<SkeletalPoseTransform> localBindPose;
        // ノードごとの全体バインド行列
        std::vector<DirectX::XMFLOAT4X4> globalBindPose;
        SkeletalModel::SamplePose(
            model->nodes,
            nullptr,
            0.0f,
            localBindPose,
            globalBindPose);
        // 結合前の頂点と所属情報
        for (const auto& staging : stagingParts)
        {
            if (staging.meshNode >= globalBindPose.size())
            {
                continue;
            }
            ExpandModelBounds(
                *model,
                staging.vertices,
                DirectX::XMLoadFloat4x4(
                    &globalBindPose[staging.meshNode]));
        }

        // アニメーションが無いモデルでは、スキン無しのパーツを材質ごとに結合する。
        // 結合前の描画部分の数
        const std::size_t originalPartCount = stagingParts.size();
        // 材質ごとに結合した群の数
        std::size_t mergedGroupCount = 0;
        // 結合後の頂点は全体変換済みなので、単位変換の根ノードに所属させる。
        if (model->animations.empty())
        {
            // 静的結合する材質の一覧
            std::vector<const ufbx_material*> groupMaterials;
            // 材質別の結合先の頂点列
            std::vector<std::vector<Vertex>> groupVertices;
            // 結合前の頂点と所属情報
            for (auto& staging : stagingParts)
            {
                if (staging.skin >= 0
                    || staging.meshNode >= globalBindPose.size())
                {
                    continue;
                }

                // 静的頂点を結合する群の番号
                std::size_t groupIndex = groupMaterials.size();
                // 元データ列の要素番号
                for (std::size_t index = 0;
                    index < groupMaterials.size();
                    ++index)
                {
                    if (groupMaterials[index]
                        == staging.material)
                    {
                        groupIndex = index;
                        break;
                    }
                }
                if (groupIndex == groupMaterials.size())
                {
                    groupMaterials.push_back(staging.material);
                    groupVertices.emplace_back();
                }

                using namespace DirectX;
                // 頂点へ焼き込む全体変換
                const XMMATRIX bind = XMLoadFloat4x4(
                    &globalBindPose[staging.meshNode]);
                // 法線用の全体変換の逆転置
                const XMMATRIX normalMatrix =
                    XMMatrixTranspose(
                        XMMatrixInverse(nullptr, bind));
                // 材質別の静的頂点の結合先
                auto& target = groupVertices[groupIndex];
                target.reserve(
                    target.size() + staging.vertices.size());
                // 処理する元の頂点
                for (const auto& vertex : staging.vertices)
                {
                    target.push_back(
                        TransformStaticVertex(
                            vertex,
                            bind,
                            normalMatrix));
                }
                staging.vertices.clear();
            }

            if (!groupMaterials.empty())
            {
                // 結合した静的幾何の根ノード
                SkeletalNode mergedRoot;
                mergedRoot.name = "MergedStaticGeometry";
                mergedRoot.parent = -1;
                model->nodes.push_back(std::move(mergedRoot));
                // 静的幾何の根ノード番号
                const std::size_t mergedRootIndex =
                    model->nodes.size() - 1;

                // 元データ列の要素番号
                for (std::size_t index = 0;
                    index < groupMaterials.size();
                    ++index)
                {
                    // リソース作成前の描画材質
                    SkeletalPrimitive material;
                    ApplyMaterial(
                        material,
                        groupMaterials[index],
                        device,
                        assets,
                        path,
                        textureCache,
                        recorder);
                    model->primitives.emplace_back(
                        FinalizePrimitive(
                            device,
                            assets,
                            groupVertices[index],
                            std::move(material),
                            mergedRootIndex,
                            -1,
                            recorder));
                }
                mergedGroupCount = groupMaterials.size();
            }
        }

        // 結合前の頂点と所属情報
        for (auto& staging : stagingParts)
        {
            if (staging.vertices.empty())
            {

                continue;
            }
            // リソース作成前の描画材質
            SkeletalPrimitive material;
            ApplyMaterial(
                material,
                staging.material,
                device,
                assets,
                path,
                textureCache,
                recorder);
            model->primitives.emplace_back(
                FinalizePrimitive(
                    device,
                    assets,
                    staging.vertices,
                    std::move(material),
                    staging.meshNode,
                    staging.skin,
                    recorder));
        }

        if (model->nodes.empty()
            || model->primitives.empty())
        {
            throw std::runtime_error(
                "FBX does not contain a supported triangle mesh.");
        }

        {
            // 作成したモデルの総索引数
            std::size_t totalIndices{};
            // 作成する描画部分
            for (const auto& primitive : model->primitives)
            {
                totalIndices += primitive.indexCount;
            }
            // 取込結果の診断文
            std::string summary =
                "FBX imported: " + PathToUtf8(path)
                + " nodes=" + std::to_string(model->nodes.size())
                + " parts=" + std::to_string(originalPartCount);
            if (mergedGroupCount > 0)
            {
                summary += "->"
                    + std::to_string(model->primitives.size())
                    + " (merged)";
            }
            summary += " skins="
                + std::to_string(model->skins.size())
                + " triangles="
                + std::to_string(totalIndices / 3);

            // 描画部分数の警告しきい値
            constexpr std::size_t primitiveCountWarningThreshold = 64;
            if (model->primitives.size()
                > primitiveCountWarningThreshold)
            {
                Logger::Instance().Warning(
                    summary
                    + " -- ドローコール数が多いモデルです。"
                        "DCCツール側で同一マテリアルのメッシュを"
                        "結合してから書き出すことを検討してください。");
            }
            else
            {
                Logger::Instance().Info(summary);
            }
        }

        // D3D11の復元情報だけをモデルキャッシュへ保存する。
        if (device != nullptr)
        {
            ModelCache::Store(cacheKey, *model, recorder);
        }
        return model;
    }
}
