#include "LamaPon/Graphics/GraphicsDevice.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <CommonStates.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace
{
    [[nodiscard]] LamaPon::D3D11Backend*
        AsD3D11Backend(
            LamaPon::GraphicsBackend* const backend) noexcept
    {
        return dynamic_cast<LamaPon::D3D11Backend*>(backend);
    }
}

namespace LamaPon
{
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
        if (resources == nullptr)
        {
            return nullptr;
        }
        auto* const view =
            TryResolveD3D11ShaderResourceView(*resources);
        if (view != nullptr)
        {
            m_spriteTexturePins.emplace_back(std::move(resources));
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
        if (!m_commonStates)
        {
            throw std::logic_error("GraphicsDevice has not been initialized.");
        }

        return *m_commonStates;
    }

    ID3D11BlendState* GraphicsDevice::AdditiveBlendPreservingAlpha() const
    {
        if (!m_additiveBlendPreservingAlpha)
        {
            m_additiveBlendPreservingAlpha =
                CreateAdditiveBlendPreservingAlpha(Device());
        }
        return m_additiveBlendPreservingAlpha.Get();
    }
}
