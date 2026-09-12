#include "LamaPon/Graphics/D3D12Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/DxgiTextureLayout.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[noreturn]] void ThrowHResult(
        const HRESULT result,
        const char* const operation,
        ID3D12Device* const device = nullptr)
    {
        std::ostringstream message;
        message << operation << " failed (HRESULT=0x"
            << std::hex << std::uppercase
            << static_cast<unsigned long>(result);
        if (device != nullptr
            && (result == DXGI_ERROR_DEVICE_REMOVED
                || result == DXGI_ERROR_DEVICE_RESET
                || result == DXGI_ERROR_DEVICE_HUNG))
        {
            message << ", device reason=0x"
                << static_cast<unsigned long>(
                    device->GetDeviceRemovedReason());
        }
        message << ").";
        throw std::runtime_error(message.str());
    }

    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation,
        ID3D12Device* const device = nullptr)
    {
        if (FAILED(result))
        {
            ThrowHResult(result, operation, device);
        }
    }

    [[nodiscard]] bool QueryTearingSupport(
        IDXGIFactory4* const factory) noexcept
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        if (factory == nullptr
            || FAILED(factory->QueryInterface(
                IID_PPV_ARGS(factory5.GetAddressOf()))))
        {
            return false;
        }
        BOOL allowed = FALSE;
        return SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowed,
                sizeof(allowed)))
            && allowed != FALSE;
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE OffsetDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        const std::size_t index,
        const std::uint32_t increment) noexcept
    {
        handle.ptr += static_cast<SIZE_T>(index) * increment;
        return handle;
    }

    [[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(
        const D3D12_HEAP_TYPE type) noexcept
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = type;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::uint64_t size) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = size;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return description;
    }

    [[nodiscard]] std::string AdapterName(
        const DXGI_ADAPTER_DESC1& description)
    {
        std::string name;
        for (const auto character : description.Description)
        {
            if (character == L'\0')
            {
                break;
            }
            name.push_back(
                character < 128
                    ? static_cast<char>(character)
                    : '?');
        }
        return name;
    }

    // D3D12 bootstrapではdebug line用のpipelineをまだ持たないため、
    // GraphicsDeviceのDebugRenderer契約だけを満たすno-op sinkです。
    // Scene/UI描画を有効にするものではありません。
    class D3D12BootstrapDebugDrawingBackend final
        : public LamaPon::DebugDrawingBackend
    {
    public:
        void DrawLines(
            std::span<const LamaPon::DebugLine>,
            const DirectX::XMFLOAT4X4&,
            const DirectX::XMFLOAT4X4&) override
        {
        }
    };
}

namespace LamaPon::Detail
{
    // D3D12 Backend世代のresource domainです。handleは任意のthreadで最後の
    // 参照を失い得るため、GPUがまだ参照し得るresourceやdescriptorは直接
    // 解放せず、退避時点で次に発行されるframe fenceの完了まで保持します。
    class D3D12ResourceDomain final
        : public GraphicsResourceDomain
    {
    public:
        static constexpr std::uint32_t ShaderResourceCapacity = 16384u;

        D3D12ResourceDomain(
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shaderResourceHeap,
            const std::uint32_t descriptorSize)
            : m_shaderResourceHeap(std::move(shaderResourceHeap))
            , m_cpuStart(
                m_shaderResourceHeap->GetCPUDescriptorHandleForHeapStart())
            , m_gpuStart(
                m_shaderResourceHeap->GetGPUDescriptorHandleForHeapStart())
            , m_descriptorSize(descriptorSize)
        {
        }

        [[nodiscard]] ID3D12DescriptorHeap*
            ShaderResourceHeap() const noexcept
        {
            return m_shaderResourceHeap.Get();
        }

        [[nodiscard]] std::uint32_t AllocateShaderResourceSlot()
        {
            std::scoped_lock lock(m_mutex);
            if (m_closed)
            {
                throw std::logic_error(
                    "The D3D12 resource domain has been shut down.");
            }
            if (!m_freeSlots.empty())
            {
                const auto slot = m_freeSlots.back();
                m_freeSlots.pop_back();
                return slot;
            }
            if (m_nextSlot >= ShaderResourceCapacity)
            {
                throw std::runtime_error(
                    "The D3D12 shader resource descriptor heap is full.");
            }
            return m_nextSlot++;
        }

        // handleへ公開する前に生成が失敗したslotはGPUから参照されて
        // いないため、fenceを待たずに再利用へ戻します。
        void ReleaseUnpublishedShaderResourceSlot(
            const std::uint32_t slot) noexcept
        {
            try
            {
                std::scoped_lock lock(m_mutex);
                m_freeSlots.push_back(slot);
            }
            catch (...)
            {
                // 戻せないslotは再利用しないだけで、安全性は損ないません。
            }
        }

        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE
            ShaderResourceCpuHandle(
                const std::uint32_t slot) const noexcept
        {
            auto handle = m_cpuStart;
            handle.ptr += static_cast<SIZE_T>(slot) * m_descriptorSize;
            return handle;
        }

        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE
            ShaderResourceGpuHandle(
                const std::uint32_t slot) const noexcept
        {
            auto handle = m_gpuStart;
            handle.ptr += static_cast<UINT64>(slot) * m_descriptorSize;
            return handle;
        }

        void PublishNextFrameFenceValue(
            const std::uint64_t value) noexcept
        {
            m_nextFrameFenceValue.store(value);
        }

        // 記録中のframe command listは次に発行されるfenceより前に実行
        // されるため、その値の完了後ならresourceとdescriptorを解放できます。
        void Retire(
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            const std::optional<std::uint32_t> slot) noexcept
        {
            RetiredResource retired{
                m_nextFrameFenceValue.load(),
                std::move(resource),
                slot.value_or(0u),
                slot.has_value()
            };
            try
            {
                std::scoped_lock lock(m_mutex);
                if (!m_closed)
                {
                    m_retired.push_back(std::move(retired));
                    return;
                }
                if (retired.hasSlot)
                {
                    m_freeSlots.push_back(retired.slot);
                }
            }
            catch (...)
            {
                // 退避先を確保できなければ、GPUが参照中かもしれない
                // resourceを解放せず意図的に保持し続けます。
                if (retired.resource != nullptr)
                {
                    retired.resource->AddRef();
                }
            }
        }

        void RetireDescriptorHeap(
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap) noexcept
        {
            if (heap == nullptr)
            {
                return;
            }
            RetiredResource retired{
                m_nextFrameFenceValue.load(),
                nullptr,
                0u,
                false,
                std::move(heap)
            };
            try
            {
                std::scoped_lock lock(m_mutex);
                if (!m_closed)
                {
                    m_retired.push_back(std::move(retired));
                    return;
                }
            }
            catch (...)
            {
                // GPUがdescriptorを参照中かもしれないため、
                // 退避できない場合は意図的に解放しません。
                if (retired.descriptorHeap != nullptr)
                {
                    retired.descriptorHeap->AddRef();
                }
            }
        }

        void CollectCompleted(
            const std::uint64_t completedValue) noexcept
        {
            std::vector<RetiredResource> released;
            try
            {
                std::scoped_lock lock(m_mutex);
                const auto pending = std::partition(
                    m_retired.begin(),
                    m_retired.end(),
                    [completedValue](
                        const RetiredResource& entry) noexcept
                    {
                        return entry.fenceValue <= completedValue;
                    });
                const auto completedCount = static_cast<std::size_t>(
                    std::distance(m_retired.begin(), pending));
                if (completedCount == 0)
                {
                    return;
                }
                // 途中で失敗して同じslotを二重に戻さないよう、確保を
                // 先に済ませてから状態を変更します。
                released.reserve(completedCount);
                m_freeSlots.reserve(
                    m_freeSlots.size() + completedCount);
                for (auto entry = m_retired.begin();
                    entry != pending;
                    ++entry)
                {
                    if (entry->hasSlot)
                    {
                        m_freeSlots.push_back(entry->slot);
                    }
                    released.push_back(std::move(*entry));
                }
                m_retired.erase(m_retired.begin(), pending);
            }
            catch (...)
            {
                // 解放できなかった項目は次回の回収で再試行します。
            }
        }

        // GPUの停止後に呼びます。退避中の資源を解放し、以後の退避は
        // 即時解放へ切り替えます。
        void Close() noexcept
        {
            std::vector<RetiredResource> released;
            try
            {
                std::scoped_lock lock(m_mutex);
                m_closed = true;
                released.swap(m_retired);
            }
            catch (...)
            {
                // lockを取得できない場合、退避中の資源は保持したままにします。
            }
            m_shaderResourceHeap.Reset();
        }

    private:
        struct RetiredResource final
        {
            std::uint64_t fenceValue{};
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            std::uint32_t slot{};
            bool hasSlot{};
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
                descriptorHeap;
        };

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shaderResourceHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
        std::uint32_t m_descriptorSize{};
        std::mutex m_mutex;
        std::vector<std::uint32_t> m_freeSlots;
        std::vector<RetiredResource> m_retired;
        std::atomic<std::uint64_t> m_nextFrameFenceValue{ 1u };
        std::uint32_t m_nextSlot{};
        bool m_closed{};
    };
}

namespace
{
    // 共通Texture handleが所有するDirectX 12側の実体です。
    class D3D12TexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D12TexturePayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> texture,
            const LamaPon::GraphicsTexture2DDescription&
                textureDescription,
            const DXGI_FORMAT textureFormat)
            : GraphicsTexturePayload(resourceDomain)
            , native(std::move(texture))
            , description(textureDescription)
            , format(textureFormat)
        {
        }

        ~D3D12TexturePayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        LamaPon::GraphicsTexture2DDescription description;
        DXGI_FORMAT format{};
    };

    // Shadow textureは通常の2D texture契約では表せないarray/cubeです。
    // 専用payloadに閉じ込め、公開側にはShaderResource viewだけを渡します。
    class D3D12ShadowTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D12ShadowTexturePayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> texture)
            : GraphicsTexturePayload(resourceDomain)
            , native(std::move(texture))
        {
        }

        ~D3D12ShadowTexturePayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> native;
    };

    // shader-visible heap内のSRV descriptorです。参照先textureは基底の
    // GraphicsViewPayloadが強所有します。
    class D3D12ShaderResourceViewPayload final
        : public LamaPon::Detail::GraphicsViewPayload
    {
    public:
        D3D12ShaderResourceViewPayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            LamaPon::GraphicsTextureHandle texture,
            const std::uint32_t descriptorSlot,
            const std::uint32_t textureWidth,
            const std::uint32_t textureHeight)
            : GraphicsViewPayload(
                resourceDomain,
                LamaPon::GraphicsViewKind::ShaderResource,
                std::move(texture))
            , slot(descriptorSlot)
            , width(textureWidth)
            , height(textureHeight)
        {
        }

        ~D3D12ShaderResourceViewPayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(nullptr, slot);
        }

        std::uint32_t slot{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct D3D12ShadowMapState final
        : LamaPon::Detail::ShadowMapBackendState
    {
        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && texture != nullptr
                && depthStencilHeap != nullptr
                && descriptorSize != 0u
                && m_cascadeCount != 0u;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
            depthStencilHeap;
        D3D12_RESOURCE_STATES resourceState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        std::uint32_t descriptorSize{};
        bool cube{};
    };

    struct D3D12RenderTargetState final
        : LamaPon::Detail::RenderTargetBackendState
    {
        ~D3D12RenderTargetState() noexcept override
        {
            if (resourceDomain != nullptr)
            {
                resourceDomain->RetireDescriptorHeap(
                    std::move(renderTargetHeap));
                resourceDomain->RetireDescriptorHeap(
                    std::move(depthStencilHeap));
            }
        }

        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && color != nullptr
                && postColor != nullptr
                && displayColor != nullptr
                && depth != nullptr
                && depthCopy != nullptr
                && renderTargetHeap != nullptr
                && depthStencilHeap != nullptr
                && renderTargetDescriptorSize != 0u;
        }

        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>
            resourceDomain;
        Microsoft::WRL::ComPtr<ID3D12Resource> color;
        Microsoft::WRL::ComPtr<ID3D12Resource> postColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> displayColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> depth;
        Microsoft::WRL::ComPtr<ID3D12Resource> depthCopy;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> renderTargetHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depthStencilHeap;
        D3D12_RESOURCE_STATES colorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES postColorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES displayColorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES depthState{
            D3D12_RESOURCE_STATE_DEPTH_WRITE };
        D3D12_RESOURCE_STATES depthCopyState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        std::uint32_t renderTargetDescriptorSize{};
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        bool computeWritable{};
    };

    class D3D12OutputState final : public LamaPon::GraphicsOutputState
    {
    public:
        // RenderTargetの指す先はSceneの短命なRAII scope内だけで
        // 復元します。domainを強所有し、別Backend世代の
        // tokenを誤って適用しないためのidentityにも使います。
        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>
            resourceDomain;
        LamaPon::RenderTarget* offscreenTarget{};
        bool depthOnly{};
    };

    [[nodiscard]] const D3D12TexturePayload* TryTexturePayload(
        const LamaPon::GraphicsTextureHandle& texture,
        const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        if (domain == nullptr
            || GraphicsResourceHandleAccess::Domain(texture) != domain)
        {
            return nullptr;
        }
        return dynamic_cast<const D3D12TexturePayload*>(
            GraphicsResourceHandleAccess::Payload(texture));
    }

    [[nodiscard]] const D3D12ShaderResourceViewPayload*
        TryShaderResourceViewPayload(
            const LamaPon::GraphicsViewHandle& view,
            const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        if (domain == nullptr
            || GraphicsResourceHandleAccess::Domain(view) != domain)
        {
            return nullptr;
        }
        return dynamic_cast<const D3D12ShaderResourceViewPayload*>(
            GraphicsResourceHandleAccess::Payload(view));
    }

    [[nodiscard]] LamaPon::GraphicsViewHandle CreateTextureView(
        ID3D12Device* const device,
        const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>& domain,
        const Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& nativeDescription,
        const std::uint32_t width,
        const std::uint32_t height,
        const LamaPon::GraphicsTextureFormat publicFormat)
    {
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            device->CreateShaderResourceView(
                resource.Get(),
                &nativeDescription,
                domain->ShaderResourceCpuHandle(slot));
            LamaPon::GraphicsTexture2DDescription description;
            description.width = width;
            description.height = height;
            description.format = publicFormat;
            auto texture = LamaPon::Detail::GraphicsResourceHandleAccess::
                MakeTexture(std::make_shared<D3D12TexturePayload>(
                    domain,
                    resource,
                    description,
                    nativeDescription.Format));
            return LamaPon::Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    std::move(texture),
                    slot,
                    width,
                    height));
        }
        catch (...)
        {
            domain->ReleaseUnpublishedShaderResourceSlot(slot);
            throw;
        }
    }

    void TransitionResource(
        ID3D12GraphicsCommandList* const commandList,
        ID3D12Resource* const resource,
        D3D12_RESOURCE_STATES& currentState,
        const D3D12_RESOURCE_STATES requiredState)
    {
        if (currentState == requiredState)
        {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = requiredState;
        commandList->ResourceBarrier(1, &barrier);
        currentState = requiredState;
    }

    [[nodiscard]] std::uint64_t AlignUp(
        const std::uint64_t value,
        const std::uint64_t alignment)
    {
        if (value
            > std::numeric_limits<std::uint64_t>::max()
                - (alignment - 1u))
        {
            throw std::length_error(
                "The D3D12 upload size cannot be aligned.");
        }
        return (value + alignment - 1u) & ~(alignment - 1u);
    }

    void WaitForFenceCompletion(
        ID3D12Device* const device,
        ID3D12Fence* const fence,
        const HANDLE event,
        const std::uint64_t value)
    {
        if (value == 0)
        {
            return;
        }
        const std::uint64_t completedValue =
            fence->GetCompletedValue();
        if (completedValue
            == std::numeric_limits<std::uint64_t>::max())
        {
            ThrowHResult(
                DXGI_ERROR_DEVICE_REMOVED,
                "ID3D12Fence::GetCompletedValue",
                device);
        }
        if (completedValue >= value)
        {
            return;
        }
        ThrowIfFailed(
            fence->SetEventOnCompletion(value, event),
            "ID3D12Fence::SetEventOnCompletion",
            device);
        if (WaitForSingleObject(event, INFINITE)
            != WAIT_OBJECT_0)
        {
            throw std::runtime_error(
                "Waiting for the D3D12 fence failed.");
        }
        const std::uint64_t completedAfterWait =
            fence->GetCompletedValue();
        if (completedAfterWait
            == std::numeric_limits<std::uint64_t>::max())
        {
            ThrowHResult(
                DXGI_ERROR_DEVICE_REMOVED,
                "ID3D12Fence::GetCompletedValue",
                device);
        }
        if (completedAfterWait < value)
        {
            throw std::runtime_error(
                "The D3D12 fence event fired before the requested "
                "value completed.");
        }
    }
}

namespace LamaPon
{
    D3D12Backend::D3D12Backend() = default;

    D3D12Backend::~D3D12Backend()
    {
        Shutdown();
    }

    bool D3D12Backend::IsInitialized() const noexcept
    {
        return m_factory != nullptr
            && m_adapter != nullptr
            && m_device != nullptr
            && m_commandQueue != nullptr
            && m_swapChain != nullptr
            && m_rtvHeap != nullptr
            && m_dsvHeap != nullptr
            && m_backBuffers[0] != nullptr
            && m_backBuffers[1] != nullptr
            && m_depthBuffer != nullptr
            && m_commandAllocators[0] != nullptr
            && m_commandAllocators[1] != nullptr
            && m_commandList != nullptr
            && m_fence != nullptr
            && m_fenceEvent != nullptr
            && m_resourceDomain != nullptr
            && m_uploadCommandList != nullptr
            && m_uploadFence != nullptr
            && m_uploadFenceEvent != nullptr
            && !m_terminalFailure;
    }

    void D3D12Backend::Initialize(
        const GraphicsBackendCreateInfo& createInfo)
    {
        Shutdown();

        try
        {
            const HWND window = static_cast<HWND>(
                createInfo.nativeWindow);
            if (window == nullptr)
            {
                throw std::invalid_argument(
                    "D3D12Backend requires a native window.");
            }
            const std::uint32_t width = std::max(
                createInfo.width,
                1u);
            const std::uint32_t height = std::max(
                createInfo.height,
                1u);

            const bool wantDebugLayer =
#if defined(_DEBUG)
                true;
#else
                createInfo.enableDebugLayer;
#endif
            UINT factoryFlags{};
            if (wantDebugLayer)
            {
                Microsoft::WRL::ComPtr<ID3D12Debug> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(
                        IID_PPV_ARGS(debug.GetAddressOf()))))
                {
                    debug->EnableDebugLayer();
                    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                    Logger::Instance().Info(
                        "D3D12のデバッグレイヤーを有効にしました。");
                }
                else
                {
                    Logger::Instance().Warning(
                        "D3D12のデバッグレイヤーを有効にできませんでした。"
                        "Windowsのオプション機能「グラフィックス ツール」"
                        "が必要です。");
                }
            }

            HRESULT factoryResult = CreateDXGIFactory2(
                factoryFlags,
                IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf()));
            if (FAILED(factoryResult) && factoryFlags != 0)
            {
                m_factory.Reset();
                factoryResult = CreateDXGIFactory2(
                    0,
                    IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf()));
                if (SUCCEEDED(factoryResult))
                {
                    Logger::Instance().Warning(
                        "DXGIのデバッグfactoryを作成できなかったため、"
                        "通常factoryでD3D12を起動しました。");
                }
            }
            ThrowIfFailed(factoryResult, "CreateDXGIFactory2");

            const auto createDevice =
                [this](IDXGIAdapter1* const adapter)
            {
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                const HRESULT result = D3D12CreateDevice(
                    adapter,
                    D3D_FEATURE_LEVEL_11_0,
                    IID_PPV_ARGS(device.GetAddressOf()));
                if (SUCCEEDED(result))
                {
                    m_adapter = adapter;
                    m_device = std::move(device);
                    return true;
                }
                return false;
            };

            bool usingWarp = createInfo.preferWarpAdapter;
            if (!createInfo.preferWarpAdapter)
            {
                Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
                static_cast<void>(m_factory.As(&factory6));
                if (factory6)
                {
                    for (UINT index{};; ++index)
                    {
                        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
                        const HRESULT enumerated =
                            factory6->EnumAdapterByGpuPreference(
                                index,
                                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                IID_PPV_ARGS(candidate.GetAddressOf()));
                        if (enumerated == DXGI_ERROR_NOT_FOUND)
                        {
                            break;
                        }
                        if (FAILED(enumerated))
                        {
                            continue;
                        }
                        DXGI_ADAPTER_DESC1 description{};
                        if (FAILED(candidate->GetDesc1(&description))
                            || (description.Flags
                                & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                        {
                            continue;
                        }
                        if (createDevice(candidate.Get()))
                        {
                            break;
                        }
                    }
                }
                else
                {
                    for (UINT index{};; ++index)
                    {
                        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
                        const HRESULT enumerated =
                            m_factory->EnumAdapters1(
                                index,
                                candidate.GetAddressOf());
                        if (enumerated == DXGI_ERROR_NOT_FOUND)
                        {
                            break;
                        }
                        if (FAILED(enumerated))
                        {
                            continue;
                        }
                        DXGI_ADAPTER_DESC1 description{};
                        if (FAILED(candidate->GetDesc1(&description))
                            || (description.Flags
                                & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                        {
                            continue;
                        }
                        if (createDevice(candidate.Get()))
                        {
                            break;
                        }
                    }
                }
            }

            if (m_device == nullptr)
            {
                Microsoft::WRL::ComPtr<IDXGIAdapter> warpBase;
                ThrowIfFailed(
                    m_factory->EnumWarpAdapter(
                        IID_PPV_ARGS(warpBase.GetAddressOf())),
                    "IDXGIFactory4::EnumWarpAdapter");
                Microsoft::WRL::ComPtr<IDXGIAdapter1> warp;
                ThrowIfFailed(
                    warpBase.As(&warp),
                    "IDXGIAdapter::QueryInterface(IDXGIAdapter1)");
                if (!createDevice(warp.Get()))
                {
                    ThrowHResult(
                        DXGI_ERROR_UNSUPPORTED,
                        "D3D12CreateDevice(WARP)");
                }
                usingWarp = true;
            }

            if (usingWarp)
            {
                Logger::Instance().Warning(
                    "D3D12をWARP（CPU描画）で起動しました。"
                    "描画性能は大幅に低下します。");
            }
            DXGI_ADAPTER_DESC1 adapterDescription{};
            if (SUCCEEDED(m_adapter->GetDesc1(&adapterDescription)))
            {
                const auto videoMemoryMegabytes =
                    static_cast<std::uint64_t>(
                        adapterDescription.DedicatedVideoMemory)
                    / (1024u * 1024u);
                Logger::Instance().Info(
                    "D3D12描画アダプター: "
                    + AdapterName(adapterDescription)
                    + "（VRAM "
                    + std::to_string(videoMemoryMegabytes)
                    + " MB）");
            }

            if (wantDebugLayer)
            {
                static_cast<void>(m_device.As(&m_infoQueue));
            }

            D3D12_COMMAND_QUEUE_DESC queueDescription{};
            queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ThrowIfFailed(
                m_device->CreateCommandQueue(
                    &queueDescription,
                    IID_PPV_ARGS(
                        m_commandQueue.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateCommandQueue",
                m_device.Get());

            m_tearingAllowed = QueryTearingSupport(m_factory.Get());
            DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
            swapChainDescription.Width = width;
            swapChainDescription.Height = height;
            swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapChainDescription.SampleDesc.Count = 1;
            swapChainDescription.BufferUsage =
                DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDescription.BufferCount =
                static_cast<UINT>(BackBufferCount);
            swapChainDescription.SwapEffect =
                DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapChainDescription.Flags = m_tearingAllowed
                ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                : 0u;

            Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
            ThrowIfFailed(
                m_factory->CreateSwapChainForHwnd(
                    m_commandQueue.Get(),
                    window,
                    &swapChainDescription,
                    nullptr,
                    nullptr,
                    swapChain.GetAddressOf()),
                "IDXGIFactory4::CreateSwapChainForHwnd",
                m_device.Get());
            static_cast<void>(m_factory->MakeWindowAssociation(
                window,
                DXGI_MWA_NO_ALT_ENTER));
            ThrowIfFailed(
                swapChain.As(&m_swapChain),
                "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)",
                m_device.Get());
            m_currentBackBufferIndex =
                m_swapChain->GetCurrentBackBufferIndex();

            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDescription{};
            rtvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDescription.NumDescriptors =
                static_cast<UINT>(BackBufferCount);
            ThrowIfFailed(
                m_device->CreateDescriptorHeap(
                    &rtvHeapDescription,
                    IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateDescriptorHeap(RTV)",
                m_device.Get());
            m_rtvDescriptorSize =
                m_device->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

            D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
            dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvHeapDescription.NumDescriptors = 1;
            ThrowIfFailed(
                m_device->CreateDescriptorHeap(
                    &dsvHeapDescription,
                    IID_PPV_ARGS(m_dsvHeap.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateDescriptorHeap(DSV)",
                m_device.Get());

            for (auto& allocator : m_commandAllocators)
            {
                ThrowIfFailed(
                    m_device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(
                            allocator.ReleaseAndGetAddressOf())),
                    "ID3D12Device::CreateCommandAllocator",
                    m_device.Get());
            }
            ThrowIfFailed(
                m_device->CreateCommandList(
                    0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    m_commandAllocators[0].Get(),
                    nullptr,
                    IID_PPV_ARGS(
                        m_commandList.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateCommandList",
                m_device.Get());
            ThrowIfFailed(
                m_commandList->Close(),
                "ID3D12GraphicsCommandList::Close",
                m_device.Get());

            ThrowIfFailed(
                m_device->CreateFence(
                    0,
                    D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateFence",
                m_device.Get());
            m_fenceEvent = CreateEventW(
                nullptr,
                FALSE,
                FALSE,
                nullptr);
            if (m_fenceEvent == nullptr)
            {
                throw std::runtime_error(
                    "CreateEventW failed for the D3D12 fence.");
            }

            // Sprite等が参照するtexture viewは1枚のshader-visible heapへ
            // 置き、描画時はdescriptor tableとしてbindします。
            D3D12_DESCRIPTOR_HEAP_DESC shaderResourceHeapDescription{};
            shaderResourceHeapDescription.Type =
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            shaderResourceHeapDescription.NumDescriptors =
                Detail::D3D12ResourceDomain::ShaderResourceCapacity;
            shaderResourceHeapDescription.Flags =
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
                shaderResourceHeap;
            ThrowIfFailed(
                m_device->CreateDescriptorHeap(
                    &shaderResourceHeapDescription,
                    IID_PPV_ARGS(shaderResourceHeap.GetAddressOf())),
                "ID3D12Device::CreateDescriptorHeap(SRV)",
                m_device.Get());
            m_resourceDomain =
                std::make_shared<Detail::D3D12ResourceDomain>(
                    std::move(shaderResourceHeap),
                    m_device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            m_resourceDomain->PublishNextFrameFenceValue(
                m_nextFenceValue);
            CreateUploadContext();

            CreateSizeDependentResources(width, height);
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    void D3D12Backend::PrepareForResourceRelease() noexcept
    {
        if (m_commandQueue == nullptr
            || m_fence == nullptr
            || m_fenceEvent == nullptr)
        {
            return;
        }
        try
        {
            DrainGpu();
        }
        catch (...)
        {
            // Device-lossを含む終了処理の失敗は、後続のCOM解放を妨げません。
            m_commandListOpen = false;
            m_terminalFailure = true;
        }
    }

    void D3D12Backend::Shutdown() noexcept
    {
        PrepareForResourceRelease();
        m_activeShadowMap = nullptr;
        // GPUの完了を待った後で、handleが退避したresource / descriptorを
        // 解放します。以後に破棄されるhandleは即時解放されます。
        if (m_resourceDomain != nullptr)
        {
            m_resourceDomain->Close();
            m_resourceDomain.reset();
        }
        for (auto& arena : m_frameUploadArenas)
        {
            arena.clear();
        }
        ReleaseUploadContext();
        ReleaseSizeDependentResources();
        m_infoQueue.Reset();
        m_commandList.Reset();
        for (auto& allocator : m_commandAllocators)
        {
            allocator.Reset();
        }
        m_dsvHeap.Reset();
        m_rtvHeap.Reset();
        m_swapChain.Reset();
        m_fence.Reset();
        if (m_fenceEvent != nullptr)
        {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        m_commandQueue.Reset();
        m_retainedSubmissionResources.clear();
        m_device.Reset();
        m_adapter.Reset();
        m_factory.Reset();
        m_frameFenceValues = {};
        m_backBufferStates = {
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT };
        m_nextFenceValue = 1;
        m_currentBackBufferIndex = 0;
        m_rtvDescriptorSize = 0;
        m_width = 0;
        m_height = 0;
        m_viewport = {};
        m_scissorRect = {};
        m_tearingAllowed = false;
        m_commandListOpen = false;
        m_terminalFailure = false;
    }

    void D3D12Backend::Resize(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized() || width == 0 || height == 0)
        {
            return;
        }
        if (width == m_width && height == m_height)
        {
            return;
        }

        // Resizeは失敗を握り潰さず、GPUの完了を確認できた場合だけ
        // swap-chain bufferを解放します。
        try
        {
            DrainGpu();
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
        const std::uint32_t previousWidth = m_width;
        const std::uint32_t previousHeight = m_height;
        ReleaseSizeDependentResources();
        const HRESULT resized = m_swapChain->ResizeBuffers(
            static_cast<UINT>(BackBufferCount),
            width,
            height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            m_tearingAllowed
                ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                : 0u);
        if (FAILED(resized))
        {
            try
            {
                CreateSizeDependentResources(
                    previousWidth,
                    previousHeight);
            }
            catch (...)
            {
                Shutdown();
            }
            ThrowHResult(
                resized,
                "IDXGISwapChain3::ResizeBuffers",
                m_device.Get());
        }

        try
        {
            CreateSizeDependentResources(width, height);
        }
        catch (...)
        {
            Shutdown();
            throw;
        }
    }

    void D3D12Backend::BindAndClearBackBuffer(
        const float clearColor[4])
    {
        if (clearColor == nullptr)
        {
            throw std::invalid_argument(
                "BindAndClearBackBuffer requires a clear color.");
        }
        BindBackBuffer();
        const auto rtv = OffsetDescriptor(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            m_currentBackBufferIndex,
            m_rtvDescriptorSize);
        m_commandList->ClearRenderTargetView(
            rtv,
            clearColor,
            0,
            nullptr);
        m_commandList->ClearDepthStencilView(
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
    }

    void D3D12Backend::DrainDebugMessages()
    {
        if (m_infoQueue == nullptr)
        {
            return;
        }
        const auto stored = m_infoQueue->GetNumStoredMessages();
        for (std::uint64_t index{}; index < stored; ++index)
        {
            SIZE_T length{};
            if (FAILED(m_infoQueue->GetMessage(index, nullptr, &length))
                || length == 0)
            {
                continue;
            }
            std::vector<std::byte> storage(length);
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(
                storage.data());
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    message,
                    &length)))
            {
                continue;
            }
            const std::string text(
                message->pDescription,
                message->DescriptionByteLength > 0
                    ? message->DescriptionByteLength - 1
                    : 0);
            switch (message->Severity)
            {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                Logger::Instance().Error("D3D12: " + text);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                Logger::Instance().Warning("D3D12: " + text);
                break;
            default:
                Logger::Instance().Info("D3D12: " + text);
                break;
            }
        }
        m_infoQueue->ClearStoredMessages();
    }

    void D3D12Backend::Present(const bool vSyncEnabled)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "Present requires an initialized D3D12 backend.");
        }

        try
        {
            const std::uint32_t submittedIndex =
                m_currentBackBufferIndex;
            const bool submittedCommands = m_commandListOpen;
            if (submittedCommands)
            {
                TransitionCurrentBackBuffer(
                    D3D12_RESOURCE_STATE_PRESENT);
                CloseAndExecuteOpenCommands();
            }

            const bool immediate = !vSyncEnabled;
            const HRESULT presented = m_swapChain->Present(
                immediate ? 0u : 1u,
                immediate && m_tearingAllowed
                    ? DXGI_PRESENT_ALLOW_TEARING
                    : 0u);
            if (submittedCommands)
            {
                const std::uint64_t value = ReserveFrameFenceValue();
                ThrowIfFailed(
                    m_commandQueue->Signal(m_fence.Get(), value),
                    "ID3D12CommandQueue::Signal",
                    m_device.Get());
                m_frameFenceValues[submittedIndex] = value;
            }
            if (FAILED(presented))
            {
                ThrowHResult(
                    presented,
                    "IDXGISwapChain3::Present",
                    m_device.Get());
            }
            m_currentBackBufferIndex =
                m_swapChain->GetCurrentBackBufferIndex();
            CollectRetiredResources();
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
    }

    std::vector<std::uint8_t> D3D12Backend::CaptureBackBuffer(
        std::uint32_t& width,
        std::uint32_t& height) const
    {
        return const_cast<D3D12Backend*>(this)
            ->CaptureBackBufferImpl(width, height);
    }

    void D3D12Backend::BindBackBuffer()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindBackBuffer requires an initialized D3D12 backend.");
        }
        try
        {
            if (!m_commandListOpen)
            {
                OpenCommandList();
            }
            TransitionCurrentBackBuffer(
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            BindPrimaryOutput();
            m_activeOffscreenTarget = nullptr;
            m_activeOffscreenDepthOnly = false;
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
    }

    GraphicsVideoMemoryStatistics
        D3D12Backend::QueryVideoMemoryStatistics() const noexcept
    {
        GraphicsVideoMemoryStatistics statistics;
        if (m_adapter == nullptr)
        {
            return statistics;
        }
        statistics.adapterAvailable = true;

        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(m_adapter->GetDesc1(&description)))
        {
            statistics.descriptionAvailable = true;
            statistics.dedicatedBytes =
                static_cast<std::uint64_t>(
                    description.DedicatedVideoMemory);
            statistics.sharedSystemBytes =
                static_cast<std::uint64_t>(
                    description.SharedSystemMemory);
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(m_adapter.As(&adapter3)))
        {
            return statistics;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
        statistics.localBudgetAvailable = SUCCEEDED(
            adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &local));
        statistics.nonLocalBudgetAvailable = SUCCEEDED(
            adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                &nonLocal));
        if (statistics.localBudgetAvailable)
        {
            statistics.localUsageBytes = local.CurrentUsage;
            statistics.localBudgetBytes = local.Budget;
        }
        if (statistics.nonLocalBudgetAvailable)
        {
            statistics.nonLocalUsageBytes = nonLocal.CurrentUsage;
            statistics.nonLocalBudgetBytes = nonLocal.Budget;
        }
        return statistics;
    }

    void D3D12Backend::CreateSizeDependentResources(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        for (auto& backBuffer : m_backBuffers)
        {
            backBuffer.Reset();
        }
        m_depthBuffer.Reset();

        const auto rtvStart =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::size_t index{}; index < BackBufferCount; ++index)
        {
            ThrowIfFailed(
                m_swapChain->GetBuffer(
                    static_cast<UINT>(index),
                    IID_PPV_ARGS(
                        m_backBuffers[index].ReleaseAndGetAddressOf())),
                "IDXGISwapChain3::GetBuffer",
                m_device.Get());
            m_device->CreateRenderTargetView(
                m_backBuffers[index].Get(),
                nullptr,
                OffsetDescriptor(
                    rtvStart,
                    index,
                    m_rtvDescriptorSize));
            m_backBufferStates[index] =
                D3D12_RESOURCE_STATE_PRESENT;
        }

        D3D12_RESOURCE_DESC depthDescription{};
        depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.DepthOrArraySize = 1;
        depthDescription.MipLevels = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDescription.Flags =
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthClear.DepthStencil.Depth = 1.0f;
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &depthDescription,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &depthClear,
                IID_PPV_ARGS(
                    m_depthBuffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(depth)",
            m_device.Get());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
        dsvDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(
            m_depthBuffer.Get(),
            &dsvDescription,
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

        m_currentBackBufferIndex =
            m_swapChain->GetCurrentBackBufferIndex();
        m_width = width;
        m_height = height;
        m_viewport = {
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height),
            0.0f,
            1.0f
        };
        m_scissorRect = {
            0,
            0,
            static_cast<LONG>(width),
            static_cast<LONG>(height)
        };
        m_activeViewport = m_viewport;
        m_activeScissorRect = m_scissorRect;
        m_activeOffscreenTarget = nullptr;
        m_activeOffscreenDepthOnly = false;
        m_frameFenceValues = {};
        m_commandListOpen = false;
    }

    void D3D12Backend::ReleaseSizeDependentResources() noexcept
    {
        m_depthBuffer.Reset();
        for (auto& backBuffer : m_backBuffers)
        {
            backBuffer.Reset();
        }
        m_width = 0;
        m_height = 0;
        m_activeViewport = {};
        m_activeScissorRect = {};
        m_activeOffscreenTarget = nullptr;
        m_activeOffscreenDepthOnly = false;
        m_commandListOpen = false;
    }

    void D3D12Backend::OpenCommandList()
    {
        if (m_commandListOpen)
        {
            return;
        }
        m_currentBackBufferIndex =
            m_swapChain->GetCurrentBackBufferIndex();
        WaitForFence(
            m_frameFenceValues[m_currentBackBufferIndex]);
        // このallocatorで記録したframeがGPUで完了した後だけ、同じframeの
        // upload領域を再利用します。
        ResetFrameUploadArena(m_currentBackBufferIndex);
        CollectRetiredResources();
        auto* const allocator =
            m_commandAllocators[m_currentBackBufferIndex].Get();
        ThrowIfFailed(
            allocator->Reset(),
            "ID3D12CommandAllocator::Reset",
            m_device.Get());
        ThrowIfFailed(
            m_commandList->Reset(allocator, nullptr),
            "ID3D12GraphicsCommandList::Reset",
            m_device.Get());
        m_commandListOpen = true;
    }

    void D3D12Backend::TransitionCurrentBackBuffer(
        const D3D12_RESOURCE_STATES state)
    {
        auto& currentState =
            m_backBufferStates[m_currentBackBufferIndex];
        if (currentState == state)
        {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource =
            m_backBuffers[m_currentBackBufferIndex].Get();
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = state;
        m_commandList->ResourceBarrier(1, &barrier);
        currentState = state;
    }

    void D3D12Backend::BindPrimaryOutput()
    {
        const auto rtv = OffsetDescriptor(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            m_currentBackBufferIndex,
            m_rtvDescriptorSize);
        const auto dsv =
            m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_commandList->OMSetRenderTargets(
            1,
            &rtv,
            FALSE,
            &dsv);
        m_commandList->RSSetViewports(1, &m_viewport);
        m_commandList->RSSetScissorRects(1, &m_scissorRect);
        m_activeViewport = m_viewport;
        m_activeScissorRect = m_scissorRect;
    }

    void D3D12Backend::CloseAndExecuteOpenCommands()
    {
        if (!m_commandListOpen)
        {
            return;
        }
        ThrowIfFailed(
            m_commandList->Close(),
            "ID3D12GraphicsCommandList::Close",
            m_device.Get());
        ID3D12CommandList* commandLists[]{ m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, commandLists);
        m_commandListOpen = false;
    }

    std::uint64_t D3D12Backend::SignalCurrentBackBuffer()
    {
        const std::uint64_t value = ReserveFrameFenceValue();
        ThrowIfFailed(
            m_commandQueue->Signal(m_fence.Get(), value),
            "ID3D12CommandQueue::Signal",
            m_device.Get());
        m_frameFenceValues[m_currentBackBufferIndex] = value;
        return value;
    }

    void D3D12Backend::WaitForFence(const std::uint64_t value)
    {
        WaitForFenceCompletion(
            m_device.Get(),
            m_fence.Get(),
            m_fenceEvent,
            value);
    }

    void D3D12Backend::WaitForGpu()
    {
        const std::uint64_t value = ReserveFrameFenceValue();
        ThrowIfFailed(
            m_commandQueue->Signal(m_fence.Get(), value),
            "ID3D12CommandQueue::Signal(wait)",
            m_device.Get());
        WaitForFence(value);
    }

    void D3D12Backend::DrainGpu()
    {
        if (m_commandListOpen)
        {
            TransitionCurrentBackBuffer(
                D3D12_RESOURCE_STATE_PRESENT);
            CloseAndExecuteOpenCommands();
        }
        WaitForGpu();
    }

    std::uint64_t D3D12Backend::ReserveFrameFenceValue() noexcept
    {
        const std::uint64_t value = m_nextFenceValue++;
        // 以後に退避されたresourceは、この後に発行される値の完了まで
        // 保持されます。
        if (m_resourceDomain != nullptr)
        {
            m_resourceDomain->PublishNextFrameFenceValue(
                m_nextFenceValue);
        }
        return value;
    }

    void D3D12Backend::CollectRetiredResources() noexcept
    {
        if (m_resourceDomain == nullptr || m_fence == nullptr)
        {
            return;
        }
        const std::uint64_t completedValue =
            m_fence->GetCompletedValue();
        // device removal中は完了を判定できないため、Shutdownまで保持します。
        if (completedValue
            == std::numeric_limits<std::uint64_t>::max())
        {
            return;
        }
        m_resourceDomain->CollectCompleted(completedValue);
    }

    void D3D12Backend::ResetFrameUploadArena(
        const std::size_t index) noexcept
    {
        for (auto& chunk : m_frameUploadArenas[index])
        {
            chunk.used = 0;
        }
    }

    void D3D12Backend::CreateUploadContext()
    {
        ThrowIfFailed(
            m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(
                    m_uploadCommandAllocator.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandAllocator(upload)",
            m_device.Get());
        ThrowIfFailed(
            m_device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_uploadCommandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(
                    m_uploadCommandList.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandList(upload)",
            m_device.Get());
        ThrowIfFailed(
            m_uploadCommandList->Close(),
            "ID3D12GraphicsCommandList::Close(upload)",
            m_device.Get());
        ThrowIfFailed(
            m_device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(m_uploadFence.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateFence(upload)",
            m_device.Get());
        m_uploadFenceEvent = CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            nullptr);
        if (m_uploadFenceEvent == nullptr)
        {
            throw std::runtime_error(
                "CreateEventW failed for the D3D12 upload fence.");
        }
    }

    void D3D12Backend::ReleaseUploadContext() noexcept
    {
        m_uploadCommandList.Reset();
        m_uploadCommandAllocator.Reset();
        m_uploadFence.Reset();
        if (m_uploadFenceEvent != nullptr)
        {
            CloseHandle(m_uploadFenceEvent);
            m_uploadFenceEvent = nullptr;
        }
        m_nextUploadFenceValue = 1;
        m_retainedUploadResources.clear();
    }

    void D3D12Backend::SubmitTextureUpload(
        const Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
        const std::uint32_t firstMipLevel,
        const std::span<const GraphicsTextureSubresourceData> data,
        const bool updateExistingTexture)
    {
        const auto description = texture->GetDesc();
        const auto subresourceCount = static_cast<UINT>(data.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(
            subresourceCount);
        std::vector<UINT> rowCounts(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        UINT64 totalBytes{};
        m_device->GetCopyableFootprints(
            &description,
            firstMipLevel,
            subresourceCount,
            0,
            footprints.data(),
            rowCounts.data(),
            rowSizes.data(),
            &totalBytes);
        if (totalBytes == 0
            || totalBytes == std::numeric_limits<UINT64>::max())
        {
            throw std::invalid_argument(
                "The D3D12 texture upload has no copyable footprint.");
        }

        const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto stagingDescription = BufferDescription(totalBytes);
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &stagingDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(staging.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(texture upload)",
            m_device.Get());
        void* mapped{};
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            staging->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(texture upload)",
            m_device.Get());
        try
        {
            for (UINT index{}; index < subresourceCount; ++index)
            {
                const auto mipLevel = firstMipLevel + index;
                const auto layout = Detail::RequiredTextureLayout(
                    description.Format,
                    std::max(
                        static_cast<std::uint32_t>(
                            description.Width >> mipLevel),
                        1u),
                    std::max(description.Height >> mipLevel, 1u));
                const auto& footprint = footprints[index];
                if (rowCounts[index] < layout.rowCount
                    || footprint.Footprint.RowPitch
                        < layout.minimumRowBytes)
                {
                    throw std::runtime_error(
                        "The D3D12 texture footprint is smaller than its "
                        "upload layout.");
                }
                // 呼び出し側のrow pitchとD3D12のfootprintは一致しないため、
                // 1行ずつ必要なbyte数だけを詰め直します。
                const auto& subresource = data[index];
                for (std::uint32_t row{}; row < layout.rowCount; ++row)
                {
                    std::memcpy(
                        static_cast<std::byte*>(mapped)
                            + footprint.Offset
                            + static_cast<std::size_t>(row)
                                * footprint.Footprint.RowPitch,
                        subresource.bytes.data()
                            + static_cast<std::size_t>(row)
                                * subresource.rowPitch,
                        layout.minimumRowBytes);
                }
            }
        }
        catch (...)
        {
            staging->Unmap(0, nullptr);
            throw;
        }
        staging->Unmap(0, nullptr);

        std::scoped_lock lock(m_uploadMutex);
        if (m_uploadCommandList == nullptr)
        {
            throw std::logic_error(
                "Texture upload requires an initialized D3D12 backend.");
        }
        bool executed{};
        try
        {
            ThrowIfFailed(
                m_uploadCommandAllocator->Reset(),
                "ID3D12CommandAllocator::Reset(upload)",
                m_device.Get());
            ThrowIfFailed(
                m_uploadCommandList->Reset(
                    m_uploadCommandAllocator.Get(),
                    nullptr),
                "ID3D12GraphicsCommandList::Reset(upload)",
                m_device.Get());
            const auto transition = [this, &texture](
                const UINT subresource,
                const D3D12_RESOURCE_STATES before,
                const D3D12_RESOURCE_STATES after)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = texture.Get();
                barrier.Transition.Subresource = subresource;
                barrier.Transition.StateBefore = before;
                barrier.Transition.StateAfter = after;
                m_uploadCommandList->ResourceBarrier(1, &barrier);
            };
            // 既存textureは描画用にPIXEL_SHADER_RESOURCEへ置いているため、
            // 更新するmipだけを一時的にcopy先へ切り替えます。
            if (updateExistingTexture)
            {
                for (UINT index{}; index < subresourceCount; ++index)
                {
                    transition(
                        firstMipLevel + index,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                }
            }
            for (UINT index{}; index < subresourceCount; ++index)
            {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = texture.Get();
                destination.Type =
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = firstMipLevel + index;
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = staging.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprints[index];
                m_uploadCommandList->CopyTextureRegion(
                    &destination,
                    0,
                    0,
                    0,
                    &source,
                    nullptr);
            }
            if (updateExistingTexture)
            {
                for (UINT index{}; index < subresourceCount; ++index)
                {
                    transition(
                        firstMipLevel + index,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
            }
            else
            {
                transition(
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            ThrowIfFailed(
                m_uploadCommandList->Close(),
                "ID3D12GraphicsCommandList::Close(upload)",
                m_device.Get());
            ID3D12CommandList* commandLists[]{
                m_uploadCommandList.Get() };
            m_commandQueue->ExecuteCommandLists(1, commandLists);
            executed = true;
            const std::uint64_t value = m_nextUploadFenceValue++;
            ThrowIfFailed(
                m_commandQueue->Signal(m_uploadFence.Get(), value),
                "ID3D12CommandQueue::Signal(upload)",
                m_device.Get());
            WaitForFenceCompletion(
                m_device.Get(),
                m_uploadFence.Get(),
                m_uploadFenceEvent,
                value);
        }
        catch (...)
        {
            if (executed)
            {
                // GPUが参照中かもしれない転送元と転送先を、Shutdownまで
                // 保持します。
                try
                {
                    m_retainedUploadResources.push_back(staging);
                    m_retainedUploadResources.push_back(texture);
                }
                catch (...)
                {
                    staging->AddRef();
                    texture->AddRef();
                }
            }
            m_terminalFailure = true;
            throw;
        }
    }

    ID3D12GraphicsCommandList* D3D12Backend::BeginFrameCommands()
    {
        if (!m_commandListOpen)
        {
            if (m_activeOffscreenTarget != nullptr)
            {
                if (m_activeOffscreenDepthOnly)
                {
                    BindOffscreenTargetDepthOnly(
                        *m_activeOffscreenTarget);
                }
                else
                {
                    BindOffscreenTarget(*m_activeOffscreenTarget);
                }
            }
            else
            {
                BindBackBuffer();
            }
        }
        return m_commandList.Get();
    }

    ID3D12GraphicsCommandList* D3D12Backend::CurrentFrameCommands()
    {
        if (!IsInitialized() || !m_commandListOpen)
        {
            throw std::logic_error(
                "CurrentFrameCommands requires an open D3D12 frame "
                "command list.");
        }
        return m_commandList.Get();
    }

    ID3D12DescriptorHeap*
        D3D12Backend::ShaderResourceDescriptorHeap() const noexcept
    {
        return m_resourceDomain != nullptr
            ? m_resourceDomain->ShaderResourceHeap()
            : nullptr;
    }

    std::optional<D3D12Backend::ShaderResourceBinding>
        D3D12Backend::TryResolveShaderResource(
            const GraphicsViewHandle& view) const noexcept
    {
        const auto* const payload = TryShaderResourceViewPayload(
            view,
            m_resourceDomain.get());
        if (payload == nullptr)
        {
            return std::nullopt;
        }
        return ShaderResourceBinding{
            m_resourceDomain->ShaderResourceGpuHandle(payload->slot),
            payload->width,
            payload->height
        };
    }

    D3D12Backend::FrameUploadAllocation D3D12Backend::AllocateFrameUpload(
        const std::uint64_t bytes,
        const std::uint64_t alignment)
    {
        if (!IsInitialized() || !m_commandListOpen)
        {
            throw std::logic_error(
                "AllocateFrameUpload requires an open D3D12 frame "
                "command list.");
        }
        if (bytes == 0
            || alignment == 0
            || (alignment & (alignment - 1u)) != 0)
        {
            throw std::invalid_argument(
                "AllocateFrameUpload requires a non-empty size and a "
                "power-of-two alignment.");
        }

        auto& arena = m_frameUploadArenas[m_currentBackBufferIndex];
        for (auto& chunk : arena)
        {
            const auto offset = AlignUp(chunk.used, alignment);
            if (offset <= chunk.capacity
                && bytes <= chunk.capacity - offset)
            {
                chunk.used = offset + bytes;
                return {
                    chunk.resource.Get(),
                    offset,
                    chunk.resource->GetGPUVirtualAddress() + offset,
                    chunk.data + offset
                };
            }
        }

        constexpr std::uint64_t MinimumChunkBytes = 1024u * 1024u;
        FrameUploadChunk chunk;
        chunk.capacity = std::max(
            AlignUp(bytes, 64u * 1024u),
            MinimumChunkBytes);
        const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = BufferDescription(chunk.capacity);
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(chunk.resource.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(frame upload)",
            m_device.Get());
        void* mapped{};
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            chunk.resource->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(frame upload)",
            m_device.Get());
        chunk.data = static_cast<std::byte*>(mapped);
        chunk.used = bytes;
        arena.push_back(std::move(chunk));
        const auto& added = arena.back();
        return {
            added.resource.Get(),
            0u,
            added.resource->GetGPUVirtualAddress(),
            added.data
        };
    }

    std::vector<std::uint8_t>
        D3D12Backend::CaptureBackBufferImpl(
            std::uint32_t& width,
            std::uint32_t& height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureBackBuffer requires an initialized D3D12 backend.");
        }
        m_currentBackBufferIndex =
            m_swapChain->GetCurrentBackBufferIndex();
        const bool resumeRenderTarget =
            m_commandListOpen
            && m_backBufferStates[m_currentBackBufferIndex]
                == D3D12_RESOURCE_STATE_RENDER_TARGET;
        auto* const backBuffer =
            m_backBuffers[m_currentBackBufferIndex].Get();
        const auto backBufferDescription = backBuffer->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount{};
        UINT64 rowSize{};
        UINT64 totalBytes{};
        m_device->GetCopyableFootprints(
            &backBufferDescription,
            0,
            1,
            0,
            &footprint,
            &rowCount,
            &rowSize,
            &totalBytes);
        if (totalBytes == 0
            || backBufferDescription.Width
                > std::numeric_limits<std::uint32_t>::max()
            || backBufferDescription.Height
                > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "The D3D12 back buffer cannot be copied to CPU memory.");
        }

        const auto readbackHeap =
            HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto readbackDescription =
            BufferDescription(totalBytes);
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &readbackHeap,
                D3D12_HEAP_FLAG_NONE,
                &readbackDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(readback.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(readback)",
            m_device.Get());
        m_retainedSubmissionResources.emplace_back(readback);

        try
        {
            if (!m_commandListOpen)
            {
                OpenCommandList();
            }
            TransitionCurrentBackBuffer(
                D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type =
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = backBuffer;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;
            m_commandList->CopyTextureRegion(
                &destination,
                0,
                0,
                0,
                &source,
                nullptr);
            TransitionCurrentBackBuffer(
                resumeRenderTarget
                    ? D3D12_RESOURCE_STATE_RENDER_TARGET
                    : D3D12_RESOURCE_STATE_PRESENT);
            CloseAndExecuteOpenCommands();
            const auto fenceValue = SignalCurrentBackBuffer();
            WaitForFence(fenceValue);

            const auto capturedWidth = static_cast<std::uint32_t>(
                backBufferDescription.Width);
            const auto capturedHeight = backBufferDescription.Height;
            const std::size_t rowBytes =
                static_cast<std::size_t>(capturedWidth) * 4u;
            if (footprint.Footprint.RowPitch < rowBytes
                || capturedHeight > 0
                    && rowBytes
                        > std::numeric_limits<std::size_t>::max()
                            / capturedHeight)
            {
                throw std::runtime_error(
                    "The D3D12 back-buffer footprint is invalid.");
            }
            std::vector<std::uint8_t> pixels(
                rowBytes * capturedHeight);
            void* mapped{};
            const D3D12_RANGE readRange{ 0, totalBytes };
            ThrowIfFailed(
                readback->Map(0, &readRange, &mapped),
                "ID3D12Resource::Map(readback)",
                m_device.Get());
            for (std::uint32_t row{}; row < capturedHeight; ++row)
            {
                std::memcpy(
                    pixels.data()
                        + static_cast<std::size_t>(row) * rowBytes,
                    static_cast<const std::uint8_t*>(mapped)
                        + footprint.Offset
                        + static_cast<std::size_t>(row)
                            * footprint.Footprint.RowPitch,
                    rowBytes);
            }
            const D3D12_RANGE writtenRange{};
            readback->Unmap(0, &writtenRange);

            if (resumeRenderTarget)
            {
                OpenCommandList();
                BindPrimaryOutput();
            }
            m_retainedSubmissionResources.pop_back();
            width = capturedWidth;
            height = capturedHeight;
            return pixels;
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
    }

    void D3D12Backend::ThrowUnsupported(
        const char* const operation) const
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                std::string(operation)
                + " requires an initialized D3D12 backend.");
        }
        throw std::logic_error(
            std::string(operation)
            + " is not implemented for DirectX 12 Experimental.");
    }

    void D3D12Backend::ResizeOffscreenTarget(
        RenderTarget& target,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "ResizeOffscreenTarget requires an initialized D3D12 "
                "backend.");
        }
        const auto requestedWidth = std::max(width, 1u);
        const auto requestedHeight = std::max(height, 1u);
        if (requestedWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || requestedHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            throw std::invalid_argument(
                "The offscreen target dimensions exceed the DirectX 12 "
                "limit.");
        }
        const bool computeWritable =
            Detail::RenderTargetBackendAccess::ComputeWritable(target);
        const auto* const existing = dynamic_cast<
            const D3D12RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target));
        if (existing != nullptr
            && existing->HasNativeResources()
            && existing->resourceDomain.get() == m_resourceDomain.get()
            && existing->m_width == requestedWidth
            && existing->m_height == requestedHeight
            && existing->computeWritable == computeWritable
            && IsViewCurrent(existing->m_currentColorView)
            && IsViewCurrent(existing->m_postColorView)
            && IsViewCurrent(existing->m_displayView)
            && IsViewCurrent(existing->m_depthView))
        {
            return;
        }
        if (m_activeOffscreenTarget == &target)
        {
            BindBackBuffer();
        }

        auto pending = std::make_unique<D3D12RenderTargetState>();
        pending->resourceDomain = m_resourceDomain;
        pending->m_width = requestedWidth;
        pending->m_height = requestedHeight;
        pending->computeWritable = computeWritable;

        D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription{};
        renderTargetHeapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        renderTargetHeapDescription.NumDescriptors = 2u;
        ThrowIfFailed(
            m_device->CreateDescriptorHeap(
                &renderTargetHeapDescription,
                IID_PPV_ARGS(
                    pending->renderTargetHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(offscreen RTV)",
            m_device.Get());
        pending->renderTargetDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
        depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        depthHeapDescription.NumDescriptors = 1u;
        ThrowIfFailed(
            m_device->CreateDescriptorHeap(
                &depthHeapDescription,
                IID_PPV_ARGS(
                    pending->depthStencilHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(offscreen DSV)",
            m_device.Get());

        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC colorDescription{};
        colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        colorDescription.Width = requestedWidth;
        colorDescription.Height = requestedHeight;
        colorDescription.DepthOrArraySize = 1u;
        colorDescription.MipLevels = 1u;
        // 現在のD3D12 Sprite / Primitive PSOと同じ形式にし、
        // まずLDRのoffscreen描画を有効にします。HDR化は
        // PSOのRTV format切り替えとトーンマップ移植と一緒に行います。
        colorDescription.Format = PrimaryColorFormat;
        colorDescription.SampleDesc.Count = 1u;
        colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        colorDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = colorDescription.Format;

        const auto createColor =
            [&](Microsoft::WRL::ComPtr<ID3D12Resource>& destination,
                const char* const operation,
                const D3D12_RESOURCE_FLAGS flags)
        {
            auto description = colorDescription;
            description.Flags = flags;
            ThrowIfFailed(
                m_device->CreateCommittedResource(
                    &defaultHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &description,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0
                        ? &colorClear
                        : nullptr,
                    IID_PPV_ARGS(destination.ReleaseAndGetAddressOf())),
                operation,
                m_device.Get());
        };
        createColor(
            pending->color,
            "ID3D12Device::CreateCommittedResource(offscreen color)",
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        createColor(
            pending->postColor,
            "ID3D12Device::CreateCommittedResource(offscreen post color)",
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        createColor(
            pending->displayColor,
            "ID3D12Device::CreateCommittedResource(offscreen display)",
            computeWritable
                ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                : D3D12_RESOURCE_FLAG_NONE);

        const auto renderTargetStart = pending->renderTargetHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateRenderTargetView(
            pending->color.Get(),
            nullptr,
            renderTargetStart);
        m_device->CreateRenderTargetView(
            pending->postColor.Get(),
            nullptr,
            OffsetDescriptor(
                renderTargetStart,
                1u,
                pending->renderTargetDescriptorSize));

        D3D12_RESOURCE_DESC depthDescription{};
        depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDescription.Width = requestedWidth;
        depthDescription.Height = requestedHeight;
        depthDescription.DepthOrArraySize = 1u;
        depthDescription.MipLevels = 1u;
        depthDescription.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depthDescription.SampleDesc.Count = 1u;
        depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = PrimaryDepthFormat;
        depthClear.DepthStencil.Depth = 1.0f;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &depthDescription,
                pending->depthState,
                &depthClear,
                IID_PPV_ARGS(pending->depth.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(offscreen depth)",
            m_device.Get());
        auto depthCopyDescription = depthDescription;
        depthCopyDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &depthCopyDescription,
                pending->depthCopyState,
                nullptr,
                IID_PPV_ARGS(
                    pending->depthCopy.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(offscreen depth copy)",
            m_device.Get());
        D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format = PrimaryDepthFormat;
        depthViewDescription.ViewDimension =
            D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(
            pending->depth.Get(),
            &depthViewDescription,
            pending->depthStencilHeap
                ->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC colorViewDescription{};
        colorViewDescription.Format = colorDescription.Format;
        colorViewDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        colorViewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        colorViewDescription.Texture2D.MipLevels = 1u;
        pending->m_currentColorView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->color,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba8Unorm);
        pending->m_postColorView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->postColor,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba8Unorm);
        pending->m_displayView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->displayColor,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba8Unorm);

        D3D12_SHADER_RESOURCE_VIEW_DESC depthResourceDescription{};
        depthResourceDescription.Format =
            DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthResourceDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        depthResourceDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthResourceDescription.Texture2D.MipLevels = 1u;
        pending->m_depthView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->depthCopy,
            depthResourceDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba8Unorm);
        pending->viewport = {
            0.0f,
            0.0f,
            static_cast<float>(requestedWidth),
            static_cast<float>(requestedHeight),
            0.0f,
            1.0f };
        pending->scissor = {
            0,
            0,
            static_cast<LONG>(requestedWidth),
            static_cast<LONG>(requestedHeight) };
        pending->m_initialized = true;
        Detail::RenderTargetBackendAccess::Publish(
            target,
            std::move(pending));
    }

    void D3D12Backend::BeginOffscreenTarget(
        RenderTarget& target,
        const float clearColor[4])
    {
        if (clearColor == nullptr)
        {
            throw std::invalid_argument(
                "BeginOffscreenTarget requires a clear color.");
        }
        BindOffscreenTarget(target);
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        const auto renderTarget = state->renderTargetHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_commandList->ClearRenderTargetView(
            renderTarget,
            clearColor,
            0,
            nullptr);
        m_commandList->ClearDepthStencilView(
            state->depthStencilHeap
                ->GetCPUDescriptorHandleForHeapStart(),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0u,
            0u,
            nullptr);
    }

    void D3D12Backend::BindOffscreenTarget(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTarget requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_currentColorView)
            || !IsViewCurrent(state->m_depthView))
        {
            throw std::invalid_argument(
                "BindOffscreenTarget requires a target owned by this "
                "D3D12 backend generation.");
        }
        OpenCommandList();
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        TransitionResource(
            m_commandList.Get(),
            state->depth.Get(),
            state->depthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const auto renderTarget = state->renderTargetHeap
            ->GetCPUDescriptorHandleForHeapStart();
        const auto depthTarget = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            &depthTarget);
        m_commandList->RSSetViewports(1, &state->viewport);
        m_commandList->RSSetScissorRects(1, &state->scissor);
        m_activeViewport = state->viewport;
        m_activeScissorRect = state->scissor;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
    }

    void D3D12Backend::PublishOffscreenTarget(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "PublishOffscreenTarget requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_displayView))
        {
            throw std::invalid_argument(
                "PublishOffscreenTarget requires a target owned by this "
                "D3D12 backend generation.");
        }
        OpenCommandList();
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->displayColor.Get(),
            state->displayColorState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(
            state->displayColor.Get(),
            state->color.Get());
        TransitionResource(
            m_commandList.Get(),
            state->displayColor.Get(),
            state->displayColorState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            m_activeOffscreenTarget == &target
                ? D3D12_RESOURCE_STATE_RENDER_TARGET
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void D3D12Backend::BindOffscreenTargetDepthOnly(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BindOffscreenTargetDepthOnly requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "BindOffscreenTargetDepthOnly requires a target owned by "
                "this D3D12 backend generation.");
        }
        OpenCommandList();
        TransitionResource(
            m_commandList.Get(),
            state->depth.Get(),
            state->depthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const auto depthTarget = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            &depthTarget);
        m_commandList->RSSetViewports(1, &state->viewport);
        m_commandList->RSSetScissorRects(1, &state->scissor);
        m_activeViewport = state->viewport;
        m_activeScissorRect = state->scissor;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = true;
    }

    void D3D12Backend::CaptureOffscreenTargetDepth(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetDepth requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_depthView))
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetDepth requires a target owned by "
                "this D3D12 backend generation.");
        }
        OpenCommandList();
        const bool restoreTarget = m_activeOffscreenTarget == &target;
        if (restoreTarget)
        {
            m_commandList->OMSetRenderTargets(
                0,
                nullptr,
                FALSE,
                nullptr);
        }
        TransitionResource(
            m_commandList.Get(),
            state->depth.Get(),
            state->depthState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->depthCopy.Get(),
            state->depthCopyState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(
            state->depthCopy.Get(),
            state->depth.Get());
        TransitionResource(
            m_commandList.Get(),
            state->depthCopy.Get(),
            state->depthCopyState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->depth.Get(),
            state->depthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (restoreTarget)
        {
            if (m_activeOffscreenDepthOnly)
            {
                BindOffscreenTargetDepthOnly(target);
            }
            else
            {
                BindOffscreenTarget(target);
            }
        }
    }

    void D3D12Backend::CaptureOffscreenTargetColorHistory(
        RenderTarget&,
        const DirectX::XMFLOAT4X4&)
    {
        ThrowUnsupported("CaptureOffscreenTargetColorHistory");
    }

    void D3D12Backend::CaptureOffscreenTargetTemporalHistory(
        RenderTarget&,
        const DirectX::XMFLOAT4X4&)
    {
        ThrowUnsupported("CaptureOffscreenTargetTemporalHistory");
    }

    std::optional<float>
        D3D12Backend::TryReadOffscreenTargetLuminance(RenderTarget&)
    {
        ThrowUnsupported("TryReadOffscreenTargetLuminance");
    }

    void D3D12Backend::CaptureOffscreenTargetLuminance(RenderTarget&)
    {
        ThrowUnsupported("CaptureOffscreenTargetLuminance");
    }

    void D3D12Backend::InitializeShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t resolution,
        const std::uint32_t cascadeCount,
        const bool cube)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "InitializeShadowMap requires an initialized backend.");
        }
        if (resolution > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            throw std::invalid_argument(
                "ShadowMap resolution exceeds the DirectX 12 limit.");
        }

        auto* const previousState =
            Detail::ShadowMapBackendAccess::Get(shadowMap);
        auto* const previous = dynamic_cast<D3D12ShadowMapState*>(
            previousState);
        const bool restorePrevious = previousState != nullptr
            && previousState->m_rendering;
        if (restorePrevious
            && (previous == nullptr
                || m_activeShadowMap != previous
                || Detail::GraphicsResourceHandleAccess::Domain(
                    previous->m_view) != m_resourceDomain.get()))
        {
            throw std::logic_error(
                "Cannot replace a shadow map while another graphics API "
                "is rendering it.");
        }

        auto state = std::make_unique<D3D12ShadowMapState>();
        state->m_resolution = std::max(resolution, 1u);
        state->m_cascadeCount = cube
            ? 6u
            : std::clamp(cascadeCount, 1u, 4u);
        state->cube = cube;

        D3D12_RESOURCE_DESC textureDescription{};
        textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDescription.Width = state->m_resolution;
        textureDescription.Height = state->m_resolution;
        textureDescription.DepthOrArraySize = static_cast<UINT16>(
            state->m_cascadeCount);
        textureDescription.MipLevels = 1;
        textureDescription.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDescription.Flags =
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = ShadowDepthFormat;
        clearValue.DepthStencil.Depth = 1.0f;
        const auto heapProperties =
            HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &textureDescription,
                state->resourceState,
                &clearValue,
                IID_PPV_ARGS(state->texture.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(shadow map)",
            m_device.Get());

        D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
        depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        depthHeapDescription.NumDescriptors = state->m_cascadeCount;
        ThrowIfFailed(
            m_device->CreateDescriptorHeap(
                &depthHeapDescription,
                IID_PPV_ARGS(
                    state->depthStencilHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(shadow DSV)",
            m_device.Get());
        state->descriptorSize =
            m_device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format = ShadowDepthFormat;
        depthViewDescription.ViewDimension =
            D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        depthViewDescription.Texture2DArray.ArraySize = 1;
        auto depthHandle = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index{};
            index < state->m_cascadeCount;
            ++index)
        {
            depthViewDescription.Texture2DArray.FirstArraySlice = index;
            m_device->CreateDepthStencilView(
                state->texture.Get(),
                &depthViewDescription,
                depthHandle);
            depthHandle = OffsetDescriptor(
                depthHandle,
                1u,
                state->descriptorSize);
        }

        const auto descriptorSlot =
            m_resourceDomain->AllocateShaderResourceSlot();
        try
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC resourceViewDescription{};
            resourceViewDescription.Format = DXGI_FORMAT_R32_FLOAT;
            resourceViewDescription.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (cube)
            {
                resourceViewDescription.ViewDimension =
                    D3D12_SRV_DIMENSION_TEXTURECUBE;
                resourceViewDescription.TextureCube.MipLevels = 1;
            }
            else
            {
                resourceViewDescription.ViewDimension =
                    D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                resourceViewDescription.Texture2DArray.MipLevels = 1;
                resourceViewDescription.Texture2DArray.ArraySize =
                    state->m_cascadeCount;
            }
            m_device->CreateShaderResourceView(
                state->texture.Get(),
                &resourceViewDescription,
                m_resourceDomain->ShaderResourceCpuHandle(
                    descriptorSlot));
            auto texture =
                Detail::GraphicsResourceHandleAccess::MakeTexture(
                    std::make_shared<D3D12ShadowTexturePayload>(
                        m_resourceDomain,
                        state->texture));
            state->m_view =
                Detail::GraphicsResourceHandleAccess::MakeView(
                    std::make_shared<
                        D3D12ShaderResourceViewPayload>(
                            m_resourceDomain,
                            std::move(texture),
                            descriptorSlot,
                            state->m_resolution,
                            state->m_resolution));
        }
        catch (...)
        {
            m_resourceDomain->ReleaseUnpublishedShaderResourceSlot(
                descriptorSlot);
            throw;
        }
        state->m_initialized = true;

        if (restorePrevious)
        {
            EndShadowMap(shadowMap);
        }
        Detail::ShadowMapBackendAccess::Publish(
            shadowMap,
            std::move(state));
    }

    void D3D12Backend::BeginShadowMap(
        ShadowMap& shadowMap,
        const std::uint32_t cascadeIndex)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginShadowMap requires an initialized backend.");
        }
        auto* const state = dynamic_cast<D3D12ShadowMapState*>(
            Detail::ShadowMapBackendAccess::Get(shadowMap));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->m_rendering
            || m_activeShadowMap != nullptr
            || cascadeIndex >= state->m_cascadeCount
            || Detail::GraphicsResourceHandleAccess::Domain(
                state->m_view) != m_resourceDomain.get())
        {
            return;
        }

        try
        {
            OpenCommandList();
            if (state->resourceState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = state->texture.Get();
                barrier.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = state->resourceState;
                barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_DEPTH_WRITE;
                m_commandList->ResourceBarrier(1, &barrier);
                state->resourceState =
                    D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            const auto depthView = OffsetDescriptor(
                state->depthStencilHeap
                    ->GetCPUDescriptorHandleForHeapStart(),
                cascadeIndex,
                state->descriptorSize);
            m_commandList->OMSetRenderTargets(
                0,
                nullptr,
                FALSE,
                &depthView);
            const D3D12_VIEWPORT viewport{
                0.0f,
                0.0f,
                static_cast<float>(state->m_resolution),
                static_cast<float>(state->m_resolution),
                0.0f,
                1.0f };
            const D3D12_RECT scissor{
                0,
                0,
                static_cast<LONG>(state->m_resolution),
                static_cast<LONG>(state->m_resolution) };
            m_commandList->RSSetViewports(1, &viewport);
            m_commandList->RSSetScissorRects(1, &scissor);
            m_commandList->ClearDepthStencilView(
                depthView,
                D3D12_CLEAR_FLAG_DEPTH,
                1.0f,
                0,
                0,
                nullptr);
            state->m_rendering = true;
            m_activeShadowMap = state;
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
    }

    void D3D12Backend::EndShadowMap(ShadowMap& shadowMap)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndShadowMap requires an initialized backend.");
        }
        auto* const state = dynamic_cast<D3D12ShadowMapState*>(
            Detail::ShadowMapBackendAccess::Get(shadowMap));
        if (state == nullptr
            || !state->HasNativeResources()
            || !state->m_rendering
            || m_activeShadowMap != state
            || Detail::GraphicsResourceHandleAccess::Domain(
                state->m_view) != m_resourceDomain.get())
        {
            return;
        }

        if (state->resourceState
            != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = state->texture.Get();
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state->resourceState;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &barrier);
            state->resourceState =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        state->m_rendering = false;
        m_activeShadowMap = nullptr;
        if (m_activeOffscreenTarget != nullptr)
        {
            if (m_activeOffscreenDepthOnly)
            {
                BindOffscreenTargetDepthOnly(
                    *m_activeOffscreenTarget);
            }
            else
            {
                BindOffscreenTarget(*m_activeOffscreenTarget);
            }
        }
        else
        {
            TransitionCurrentBackBuffer(
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            BindPrimaryOutput();
        }
    }

    void D3D12Backend::UpdateClusteredLights(
        ClusteredLights&,
        LightingState&,
        const DirectX::XMFLOAT4X4&,
        const DirectX::XMFLOAT4X4&,
        std::uint32_t,
        std::uint32_t)
    {
        ThrowUnsupported("UpdateClusteredLights");
    }

    std::unique_ptr<GraphicsOutputState>
        D3D12Backend::CaptureOutputState()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOutputState requires an initialized D3D12 "
                "backend.");
        }
        if (m_activeShadowMap != nullptr)
        {
            throw std::logic_error(
                "CaptureOutputState cannot capture an active shadow "
                "pass.");
        }
        auto state = std::make_unique<D3D12OutputState>();
        state->resourceDomain = m_resourceDomain;
        state->offscreenTarget = m_activeOffscreenTarget;
        state->depthOnly = m_activeOffscreenDepthOnly;
        return state;
    }

    void D3D12Backend::RestoreOutputState(
        const GraphicsOutputState& state)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "RestoreOutputState requires an initialized D3D12 "
                "backend.");
        }
        if (m_activeShadowMap != nullptr)
        {
            throw std::logic_error(
                "RestoreOutputState cannot interrupt an active shadow "
                "pass.");
        }
        const auto* const captured = dynamic_cast<
            const D3D12OutputState*>(&state);
        if (captured == nullptr
            || captured->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "RestoreOutputState requires a state captured by this "
                "D3D12 backend generation.");
        }
        if (captured->offscreenTarget == nullptr)
        {
            BindBackBuffer();
        }
        else if (captured->depthOnly)
        {
            BindOffscreenTargetDepthOnly(
                *captured->offscreenTarget);
        }
        else
        {
            BindOffscreenTarget(*captured->offscreenTarget);
        }
    }

    std::unique_ptr<DebugDrawingBackend>
        D3D12Backend::CreateDebugDrawingBackend()
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateDebugDrawingBackend requires an initialized "
                "D3D12 backend.");
        }
        return std::make_unique<D3D12BootstrapDebugDrawingBackend>();
    }

    GraphicsTextureHandle D3D12Backend::CreateSolidRgba8Texture(
        const std::array<std::uint8_t, 4>& color)
    {
        const GraphicsTexture2DDescription description{
            1,
            1,
            1,
            GraphicsTextureFormat::Rgba8Unorm
        };
        const std::array initialData{
            GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ color }),
                static_cast<std::uint32_t>(color.size()),
                static_cast<std::uint32_t>(color.size())
            }
        };
        return CreateTexture2D(description, initialData);
    }

    GraphicsViewHandle D3D12Backend::CreateShaderResourceView(
        const GraphicsTextureHandle& texture)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized D3D12 "
                "backend.");
        }
        const auto* const payload = TryTexturePayload(
            texture,
            m_resourceDomain.get());
        if (payload == nullptr)
        {
            throw std::invalid_argument(
                "CreateShaderResourceView requires a texture from this "
                "backend generation.");
        }
        return CreateShaderResourceView(
            texture,
            GraphicsTextureViewDescription{
                0,
                payload->description.mipLevels
            });
    }

    bool D3D12Backend::UpdateDynamicVertexBuffer(
        GraphicsBufferHandle&,
        std::span<const std::byte>)
    {
        ThrowUnsupported("UpdateDynamicVertexBuffer");
    }

    GraphicsTextureHandle D3D12Backend::CreateTexture2D(
        const GraphicsTexture2DDescription& description,
        const std::span<const GraphicsTextureSubresourceData>
            initialData)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateTexture2D requires an initialized D3D12 backend.");
        }
        if (description.width == 0
            || description.height == 0
            || description.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || description.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || description.mipLevels == 0
            || description.mipLevels > Detail::MaximumTextureMipLevels(
                description.width,
                description.height)
            || (description.updateMode
                    != GraphicsTextureUpdateMode::Immutable
                && description.updateMode
                    != GraphicsTextureUpdateMode::PerMipUpdate)
            || (description.updateMode
                    == GraphicsTextureUpdateMode::Immutable
                && initialData.empty())
            || (!initialData.empty()
                && initialData.size() != description.mipLevels))
        {
            throw std::invalid_argument(
                "CreateTexture2D received an invalid description or "
                "subresource count.");
        }
        const auto format = Detail::ToDxgiTextureFormat(
            description.format);
        for (std::size_t mipLevel{};
            mipLevel < initialData.size();
            ++mipLevel)
        {
            Detail::ValidateTexture2DSubresourceData(
                format,
                description.width,
                description.height,
                description.mipLevels,
                static_cast<std::uint32_t>(mipLevel),
                initialData[mipLevel]);
        }

        D3D12_RESOURCE_DESC nativeDescription{};
        nativeDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        nativeDescription.Width = description.width;
        nativeDescription.Height = description.height;
        nativeDescription.DepthOrArraySize = 1;
        nativeDescription.MipLevels =
            static_cast<UINT16>(description.mipLevels);
        nativeDescription.Format = format;
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        nativeDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

        // 初期dataの無いPerMipUpdate textureは、UpdateTexture2Dが前提と
        // する描画用stateで作ります。dataがある場合はcopy先で作り、転送
        // 完了時に同じstateへ移します。
        const auto domain = m_resourceDomain;
        const bool uploadInitialData = !initialData.empty();
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &nativeDescription,
                uploadInitialData
                    ? D3D12_RESOURCE_STATE_COPY_DEST
                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(texture.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(texture)",
            m_device.Get());
        if (uploadInitialData)
        {
            SubmitTextureUpload(
                texture,
                0u,
                initialData,
                false);
        }
        return Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D12TexturePayload>(
                domain,
                std::move(texture),
                description,
                format));
    }

    void D3D12Backend::UpdateTexture2D(
        const GraphicsTextureHandle& texture,
        const std::uint32_t mipLevel,
        const GraphicsTextureSubresourceData& data)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "UpdateTexture2D requires an initialized D3D12 backend.");
        }
        const auto* const payload = TryTexturePayload(
            texture,
            m_resourceDomain.get());
        if (payload == nullptr
            || payload->description.updateMode
                != GraphicsTextureUpdateMode::PerMipUpdate)
        {
            throw std::invalid_argument(
                "UpdateTexture2D requires valid data and a texture from "
                "this backend generation.");
        }
        Detail::ValidateTexture2DSubresourceData(
            payload->format,
            payload->description.width,
            payload->description.height,
            payload->description.mipLevels,
            mipLevel,
            data);
        SubmitTextureUpload(
            payload->native,
            mipLevel,
            std::span{ &data, 1u },
            true);
    }

    GraphicsViewHandle D3D12Backend::CreateShaderResourceView(
        const GraphicsTextureHandle& texture,
        const GraphicsTextureViewDescription& description)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateShaderResourceView requires an initialized D3D12 "
                "backend.");
        }
        const auto* const payload = TryTexturePayload(
            texture,
            m_resourceDomain.get());
        const auto availableMipLevels = payload != nullptr
            ? payload->description.mipLevels
            : 0u;
        if (availableMipLevels == 0
            || description.mipLevels == 0
            || description.mostDetailedMip >= availableMipLevels
            || description.mipLevels
                > availableMipLevels - description.mostDetailedMip)
        {
            throw std::invalid_argument(
                "CreateShaderResourceView requires a valid mip range and "
                "a texture from this backend generation.");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC nativeDescription{};
        nativeDescription.Format = payload->format;
        nativeDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nativeDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nativeDescription.Texture2D.MostDetailedMip =
            description.mostDetailedMip;
        nativeDescription.Texture2D.MipLevels = description.mipLevels;

        const auto domain = m_resourceDomain;
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            m_device->CreateShaderResourceView(
                payload->native.Get(),
                &nativeDescription,
                domain->ShaderResourceCpuHandle(slot));
            return Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    texture,
                    slot,
                    payload->description.width,
                    payload->description.height));
        }
        catch (...)
        {
            domain->ReleaseUnpublishedShaderResourceSlot(slot);
            throw;
        }
    }

    GraphicsViewHandle D3D12Backend::CreateOffscreenDisplayView(
        const RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateOffscreenDisplayView requires an initialized "
                "D3D12 backend.");
        }
        const auto* const state = dynamic_cast<
            const D3D12RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target));
        const auto displayView = target.DisplayViewHandle();
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !displayView
            || !IsViewCurrent(displayView))
        {
            throw std::invalid_argument(
                "CreateOffscreenDisplayView requires a valid offscreen "
                "target display view.");
        }
        return displayView;
    }

    void D3D12Backend::BindVertexBuffer(
        const GraphicsBufferHandle&,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t)
    {
        ThrowUnsupported("BindVertexBuffer");
    }

    bool D3D12Backend::TryBindPixelShaderResources(
        std::uint32_t,
        std::span<const GraphicsViewHandle>,
        const GraphicsViewHandle&) noexcept
    {
        return false;
    }

    GraphicsTextureHandle D3D12Backend::CreateTexture3D(
        const GraphicsTexture3DDescription&,
        std::span<const GraphicsTextureSubresourceData>)
    {
        ThrowUnsupported("CreateTexture3D");
    }

    bool D3D12Backend::IsViewCurrent(
        const GraphicsViewHandle& view) const noexcept
    {
        return TryShaderResourceViewPayload(
            view,
            m_resourceDomain.get()) != nullptr;
    }

    void D3D12Backend::InitializeClusteredLights(
        ClusteredLights&,
        AssetManager&,
        const std::filesystem::path&)
    {
        ThrowUnsupported("InitializeClusteredLights");
    }
}
