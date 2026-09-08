#pragma once

#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <cstdint>

namespace LamaPon
{
    class D3D11Backend final : public GraphicsBackend
    {
    public:
        D3D11Backend() = default;
        ~D3D11Backend() override;

        D3D11Backend(const D3D11Backend&) = delete;
        D3D11Backend& operator=(
            const D3D11Backend&) = delete;

        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
        }
        [[nodiscard]] bool
            IsInitialized() const noexcept override
        {
            return m_device != nullptr;
        }

        void Initialize(
            const GraphicsBackendCreateInfo& createInfo) override;
        void PrepareForResourceRelease() noexcept override;
        void Shutdown() noexcept override;
        void Resize(
            std::uint32_t width,
            std::uint32_t height) override;
        void BindAndClearBackBuffer(
            const float clearColor[4]) override;
        void DrainDebugMessages() override;
        void Present(bool vSyncEnabled) override;
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const override;
        [[nodiscard]] bool
            TearingAllowed() const noexcept override
        {
            return m_tearingAllowed;
        }

        // 既存のDirectX 11描画経路へ貸し出す非所有ポインターです。
        // GraphicsDeviceは移行期間中、従来のDevice/Context APIを
        // このアクセサへ転送します。
        [[nodiscard]] ID3D11Device*
            Device() const noexcept
        {
            return m_device.Get();
        }
        [[nodiscard]] ID3D11DeviceContext*
            Context() const noexcept
        {
            return m_context.Get();
        }
        [[nodiscard]] ID3D11RenderTargetView*
            BackBufferRenderTargetView() const noexcept
        {
            return m_renderTargetView.Get();
        }

    private:
        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        void LogSelectedAdapter() const;

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_depthStencilView;
        D3D11_VIEWPORT m_viewport{};
        bool m_tearingAllowed{};
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
        std::uint64_t m_debugMessagesLogged{};
    };
}
