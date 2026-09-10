#pragma once

#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>

namespace LamaPon
{
    class D3D11Backend final : public GraphicsBackend
    {
    public:
        D3D11Backend();
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
        void BindBackBuffer() override;
        void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height) override;
        void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]) override;
        void BindOffscreenTarget(
            RenderTarget& target) override;
        void PublishOffscreenTarget(
            RenderTarget& target) override;
        void BindOffscreenTargetDepthOnly(
            RenderTarget& target) override;
        void CaptureOffscreenTargetDepth(
            RenderTarget& target) override;
        void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) override;
        void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) override;
        [[nodiscard]] std::optional<float>
            TryReadOffscreenTargetLuminance(
                RenderTarget& target) override;
        void CaptureOffscreenTargetLuminance(
            RenderTarget& target) override;
        void InitializeShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube) override;
        void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex) override;
        void EndShadowMap(
            ShadowMap& shadowMap) override;
        void UpdateClusteredLights(
            ClusteredLights& clusteredLights,
            LightingState& lighting,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t width,
            std::uint32_t height) override;
        [[nodiscard]] std::unique_ptr<GraphicsOutputState>
            CaptureOutputState() override;
        void RestoreOutputState(
            const GraphicsOutputState& state) override;
        [[nodiscard]] GraphicsVideoMemoryStatistics
            QueryVideoMemoryStatistics() const noexcept override;
        [[nodiscard]] std::unique_ptr<DebugDrawingBackend>
            CreateDebugDrawingBackend() override;
        [[nodiscard]] GpuProfilerBackend*
            ProfilerBackend() noexcept override;
        [[nodiscard]] GraphicsTextureHandle
            CreateSolidRgba8Texture(
                const std::array<std::uint8_t, 4>& color) override;
        [[nodiscard]] GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture) override;
        [[nodiscard]] bool UpdateDynamicVertexBuffer(
            GraphicsBufferHandle& buffer,
            std::span<const std::byte> data) override;
        [[nodiscard]] GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) override;
        void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data) override;
        [[nodiscard]] GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture,
                const GraphicsTextureViewDescription& description) override;
        [[nodiscard]] GraphicsViewHandle
            CreateOffscreenDisplayView(
                const RenderTarget& target) override;

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
        // 移行期間中のD3D11描画向けnative解決です。emptyはnullptr、別の
        // Backend初期化世代から来たhandleはinvalid_argumentになります。
        [[nodiscard]] ID3D11ShaderResourceView*
            ResolveShaderResourceView(
                const GraphicsViewHandle& view) const;
        [[nodiscard]] ID3D11Buffer* ResolveBuffer(
            const GraphicsBufferHandle& buffer) const;
        // DDS / DirectXTK11など、移行途中のloaderが生成したnative SRVを
        // 現在のBackend世代へ取り込みます。返したviewはtextureを強所有し、
        // 入力COM pointerの所有権は移しません。
        [[nodiscard]] std::pair<
            GraphicsTextureHandle,
            GraphicsViewHandle> ImportShaderResourceView(
                ID3D11ShaderResourceView* view);

    private:
        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        void LogSelectedAdapter() const;
        [[nodiscard]] static std::unique_ptr<GpuProfilerBackend>
            CreateProfilerBackend(
                ID3D11Device* device,
                ID3D11DeviceContext* context);

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
        std::shared_ptr<Detail::GraphicsResourceDomain>
            m_resourceDomain;
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
        std::uint64_t m_debugMessagesLogged{};
        // Device / Contextより先に破棄されるよう末尾で所有します。
        std::unique_ptr<GpuProfilerBackend> m_gpuProfilerBackend;
    };
}
