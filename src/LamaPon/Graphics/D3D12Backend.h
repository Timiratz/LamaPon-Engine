#pragma once

// DirectX 12の起動・swap chain lifecycleと、Sprite／最小3D Mesh／
// 基本offscreen描画に必要なtexture資源を扱うRuntime内部Backendです。
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
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace Detail
    {
        class D3D12ResourceDomain;
        struct ShadowMapBackendState;
    }

    class D3D12GpuProfilerBackend;

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
            ProfilerBackend() noexcept override;
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
            // TextureCubeなど、shaderから見えるviewの次元です。
            D3D12_SRV_DIMENSION dimension{ D3D12_SRV_DIMENSION_TEXTURE2D };
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

        struct ExternalShaderResourceDescriptor final
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            std::uint32_t slot{};
        };

        [[nodiscard]] ID3D12Device* Device() const noexcept
        {
            return m_device.Get();
        }
        [[nodiscard]] ID3D12CommandQueue* CommandQueue() const noexcept
        {
            return m_commandQueue.Get();
        }
        [[nodiscard]] std::size_t FramesInFlight() const noexcept
        {
            return BackBufferCount;
        }
        [[nodiscard]] std::size_t CurrentBackBufferIndex() const noexcept
        {
            return m_currentBackBufferIndex;
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
        [[nodiscard]] bool IsDepthOnlyPassActive() const noexcept
        {
            return m_activeShadowMap != nullptr
                || (m_activeOffscreenTarget != nullptr
                    && m_activeOffscreenDepthOnly);
        }
        [[nodiscard]] ID3D12DescriptorHeap*
            ShaderResourceDescriptorHeap() const noexcept;
        // Dear ImGuiなどBackend外の内部rendererが、同じshader-visible
        // heapへ自分のSRVを作るためのdescriptorです。返したslotは必ず
        // ReleaseExternalShaderResourceDescriptorへ返します。
        [[nodiscard]] ExternalShaderResourceDescriptor
            AllocateExternalShaderResourceDescriptor();
        void ReleaseExternalShaderResourceDescriptor(
            std::uint32_t slot) noexcept;
        // 別Backend世代やShaderResource以外のviewはnulloptです。
        [[nodiscard]] std::optional<ShaderResourceBinding>
            TryResolveShaderResource(
                const GraphicsViewHandle& view) const noexcept;
        // BeginFrameCommandsの後、同じframeの記録中だけ呼べます。
        [[nodiscard]] FrameUploadAllocation AllocateFrameUpload(
            std::uint64_t bytes,
            std::uint64_t alignment);
        [[nodiscard]] const D3D12_VIEWPORT&
            ActiveViewport() const noexcept
        {
            return m_activeViewport;
        }
        [[nodiscard]] const D3D12_RECT&
            ActiveScissorRectangle() const noexcept
        {
            return m_activeScissorRect;
        }
        [[nodiscard]] DXGI_FORMAT ActiveColorFormat() const noexcept
        {
            return m_activeColorFormat;
        }
        [[nodiscard]] DXGI_FORMAT ActiveDepthFormat() const noexcept
        {
            return m_activeDepthFormat;
        }
        // Post-process passの入口です。current colorをSRVとして読める状態で
        // post colorを描画先へbindし、読み取り用のviewを返します。Endは
        // current / postを交換してtargetを再bindし、Abortは交換せずに
        // 元のcurrent colorへ戻します。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenPostProcess(
            RenderTarget& target);
        void EndOffscreenPostProcess(RenderTarget& target);
        void AbortOffscreenPostProcess(RenderTarget& target) noexcept;
        // 自動露出の輝度測定passです。level 0は1/4解像度の対数輝度、以降は
        // 前段を2x2平均した縮小段で、最後の段が1x1です。
        [[nodiscard]] std::uint32_t OffscreenLuminanceLevelCount(
            const RenderTarget& target) const;
        // current color（level 0）または前段を読める状態で指定段を深度無しの
        // 描画先へbindし、読み取り用のviewを返します。測定後は
        // BindOffscreenTargetで通常の描画先へ戻します。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenLuminancePass(
            RenderTarget& target,
            std::uint32_t level);
        // 自動露出を無効にしたとき、読み残した測定値を捨てます。
        void DiscardOffscreenTargetLuminance(RenderTarget& target) noexcept;
        // SSAOのpassです。blur=falseは深度コピーを読んで半解像度の遮蔽へ、
        // trueはその遮蔽を読んでブラー先へ書く描画先を深度無しでbindし、
        // t0へ渡す読み取り用viewを返します。深度はCaptureOffscreenTarget
        // Depthで確定したコピーを読みます。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenAmbientOcclusionPass(
            RenderTarget& target,
            bool blur);
        // 遮蔽textureをshaderから読めるstateへ戻し、深度プリパスと同じ
        // 深度専用の描画先を再bindします。Abortは失敗後の復元で、例外を
        // 外へ出しません。
        void EndOffscreenAmbientOcclusion(RenderTarget& target);
        void AbortOffscreenAmbientOcclusion(RenderTarget& target) noexcept;
        // Screen Space Lens Flareの1/4解像度ストリークpassです。pass 0は
        // current colorを読み、以降は2枚の中間textureを交互に読み書き
        // します。3pass完了後はEndが最後の結果を返して通常の描画先へ
        // 戻します。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenLensFlareStreakPass(
            RenderTarget& target,
            std::uint32_t pass);
        [[nodiscard]] GraphicsViewHandle EndOffscreenLensFlareStreaks(
            RenderTarget& target);
        void AbortOffscreenLensFlareStreaks(RenderTarget& target) noexcept;
        // SSRのHi-Z深度ピラミッドを作るpassです。mip 0は深度コピーを、
        // 以降は1段細かいミップを読める状態で指定ミップを深度無しの
        // 描画先にし、t0へ渡す読み取り用viewを返します。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenReflectionDepthPass(
            RenderTarget& target,
            std::uint32_t mip);
        // 全ミップをshaderから読めるstateへ戻し、depthOnlyに応じて深度専用
        // またはカラーの描画先を再bindします。Abortは失敗後の復元で、例外を
        // 外へ出しません。
        void EndOffscreenReflectionDepthPyramid(
            RenderTarget& target,
            bool depthOnly);
        void AbortOffscreenReflectionDepthPyramid(
            RenderTarget& target,
            bool depthOnly) noexcept;
        // targetが深度プリパスなどの深度専用描画先としてbindされているかです。
        [[nodiscard]] bool IsOffscreenTargetBoundDepthOnly(
            const RenderTarget& target) const noexcept;
        // Compute Shaderの入出力passです。computeWritableなtargetの表示用
        // textureをUAVとして書けるstateへ、入力textureをcompute shaderから
        // も読めるstateへ移し、descriptorと出力の寸法を返します。Endは
        // 入力と出力をpixel shader用のstateへ戻し、例外を外へ出しません。
        struct ComputeBindings final
        {
            D3D12_GPU_DESCRIPTOR_HANDLE output{};
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> inputs{};
            std::uint32_t width{};
            std::uint32_t height{};
        };
        [[nodiscard]] ComputeBindings BeginOffscreenCompute(
            RenderTarget& target,
            const std::array<GraphicsViewHandle, 2>& inputs);
        void EndOffscreenCompute(
            RenderTarget& target,
            const std::array<GraphicsViewHandle, 2>& inputs) noexcept;
        // 使わないtexture枠へ置く、次元ごとのnull SRVです。D3D11で
        // nullptrをbindしたときと同じく、shaderの読み取り結果は0になります。
        // 対応する次元はTEXTURE2D／TEXTURE2DARRAY／TEXTURECUBE／TEXTURE3D／
        // BUFFERです。
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE
            NullShaderResourceDescriptor(D3D12_SRV_DIMENSION dimension);
        // DDS cubeなど6面のimmutable textureを作り、TextureCubeのSRVと
        // 組で返します。subresourcesは面ごとに全ミップを並べます（面数×
        // ミップ数）。共通Backend契約には足さず、D3D12のAsset読み込み
        // だけが使います。
        [[nodiscard]] std::pair<GraphicsTextureHandle, GraphicsViewHandle>
            CreateTextureCube(
                const GraphicsTexture2DDescription& faceDescription,
                std::span<const GraphicsTextureSubresourceData>
                    subresources);
        // DDSの2D array／cube arrayを正しいSRV次元で作ります。
        // arraySizeはcubeArray=falseならslice数、trueならcube数です。
        [[nodiscard]] std::pair<GraphicsTextureHandle, GraphicsViewHandle>
            CreateTextureArray(
                const GraphicsTexture2DDescription& description,
                std::uint32_t arraySize,
                bool cubeArray,
                std::span<const GraphicsTextureSubresourceData>
                    subresources);
        // IBLの事前畳み込み先です。Compute Shaderがミップごとの6面UAV
        // （Texture2DArray）へ書き、描画ではTextureCubeとして読みます。
        struct ComputeCubeTarget final
        {
            GraphicsTextureHandle texture;
            GraphicsViewHandle view;
            std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mipAccess;
        };
        // RGBA16Fで一辺size、mipLevels段のcubeをpixel shader用のstateで
        // 作ります。
        [[nodiscard]] ComputeCubeTarget CreateComputeCubeTarget(
            std::uint32_t size,
            std::uint32_t mipLevels);
        // source（TextureCube、またはプローブの面を写す2D texture）を
        // Compute Shaderからも読めるstateへ、targetの全ミップをUAVへ移し、
        // sourceのbindingを返します。Endは両方をpixel shader用のstateへ
        // 戻し、例外を外へ出しません。
        [[nodiscard]] ShaderResourceBinding BeginCubeCompute(
            const GraphicsViewHandle& source,
            const GraphicsTextureHandle& target);
        void EndCubeCompute(
            const GraphicsViewHandle& source,
            const GraphicsTextureHandle& target) noexcept;

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
        D3D12_VIEWPORT m_activeViewport{};
        D3D12_RECT m_activeScissorRect{};
        DXGI_FORMAT m_activeColorFormat{ PrimaryColorFormat };
        DXGI_FORMAT m_activeDepthFormat{ PrimaryDepthFormat };
        bool m_tearingAllowed{};
        bool m_commandListOpen{};
        RenderTarget* m_activeOffscreenTarget{};
        bool m_activeOffscreenDepthOnly{};
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
        // NullShaderResourceDescriptorが次元ごとに1つだけ作るslotです。
        // domainと同じ寿命で、Shutdownで忘れます。
        std::array<std::optional<std::uint32_t>, 5>
            m_nullShaderResourceSlots{};
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
        std::unique_ptr<D3D12GpuProfilerBackend> m_gpuProfilerBackend;
    };
}
