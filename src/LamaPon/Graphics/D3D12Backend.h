#pragma once

// DirectX 12の起動・swap chain lifecycleと、Sprite／最小3D Mesh描画に
// 必要なtexture資源を扱うRuntime内部Backendです。
#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace LamaPon
{
    namespace Detail
    {
        class D3D12ResourceDomain;
        struct ShadowMapBackendState;
    }

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

        // D3D12描画島（Sprite renderer等）だけが使うRuntime内部入口です。
        // 共通Backend契約へは追加せず、このheaderもSDKへinstallしません。
        struct ShaderResourceBinding final
        {
            D3D12_GPU_DESCRIPTOR_HANDLE descriptor{};
            // viewが参照するtextureの最上位mipの寸法です。
            std::uint32_t width{};
            std::uint32_t height{};
        };

        // 現在のframe command listがGPUで完了するまで有効なupload領域です。
        struct FrameUploadAllocation final
        {
            ID3D12Resource* resource{};
            std::uint64_t offset{};
            D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
            std::byte* data{};
        };

        static constexpr DXGI_FORMAT PrimaryColorFormat =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        static constexpr DXGI_FORMAT PrimaryDepthFormat =
            DXGI_FORMAT_D24_UNORM_S8_UINT;
        static constexpr DXGI_FORMAT ShadowDepthFormat =
            DXGI_FORMAT_D32_FLOAT;

        [[nodiscard]] ID3D12Device* Device() const noexcept
        {
            return m_device.Get();
        }
        // frame command listを開いてprimary outputをbindし、記録先を返します。
        [[nodiscard]] ID3D12GraphicsCommandList* BeginFrameCommands();
        // ShadowMap等が設定した現在のoutputを変えず、記録中のcommand
        // listを返します。深度専用描画中だけrender serviceが使います。
        [[nodiscard]] ID3D12GraphicsCommandList*
            CurrentFrameCommands();
        [[nodiscard]] bool IsShadowPassActive() const noexcept
        {
            return m_activeShadowMap != nullptr;
        }
        [[nodiscard]] ID3D12DescriptorHeap*
            ShaderResourceDescriptorHeap() const noexcept;
        // 別Backend世代やShaderResource以外のviewはnulloptです。
        [[nodiscard]] std::optional<ShaderResourceBinding>
            TryResolveShaderResource(
                const GraphicsViewHandle& view) const noexcept;
        // BeginFrameCommandsの後、同じframeの記録中だけ呼べます。
        [[nodiscard]] FrameUploadAllocation AllocateFrameUpload(
            std::uint64_t bytes,
            std::uint64_t alignment);
        [[nodiscard]] const D3D12_VIEWPORT&
            PrimaryViewport() const noexcept
        {
            return m_viewport;
        }
        [[nodiscard]] const D3D12_RECT&
            PrimaryScissorRectangle() const noexcept
        {
            return m_scissorRect;
        }

    private:
        static constexpr std::size_t BackBufferCount = 2;

        struct FrameUploadChunk final
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            std::byte* data{};
            std::uint64_t capacity{};
            std::uint64_t used{};
        };

        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        void ReleaseSizeDependentResources() noexcept;
        void CreateUploadContext();
        void ReleaseUploadContext() noexcept;
        void OpenCommandList();
        void TransitionCurrentBackBuffer(
            D3D12_RESOURCE_STATES state);
        void BindPrimaryOutput();
        void CloseAndExecuteOpenCommands();
        [[nodiscard]] std::uint64_t ReserveFrameFenceValue() noexcept;
        [[nodiscard]] std::uint64_t SignalCurrentBackBuffer();
        void WaitForFence(std::uint64_t value);
        void WaitForGpu();
        void DrainGpu();
        void CollectRetiredResources() noexcept;
        void ResetFrameUploadArena(std::size_t index) noexcept;
        // 転送元bufferを作ってCPU dataを詰め、専用command listで同期転送
        // します。Asset準備workerからも呼べるよう、frame command listとは
        // 独立したallocator / fenceを使います。
        void SubmitTextureUpload(
            const Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
            std::uint32_t firstMipLevel,
            std::span<const GraphicsTextureSubresourceData> data,
            bool updateExistingTexture);
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
        Detail::ShadowMapBackendState* m_activeShadowMap{};
        // texture uploadはworker threadからも終端状態へ遷移させます。
        std::atomic_bool m_terminalFailure{ false };
        // Execute後にfence signal/waitが失敗しても、GPUが参照し得る
        // readback資源をShutdownまで保持します。
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>
            m_retainedSubmissionResources;

        // handleが最後の参照を失ったresource / descriptorを、GPUの完了まで
        // 退避するBackend世代のdomainです。
        std::shared_ptr<Detail::D3D12ResourceDomain> m_resourceDomain;
        // back bufferごとのframe allocatorと同じ寿命で再利用します。
        std::array<std::vector<FrameUploadChunk>, BackBufferCount>
            m_frameUploadArenas;
        std::mutex m_uploadMutex;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
            m_uploadCommandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>
            m_uploadCommandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> m_uploadFence;
        HANDLE m_uploadFenceEvent{};
        std::uint64_t m_nextUploadFenceValue{ 1 };
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>
            m_retainedUploadResources;
    };
}
