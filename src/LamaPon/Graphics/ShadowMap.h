#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    class ShadowMap final
    {
    public:
        ShadowMap() = default;

        ShadowMap(const ShadowMap&) = delete;
        ShadowMap& operator=(const ShadowMap&) = delete;

        // cube=trueにするとポイントライト用のキューブ深度マップ
        // （6スライス、SRVはTextureCube）を作ります。
        void Initialize(
            ID3D11Device* device,
            std::uint32_t resolution = 2048,
            std::uint32_t cascadeCount = 4,
            bool cube = false);
        void Begin(
            ID3D11DeviceContext* context,
            std::uint32_t cascadeIndex);
        void End(ID3D11DeviceContext* context);

        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept
        {
            return m_shaderResourceView.Get();
        }
        [[nodiscard]] std::uint32_t Resolution() const noexcept
        {
            return m_resolution;
        }
        [[nodiscard]] std::uint32_t CascadeCount() const noexcept
        {
            return static_cast<std::uint32_t>(
                m_depthStencilViews.size());
        }
        [[nodiscard]] bool IsValid() const noexcept
        {
            return !m_depthStencilViews.empty()
                && m_shaderResourceView != nullptr;
        }

    private:
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
        std::uint32_t m_resolution{};
        bool m_hasSavedViewport{};
        bool m_rendering{};
    };
}
