#pragma once

// DirectX 11専用のShadowMap stateです。共通stateと分けることで、
// 将来のD3D12 BackendはD3D11型をincludeせず実装できます。
#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace LamaPon::Detail
{
    struct D3D11ShadowMapState final : ShadowMapBackendState
    {
        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && !m_depthStencilViews.empty()
                && m_depthStencilViews.size() == m_cascadeCount
                && m_shaderResourceView != nullptr;
        }
        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept;
        void Initialize(
            ID3D11Device* device,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube);
        void Begin(
            ID3D11DeviceContext* context,
            std::uint32_t cascadeIndex);
        void End(ID3D11DeviceContext* context);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
        std::vector<
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>>
            m_depthStencilViews;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_shaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_savedRenderTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_savedDepthStencil;
        D3D11_VIEWPORT m_viewport{};
        D3D11_VIEWPORT m_savedViewport{};
        bool m_hasSavedViewport{};
    };
}
