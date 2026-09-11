#pragma once

// DirectX 12の起動・swap chain lifecycleを先行して検証するための
// Runtime内部Backendです。描画資源と高水準pipelineは後続段階で実装します。
#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    class D3D12Backend final : public GraphicsBackend
    {
    public:
        D3D12Backend();
        ~D3D12Backend() override;

        D3D12Backend(const D3D12Backend&) = delete;
        D3D12Backend& operator=(const D3D12Backend&) = delete;

        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        [[nodiscard]] bool IsInitialized() const noexcept override;

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
        [[nodiscard]] std::vector<std::uint8_t> CaptureBackBuffer(
            std::uint32_t& width,
            std::uint32_t& height) const override;
        [[nodiscard]] bool TearingAllowed() const noexcept override
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
        void BindOffscreenTarget(RenderTarget& target) override;
        void PublishOffscreenTarget(RenderTarget& target) override;
        void BindOffscreenTargetDepthOnly(RenderTarget& target) override;
        void CaptureOffscreenTargetDepth(RenderTarget& target) override;
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
        void EndShadowMap(ShadowMap& shadowMap) override;
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
            ProfilerBackend() noexcept override
        {
            return nullptr;
        }
        [[nodiscard]] GraphicsTextureHandle CreateSolidRgba8Texture(
            const std::array<std::uint8_t, 4>& color) override;
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
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
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
            const GraphicsTextureHandle& texture,
            const GraphicsTextureViewDescription& description) override;
        [[nodiscard]] GraphicsViewHandle CreateOffscreenDisplayView(
            const RenderTarget& target) override;
        void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset) override;
        [[nodiscard]] bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback) noexcept override;
        [[nodiscard]] GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) override;
        [[nodiscard]] bool IsViewCurrent(
            const GraphicsViewHandle& view) const noexcept override;
        void InitializeClusteredLights(
            ClusteredLights& clusteredLights,
            AssetManager& assets,
            const std::filesystem::path& shaderPath) override;

    private:
        static constexpr std::size_t BackBufferCount = 2;

        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        void ReleaseSizeDependentResources() noexcept;
        void OpenCommandList();
        void TransitionCurrentBackBuffer(
            D3D12_RESOURCE_STATES state);
        void BindPrimaryOutput();
        void CloseAndExecuteOpenCommands();
        [[nodiscard]] std::uint64_t SignalCurrentBackBuffer();
        void WaitForFence(std::uint64_t value);
        void WaitForGpu();
        void DrainGpu();
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBufferImpl(
                std::uint32_t& width,
                std::uint32_t& height);
        [[noreturn]] void ThrowUnsupported(
            const char* operation) const;

        Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        std::array<
            Microsoft::WRL::ComPtr<ID3D12Resource>,
            BackBufferCount> m_backBuffers;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
        std::array<
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
            BackBufferCount> m_commandAllocators;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
        HANDLE m_fenceEvent{};
        std::array<std::uint64_t, BackBufferCount> m_frameFenceValues{};
        std::array<D3D12_RESOURCE_STATES, BackBufferCount>
            m_backBufferStates{
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_PRESENT };
        std::uint64_t m_nextFenceValue{ 1 };
        std::uint32_t m_currentBackBufferIndex{};
        std::uint32_t m_rtvDescriptorSize{};
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        D3D12_VIEWPORT m_viewport{};
        D3D12_RECT m_scissorRect{};
        bool m_tearingAllowed{};
        bool m_commandListOpen{};
        bool m_terminalFailure{};
        // Execute後にfence signal/waitが失敗しても、GPUが参照し得る
        // readback資源をShutdownまで保持します。
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>
            m_retainedSubmissionResources;
    };
}
