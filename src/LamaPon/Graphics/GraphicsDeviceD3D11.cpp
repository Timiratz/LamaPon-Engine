#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <CommonStates.h>
#include <SpriteBatch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
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

    [[nodiscard]] LamaPon::D3D11Backend*
        AsD3D11Backend(
            LamaPon::GraphicsBackend* const backend) noexcept
    {
        return dynamic_cast<LamaPon::D3D11Backend*>(backend);
    }
}

namespace LamaPon::Detail
{
    GraphicsDeviceD3D11Resources::GraphicsDeviceD3D11Resources(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context)
        : spriteBatch(
            std::make_unique<DirectX::SpriteBatch>(context))
        , commonStates(
            std::make_unique<DirectX::CommonStates>(device))
    {
        // UIクリッピング（ScrollView等）用のシザー有効
        // ラスタライザ。
        D3D11_RASTERIZER_DESC scissorDescription{};
        scissorDescription.FillMode =
            D3D11_FILL_SOLID;
        scissorDescription.CullMode = D3D11_CULL_NONE;
        scissorDescription.DepthClipEnable = TRUE;
        scissorDescription.ScissorEnable = TRUE;
        ThrowIfFailed(
            device->CreateRasterizerState(
                &scissorDescription,
                uiScissorRasterizer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateRasterizerState");
    }

    GraphicsDeviceD3D11Resources::~GraphicsDeviceD3D11Resources()
    {
        Reset();
    }

    void GraphicsDeviceD3D11Resources::Reset() noexcept
    {
        spriteShaderCallback = {};
        spriteBatchOwner = D3D11SpriteBatchOwner::None;
        spriteBatchToken = 0;
        spriteBatchNativeBegun = false;
        spriteBlendState = nullptr;
        additiveBlendPreservingAlpha.Reset();
        uiScissorRasterizer.Reset();
        uiScissorStack.clear();
        spriteViewPins.clear();
        spriteTexturePins.clear();
        commonStates.reset();
        spriteBatch.reset();
    }

    GraphicsDeviceApiResources::GraphicsDeviceApiResources() = default;

    GraphicsDeviceApiResources::~GraphicsDeviceApiResources()
    {
        Reset();
    }

    void GraphicsDeviceApiResources::Reset() noexcept
    {
        namedRenderTextureViews.clear();
        // serviceはBackendとD3D11 API資源を参照するため先に破棄します。
        renderServices.reset();
        if (d3d11)
        {
            d3d11->Reset();
            d3d11.reset();
        }
    }

    std::unique_ptr<GraphicsDeviceApiResources>
        CreateD3D11GraphicsDeviceApiResources(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            GraphicsBackend& backend)
    {
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "DirectX 11 API resources require an initialized device "
                "and context.");
        }

        auto resources =
            std::make_unique<GraphicsDeviceApiResources>();
        resources->d3d11 =
            std::make_unique<GraphicsDeviceD3D11Resources>(
                device,
                context);
        resources->renderServices =
            CreateD3D11GraphicsRenderServices(
                device,
                context,
                backend);
        return resources;
    }
}

namespace LamaPon
{
    void GraphicsDevice::CreateApiResources(
        const RenderingApi activeApi)
    {
        if (m_apiResources != nullptr)
        {
            throw std::logic_error(
                "Graphics API resources are already initialized.");
        }
        if (m_backend == nullptr
            || m_backend->Api() != activeApi)
        {
            throw std::logic_error(
                "Graphics API resources require the active backend.");
        }

        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            resources;
        switch (activeApi)
        {
        case RenderingApi::DirectX11:
            resources =
                Detail::CreateD3D11GraphicsDeviceApiResources(
                    Device(),
                    Context(),
                    *m_backend);
            break;
        case RenderingApi::Auto:
        case RenderingApi::DirectX12Experimental:
        default:
            throw std::logic_error(
                "API resources for the active rendering API are not "
                "implemented.");
        }

        // Factoryが全資源を作り終えてから一度だけ公開します。
        m_apiResources = std::move(resources);
    }

    void GraphicsDevice::ResetApiResources() noexcept
    {
        if (m_apiResources)
        {
            m_apiResources->Reset();
            m_apiResources.reset();
        }
    }

    Detail::GraphicsDeviceD3D11Resources*
        GraphicsDevice::TryD3D11ApiResources() const noexcept
    {
        if (m_backend == nullptr
            || m_backend->Api() != RenderingApi::DirectX11
            || m_apiResources == nullptr)
        {
            return nullptr;
        }
        return m_apiResources->d3d11.get();
    }

    Detail::GraphicsDeviceD3D11Resources&
        GraphicsDevice::RequireD3D11ApiResources()
    {
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            throw std::logic_error(
                "DirectX 11 API resources are not initialized.");
        }
        return *resources;
    }

    const Detail::GraphicsDeviceD3D11Resources&
        GraphicsDevice::RequireD3D11ApiResources() const
    {
        const auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            throw std::logic_error(
                "DirectX 11 API resources are not initialized.");
        }
        return *resources;
    }

    ID3D11Device* GraphicsDevice::Device() const noexcept
    {
        const auto* const backend =
            AsD3D11Backend(m_backend.get());
        return backend != nullptr
            ? backend->Device()
            : nullptr;
    }

    ID3D11DeviceContext* GraphicsDevice::Context() const noexcept
    {
        const auto* const backend =
            AsD3D11Backend(m_backend.get());
        return backend != nullptr
            ? backend->Context()
            : nullptr;
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::WhiteTexture() const noexcept
    {
        const auto* const backend =
            AsD3D11Backend(m_backend.get());
        if (backend == nullptr)
        {
            return nullptr;
        }
        try
        {
            return backend->ResolveShaderResourceView(
                m_whiteTextureView);
        }
        catch (...)
        {
            // 既存のnoexcept互換getterは内部不変条件の破損時も安全側へ
            // 倒します。外部handle用の明示resolverは厳密な例外を維持します。
            return nullptr;
        }
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::ResolveD3D11ShaderResourceView(
            const GraphicsViewHandle& view) const
    {
        const auto* const backend =
            AsD3D11Backend(m_backend.get());
        if (backend == nullptr)
        {
            if (view)
            {
                throw std::invalid_argument(
                    "A non-empty shader-resource view requires an active "
                    "DirectX 11 backend.");
            }
            return nullptr;
        }
        return backend->ResolveShaderResourceView(view);
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::TryResolveD3D11ShaderResourceView(
            const GraphicsViewHandle& view) const noexcept
    {
        try
        {
            return ResolveD3D11ShaderResourceView(view);
        }
        catch (...)
        {
            return nullptr;
        }
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::TryResolveD3D11ShaderResourceView(
            const TextureResourceSnapshot& resources)
            const noexcept
    {
        if (resources.shaderResourceView)
        {
            // handleが存在するsnapshotでは、これが別Backend世代なら必ず
            // nullptrへ倒します。旧raw mirrorを誤ってbindしてはいけません。
            return TryResolveD3D11ShaderResourceView(
                resources.shaderResourceView);
        }
        if (resources.texture)
        {
            // neutral textureだけが存在する不完全snapshotもlegacy扱いには
            // しません。raw mirrorへのfallbackは両handleが空の旧経路だけです。
            return nullptr;
        }

        auto* const legacy =
            resources.d3d11ShaderResourceView.Get();
        if (legacy == nullptr)
        {
            return nullptr;
        }
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        legacy->GetDevice(owner.ReleaseAndGetAddressOf());
        return owner.Get() == Device() ? legacy : nullptr;
    }

    ID3D11Buffer* GraphicsDevice::ResolveD3D11Buffer(
        const GraphicsBufferHandle& buffer) const
    {
        const auto* const backend =
            AsD3D11Backend(m_backend.get());
        if (backend == nullptr)
        {
            if (buffer)
            {
                throw std::invalid_argument(
                    "A non-empty buffer requires an active DirectX 11 "
                    "backend.");
            }
            return nullptr;
        }
        return backend->ResolveBuffer(buffer);
    }

    GraphicsViewHandle
        GraphicsDevice::ImportD3D11ShaderResourceView(
            ID3D11ShaderResourceView* const view)
    {
        if (view == nullptr)
        {
            return {};
        }
        auto* const backend = AsD3D11Backend(m_backend.get());
        if (backend == nullptr)
        {
            throw std::logic_error(
                "Importing a DirectX 11 shader-resource view requires "
                "an active DirectX 11 backend.");
        }
        return backend->ImportShaderResourceViewHandle(view);
    }

    EnvironmentRenderer::OwnedPrefilteredEnvironment
        GraphicsDevice::TryLoadCachedEnvironment(
            const std::uint64_t key) const
    {
        return EnvironmentCache::TryLoad(Device(), key);
    }

    std::array<Microsoft::WRL::ComPtr<
        ID3D11ShaderResourceView>, 3>
        GraphicsDevice::UploadBakedGlobalIllumination(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t depth,
        const std::span<const std::uint16_t> coefficients) const noexcept
    {
        std::array<Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>, 3> createdViews;
        auto* const device = Device();
        const auto probeCount =
            BakedGlobalIlluminationProbeCount(
                width,
                height,
                depth);
        if (device == nullptr || !probeCount.has_value()
            || coefficients.size()
                != *probeCount
                    * BakedGlobalIlluminationCoefficientsPerProbe)
        {
            return createdViews;
        }

        for (std::size_t channel = 0;
            channel < createdViews.size();
            ++channel)
        {
            D3D11_TEXTURE3D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.Depth = depth;
            description.MipLevels = 1;
            description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = coefficients.data()
                + channel * *probeCount * 4;
            initialData.SysMemPitch = width * 8;
            initialData.SysMemSlicePitch = width * height * 8;

            Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
            if (FAILED(device->CreateTexture3D(
                &description,
                &initialData,
                texture.ReleaseAndGetAddressOf())))
            {
                return {};
            }
            if (FAILED(device->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                createdViews[channel].ReleaseAndGetAddressOf())))
            {
                return {};
            }
        }

        return createdViews;
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::PinD3D11TextureForSpriteBatch(
            std::shared_ptr<const TextureResourceSnapshot> resources)
    {
        auto& apiResources = RequireD3D11ApiResources();
        if (apiResources.spriteBatchOwner
                != Detail::D3D11SpriteBatchOwner::Legacy
            || !apiResources.spriteBatchNativeBegun)
        {
            throw std::logic_error(
                "A legacy SpriteBatch texture can only be pinned during "
                "an active legacy sprite pass.");
        }
        if (resources == nullptr)
        {
            return nullptr;
        }
        auto* const view =
            TryResolveD3D11ShaderResourceView(*resources);
        if (view != nullptr)
        {
            apiResources.spriteTexturePins.emplace_back(
                std::move(resources));
        }
        return view;
    }

    ID3D11Buffer* GraphicsDevice::AcquireInstanceBuffer(
        const void* data,
        const std::size_t bytes)
    {
        if (data == nullptr || bytes == 0)
        {
            return nullptr;
        }
        return ResolveD3D11Buffer(
            AcquireInstanceBufferHandle(
                std::span{
                    static_cast<const std::byte*>(data),
                    bytes }));
    }

    ID3D11ShaderResourceView*
        GraphicsDevice::RenderTextureView(
            const std::string& name) const noexcept
    {
        const auto* target = FindRenderTexture(name);
        if (target == nullptr
            || !target->IsValid())
        {
            return nullptr;
        }
        return target->DisplayShaderResourceView();
    }

    DirectX::CommonStates& GraphicsDevice::States() const
    {
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr || !resources->commonStates)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *resources->commonStates;
    }

    ID3D11BlendState* GraphicsDevice::AdditiveBlendPreservingAlpha() const
    {
        auto* const resources = TryD3D11ApiResources();
        if (resources == nullptr)
        {
            return nullptr;
        }
        if (!resources->additiveBlendPreservingAlpha)
        {
            resources->additiveBlendPreservingAlpha =
                CreateAdditiveBlendPreservingAlpha(Device());
        }
        return resources->additiveBlendPreservingAlpha.Get();
    }
}
