#include "LamaPon/Graphics/ShadowMap.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }
}

namespace LamaPon
{
    void ShadowMap::Initialize(
        ID3D11Device* device,
        const std::uint32_t resolution,
        const std::uint32_t cascadeCount,
        const bool cube)
    {
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "ShadowMap requires a Direct3D device.");
        }

        m_resolution = std::max(resolution, 1u);
        const auto sliceCount = cube
            ? 6u
            : std::clamp(cascadeCount, 1u, 4u);

        D3D11_TEXTURE2D_DESC textureDescription{};
        textureDescription.Width = m_resolution;
        textureDescription.Height = m_resolution;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = sliceCount;
        textureDescription.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags =
            D3D11_BIND_DEPTH_STENCIL
            | D3D11_BIND_SHADER_RESOURCE;
        if (cube)
        {
            textureDescription.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE;
        }

        ThrowIfFailed(
            device->CreateTexture2D(
                &textureDescription,
                nullptr,
                m_texture.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateTexture2D(shadow map)");

        D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format = DXGI_FORMAT_D32_FLOAT;
        depthViewDescription.ViewDimension =
            D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        depthViewDescription.Texture2DArray.ArraySize = 1;
        m_depthStencilViews.clear();
        m_depthStencilViews.resize(sliceCount);
        for (std::uint32_t index = 0;
            index < sliceCount;
            ++index)
        {
            depthViewDescription.Texture2DArray.
                FirstArraySlice = index;
            ThrowIfFailed(
                device->CreateDepthStencilView(
                    m_texture.Get(),
                    &depthViewDescription,
                    m_depthStencilViews[index].
                        ReleaseAndGetAddressOf()),
                "ID3D11Device::CreateDepthStencilView(shadow cascade)");
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC resourceViewDescription{};
        resourceViewDescription.Format = DXGI_FORMAT_R32_FLOAT;
        if (cube)
        {
            resourceViewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURECUBE;
            resourceViewDescription.TextureCube.MipLevels =
                1;
        }
        else
        {
            resourceViewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            resourceViewDescription.Texture2DArray.MipLevels
                = 1;
            resourceViewDescription.Texture2DArray.ArraySize
                = sliceCount;
        }
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_texture.Get(),
                &resourceViewDescription,
                m_shaderResourceView.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView(shadow map)");

        m_viewport.TopLeftX = 0.0f;
        m_viewport.TopLeftY = 0.0f;
        m_viewport.Width =
            static_cast<float>(m_resolution);
        m_viewport.Height =
            static_cast<float>(m_resolution);
        m_viewport.MinDepth = 0.0f;
        m_viewport.MaxDepth = 1.0f;
    }

    void ShadowMap::Begin(
        ID3D11DeviceContext* context,
        const std::uint32_t cascadeIndex)
    {
        if (context == nullptr
            || !IsValid()
            || m_rendering
            || cascadeIndex >= m_depthStencilViews.size())
        {
            return;
        }

        m_savedRenderTarget.Reset();
        m_savedDepthStencil.Reset();
        context->OMGetRenderTargets(
            1,
            m_savedRenderTarget.ReleaseAndGetAddressOf(),
            m_savedDepthStencil.ReleaseAndGetAddressOf());

        UINT viewportCount = 1;
        context->RSGetViewports(
            &viewportCount,
            &m_savedViewport);
        m_hasSavedViewport = viewportCount != 0;

        // 書き込み先になり得る影SRV（t2〜t5）を全て外します。
        ID3D11ShaderResourceView* nullShadow[]{
            nullptr, nullptr, nullptr, nullptr };
        context->PSSetShaderResources(2, 4, nullShadow);
        context->OMSetRenderTargets(
            0,
            nullptr,
            m_depthStencilViews[cascadeIndex].Get());
        context->RSSetViewports(1, &m_viewport);
        context->ClearDepthStencilView(
            m_depthStencilViews[cascadeIndex].Get(),
            D3D11_CLEAR_DEPTH,
            1.0f,
            0);
        m_rendering = true;
    }

    void ShadowMap::End(ID3D11DeviceContext* context)
    {
        if (context == nullptr || !m_rendering)
        {
            return;
        }

        ID3D11RenderTargetView* renderTargets[]{
            m_savedRenderTarget.Get()
        };
        context->OMSetRenderTargets(
            m_savedRenderTarget != nullptr ? 1u : 0u,
            m_savedRenderTarget != nullptr
                ? renderTargets
                : nullptr,
            m_savedDepthStencil.Get());
        if (m_hasSavedViewport)
        {
            context->RSSetViewports(
                1,
                &m_savedViewport);
        }

        m_savedRenderTarget.Reset();
        m_savedDepthStencil.Reset();
        m_hasSavedViewport = false;
        m_rendering = false;
    }
}
