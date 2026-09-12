#include "LamaPon/Assets/AssetManager.h"

#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Assets/AssetArchive.h"
#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Assets/FbxImporter.h"
#include "LamaPon/Assets/GltfImporter.h"
#include "LamaPon/Assets/TextureCache.h"
#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D11Backend.h"
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
    bool CalculateModelBounds(
        const DirectX::Model& model,
        LamaPon::Bounds3D& bounds)
    {
        std::vector<DirectX::XMMATRIX> boneTransforms(
            model.bones.size());
        if (!boneTransforms.empty())
        {
            model.CopyAbsoluteBoneTransformsTo(
                boneTransforms.size(),
                boneTransforms.data());
        }

        bool initialized{};
        for (const auto& mesh : model.meshes)
        {
            if (!mesh)
            {
                continue;
            }
            DirectX::BoundingBox transformed =
                mesh->boundingBox;
            if (mesh->boneIndex < boneTransforms.size())
            {
                mesh->boundingBox.Transform(
                    transformed,
                    boneTransforms[mesh->boneIndex]);
            }
            const DirectX::XMFLOAT3 minimum{
                transformed.Center.x - transformed.Extents.x,
                transformed.Center.y - transformed.Extents.y,
                transformed.Center.z - transformed.Extents.z
            };
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

    // DirectXTKのEffectFactoryへ処理を委譲しつつ、モデル読込時にしか
    // 取得できないマテリアル色をEffectごとに控えます。
    class MaterialCapturingEffectFactory final
        : public DirectX::IEffectFactory
    {
    public:
        explicit MaterialCapturingEffectFactory(ID3D11Device* device)
            : m_factory(device)
        {
        }

        void SetDirectory(const wchar_t* path) noexcept
        {
            m_factory.SetDirectory(path);
        }

        std::shared_ptr<DirectX::IEffect> __cdecl CreateEffect(
            const EffectInfo& info,
            ID3D11DeviceContext* context) override
        {
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

        void __cdecl CreateTexture(
            const wchar_t* name,
            ID3D11DeviceContext* context,
            ID3D11ShaderResourceView** textureView) override
        {
            m_factory.CreateTexture(name, context, textureView);
        }

        [[nodiscard]] std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> TakeDiffuseColors() noexcept
        {
            return std::move(m_diffuseColors);
        }

    private:
        DirectX::EffectFactory m_factory;
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> m_diffuseColors;
    };

    std::filesystem::path FindCmoTextureDirectory(
        const std::filesystem::path& modelPath)
    {
        auto directory = modelPath.parent_path();
        for (std::size_t depth = 0;
             depth < 8 && !directory.empty();
             ++depth)
        {
            const auto sharedTextures =
                directory / L"ModelTexture";
            if (std::filesystem::is_directory(
                    sharedTextures))
            {
                return sharedTextures;
            }

            const auto parent = directory.parent_path();
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return modelPath.parent_path();
    }

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

    void ThrowIfFailed(const HRESULT result, const std::filesystem::path& path)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                "Failed to load texture: " + LamaPon::PathToUtf8(path)
                + " (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ")");
        }
    }

    [[nodiscard]] LamaPon::D3D11Backend* AsD3D11Backend(
        LamaPon::GraphicsBackend* const backend) noexcept
    {
        return dynamic_cast<LamaPon::D3D11Backend*>(backend);
    }

    void ImportSkeletalTextureViews(
        LamaPon::SkeletalModel& model,
        LamaPon::GraphicsBackend* const backend)
    {
        auto* const d3d11 = AsD3D11Backend(backend);
        if (d3d11 == nullptr)
        {
            return;
        }

        // glTFではroughnessとmetallicが同じSRVを共有するなど、複数の
        // primitive/slotが同じnative viewを参照します。1回だけBackend
        // 世代へ取り込み、同じ強所有handleをcopyして共有します。
        std::unordered_map<
            ID3D11ShaderResourceView*,
            LamaPon::GraphicsViewHandle> importedViews;
        const auto importView =
            [d3d11, &importedViews](
                ID3D11ShaderResourceView* const native)
            -> LamaPon::GraphicsViewHandle
        {
            if (native == nullptr)
            {
                return {};
            }
            if (const auto found = importedViews.find(native);
                found != importedViews.end())
            {
                return found->second;
            }
            auto imported = d3d11->ImportShaderResourceView(native);
            auto view = std::move(imported.second);
            importedViews.emplace(native, view);
            return view;
        };

        for (auto& primitive : model.primitives)
        {
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
        default:
            throw std::invalid_argument(
                "The prepared texture format has no graphics backend mapping.");
        }
    }

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

    [[nodiscard]] std::vector<LamaPon::GraphicsTextureSubresourceData>
        MakeTextureSubresources(
            const LamaPon::TextureLoader::PreparedTextureData& data)
    {
        std::vector<LamaPon::GraphicsTextureSubresourceData> subresources;
        subresources.reserve(data.levels.size());
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

    [[nodiscard]] Microsoft::WRL::ComPtr<
        ID3D11ShaderResourceView> ResolveCompatibilityView(
            LamaPon::GraphicsBackend* const backend,
            const LamaPon::GraphicsViewHandle& view)
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resolved;
        if (auto* const d3d11 = AsD3D11Backend(backend))
        {
            resolved = d3d11->ResolveShaderResourceView(view);
        }
        return resolved;
    }

    template <typename Asset>
    void PublishTextureResources(
        Asset& asset,
        LamaPon::GraphicsBackend* const backend,
        LamaPon::GraphicsTextureHandle texture,
        LamaPon::GraphicsViewHandle view)
    {
        // resolverを含む失敗し得る処理を先に終え、assetへは完成した一式だけを
        // 公開します。DirectX 12ではcompatibility viewは空になります。
        auto compatibility = ResolveCompatibilityView(backend, view);
        asset.resources.Publish(
            LamaPon::TextureResourceSnapshot{
                std::move(texture),
                std::move(view),
                std::move(compatibility)
            });
    }

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

    template <typename Asset>
    void CreatePreparedTextureResources(
        Asset& asset,
        LamaPon::GraphicsBackend& backend,
        const LamaPon::TextureLoader::PreparedTextureData& data)
    {
        const auto description = MakeTextureDescription(data);
        const auto subresources = MakeTextureSubresources(data);
        auto texture = backend.CreateTexture2D(
            description,
            subresources);
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

    // 小文字化した拡張子を返します。
    [[nodiscard]] std::wstring LoweredExtension(
        const std::filesystem::path& path)
    {
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            std::towlower);
        return extension;
    }

    // プリフェッチ中にGPUテクスチャまで作成する対象の拡張子か。
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

    [[nodiscard]] std::wstring BuiltInTextureKind(
        const std::filesystem::path& path)
    {
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

    [[nodiscard]] float SegmentDistance(
        const float pointX,
        const float pointY,
        const float ax,
        const float ay,
        const float bx,
        const float by) noexcept
    {
        const float edgeX = bx - ax;
        const float edgeY = by - ay;
        const float lengthSquared = edgeX * edgeX + edgeY * edgeY;
        const float projection = lengthSquared > 0.0f
            ? std::clamp(
                ((pointX - ax) * edgeX
                    + (pointY - ay) * edgeY)
                    / lengthSquared,
                0.0f,
                1.0f)
            : 0.0f;
        const float deltaX = pointX - (ax + edgeX * projection);
        const float deltaY = pointY - (ay + edgeY * projection);
        return std::sqrt(deltaX * deltaX + deltaY * deltaY);
    }

    [[nodiscard]] std::shared_ptr<LamaPon::TextureAsset>
        CreateBuiltInTexture(
            LamaPon::GraphicsBackend* const backend,
            ID3D11Device* device,
            const std::filesystem::path& sourcePath,
            const std::wstring& kind)
    {
        constexpr std::uint32_t Size = 256;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(Size) * Size * 4u,
            0u);
        const auto edge = [](
                const float signedDistance) noexcept
        {
            return std::clamp(
                signedDistance + 0.5f,
                0.0f,
                1.0f);
        };
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
        for (std::uint32_t y{}; y < Size; ++y)
        {
            for (std::uint32_t x{}; x < Size; ++x)
            {
                const float pointX = static_cast<float>(x) + 0.5f;
                const float pointY = static_cast<float>(y) + 0.5f;
                float signedDistance = -1000.0f;
                if (kind == L"circle")
                {
                    const float dx = pointX - 128.0f;
                    const float dy = pointY - 128.0f;
                    signedDistance =
                        112.0f - std::sqrt(dx * dx + dy * dy);
                }
                else if (kind == L"ring")
                {
                    const float dx = pointX - 128.0f;
                    const float dy = pointY - 128.0f;
                    const float distance =
                        std::sqrt(dx * dx + dy * dy);
                    signedDistance =
                        8.0f - std::abs(distance - 96.0f);
                }
                else
                {
                    constexpr float ax = 128.0f;
                    constexpr float ay = 16.0f;
                    constexpr float bx = 240.0f;
                    constexpr float by = 238.0f;
                    constexpr float cx = 16.0f;
                    constexpr float cy = 238.0f;
                    const bool inside =
                        cross(ax, ay, bx, by, pointX, pointY) >= 0.0f
                        && cross(bx, by, cx, cy, pointX, pointY)
                            >= 0.0f
                        && cross(cx, cy, ax, ay, pointX, pointY)
                            >= 0.0f;
                    const float distance = std::min({
                        SegmentDistance(
                            pointX, pointY, ax, ay, bx, by),
                        SegmentDistance(
                            pointX, pointY, bx, by, cx, cy),
                        SegmentDistance(
                            pointX, pointY, cx, cy, ax, ay) });
                    signedDistance = inside ? distance : -distance;
                }
                const auto alpha = static_cast<std::uint8_t>(
                    std::lround(edge(signedDistance) * 255.0f));
                const auto pixelIndex =
                    (static_cast<std::size_t>(y) * Size + x) * 4u;
                pixels[pixelIndex] = 255u;
                pixels[pixelIndex + 1] = 255u;
                pixels[pixelIndex + 2] = 255u;
                pixels[pixelIndex + 3] = alpha;
            }
        }
        auto asset = std::make_shared<LamaPon::TextureAsset>();
        asset->width = Size;
        asset->height = Size;
        asset->sourcePath = sourcePath.lexically_normal();
        if (backend != nullptr)
        {
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
        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = Size;
        textureDescription.Height = Size;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = pixels.data();
        initialData.SysMemPitch = Size * 4u;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                &initialData,
                texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(built-in)");
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = textureDescription.Format;
        viewDescription.ViewDimension =
            D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipLevels = 1;
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
            TextureResourceSlot()
                : current(
                    std::make_shared<const TextureResourceSnapshot>())
            {
            }

            std::atomic<std::shared_ptr<const TextureResourceSnapshot>>
                current;
        };
    }

    TextureResourceBinding::TextureResourceBinding()
        : m_slot(std::make_shared<Detail::TextureResourceSlot>())
    {
    }

    TextureResourceBinding::~TextureResourceBinding() = default;

    TextureResourceBinding::TextureResourceBinding(
        const TextureResourceBinding&) noexcept = default;

    TextureResourceBinding::TextureResourceBinding(
        TextureResourceBinding&& other) noexcept
        : m_slot(other.m_slot)
    {
    }

    TextureResourceBinding& TextureResourceBinding::operator=(
        const TextureResourceBinding&) noexcept = default;

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
                std::scoped_lock lock(m_graphicsWorkMutex);
                m_acceptingGraphicsWork = false;
            }

            // A worker may be waiting for the next frame's upload budget.
            // Release that wait before waiting for the worker count to reach
            // zero, otherwise teardown could deadlock without another frame.
            DisableModelUploadThrottle();

            std::unique_lock lock(m_graphicsWorkMutex);
            m_graphicsWorkCondition.wait(
                lock,
                [this]
                {
                    return m_activeGraphicsWork == 0;
                });
        }
        catch (...)
        {
            // Destruction remains noexcept. The normal mutex/condition
            // variable path does not throw after successful construction.
        }
        WaitForModelPreparation();
    }

    bool AssetManager::TryBeginGraphicsWork() noexcept
    {
        try
        {
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
        // ワーカーはm_assetRoot/m_archiveを参照するため、切り替える
        // 前に必ず完了させます。
        WaitForModelPreparation();
        m_assetRoot = std::filesystem::absolute(std::move(assetRoot)).lexically_normal();
        m_archive.reset();
        if (!std::filesystem::is_directory(m_assetRoot))
        {
            // 配布ゲームでは、展開された"assets"フォルダーを置く場所に
            // 暗号化済み"assets.tpak"を置き、フォルダー自体は同梱しません。
            auto archivePath = m_assetRoot;
            archivePath += L".tpak";
            if (std::filesystem::is_regular_file(archivePath))
            {
                m_archive = AssetArchive::Open(archivePath);
            }
        }
        m_database.SetAssetRoot(m_assetRoot);
        static_cast<void>(m_database.Refresh(true));
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
        const auto resolvedPath = ResolvePath(path);
        const auto cacheKey = MakeCacheKey(resolvedPath);
        std::shared_ptr<
            const std::vector<std::uint8_t>>
                cachedBytes;
        {
            std::scoped_lock lock(m_prefetchMutex);
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

    std::vector<std::uint8_t>
        AssetManager::ReadFileBytesUncached(
            const std::filesystem::path& resolvedPath) const
    {
        if (m_archive)
        {
            const auto relative =
                resolvedPath.lexically_relative(m_assetRoot);
            if (auto bytes = m_archive->TryRead(relative))
            {
                return std::move(*bytes);
            }
            throw std::runtime_error(
                "Asset not found in archive: "
                + PathToUtf8(relative));
        }

        std::ifstream input(
            resolvedPath,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open asset file: "
                + PathToUtf8(resolvedPath));
        }
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine asset size: "
                + PathToUtf8(resolvedPath));
        }
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
        AssetPrefetchReport report;
        report.requestedFiles = paths.size();
        if (progress && !progress(0, paths.size()))
        {
            report.cancelled = true;
            return report;
        }

        std::size_t completed{};
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
            const auto resolvedPath = ResolvePath(path);
            const auto key = MakeCacheKey(resolvedPath);

            try
            {
                bool foundExisting{};
                for (;;)
                {
                    std::uint64_t textureEpoch{};
                    std::uint64_t pathGeneration{};
                    std::uint64_t prefetchEpoch{};
                    {
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
                        const auto generation =
                            m_texturePathGenerations.find(key);
                        pathGeneration = generation
                                != m_texturePathGenerations.end()
                            ? generation->second
                            : 0;
                        prefetchEpoch = m_prefetchEpoch;
                    }

                    auto bytes =
                        std::make_shared<
                            const std::vector<std::uint8_t>>(
                                ReadFileBytesUncached(
                                    resolvedPath));
                    bool generationChanged{};
                    {
                        // Invalidate/Clearと同じmutex集合でcommitし、古い
                        // 読み取り結果が新世代のbyte cacheへ復活しないように
                        // します。世代が変わった場合は最新状態で読み直します。
                        std::scoped_lock lock(
                            m_textureMutex,
                            m_prefetchMutex);
                        const auto generation =
                            m_texturePathGenerations.find(key);
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

                // テクスチャならGPUリソースまでこのワーカー
                // スレッドで作成しておきます（デバイスは
                // フリースレッドなので安全）。失敗しても
                // バイトキャッシュは有効なため、本番ロード側の
                // エラー処理に任せて握りつぶします。
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
            std::scoped_lock lock(m_prefetchMutex);
            ++m_prefetchEpoch;
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
        const auto builtInKind = BuiltInTextureKind(path);
        const auto resolvedPath = builtInKind.empty()
            ? ResolvePath(path)
            : path.lexically_normal();
        const auto baseCacheKey = MakeCacheKey(resolvedPath);
        // 同じ画像でも用途が違えばフォーマットが違うので、
        // メモリ上のキャッシュも用途で分けます。
        auto cacheKey = baseCacheKey;
        cacheKey += L"|u";
        cacheKey += static_cast<wchar_t>(
            L'0' + static_cast<int>(usage));

        for (;;)
        {
            std::uint64_t epoch{};
            std::uint64_t pathGeneration{};
            {
                std::scoped_lock lock(m_textureMutex);
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

            // 生成はロックの外で行い、他スレッドのキャッシュ参照を
            // 止めません。同じpathが途中でInvalidate/Clearされた場合は
            // 旧結果を公開せず、最新世代として読み直します。
            std::optional<PendingTextureUpload> pendingUpload;
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
                // 世代確認、cache採用、段階upload登録を一括commitします。
                // 同じkeyの並行loadでtry_emplaceに負けたresourceや、
                // Invalidate済みのresourceはglobal queueへ公開しません。
                std::scoped_lock lock(m_textureMutex, m_uploadMutex);
                const auto currentGeneration =
                    m_texturePathGenerations.find(baseCacheKey);
                const auto currentPathGeneration =
                    currentGeneration !=
                            m_texturePathGenerations.end()
                        ? currentGeneration->second
                        : 0;
                if (epoch == m_textureEpoch
                    && pathGeneration == currentPathGeneration)
                {
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
                            // placeholderだけがcacheへ残る半端なcommitを
                            // 防ぎ、次回loadで生成から再試行できるようにします。
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
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (bytes.empty())
        {
            return view;
        }
        if (isDds)
        {
            // DDSは既に最終形（多くはBC圧縮済み）なので、
            // 展開し直さずそのまま渡します。
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

        const bool compress = RuntimeTextureCompressionEnabled();
        const std::uint64_t diskCacheKey =
            TextureCache::ComputeKey(bytes, compress, usage);
        TextureLoader::PreparedTextureData prepared;
        if (auto cached = TextureCache::TryLoad(diskCacheKey))
        {
            prepared = std::move(cached->data);
        }
        else
        {
            auto mips = TextureLoader::GenerateMipChain(
                TextureLoader::DecodeImageBytes(bytes));
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
        // モデルのテクスチャは段階アップロードに載せません。
        // 途中のフレームで法線が1x1の平均色になると陰影が崩れ、
        // 「読み込み中」ではなく「壊れている」ように見えるためです。
        return TextureLoader::CreateTexture(m_device, prepared);
    }

    GraphicsViewHandle
        AssetManager::CreateTextureViewHandleFromMemory(
            const std::span<const std::uint8_t> bytes,
            const bool isDds,
            const TextureLoader::TextureUsage usage)
    {
        if (bytes.empty() || m_backend == nullptr || isDds)
        {
            return {};
        }
        auto mips = TextureLoader::GenerateMipChain(
            TextureLoader::DecodeImageBytes(bytes));
        const auto prepared = TextureLoader::PrepareTextureData(
            std::move(mips),
            RuntimeTextureCompressionEnabled(),
            usage);
        const auto description = MakeTextureDescription(prepared);
        const auto subresources = MakeTextureSubresources(prepared);
        auto texture = m_backend->CreateTexture2D(
            description,
            subresources);
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

        auto texture = std::make_shared<TextureAsset>();
        texture->sourcePath = resolvedPath;

        const auto extension =
            LoweredExtension(resolvedPath);
        const auto bytes = ReadFileBytes(resolvedPath);
        if (extension == L".dds")
        {
            // DDSはコンテキストなし（=ミップ自動生成なし）で
            // 読み込むため、フリースレッドなデバイスだけで完結
            // します。
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
            if (auto* const d3d11 = AsD3D11Backend(m_backend))
            {
                auto [textureHandle, viewHandle] =
                    d3d11->ImportShaderResourceView(
                        loadedView.Get());
                PublishTextureResources(
                    *texture,
                    m_backend,
                    std::move(textureHandle),
                    std::move(viewHandle));
            }
            else
            {
                PublishLegacyTextureView(
                    *texture,
                    std::move(loadedView));
            }
        }
        else
        {
            // PNG/JPG等はWICデコード→CPUミップ生成→（設定に
            // より）BC1/BC3圧縮。immediate contextを使わないので
            // ワーカースレッドから安全です。
            //
            // この3段はCPUの重仕事なのに結果が毎回同じなので、
            // 最終形をディスクへ残して2回目以降は読むだけにします。
            // 鍵はファイルの中身から作るため、テクスチャを
            // 差し替えれば自然に作り直しになります。
            const bool compress =
                RuntimeTextureCompressionEnabled();
            const std::uint64_t diskCacheKey =
                TextureCache::ComputeKey(bytes, compress, usage);
            TextureLoader::PreparedTextureData prepared;
            // 段階アップロード中の仮表示用の最小ミップ（1x1）。
            TextureLoader::CpuImage placeholder;
            placeholder.width = 1;
            placeholder.height = 1;
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
                TextureCache::CachedTexture entry;
                // ここのlevelsはこの後ムーブされるので、保存は
                // 先に済ませます。1x1のRGBAが仮表示の色です。
                std::copy_n(
                    placeholder.pixels.begin(),
                    entry.placeholderPixel.size(),
                    entry.placeholderPixel.begin());
                entry.data = std::move(prepared);
                TextureCache::Store(diskCacheKey, entry);
                prepared = std::move(entry.data);
            }

            if ((m_backend != nullptr || m_context != nullptr)
                && prepared.TotalBytes()
                    >= ProgressiveUploadThreshold())
            {
                // 大きいテクスチャは空のDEFAULTテクスチャだけを
                // 作り、粗いミップから毎フレーム少しずつ転送
                // します（PumpTextureUploads）。それまでは1x1の
                // 平均色SRVで表示します。
                texture->width = prepared.levels[0].width;
                texture->height = prepared.levels[0].height;
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    gpuTexture;
                GraphicsTextureHandle gpuTextureHandle;
                if (m_backend != nullptr)
                {
                    auto uploadDescription =
                        MakeTextureDescription(prepared);
                    uploadDescription.updateMode =
                        GraphicsTextureUpdateMode::PerMipUpdate;
                    gpuTextureHandle = m_backend->CreateTexture2D(
                        uploadDescription,
                        {});
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

            // WIC/CPU decodeで作った2D textureの形状はprepared dataから
            // API非依存に確定できます。D3D12 backendではD3D11互換viewを
            // 公開しないため、native SRVのintrospectionへ依存させません。
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

        // DDSとlegacy D3D11経路はnative metadataを保持し得ます。通常の
        // WIC textureは上でprepared dataから設定済みであり、将来の
        // D3D12 backendではcompatibility viewが空でも安全です。
        const auto resources = texture->resources.Acquire();
        const auto compatibilityView = resources != nullptr
            ? resources->d3d11ShaderResourceView
            : nullptr;
        if (compatibilityView)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            compatibilityView->GetResource(
                resource.ReleaseAndGetAddressOf());

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
            ThrowIfFailed(resource.As(&texture2D), resolvedPath);

            D3D11_TEXTURE2D_DESC description{};
            texture2D->GetDesc(&description);
            texture->width = description.Width;
            texture->height = description.Height;
            texture->isCube =
                (description.MiscFlags
                    & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
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

        std::scoped_lock lock(m_uploadMutex);
        std::size_t uploadedBytes = 0;
        while (!m_pendingUploads.empty())
        {
            auto& pending = m_pendingUploads.front();
            // キャッシュからも参照されなくなったテクスチャは
            // 転送せずに破棄します（Clear後など）。
            if (pending.asset.use_count() == 1)
            {
                m_pendingUploads.pop_front();
                continue;
            }

            // 今回公開するmip範囲を、assetや進捗を変更せず先に決めます。
            // view生成に失敗した場合は同じ範囲を次回再試行できます。
            auto nextLevelAfterBatch = pending.nextLevel;
            std::size_t batchBytes = 0;
            while (nextLevelAfterBatch >= 0)
            {
                // 予算を使い切っても最低1レベルは進めます
                // （巨大なミップ0でも前進を保証）。
                if ((uploadedBytes != 0 || batchBytes != 0)
                    && (uploadedBytes >= byteBudget
                        || batchBytes
                            >= byteBudget - uploadedBytes))
                {
                    break;
                }
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
                // 既に別textureで予算を使い切りました。
                break;
            }

            const auto mostDetailed =
                static_cast<std::uint32_t>(
                    nextLevelAfterBatch + 1);
            const auto totalMipLevels =
                static_cast<std::uint32_t>(
                    pending.data.levels.size());
            GraphicsViewHandle nextView;
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

            // viewとcompatibility mirrorが完成してからuploadします。途中で
            // 失敗しても公開中のassetとnextLevel/CPU bytesは変わりません。
            for (auto levelIndex = pending.nextLevel;
                levelIndex > nextLevelAfterBatch;
                --levelIndex)
            {
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
            for (auto levelIndex = pending.nextLevel;
                levelIndex > nextLevelAfterBatch;
                --levelIndex)
            {
                pending.data.levels[
                    static_cast<std::size_t>(levelIndex)]
                        .bytes = {};
            }
            pending.nextLevel = nextLevelAfterBatch;
            uploadedBytes += batchBytes;

            if (pending.nextLevel < 0)
            {
                m_pendingUploads.pop_front();
                continue;
            }
            // 予算切れ。残りは次のフレームで続けます。
            break;
        }
    }

    std::size_t
        AssetManager::PendingTextureUploadCount()
            const noexcept
    {
        std::scoped_lock lock(m_uploadMutex);
        return m_pendingUploads.size();
    }

    void AssetManager::PumpModelUploads(
        const std::size_t byteBudget) noexcept
    {
        try
        {
            // 0指定でも最低1バッファは進めます。完全停止にすると
            // ワーカーが予算待ちのままになり、インポート完了を
            // 永久に受け取れません。
            const std::size_t effectiveBudget = std::max<std::size_t>(
                byteBudget,
                1u);
            {
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
        std::unique_lock lock(m_modelUploadMutex);
        if (!m_modelUploadThrottled
            || std::this_thread::get_id()
                != m_modelPreparationThread)
        {
            return;
        }
        m_modelUploadCondition.wait(
            lock,
            [this, byteCount]
            {
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
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
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

        const auto resolvedPath = ResolvePath(path);
        const auto cacheKey = MakeCacheKey(resolvedPath);
        std::future<std::shared_ptr<ModelAsset>> preparedFuture;
        std::uint64_t preparedGeneration{};
        {
            std::scoped_lock lock(m_modelMutex);
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

        const bool usedPreparedFuture = preparedFuture.valid();
        if (usedPreparedFuture)
        {
            // 同期要求へ切り替わった場合は、次フレームの予算補充を
            // 待たずに残りを完了させます。
            DisableModelUploadThrottle();
        }
        auto asset = usedPreparedFuture
            ? preparedFuture.get()
            : LoadModelUncached(resolvedPath, m_context);
        bool preparedResultIsStale{};
        {
            std::scoped_lock lock(m_modelMutex);
            preparedResultIsStale = usedPreparedFuture
                && preparedGeneration != m_modelGeneration;
        }
        if (preparedResultIsStale)
        {
            // 準備中にInvalidateされた結果は採用せず、現在の内容を
            // 同期経路で読み直します。
            asset = LoadModelUncached(resolvedPath, m_context);
        }
        std::scoped_lock lock(m_modelMutex);
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
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        const auto resolvedPath = ResolvePath(path);
        const auto cacheKey = MakeCacheKey(resolvedPath);
        std::future<std::shared_ptr<ModelAsset>> completedFuture;
        std::wstring completedKey;
        std::uint64_t completedGeneration{};
        {
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
                // 呼び出し側が別モデルへ移っていても、完了済みの
                // ジョブを回収して次の準備が永久に詰まらないように
                // します。
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
                auto completedAsset = completedFuture.get();
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
                // 回収されなかった古いジョブの失敗は、これから
                // 要求されたモデルの準備を妨げません。
            }
        }

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
        const auto generation = m_modelGeneration;
        {
            std::scoped_lock uploadLock(m_modelUploadMutex);
            m_modelUploadThrottled = true;
            m_modelPreparationThread = {};
            m_modelUploadBudgetRemaining =
                DefaultModelUploadBudgetPerFrame;
            m_modelUploadFrameBudget =
                DefaultModelUploadBudgetPerFrame;
            m_modelUploadBytesCurrentFrame = 0;
        }
        std::future<std::shared_ptr<ModelAsset>> future;
        if (!TryBeginGraphicsWork())
        {
            EndModelUploadPreparation();
            return false;
        }
        try
        {
            future = std::async(
                std::launch::async,
                [this, resolvedPath]()
            {
                const auto finishGraphicsWork = [](
                    AssetManager* owner) noexcept
                {
                    owner->EndGraphicsWork();
                };
                const std::unique_ptr<
                    AssetManager,
                    decltype(finishGraphicsWork)> graphicsWorkScope{
                        this,
                        finishGraphicsWork
                    };
                {
                    std::scoped_lock lock(m_modelUploadMutex);
                    m_modelPreparationThread =
                        std::this_thread::get_id();
                }
                const auto finishUpload = [this](
                    AssetManager*) noexcept
                {
                    EndModelUploadPreparation();
                };
                const std::unique_ptr<
                    AssetManager,
                    decltype(finishUpload)> uploadScope{
                        this,
                        finishUpload
                    };
                const HRESULT comResult = CoInitializeEx(
                    nullptr,
                    COINIT_MULTITHREADED);
                if (FAILED(comResult)
                    && comResult != RPC_E_CHANGED_MODE)
                {
                    throw std::runtime_error(
                        "Could not initialize COM for model import.");
                }
                struct ComScope final
                {
                    bool initialized{};
                    ~ComScope()
                    {
                        if (initialized)
                        {
                            CoUninitialize();
                        }
                    }
                } comScope{ SUCCEEDED(comResult) };

                // Immediate Contextを渡さないことで、ワーカー上の
                // 処理をフリースレッドなDevice操作だけに限定します。
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
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
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
        const auto resolvedPath = ResolvePath(path);
        const auto cacheKey = MakeCacheKey(resolvedPath);
        std::future<std::shared_ptr<ModelAsset>> future;
        std::uint64_t generation{};
        {
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
            auto asset = future.get();
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
        const auto finishGraphicsWork = [](AssetManager* owner) noexcept
        {
            owner->EndGraphicsWork();
        };
        const std::unique_ptr<
            AssetManager,
            decltype(finishGraphicsWork)> graphicsWorkScope{
                this,
                finishGraphicsWork
            };

        const auto resolvedPath = ResolvePath(path);
        const auto cacheKey = MakeCacheKey(resolvedPath);
        bool preparationPending{};
        {
            std::scoped_lock lock(m_modelMutex);
            preparationPending = m_pendingModelPreparation
                && m_pendingModelPreparation->cacheKey == cacheKey;
        }
        if (preparationPending)
        {
            // 同じファイルを二重解析せず、準備結果（およびその
            // ディスクキャッシュ）が完成してからインスタンス化します。
            static_cast<void>(LoadModelImpl(resolvedPath));
        }
        return LoadModelUncached(resolvedPath, m_context);
    }

    std::shared_ptr<const AnimationClip>
        AssetManager::LoadAnimationClip(
            const std::filesystem::path& path)
    {
        const auto resolvedPath =
            ResolvePath(path);
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
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
        const auto bytes = ReadFileBytes(resolvedPath);
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
        const auto resolvedPath =
            ResolvePath(path);
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
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
        const auto bytes = ReadFileBytes(resolvedPath);
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
        const auto resolvedPath =
            ResolvePath(path);
        const auto cacheKey =
            MakeCacheKey(resolvedPath);
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
        const auto bytes = ReadFileBytes(resolvedPath);
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

        auto extension = resolvedPath.extension().wstring();
        std::ranges::transform(extension, extension.begin(), std::towlower);

        std::unique_ptr<DirectX::Model> loadedModel;
        std::shared_ptr<SkeletalModel> skeletalModel;
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> embeddedDiffuseColors;
        if (extension == L".cmo")
        {
            // EffectFactoryは各マテリアルのテクスチャをディスクから直接
            // 読み込むため、CMOのテクスチャは暗号化アーカイブの対象に
            // なりません。この形式はLamaPonのアセットパイプラインより
            // 古いため、暗号化して配布するモデルにはglTFかFBXを使います。
            const auto bytes = ReadFileBytes(resolvedPath);
            MaterialCapturingEffectFactory effectFactory(m_device);
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
        else if (extension == L".sdkmesh")
        {
            const auto bytes = ReadFileBytes(resolvedPath);
            MaterialCapturingEffectFactory effectFactory(m_device);
            const auto modelDirectory = resolvedPath.parent_path().wstring();
            effectFactory.SetDirectory(modelDirectory.c_str());
            loadedModel = DirectX::Model::CreateFromSDKMESH(
                m_device,
                bytes.data(),
                bytes.size(),
                effectFactory);
            embeddedDiffuseColors =
                effectFactory.TakeDiffuseColors();
        }
        else if (extension == L".vbo")
        {
            const auto bytes = ReadFileBytes(resolvedPath);
            loadedModel = DirectX::Model::CreateFromVBO(
                m_device,
                bytes.data(),
                bytes.size());
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
        const std::wstring wideText = Utf8ToWide(text);
        const std::wstring wideFontFamily = Utf8ToWide(fontFamily);
        const float layoutWidth = std::clamp(layout.size.x, 0.0f, 4096.0f);
        const float layoutHeight = std::clamp(layout.size.y, 0.0f, 4096.0f);
        const bool constrainedWidth = layoutWidth > 0.0f;
        const bool constrainedHeight = layoutHeight > 0.0f;
        const float maximumWidth = constrainedWidth ? layoutWidth : 4096.0f;
        const float maximumHeight = constrainedHeight ? layoutHeight : 4096.0f;

        // 色はキーに入れません（白で焼いて描画時に掛けるため）。
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

        if (const auto existing = m_textCache.find(cacheKey);
            existing != m_textCache.end())
        {
            existing->second.lastUsed = ++m_textCacheClock;
            return existing->second.asset;
        }

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

        DWRITE_TEXT_METRICS metrics{};
        ThrowIfFailed(
            textLayout->GetMetrics(&metrics),
            "IDWriteTextLayout::GetMetrics");

        const float renderedWidth =
            constrainedWidth
                ? maximumWidth
                : metrics.widthIncludingTrailingWhitespace;
        const float renderedHeight =
            constrainedHeight ? maximumHeight : metrics.height;
        const std::uint32_t width = std::max(
            static_cast<std::uint32_t>(
                std::ceil(renderedWidth)) + 4u,
            1u);
        const std::uint32_t height = std::max(
            static_cast<std::uint32_t>(std::ceil(renderedHeight)) + 4u,
            1u);

        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        ThrowIfFailed(
            m_wicFactory->CreateBitmap(
                width,
                height,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnLoad,
                bitmap.ReleaseAndGetAddressOf()),
            "IWICImagingFactory::CreateBitmap(text)");

        const D2D1_RENDER_TARGET_PROPERTIES renderTargetProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED));

        Microsoft::WRL::ComPtr<ID2D1RenderTarget> renderTarget;
        ThrowIfFailed(
            m_d2dFactory->CreateWicBitmapRenderTarget(
                bitmap.Get(),
                renderTargetProperties,
                renderTarget.ReleaseAndGetAddressOf()),
            "ID2D1Factory::CreateWicBitmapRenderTarget");
        renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        // 白で焼きます。色は描画時に掛けるので、ここで色を入れると
        // 二重に掛かってしまいます（かつ色ごとにテクスチャが増えます）。
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

        WICRect lockRectangle{
            0,
            0,
            static_cast<INT>(width),
            static_cast<INT>(height)
        };
        Microsoft::WRL::ComPtr<IWICBitmapLock> bitmapLock;
        ThrowIfFailed(
            bitmap->Lock(
                &lockRectangle,
                WICBitmapLockRead,
                bitmapLock.ReleaseAndGetAddressOf()),
            "IWICBitmap::Lock");

        UINT stride{};
        UINT dataSize{};
        BYTE* data{};
        ThrowIfFailed(bitmapLock->GetStride(&stride), "IWICBitmapLock::GetStride");
        ThrowIfFailed(
            bitmapLock->GetDataPointer(&dataSize, &data),
            "IWICBitmapLock::GetDataPointer");
        static_cast<void>(dataSize);

        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = width;
        textureDescription.Height = height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
        textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        initialData.SysMemPitch = stride;

        auto asset = std::make_shared<TextTextureAsset>();
        asset->width = width;
        asset->height = height;
        if (m_backend != nullptr)
        {
            const GraphicsTexture2DDescription description{
                width,
                height,
                1,
                GraphicsTextureFormat::Bgra8Unorm
            };
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
            auto texture = m_backend->CreateTexture2D(
                description,
                subresources);
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
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            ThrowIfFailed(
                m_device->CreateTexture2D(
                    &textureDescription,
                    &initialData,
                    texture.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateTexture2D(text)");
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            ThrowIfFailed(
                m_device->CreateShaderResourceView(
                    texture.Get(),
                    nullptr,
                    view.ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateShaderResourceView(text)");
            PublishLegacyTextureView(*asset, std::move(view));
        }

        // 文字テクスチャは「文字列ごとに1枚」なので、スコアや残り時間
        // のように中身が変わり続ける表示では際限なく増えます。上限を
        // 決めて、古くて誰も参照していないものから捨てます。
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

    // 予算を超えていたら、最後に使われたのが古い項目を1つずつ探して
    // 捨てます。キーをコピーしたり、一時配列をソートしたりしないので、
    // 定常状態では1回の走査と1回のeraseだけで済みます。
    // ただしまだ誰かが表示に使っているもの（use_count > 1）は
    // 捨てません。捨てても解放されないうえ、次のフレームで作り直す
    // ことになるためです（キャッシュの意味が無くなる）。
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
                auto oldest = m_textCache.end();
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
            // 掃除や警告ログに失敗しても描画は続けられます
            // （次回また試します）。
        }
    }

    void AssetManager::Clear() noexcept
    {
        WaitForModelPreparation();
        try
        {
            // 世代更新/cache破棄とpending破棄を1つのtransactionにします。
            // 間へ新世代のLoadTextureが入り、そのpendingだけを後段のClear
            // が消してplaceholderを固定してしまう競合を防ぎます。
            std::scoped_lock lock(
                m_textureMutex,
                m_uploadMutex,
                m_prefetchMutex);
            ++m_textureEpoch;
            ++m_prefetchEpoch;
            m_texturePathGenerations.clear();
            m_textureCache.clear();
            m_pendingUploads.clear();
            m_prefetchedBytes.clear();
            m_prefetchedByteCount = 0;
        }
        catch (...)
        {
        }
        try
        {
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
            const auto builtInKind = BuiltInTextureKind(path);
            const auto cachePath = builtInKind.empty()
                ? ResolvePath(path)
                : path.lexically_normal();
            const auto cacheKey =
                MakeCacheKey(cachePath);
            {
                // Clearと同様、cache generationとpending queueを原子的に
                // 無効化し、新世代loadのuploadを誤って消さないようにします。
                std::scoped_lock lock(
                    m_textureMutex,
                    m_uploadMutex,
                    m_prefetchMutex);
                ++m_texturePathGenerations[cacheKey];
                for (int usage =
                        static_cast<int>(
                            TextureLoader::TextureUsage::Color);
                    usage <= static_cast<int>(
                        TextureLoader::TextureUsage::DataMap);
                    ++usage)
                {
                    auto textureKey = cacheKey;
                    textureKey += L"|u";
                    textureKey += static_cast<wchar_t>(L'0' + usage);
                    m_textureCache.erase(textureKey);
                }
                std::erase_if(
                    m_pendingUploads,
                    [&cacheKey](
                        const PendingTextureUpload& pending)
                    {
                        return pending.asset != nullptr
                            && MakeCacheKey(
                                pending.asset->sourcePath)
                                == cacheKey;
                    });
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
                    m_prefetchedBytes.erase(cached);
                }
            }
            {
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
        auto key = path.wstring();
        std::ranges::transform(key, key.begin(), std::towlower);
        return key;
    }

    void AssetManager::WaitForModelPreparation() noexcept
    {
        try
        {
            std::future<std::shared_ptr<ModelAsset>> future;
            {
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
