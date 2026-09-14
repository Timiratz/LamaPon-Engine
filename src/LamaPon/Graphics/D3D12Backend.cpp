#include "LamaPon/Graphics/D3D12Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/DxgiTextureLayout.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/RenderTargetBackendState.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <DirectXPackedVector.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

    // ベイクした間接光などが読む、初期dataから作る変更不可のTexture3Dです。
    class D3D12Texture3DPayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D12Texture3DPayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> texture,
            const LamaPon::GraphicsTexture3DDescription&
                textureDescription,
            const DXGI_FORMAT textureFormat)
            : GraphicsTexturePayload(resourceDomain)
            , native(std::move(texture))
            , description(textureDescription)
            , format(textureFormat)
        {
        }

        ~D3D12Texture3DPayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        LamaPon::GraphicsTexture3DDescription description;
        DXGI_FORMAT format{};
    };

    // Shadow textureやDDS cubeは通常の2D texture契約では表せないarray/cubeです。
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

    // IBLの事前畳み込み先です。Compute Shaderが書くミップごとのUAV slotを
    // textureと同じ寿命で持ち、GPUの完了後に戻します。
    class D3D12ComputeCubeTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        D3D12ComputeCubeTexturePayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> texture,
            std::vector<std::uint32_t> accessSlots)
            : GraphicsTexturePayload(resourceDomain)
            , native(std::move(texture))
            , unorderedAccessSlots(std::move(accessSlots))
        {
        }

        ~D3D12ComputeCubeTexturePayload() noexcept override
        {
            auto& domain = static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain());
            for (const auto slot : unorderedAccessSlots)
            {
                domain.Retire(nullptr, slot);
            }
            domain.Retire(std::move(native), std::nullopt);
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        std::vector<std::uint32_t> unorderedAccessSlots;
    };

    // 共通Buffer handleが所有するDirectX 12側のbufferです。
    class D3D12BufferPayload final
        : public LamaPon::Detail::GraphicsBufferPayload
    {
    public:
        D3D12BufferPayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer)
            : GraphicsBufferPayload(resourceDomain)
            , native(std::move(buffer))
        {
        }

        ~D3D12BufferPayload() noexcept override
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
            const std::uint32_t textureHeight,
            const D3D12_SRV_DIMENSION viewDimension =
                D3D12_SRV_DIMENSION_TEXTURE2D)
            : GraphicsViewPayload(
                resourceDomain,
                LamaPon::GraphicsViewKind::ShaderResource,
                std::move(texture))
            , slot(descriptorSlot)
            , width(textureWidth)
            , height(textureHeight)
            , dimension(viewDimension)
        {
        }

        // StructuredBufferのSRVです。widthへ要素数を載せます。
        D3D12ShaderResourceViewPayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            LamaPon::GraphicsBufferHandle buffer,
            const std::uint32_t descriptorSlot,
            const std::uint32_t elementCount)
            : GraphicsViewPayload(
                resourceDomain,
                LamaPon::GraphicsViewKind::ShaderResource,
                std::move(buffer))
            , slot(descriptorSlot)
            , width(elementCount)
            , height(1u)
            , dimension(D3D12_SRV_DIMENSION_BUFFER)
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
        D3D12_SRV_DIMENSION dimension{ D3D12_SRV_DIMENSION_TEXTURE2D };
    };

    // LamaPonLightCulling.hlslのClusterCullingBuffer（b0）と同じ112 bytesです。
    struct ClusterCullingConstants final
    {
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4 gridParameters{};
        DirectX::XMFLOAT4 depthParameters{};
        DirectX::XMFLOAT4 frustumParameters{};
    };

    // D3D11ClusteredLightsStateと同じクラスタライトカリングの資源です。
    // ライト一覧はframe uploadからDEFAULT bufferへ写し、番号表と数は
    // Compute ShaderがUAVへ書いてからpixel shaderが読みます。
    struct D3D12ClusteredLightsState final
        : LamaPon::Detail::ClusteredLightsBackendState
    {
        ~D3D12ClusteredLightsState() noexcept override
        {
            if (resourceDomain == nullptr)
            {
                return;
            }
            for (const auto& slot : { indexListAccessSlot, countAccessSlot })
            {
                if (slot.has_value())
                {
                    resourceDomain->Retire(nullptr, *slot);
                }
            }
        }

        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && rootSignature != nullptr
                && pipeline != nullptr
                && lightBuffer != nullptr
                && indexListBuffer != nullptr
                && countBuffer != nullptr
                && indexListAccessSlot.has_value()
                && countAccessSlot.has_value();
        }

        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain> resourceDomain;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        Microsoft::WRL::ComPtr<ID3D12Resource> lightBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexListBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> countBuffer;
        D3D12_RESOURCE_STATES lightBufferState{ D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES indexListState{ D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES countState{ D3D12_RESOURCE_STATE_COMMON };
        std::optional<std::uint32_t> indexListAccessSlot;
        std::optional<std::uint32_t> countAccessSlot;
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

    // D3D11RenderTargetStateと同じく、明るさは1/4解像度で測り、以降は
    // 2x2平均で1x1まで縮めます。最初の段から最後の1x1までの寸法です。
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
        LuminanceLevelSizes(
            const std::uint32_t width,
            const std::uint32_t height)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
        std::pair<std::uint32_t, std::uint32_t> size{
            std::max(width / 4u, 1u),
            std::max(height / 4u, 1u) };
        sizes.push_back(size);
        while (size.first > 1u || size.second > 1u)
        {
            size.first = std::max(size.first / 2u, 1u);
            size.second = std::max(size.second / 2u, 1u);
            sizes.push_back(size);
        }
        return sizes;
    }

    struct D3D12RenderTargetState final
        : LamaPon::Detail::RenderTargetBackendState
    {
        // 自動露出の測定段です。最後の段が1x1になります。
        struct LuminanceLevel final
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> texture;
            LamaPon::GraphicsViewHandle view;
            D3D12_RESOURCE_STATES state{
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            D3D12_VIEWPORT viewport{};
            D3D12_RECT scissor{};
        };

        // offscreen RTV heapの並びです。current / post colorの後にSSAOの
        // 遮蔽とブラー先、Lens Flareのストリーク先、自動露出の測定段を
        // 置きます。
        static constexpr std::uint32_t OcclusionRenderTargetSlot = 2u;
        static constexpr std::uint32_t OcclusionBlurRenderTargetSlot = 3u;
        static constexpr std::uint32_t LensFlareFirstRenderTargetSlot = 4u;
        static constexpr std::uint32_t LensFlareSecondRenderTargetSlot = 5u;
        static constexpr std::uint32_t LuminanceRenderTargetSlot = 6u;

        ~D3D12RenderTargetState() noexcept override
        {
            if (resourceDomain != nullptr)
            {
                resourceDomain->RetireDescriptorHeap(
                    std::move(renderTargetHeap));
                resourceDomain->RetireDescriptorHeap(
                    std::move(depthStencilHeap));
                if (displayUnorderedAccessSlot.has_value())
                {
                    resourceDomain->Retire(
                        nullptr,
                        *displayUnorderedAccessSlot);
                }
                // 実行中のframeがまだ参照し得るため、viewを持たない深度や
                // readback先も含めて、全資源をGPUの完了まで保持します。
                const std::array<
                    Microsoft::WRL::ComPtr<ID3D12Resource>*,
                    13> resources{
                        &color,
                        &postColor,
                        &displayColor,
                        &colorHistory,
                        &temporalHistory,
                        &depth,
                        &depthCopy,
                        &occlusion,
                        &occlusionBlur,
                        &lensFlareFirst,
                        &lensFlareSecond,
                        &reflectionDepthPyramid,
                        &luminanceReadback };
                for (auto* const resource : resources)
                {
                    if (*resource != nullptr)
                    {
                        resourceDomain->Retire(
                            std::move(*resource),
                            std::nullopt);
                    }
                }
                for (auto& level : luminanceLevels)
                {
                    if (level.texture != nullptr)
                    {
                        resourceDomain->Retire(
                            std::move(level.texture),
                            std::nullopt);
                    }
                }
            }
        }

        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && color != nullptr
                && postColor != nullptr
                && displayColor != nullptr
                && colorHistory != nullptr
                && temporalHistory != nullptr
                && depth != nullptr
                && depthCopy != nullptr
                && occlusion != nullptr
                && occlusionBlur != nullptr
                && lensFlareFirst != nullptr
                && lensFlareSecond != nullptr
                && reflectionDepthPyramid != nullptr
                && !luminanceLevels.empty()
                && luminanceReadback != nullptr
                && renderTargetHeap != nullptr
                && depthStencilHeap != nullptr
                && renderTargetDescriptorSize != 0u
                && (!computeWritable
                    || displayUnorderedAccessSlot.has_value());
        }

        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>
            resourceDomain;
        Microsoft::WRL::ComPtr<ID3D12Resource> color;
        Microsoft::WRL::ComPtr<ID3D12Resource> postColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> displayColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> colorHistory;
        Microsoft::WRL::ComPtr<ID3D12Resource> temporalHistory;
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
        D3D12_RESOURCE_STATES colorHistoryState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES temporalHistoryState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES depthState{
            D3D12_RESOURCE_STATE_DEPTH_WRITE };
        D3D12_RESOURCE_STATES depthCopyState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // SSAOの半解像度R8資源です。公開するのはブラー後の
        // m_ambientOcclusionViewで、ブラー前のviewはBackend内部だけが
        // 読みます。
        Microsoft::WRL::ComPtr<ID3D12Resource> occlusion;
        Microsoft::WRL::ComPtr<ID3D12Resource> occlusionBlur;
        D3D12_RESOURCE_STATES occlusionState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES occlusionBlurState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        LamaPon::GraphicsViewHandle occlusionView;
        D3D12_VIEWPORT occlusionViewport{};
        D3D12_RECT occlusionScissor{};
        // Screen Space Lens Flareの3段ストリークを交互に書く1/4解像度の
        // RGBA16F資源です。3pass目はfirst側へ戻ります。
        Microsoft::WRL::ComPtr<ID3D12Resource> lensFlareFirst;
        Microsoft::WRL::ComPtr<ID3D12Resource> lensFlareSecond;
        D3D12_RESOURCE_STATES lensFlareFirstState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        D3D12_RESOURCE_STATES lensFlareSecondState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        LamaPon::GraphicsViewHandle lensFlareFirstView;
        LamaPon::GraphicsViewHandle lensFlareSecondView;
        D3D12_VIEWPORT lensFlareViewport{};
        D3D12_RECT lensFlareScissor{};
        // SSRのHi-Z深度ピラミッド（R32F、全ミップ）です。全ミップのviewは
        // m_reflectionDepthPyramidViewHandleで公開し、ミップ単位のviewと
        // stateは縮小passが1段細かいミップを読むために持ちます。
        Microsoft::WRL::ComPtr<ID3D12Resource> reflectionDepthPyramid;
        std::vector<D3D12_RESOURCE_STATES> reflectionDepthPyramidStates;
        std::vector<LamaPon::GraphicsViewHandle>
            reflectionDepthPyramidMipViews;
        std::vector<D3D12_VIEWPORT> reflectionDepthPyramidViewports;
        std::uint32_t reflectionDepthPyramidRenderTargetSlot{};
        std::vector<LuminanceLevel> luminanceLevels;
        Microsoft::WRL::ComPtr<ID3D12Resource> luminanceReadback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT luminanceFootprint{};
        // 最後の測定copyを含むframeのfence値です。0は読める値がありません。
        std::uint64_t luminanceFenceValue{};
        std::uint32_t renderTargetDescriptorSize{};
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        bool computeWritable{};
        // computeWritableのとき、表示用textureへCompute Shaderが書く
        // UAVのshader resource heap slotです。
        std::optional<std::uint32_t> displayUnorderedAccessSlot;
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

    [[nodiscard]] const D3D12Texture3DPayload* TryTexture3DPayload(
        const LamaPon::GraphicsTextureHandle& texture,
        const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        if (domain == nullptr
            || GraphicsResourceHandleAccess::Domain(texture) != domain)
        {
            return nullptr;
        }
        return dynamic_cast<const D3D12Texture3DPayload*>(
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

    // 2D、DDS cube、影、IBLの事前畳み込み先のどれでも、handleが持つ
    // D3D12 resourceを返します。
    [[nodiscard]] ID3D12Resource* TryNativeTexture(
        const LamaPon::GraphicsTextureHandle& texture,
        const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        if (domain == nullptr
            || GraphicsResourceHandleAccess::Domain(texture) != domain)
        {
            return nullptr;
        }
        const auto* const payload =
            GraphicsResourceHandleAccess::Payload(texture);
        if (const auto* const texture2D =
                dynamic_cast<const D3D12TexturePayload*>(payload))
        {
            return texture2D->native.Get();
        }
        if (const auto* const texture3D =
                dynamic_cast<const D3D12Texture3DPayload*>(payload))
        {
            return texture3D->native.Get();
        }
        if (const auto* const shadow =
                dynamic_cast<const D3D12ShadowTexturePayload*>(payload))
        {
            return shadow->native.Get();
        }
        if (const auto* const computeCube =
                dynamic_cast<const D3D12ComputeCubeTexturePayload*>(payload))
        {
            return computeCube->native.Get();
        }
        return nullptr;
    }

    [[nodiscard]] const D3D12ComputeCubeTexturePayload*
        TryComputeCubePayload(
            const LamaPon::GraphicsTextureHandle& texture,
            const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        if (domain == nullptr
            || GraphicsResourceHandleAccess::Domain(texture) != domain)
        {
            return nullptr;
        }
        return dynamic_cast<const D3D12ComputeCubeTexturePayload*>(
            GraphicsResourceHandleAccess::Payload(texture));
    }

    // Compute Shaderへ渡す入力viewの参照先textureです。同じtextureが
    // 2回渡されたときは2つ目をnullptrにし、state遷移を1回だけにします。
    [[nodiscard]] std::array<ID3D12Resource*, 2> ComputeInputTextures(
        const std::array<LamaPon::GraphicsViewHandle, 2>& inputs,
        const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        std::array<ID3D12Resource*, 2> resources{};
        for (std::size_t index{}; index < inputs.size(); ++index)
        {
            const auto* const texture =
                GraphicsResourceHandleAccess::TextureResource(inputs[index]);
            const auto* const payload = texture != nullptr
                ? TryTexturePayload(*texture, domain)
                : nullptr;
            resources[index] = payload != nullptr
                ? payload->native.Get()
                : nullptr;
        }
        if (resources[1] == resources[0])
        {
            resources[1] = nullptr;
        }
        return resources;
    }

    // 描画用textureはPIXEL_SHADER_RESOURCEで待機しています。Compute
    // Shaderが読む間だけNON_PIXEL_SHADER_RESOURCEも付けます。
    void TransitionComputeInputs(
        ID3D12GraphicsCommandList* const commandList,
        const std::array<ID3D12Resource*, 2>& resources,
        const bool reading) noexcept
    {
        const auto shaderStates = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (auto* const resource : resources)
        {
            if (resource == nullptr)
            {
                continue;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = reading
                ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                : shaderStates;
            barrier.Transition.StateAfter = reading
                ? shaderStates
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList->ResourceBarrier(1, &barrier);
        }
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

    // Hi-Z深度ピラミッドのように、ミップごとに読み書きを切り替える資源の
    // 1 subresourceだけを遷移させます。
    void TransitionSubresource(
        ID3D12GraphicsCommandList* const commandList,
        ID3D12Resource* const resource,
        const UINT subresource,
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
        barrier.Transition.Subresource = subresource;
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
        m_nullShaderResourceSlots.fill(std::nullopt);
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
        m_activeColorFormat = PrimaryColorFormat;
        m_activeDepthFormat = PrimaryDepthFormat;
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
        m_activeColorFormat = PrimaryColorFormat;
        m_activeDepthFormat = PrimaryDepthFormat;
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
        m_activeColorFormat = PrimaryColorFormat;
        m_activeDepthFormat = PrimaryDepthFormat;
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
                // cubeのsubresourceは面ごとに全ミップを並べるため、寸法は
                // ミップ数で割った余りの段から求めます。
                const auto mipLevel =
                    (firstMipLevel + index) % description.MipLevels;
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
                // 1行ずつ必要なbyte数だけを詰め直します。Texture3Dは奥行きの
                // 段ごとに繰り返し、D3D12側の1段はRowPitch×行数です。
                const auto& subresource = data[index];
                const std::uint32_t sliceCount =
                    description.Dimension
                            == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                        ? std::max(
                            static_cast<std::uint32_t>(
                                description.DepthOrArraySize >> mipLevel),
                            1u)
                        : 1u;
                if (footprint.Footprint.Depth < sliceCount)
                {
                    throw std::runtime_error(
                        "The D3D12 texture footprint is smaller than its "
                        "upload layout.");
                }
                const auto destinationSlicePitch =
                    static_cast<std::size_t>(footprint.Footprint.RowPitch)
                    * rowCounts[index];
                for (std::uint32_t slice{}; slice < sliceCount; ++slice)
                {
                    for (std::uint32_t row{}; row < layout.rowCount; ++row)
                    {
                        std::memcpy(
                            static_cast<std::byte*>(mapped)
                                + footprint.Offset
                                + static_cast<std::size_t>(slice)
                                    * destinationSlicePitch
                                + static_cast<std::size_t>(row)
                                    * footprint.Footprint.RowPitch,
                            subresource.bytes.data()
                                + static_cast<std::size_t>(slice)
                                    * subresource.slicePitch
                                + static_cast<std::size_t>(row)
                                    * subresource.rowPitch,
                            layout.minimumRowBytes);
                    }
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
            payload->height,
            payload->dimension
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
            && IsViewCurrent(existing->m_colorHistoryView)
            && IsViewCurrent(existing->m_temporalHistoryView)
            && IsViewCurrent(existing->m_depthView)
            && IsViewCurrent(existing->occlusionView)
            && IsViewCurrent(existing->m_ambientOcclusionView)
            && IsViewCurrent(existing->lensFlareFirstView)
            && IsViewCurrent(existing->lensFlareSecondView)
            && IsViewCurrent(
                existing->m_reflectionDepthPyramidViewHandle))
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
        const auto luminanceSizes = LuminanceLevelSizes(
            requestedWidth,
            requestedHeight);
        // D3D11RenderTargetStateと同じく、SSRのHi-Z深度ピラミッドは
        // 長辺が1になるまでの全ミップを持ちます。
        std::uint32_t reflectionMipCount = 1u;
        for (std::uint32_t size = std::max(requestedWidth, requestedHeight);
            size > 1u;
            size >>= 1u)
        {
            ++reflectionMipCount;
        }
        pending->reflectionDepthPyramidRenderTargetSlot =
            D3D12RenderTargetState::LuminanceRenderTargetSlot
            + static_cast<std::uint32_t>(luminanceSizes.size());

        D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription{};
        renderTargetHeapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // current / post color、SSAOの遮蔽とブラー先、Lens Flareの
        // ping-pong先、自動露出の測定段、Hi-Z深度ピラミッドの各ミップ
        // ぶんです。
        renderTargetHeapDescription.NumDescriptors =
            pending->reflectionDepthPyramidRenderTargetSlot
            + reflectionMipCount;
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

        // D3D11のGENERATE_MIPS付き輝度textureの代わりに、段ごとのRGBA16F
        // textureを作ります。最後の1x1だけをreadback bufferへ写します。
        const auto luminanceHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE luminanceClear{};
        luminanceClear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D12_SHADER_RESOURCE_VIEW_DESC luminanceViewDescription{};
        luminanceViewDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        luminanceViewDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        luminanceViewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        luminanceViewDescription.Texture2D.MipLevels = 1u;
        D3D12_RESOURCE_DESC luminanceDescription{};
        luminanceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        luminanceDescription.DepthOrArraySize = 1u;
        luminanceDescription.MipLevels = 1u;
        luminanceDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        luminanceDescription.SampleDesc.Count = 1u;
        luminanceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        luminanceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        pending->luminanceLevels.reserve(luminanceSizes.size());
        for (std::size_t index{}; index < luminanceSizes.size(); ++index)
        {
            const auto [levelWidth, levelHeight] = luminanceSizes[index];
            luminanceDescription.Width = levelWidth;
            luminanceDescription.Height = levelHeight;
            D3D12RenderTargetState::LuminanceLevel level;
            ThrowIfFailed(
                m_device->CreateCommittedResource(
                    &luminanceHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &luminanceDescription,
                    level.state,
                    &luminanceClear,
                    IID_PPV_ARGS(level.texture.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateCommittedResource(luminance)",
                m_device.Get());
            m_device->CreateRenderTargetView(
                level.texture.Get(),
                nullptr,
                OffsetDescriptor(
                    pending->renderTargetHeap
                        ->GetCPUDescriptorHandleForHeapStart(),
                    D3D12RenderTargetState::LuminanceRenderTargetSlot
                        + static_cast<std::uint32_t>(index),
                    pending->renderTargetDescriptorSize));
            level.view = CreateTextureView(
                m_device.Get(),
                m_resourceDomain,
                level.texture,
                luminanceViewDescription,
                levelWidth,
                levelHeight,
                GraphicsTextureFormat::Rgba16Float);
            level.viewport = {
                0.0f,
                0.0f,
                static_cast<float>(levelWidth),
                static_cast<float>(levelHeight),
                0.0f,
                1.0f };
            level.scissor = {
                0,
                0,
                static_cast<LONG>(levelWidth),
                static_cast<LONG>(levelHeight) };
            pending->luminanceLevels.push_back(std::move(level));
        }
        auto readbackSource = luminanceDescription;
        readbackSource.Width = 1u;
        readbackSource.Height = 1u;
        UINT64 readbackBytes{};
        m_device->GetCopyableFootprints(
            &readbackSource,
            0,
            1,
            0,
            &pending->luminanceFootprint,
            nullptr,
            nullptr,
            &readbackBytes);
        const auto readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto readbackDescription = BufferDescription(readbackBytes);
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &readbackHeap,
                D3D12_HEAP_FLAG_NONE,
                &readbackDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(
                    pending->luminanceReadback.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(luminance readback)",
            m_device.Get());

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
        // Bloomやトーンマップ前の1.0を超える色を保持します。
        colorDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        createColor(
            pending->colorHistory,
            "ID3D12Device::CreateCommittedResource(offscreen color history)",
            D3D12_RESOURCE_FLAG_NONE);
        createColor(
            pending->temporalHistory,
            "ID3D12Device::CreateCommittedResource(temporal history)",
            D3D12_RESOURCE_FLAG_NONE);

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

        // D3D11RenderTargetStateと同じく、SSAOは半解像度のR8へ遮蔽と
        // そのブラー結果を書きます。
        const auto occlusionWidth = std::max(requestedWidth / 2u, 1u);
        const auto occlusionHeight = std::max(requestedHeight / 2u, 1u);
        auto occlusionDescription = colorDescription;
        occlusionDescription.Width = occlusionWidth;
        occlusionDescription.Height = occlusionHeight;
        occlusionDescription.Format = DXGI_FORMAT_R8_UNORM;
        D3D12_CLEAR_VALUE occlusionClear{};
        occlusionClear.Format = occlusionDescription.Format;
        const auto createOcclusion =
            [&](Microsoft::WRL::ComPtr<ID3D12Resource>& destination,
                const std::uint32_t slot,
                const char* const operation)
        {
            ThrowIfFailed(
                m_device->CreateCommittedResource(
                    &defaultHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &occlusionDescription,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    &occlusionClear,
                    IID_PPV_ARGS(destination.ReleaseAndGetAddressOf())),
                operation,
                m_device.Get());
            m_device->CreateRenderTargetView(
                destination.Get(),
                nullptr,
                OffsetDescriptor(
                    renderTargetStart,
                    slot,
                    pending->renderTargetDescriptorSize));
        };
        createOcclusion(
            pending->occlusion,
            D3D12RenderTargetState::OcclusionRenderTargetSlot,
            "ID3D12Device::CreateCommittedResource(ambient occlusion)");
        createOcclusion(
            pending->occlusionBlur,
            D3D12RenderTargetState::OcclusionBlurRenderTargetSlot,
            "ID3D12Device::CreateCommittedResource(ambient occlusion blur)");

        // D3D11RenderTargetStateと同じく、Screen Space Lens Flareの筋は
        // 1/4解像度のRGBA16Fを2枚使って3passのping-pongを行います。
        const auto lensFlareWidth = std::max(requestedWidth / 4u, 1u);
        const auto lensFlareHeight = std::max(requestedHeight / 4u, 1u);
        auto lensFlareDescription = colorDescription;
        lensFlareDescription.Width = lensFlareWidth;
        lensFlareDescription.Height = lensFlareHeight;
        const auto createLensFlare =
            [&](Microsoft::WRL::ComPtr<ID3D12Resource>& destination,
                const std::uint32_t slot,
                const char* const operation)
        {
            ThrowIfFailed(
                m_device->CreateCommittedResource(
                    &defaultHeap,
                    D3D12_HEAP_FLAG_NONE,
                    &lensFlareDescription,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    &colorClear,
                    IID_PPV_ARGS(destination.ReleaseAndGetAddressOf())),
                operation,
                m_device.Get());
            m_device->CreateRenderTargetView(
                destination.Get(),
                nullptr,
                OffsetDescriptor(
                    renderTargetStart,
                    slot,
                    pending->renderTargetDescriptorSize));
        };
        createLensFlare(
            pending->lensFlareFirst,
            D3D12RenderTargetState::LensFlareFirstRenderTargetSlot,
            "ID3D12Device::CreateCommittedResource(lens flare first)");
        createLensFlare(
            pending->lensFlareSecond,
            D3D12RenderTargetState::LensFlareSecondRenderTargetSlot,
            "ID3D12Device::CreateCommittedResource(lens flare second)");

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
            GraphicsTextureFormat::Rgba16Float);
        pending->m_postColorView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->postColor,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba16Float);
        pending->m_displayView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->displayColor,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba16Float);
        if (computeWritable)
        {
            // D3D11RenderTargetStateと同じく、Compute Shaderの書き込み先に
            // するtargetだけ表示用textureへUAVを作ります。slotはstateの
            // 破棄時にGPUの完了を待って戻します。
            D3D12_UNORDERED_ACCESS_VIEW_DESC accessDescription{};
            accessDescription.Format = colorDescription.Format;
            accessDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            pending->displayUnorderedAccessSlot =
                m_resourceDomain->AllocateShaderResourceSlot();
            m_device->CreateUnorderedAccessView(
                pending->displayColor.Get(),
                nullptr,
                &accessDescription,
                m_resourceDomain->ShaderResourceCpuHandle(
                    *pending->displayUnorderedAccessSlot));
        }
        pending->m_colorHistoryView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->colorHistory,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba16Float);
        pending->m_temporalHistoryView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->temporalHistory,
            colorViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba16Float);

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
        // 公開formatにR8が無いため、深度viewと同じくRgba8Unormとして
        // 扱います。shaderはnative formatのRだけを読みます。
        auto occlusionViewDescription = colorViewDescription;
        occlusionViewDescription.Format = DXGI_FORMAT_R8_UNORM;
        pending->occlusionView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->occlusion,
            occlusionViewDescription,
            occlusionWidth,
            occlusionHeight,
            GraphicsTextureFormat::Rgba8Unorm);
        pending->m_ambientOcclusionView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->occlusionBlur,
            occlusionViewDescription,
            occlusionWidth,
            occlusionHeight,
            GraphicsTextureFormat::Rgba8Unorm);
        pending->lensFlareFirstView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->lensFlareFirst,
            colorViewDescription,
            lensFlareWidth,
            lensFlareHeight,
            GraphicsTextureFormat::Rgba16Float);
        pending->lensFlareSecondView = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->lensFlareSecond,
            colorViewDescription,
            lensFlareWidth,
            lensFlareHeight,
            GraphicsTextureFormat::Rgba16Float);
        pending->occlusionViewport = {
            0.0f,
            0.0f,
            static_cast<float>(occlusionWidth),
            static_cast<float>(occlusionHeight),
            0.0f,
            1.0f };
        pending->occlusionScissor = {
            0,
            0,
            static_cast<LONG>(occlusionWidth),
            static_cast<LONG>(occlusionHeight) };
        pending->lensFlareViewport = {
            0.0f,
            0.0f,
            static_cast<float>(lensFlareWidth),
            static_cast<float>(lensFlareHeight),
            0.0f,
            1.0f };
        pending->lensFlareScissor = {
            0,
            0,
            static_cast<LONG>(lensFlareWidth),
            static_cast<LONG>(lensFlareHeight) };

        // SSRのHi-Z深度ピラミッドです。各ミップをRTVとして書き、1段粗い
        // ミップを作る縮小passが単一ミップのSRVで読みます。
        auto reflectionDescription = colorDescription;
        reflectionDescription.MipLevels =
            static_cast<UINT16>(reflectionMipCount);
        reflectionDescription.Format = DXGI_FORMAT_R32_FLOAT;
        D3D12_CLEAR_VALUE reflectionClear{};
        reflectionClear.Format = reflectionDescription.Format;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &reflectionDescription,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &reflectionClear,
                IID_PPV_ARGS(
                    pending->reflectionDepthPyramid
                        .ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(hi-z pyramid)",
            m_device.Get());
        D3D12_SHADER_RESOURCE_VIEW_DESC reflectionViewDescription{};
        reflectionViewDescription.Format = DXGI_FORMAT_R32_FLOAT;
        reflectionViewDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        reflectionViewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        reflectionViewDescription.Texture2D.MipLevels = reflectionMipCount;
        // 公開formatにR32Fが無いため、浮動小数のRgba16Floatとして
        // 扱います。shaderはnative formatのRだけを読みます。
        pending->m_reflectionDepthPyramidViewHandle = CreateTextureView(
            m_device.Get(),
            m_resourceDomain,
            pending->reflectionDepthPyramid,
            reflectionViewDescription,
            requestedWidth,
            requestedHeight,
            GraphicsTextureFormat::Rgba16Float);
        pending->reflectionDepthPyramidStates.assign(
            reflectionMipCount,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pending->reflectionDepthPyramidMipViews.reserve(reflectionMipCount);
        pending->reflectionDepthPyramidViewports.reserve(reflectionMipCount);
        for (std::uint32_t mip{}; mip < reflectionMipCount; ++mip)
        {
            const auto mipWidth = std::max(requestedWidth >> mip, 1u);
            const auto mipHeight = std::max(requestedHeight >> mip, 1u);
            D3D12_RENDER_TARGET_VIEW_DESC mipTargetDescription{};
            mipTargetDescription.Format = DXGI_FORMAT_R32_FLOAT;
            mipTargetDescription.ViewDimension =
                D3D12_RTV_DIMENSION_TEXTURE2D;
            mipTargetDescription.Texture2D.MipSlice = mip;
            m_device->CreateRenderTargetView(
                pending->reflectionDepthPyramid.Get(),
                &mipTargetDescription,
                OffsetDescriptor(
                    renderTargetStart,
                    pending->reflectionDepthPyramidRenderTargetSlot + mip,
                    pending->renderTargetDescriptorSize));
            auto mipViewDescription = reflectionViewDescription;
            mipViewDescription.Texture2D.MostDetailedMip = mip;
            mipViewDescription.Texture2D.MipLevels = 1u;
            pending->reflectionDepthPyramidMipViews.push_back(
                CreateTextureView(
                    m_device.Get(),
                    m_resourceDomain,
                    pending->reflectionDepthPyramid,
                    mipViewDescription,
                    mipWidth,
                    mipHeight,
                    GraphicsTextureFormat::Rgba16Float));
            pending->reflectionDepthPyramidViewports.push_back({
                0.0f,
                0.0f,
                static_cast<float>(mipWidth),
                static_cast<float>(mipHeight),
                0.0f,
                1.0f });
        }
        pending->m_reflectionDepthPyramidMipCount = reflectionMipCount;
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
        m_activeColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_activeDepthFormat = PrimaryDepthFormat;
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

    GraphicsViewHandle D3D12Backend::BeginOffscreenPostProcess(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenPostProcess requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_currentColorView)
            || !IsViewCurrent(state->m_postColorView))
        {
            throw std::invalid_argument(
                "BeginOffscreenPostProcess requires a target owned by this "
                "D3D12 backend generation.");
        }
        OpenCommandList();
        // D3D11のpost-processと同じく、current colorを読んでpost colorへ
        // 書きます。深度はtestしませんが、PSOのDSV formatと揃えるため
        // targetの深度もbindします。
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->postColor.Get(),
            state->postColorState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        TransitionResource(
            m_commandList.Get(),
            state->depth.Get(),
            state->depthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const auto postTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            1u,
            state->renderTargetDescriptorSize);
        const auto depthTarget = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_commandList->OMSetRenderTargets(
            1,
            &postTarget,
            FALSE,
            &depthTarget);
        m_commandList->RSSetViewports(1, &state->viewport);
        m_commandList->RSSetScissorRects(1, &state->scissor);
        m_activeViewport = state->viewport;
        m_activeScissorRect = state->scissor;
        m_activeColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_activeDepthFormat = PrimaryDepthFormat;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
        return state->m_currentColorView;
    }

    void D3D12Backend::EndOffscreenPostProcess(RenderTarget& target)
    {
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || m_activeOffscreenTarget != &target)
        {
            throw std::logic_error(
                "EndOffscreenPostProcess requires the active post-process "
                "target.");
        }
        // D3D11RenderTargetState::SwapPostProcessBuffersと同じく、書き終えた
        // post colorを次のcurrent colorにします。RTV heapの先頭をcurrent
        // として使うため、交換後の資源でRTVを作り直します。記録済みの
        // OMSetRenderTargetsはdescriptorを呼び出し時に読み終えています。
        std::swap(state->color, state->postColor);
        std::swap(state->colorState, state->postColorState);
        std::swap(state->m_currentColorView, state->m_postColorView);
        const auto renderTargetStart = state->renderTargetHeap
            ->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateRenderTargetView(
            state->color.Get(),
            nullptr,
            renderTargetStart);
        m_device->CreateRenderTargetView(
            state->postColor.Get(),
            nullptr,
            OffsetDescriptor(
                renderTargetStart,
                1u,
                state->renderTargetDescriptorSize));
        BindOffscreenTarget(target);
    }

    void D3D12Backend::AbortOffscreenPostProcess(
        RenderTarget& target) noexcept
    {
        try
        {
            BindOffscreenTarget(target);
        }
        catch (...)
        {
            // 元の例外を呼び出し側へ返すため、復元の失敗はここで止めます。
        }
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
        m_activeColorFormat = DXGI_FORMAT_UNKNOWN;
        m_activeDepthFormat = PrimaryDepthFormat;
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

    D3D12Backend::ComputeBindings D3D12Backend::BeginOffscreenCompute(
        RenderTarget& target,
        const std::array<GraphicsViewHandle, 2>& inputs)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenCompute requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !state->computeWritable
            || !state->displayUnorderedAccessSlot.has_value()
            || !IsViewCurrent(state->m_displayView))
        {
            throw std::invalid_argument(
                "BeginOffscreenCompute requires a compute-writable target "
                "owned by this D3D12 backend generation.");
        }

        ComputeBindings bindings;
        bindings.output = m_resourceDomain->ShaderResourceGpuHandle(
            *state->displayUnorderedAccessSlot);
        bindings.width = state->m_width;
        bindings.height = state->m_height;
        const auto inputTextures =
            ComputeInputTextures(inputs, m_resourceDomain.get());
        for (std::size_t index{}; index < inputs.size(); ++index)
        {
            const auto* const payload = TryShaderResourceViewPayload(
                inputs[index],
                m_resourceDomain.get());
            // 同じtextureを読み書きするとD3D11は入力を黙って外すため、
            // D3D12では記録前に拒否します。
            const auto* const texture =
                Detail::GraphicsResourceHandleAccess::TextureResource(
                    inputs[index]);
            const auto* const texturePayload = texture != nullptr
                ? TryTexturePayload(*texture, m_resourceDomain.get())
                : nullptr;
            if (payload == nullptr
                || texturePayload == nullptr
                || texturePayload->native.Get() == state->displayColor.Get())
            {
                throw std::invalid_argument(
                    "BeginOffscreenCompute requires current input texture "
                    "views that differ from the output.");
            }
            bindings.inputs[index] =
                m_resourceDomain->ShaderResourceGpuHandle(payload->slot);
        }

        OpenCommandList();
        TransitionComputeInputs(m_commandList.Get(), inputTextures, true);
        TransitionResource(
            m_commandList.Get(),
            state->displayColor.Get(),
            state->displayColorState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return bindings;
    }

    void D3D12Backend::EndOffscreenCompute(
        RenderTarget& target,
        const std::array<GraphicsViewHandle, 2>& inputs) noexcept
    {
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (!IsInitialized()
            || !m_commandListOpen
            || state == nullptr
            || state->displayColor == nullptr
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            return;
        }
        TransitionResource(
            m_commandList.Get(),
            state->displayColor.Get(),
            state->displayColorState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionComputeInputs(
            m_commandList.Get(),
            ComputeInputTextures(inputs, m_resourceDomain.get()),
            false);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE D3D12Backend::NullShaderResourceDescriptor(
        const D3D12_SRV_DIMENSION dimension)
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "NullShaderResourceDescriptor requires an initialized D3D12 "
                "backend.");
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension = dimension;
        description.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::size_t index{};
        switch (dimension)
        {
        case D3D12_SRV_DIMENSION_TEXTURE2D:
            description.Texture2D.MipLevels = 1u;
            index = 0u;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            description.Texture2DArray.MipLevels = 1u;
            description.Texture2DArray.ArraySize = 1u;
            index = 1u;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            description.TextureCube.MipLevels = 1u;
            index = 2u;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            description.Texture3D.MipLevels = 1u;
            index = 3u;
            break;
        case D3D12_SRV_DIMENSION_BUFFER:
            description.Format = DXGI_FORMAT_R32_UINT;
            description.Buffer.NumElements = 1u;
            index = 4u;
            break;
        default:
            throw std::invalid_argument(
                "NullShaderResourceDescriptor received an unsupported view "
                "dimension.");
        }
        auto& slot = m_nullShaderResourceSlots[index];
        if (!slot.has_value())
        {
            const auto allocated =
                m_resourceDomain->AllocateShaderResourceSlot();
            m_device->CreateShaderResourceView(
                nullptr,
                &description,
                m_resourceDomain->ShaderResourceCpuHandle(allocated));
            slot = allocated;
        }
        return m_resourceDomain->ShaderResourceGpuHandle(*slot);
    }

    void D3D12Backend::CaptureOffscreenTargetColorHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetColorHistory requires an "
                "initialized D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_colorHistoryView))
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetColorHistory requires a target "
                "owned by this D3D12 backend generation.");
        }
        OpenCommandList();
        const bool restoreTarget = m_activeOffscreenTarget == &target;
        if (restoreTarget && !m_activeOffscreenDepthOnly)
        {
            m_commandList->OMSetRenderTargets(
                0,
                nullptr,
                FALSE,
                nullptr);
        }
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->colorHistory.Get(),
            state->colorHistoryState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(
            state->colorHistory.Get(),
            state->color.Get());
        TransitionResource(
            m_commandList.Get(),
            state->colorHistory.Get(),
            state->colorHistoryState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            restoreTarget && !m_activeOffscreenDepthOnly
                ? D3D12_RESOURCE_STATE_RENDER_TARGET
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        state->m_historyViewProjection = viewProjection;
        state->m_historyValid = true;
        Detail::RenderTargetBackendAccess::SetPublicHistoryViewProjection(
            target,
            viewProjection);
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

    void D3D12Backend::CaptureOffscreenTargetTemporalHistory(
        RenderTarget& target,
        const DirectX::XMFLOAT4X4& viewProjection)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetTemporalHistory requires an "
                "initialized D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_temporalHistoryView))
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetTemporalHistory requires a target "
                "owned by this D3D12 backend generation.");
        }
        OpenCommandList();
        const bool restoreTarget = m_activeOffscreenTarget == &target;
        if (restoreTarget && !m_activeOffscreenDepthOnly)
        {
            m_commandList->OMSetRenderTargets(
                0,
                nullptr,
                FALSE,
                nullptr);
        }
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->temporalHistory.Get(),
            state->temporalHistoryState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->CopyResource(
            state->temporalHistory.Get(),
            state->color.Get());
        TransitionResource(
            m_commandList.Get(),
            state->temporalHistory.Get(),
            state->temporalHistoryState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->color.Get(),
            state->colorState,
            restoreTarget && !m_activeOffscreenDepthOnly
                ? D3D12_RESOURCE_STATE_RENDER_TARGET
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        state->m_temporalHistoryViewProjection = viewProjection;
        state->m_temporalHistoryValid = true;
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

    std::uint32_t D3D12Backend::OffscreenLuminanceLevelCount(
        const RenderTarget& target) const
    {
        const auto* const state = dynamic_cast<
            const D3D12RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "OffscreenLuminanceLevelCount requires a target owned by "
                "this D3D12 backend generation.");
        }
        return static_cast<std::uint32_t>(state->luminanceLevels.size());
    }

    GraphicsViewHandle D3D12Backend::BeginOffscreenLuminancePass(
        RenderTarget& target,
        const std::uint32_t level)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenLuminancePass requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || level >= state->luminanceLevels.size()
            || !IsViewCurrent(state->m_currentColorView))
        {
            throw std::invalid_argument(
                "BeginOffscreenLuminancePass requires a target owned by "
                "this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        auto& destination = state->luminanceLevels[level];
        GraphicsViewHandle source;
        if (level == 0u)
        {
            TransitionResource(
                m_commandList.Get(),
                state->color.Get(),
                state->colorState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->m_currentColorView;
        }
        else
        {
            auto& previous = state->luminanceLevels[level - 1u];
            TransitionResource(
                m_commandList.Get(),
                previous.texture.Get(),
                previous.state,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = previous.view;
        }
        TransitionResource(
            m_commandList.Get(),
            destination.texture.Get(),
            destination.state,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12RenderTargetState::LuminanceRenderTargetSlot + level,
            state->renderTargetDescriptorSize);
        // 縮小段は深度bufferと寸法が違うため、DSVはbindしません。
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            nullptr);
        m_commandList->RSSetViewports(1, &destination.viewport);
        m_commandList->RSSetScissorRects(1, &destination.scissor);
        m_activeViewport = destination.viewport;
        m_activeScissorRect = destination.scissor;
        m_activeColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_activeDepthFormat = DXGI_FORMAT_UNKNOWN;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
        return source;
    }

    GraphicsViewHandle D3D12Backend::BeginOffscreenAmbientOcclusionPass(
        RenderTarget& target,
        const bool blur)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenAmbientOcclusionPass requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_depthView)
            || !IsViewCurrent(state->occlusionView)
            || !IsViewCurrent(state->m_ambientOcclusionView))
        {
            throw std::invalid_argument(
                "BeginOffscreenAmbientOcclusionPass requires a target owned "
                "by this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        // 両passとも、DSVの本体ではなくCaptureOffscreenTargetDepthで
        // 確定した深度コピーを読みます。
        TransitionResource(
            m_commandList.Get(),
            state->depthCopy.Get(),
            state->depthCopyState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        GraphicsViewHandle source = state->m_depthView;
        ID3D12Resource* destination = state->occlusion.Get();
        D3D12_RESOURCE_STATES* destinationState = &state->occlusionState;
        std::uint32_t slot = D3D12RenderTargetState::OcclusionRenderTargetSlot;
        if (blur)
        {
            TransitionResource(
                m_commandList.Get(),
                state->occlusion.Get(),
                state->occlusionState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->occlusionView;
            destination = state->occlusionBlur.Get();
            destinationState = &state->occlusionBlurState;
            slot = D3D12RenderTargetState::OcclusionBlurRenderTargetSlot;
        }
        TransitionResource(
            m_commandList.Get(),
            destination,
            *destinationState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            slot,
            state->renderTargetDescriptorSize);
        // 半解像度の遮蔽は深度bufferと寸法が違うため、DSVはbindしません。
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            nullptr);
        m_commandList->RSSetViewports(1, &state->occlusionViewport);
        m_commandList->RSSetScissorRects(1, &state->occlusionScissor);
        m_activeViewport = state->occlusionViewport;
        m_activeScissorRect = state->occlusionScissor;
        m_activeColorFormat = DXGI_FORMAT_R8_UNORM;
        m_activeDepthFormat = DXGI_FORMAT_UNKNOWN;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
        return source;
    }

    void D3D12Backend::EndOffscreenAmbientOcclusion(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndOffscreenAmbientOcclusion requires an initialized D3D12 "
                "backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "EndOffscreenAmbientOcclusion requires a target owned by "
                "this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        // 後続のLit描画が画面UVで読むため、両textureをSRV stateへ戻します。
        TransitionResource(
            m_commandList.Get(),
            state->occlusion.Get(),
            state->occlusionState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->occlusionBlur.Get(),
            state->occlusionBlurState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        BindOffscreenTargetDepthOnly(target);
    }

    void D3D12Backend::AbortOffscreenAmbientOcclusion(
        RenderTarget& target) noexcept
    {
        try
        {
            EndOffscreenAmbientOcclusion(target);
        }
        catch (...)
        {
            // 元の例外を呼び出し側へ返すため、復元の失敗はここで止めます。
        }
    }

    GraphicsViewHandle D3D12Backend::BeginOffscreenLensFlareStreakPass(
        RenderTarget& target,
        const std::uint32_t pass)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenLensFlareStreakPass requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || pass >= 3u
            || !IsViewCurrent(state->m_currentColorView)
            || !IsViewCurrent(state->lensFlareFirstView)
            || !IsViewCurrent(state->lensFlareSecondView))
        {
            throw std::invalid_argument(
                "BeginOffscreenLensFlareStreakPass requires a target owned "
                "by this D3D12 backend generation and a pass below 3.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        GraphicsViewHandle source;
        ID3D12Resource* destination{};
        D3D12_RESOURCE_STATES* destinationState{};
        std::uint32_t destinationSlot{};
        if (pass == 0u)
        {
            TransitionResource(
                m_commandList.Get(),
                state->color.Get(),
                state->colorState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->m_currentColorView;
            destination = state->lensFlareFirst.Get();
            destinationState = &state->lensFlareFirstState;
            destinationSlot =
                D3D12RenderTargetState::LensFlareFirstRenderTargetSlot;
        }
        else if (pass == 1u)
        {
            TransitionResource(
                m_commandList.Get(),
                state->lensFlareFirst.Get(),
                state->lensFlareFirstState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->lensFlareFirstView;
            destination = state->lensFlareSecond.Get();
            destinationState = &state->lensFlareSecondState;
            destinationSlot =
                D3D12RenderTargetState::LensFlareSecondRenderTargetSlot;
        }
        else
        {
            TransitionResource(
                m_commandList.Get(),
                state->lensFlareSecond.Get(),
                state->lensFlareSecondState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->lensFlareSecondView;
            destination = state->lensFlareFirst.Get();
            destinationState = &state->lensFlareFirstState;
            destinationSlot =
                D3D12RenderTargetState::LensFlareFirstRenderTargetSlot;
        }
        TransitionResource(
            m_commandList.Get(),
            destination,
            *destinationState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            destinationSlot,
            state->renderTargetDescriptorSize);
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            nullptr);
        m_commandList->RSSetViewports(1, &state->lensFlareViewport);
        m_commandList->RSSetScissorRects(1, &state->lensFlareScissor);
        m_activeViewport = state->lensFlareViewport;
        m_activeScissorRect = state->lensFlareScissor;
        m_activeColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        m_activeDepthFormat = DXGI_FORMAT_UNKNOWN;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
        return source;
    }

    GraphicsViewHandle D3D12Backend::EndOffscreenLensFlareStreaks(
        RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndOffscreenLensFlareStreaks requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "EndOffscreenLensFlareStreaks requires a target owned by "
                "this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        TransitionResource(
            m_commandList.Get(),
            state->lensFlareFirst.Get(),
            state->lensFlareFirstState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            m_commandList.Get(),
            state->lensFlareSecond.Get(),
            state->lensFlareSecondState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        BindOffscreenTarget(target);
        return state->lensFlareFirstView;
    }

    void D3D12Backend::AbortOffscreenLensFlareStreaks(
        RenderTarget& target) noexcept
    {
        try
        {
            static_cast<void>(EndOffscreenLensFlareStreaks(target));
        }
        catch (...)
        {
            // 元の例外を呼び出し側へ返すため、復元の失敗はここで止めます。
        }
    }

    GraphicsViewHandle D3D12Backend::BeginOffscreenReflectionDepthPass(
        RenderTarget& target,
        const std::uint32_t mip)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginOffscreenReflectionDepthPass requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || mip >= state->reflectionDepthPyramidMipViews.size()
            || !IsViewCurrent(state->m_depthView)
            || !IsViewCurrent(state->m_reflectionDepthPyramidViewHandle))
        {
            throw std::invalid_argument(
                "BeginOffscreenReflectionDepthPass requires a target owned "
                "by this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        GraphicsViewHandle source;
        if (mip == 0u)
        {
            // DSVの本体ではなく、CaptureOffscreenTargetDepthで確定した
            // 深度コピーを距離へ直します。
            TransitionResource(
                m_commandList.Get(),
                state->depthCopy.Get(),
                state->depthCopyState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->m_depthView;
        }
        else
        {
            TransitionSubresource(
                m_commandList.Get(),
                state->reflectionDepthPyramid.Get(),
                mip - 1u,
                state->reflectionDepthPyramidStates[mip - 1u],
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            source = state->reflectionDepthPyramidMipViews[mip - 1u];
        }
        TransitionSubresource(
            m_commandList.Get(),
            state->reflectionDepthPyramid.Get(),
            mip,
            state->reflectionDepthPyramidStates[mip],
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            state->reflectionDepthPyramidRenderTargetSlot + mip,
            state->renderTargetDescriptorSize);
        // 縮小段は深度bufferと寸法が違うため、DSVはbindしません。
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            nullptr);
        const auto& viewport = state->reflectionDepthPyramidViewports[mip];
        const D3D12_RECT scissor{
            0,
            0,
            static_cast<LONG>(viewport.Width),
            static_cast<LONG>(viewport.Height) };
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        m_activeViewport = viewport;
        m_activeScissorRect = scissor;
        m_activeColorFormat = DXGI_FORMAT_R32_FLOAT;
        m_activeDepthFormat = DXGI_FORMAT_UNKNOWN;
        m_activeOffscreenTarget = &target;
        m_activeOffscreenDepthOnly = false;
        return source;
    }

    void D3D12Backend::EndOffscreenReflectionDepthPyramid(
        RenderTarget& target,
        const bool depthOnly)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "EndOffscreenReflectionDepthPyramid requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "EndOffscreenReflectionDepthPyramid requires a target owned "
                "by this D3D12 backend generation.");
        }
        OpenCommandList();
        m_commandList->OMSetRenderTargets(
            0,
            nullptr,
            FALSE,
            nullptr);
        // Lit描画がLoadで全ミップを読むため、全ミップをSRV stateへ戻します。
        for (std::size_t mip{};
            mip < state->reflectionDepthPyramidStates.size();
            ++mip)
        {
            TransitionSubresource(
                m_commandList.Get(),
                state->reflectionDepthPyramid.Get(),
                static_cast<UINT>(mip),
                state->reflectionDepthPyramidStates[mip],
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (depthOnly)
        {
            BindOffscreenTargetDepthOnly(target);
        }
        else
        {
            BindOffscreenTarget(target);
        }
    }

    void D3D12Backend::AbortOffscreenReflectionDepthPyramid(
        RenderTarget& target,
        const bool depthOnly) noexcept
    {
        try
        {
            EndOffscreenReflectionDepthPyramid(target, depthOnly);
        }
        catch (...)
        {
            // 元の例外を呼び出し側へ返すため、復元の失敗はここで止めます。
        }
    }

    bool D3D12Backend::IsOffscreenTargetBoundDepthOnly(
        const RenderTarget& target) const noexcept
    {
        return m_activeOffscreenTarget == &target
            && m_activeOffscreenDepthOnly;
    }

    void D3D12Backend::DiscardOffscreenTargetLuminance(
        RenderTarget& target) noexcept
    {
        if (auto* const state = dynamic_cast<D3D12RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target)))
        {
            state->luminanceFenceValue = 0u;
        }
    }

    std::optional<float>
        D3D12Backend::TryReadOffscreenTargetLuminance(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "TryReadOffscreenTargetLuminance requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "TryReadOffscreenTargetLuminance requires a target owned "
                "by this D3D12 backend generation.");
        }
        // D3D11のMAP_FLAG_DO_NOT_WAITと同じく、copyを含むframeがGPUで
        // 終わっていなければ待たずにnulloptを返します。
        const std::uint64_t completedValue = m_fence->GetCompletedValue();
        if (state->luminanceFenceValue == 0u
            || completedValue == std::numeric_limits<std::uint64_t>::max()
            || completedValue < state->luminanceFenceValue)
        {
            return std::nullopt;
        }

        const auto offset =
            static_cast<SIZE_T>(state->luminanceFootprint.Offset);
        void* mapped{};
        const D3D12_RANGE readRange{
            offset,
            offset + sizeof(DirectX::PackedVector::HALF) };
        ThrowIfFailed(
            state->luminanceReadback->Map(0, &readRange, &mapped),
            "ID3D12Resource::Map(luminance readback)",
            m_device.Get());
        DirectX::PackedVector::HALF averageLogLuminance{};
        std::memcpy(
            &averageLogLuminance,
            static_cast<const std::uint8_t*>(mapped) + offset,
            sizeof(averageLogLuminance));
        const D3D12_RANGE noWrite{};
        state->luminanceReadback->Unmap(0, &noWrite);
        // 輝度shaderは対数平均を格納するため、Backendの共通契約へ渡す前に
        // 線形空間へ戻します。
        return std::exp(
            DirectX::PackedVector::XMConvertHalfToFloat(
                averageLogLuminance));
    }

    void D3D12Backend::CaptureOffscreenTargetLuminance(RenderTarget& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CaptureOffscreenTargetLuminance requires an initialized "
                "D3D12 backend.");
        }
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get())
        {
            throw std::invalid_argument(
                "CaptureOffscreenTargetLuminance requires a target owned "
                "by this D3D12 backend generation.");
        }
        // 前回のcopyがまだGPUで終わっていなければ、同じbufferへ重ねて
        // 書かずに読み出しを待ちます。GPUが遅れても毎回上書きして
        // 永遠に読めない状態を避けるためです。
        const std::uint64_t completedValue = m_fence->GetCompletedValue();
        if (state->luminanceFenceValue != 0u
            && (completedValue == std::numeric_limits<std::uint64_t>::max()
                || completedValue < state->luminanceFenceValue))
        {
            return;
        }

        OpenCommandList();
        // 最後の1x1だけを、次フレーム以降にCPUから読めるreadback bufferへ
        // 写します。描画先のbindは変えません。
        auto& last = state->luminanceLevels.back();
        TransitionResource(
            m_commandList.Get(),
            last.texture.Get(),
            last.state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = state->luminanceReadback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = state->luminanceFootprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = last.texture.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        m_commandList->CopyTextureRegion(
            &destination,
            0,
            0,
            0,
            &source,
            nullptr);
        TransitionResource(
            m_commandList.Get(),
            last.texture.Get(),
            last.state,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 記録中のcommandは、次に発行されるfence値より前に実行されます。
        state->luminanceFenceValue = m_nextFenceValue;
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
                            state->m_resolution,
                            cube
                                ? D3D12_SRV_DIMENSION_TEXTURECUBE
                                : D3D12_SRV_DIMENSION_TEXTURE2DARRAY));
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
            m_activeColorFormat = DXGI_FORMAT_UNKNOWN;
            m_activeDepthFormat = ShadowDepthFormat;
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
        ClusteredLights& clusteredLights,
        LightingState& lighting,
        const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& projection,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "UpdateClusteredLights requires an initialized D3D12 "
                "backend.");
        }

        // D3D11と同じく、別世代のstate、正射影、ライト無しでは従来の
        // 固定配列の経路へ任せます。
        lighting.clustered = {};
        auto* const state = dynamic_cast<D3D12ClusteredLightsState*>(
            Detail::ClusteredLightsBackendAccess::Get(clusteredLights));
        if (state == nullptr
            || !state->HasNativeResources()
            || state->resourceDomain.get() != m_resourceDomain.get()
            || !IsViewCurrent(state->m_lightView)
            || !IsViewCurrent(state->m_indexListView)
            || !IsViewCurrent(state->m_countView))
        {
            return;
        }
        const bool perspective = std::abs(projection._44) < 0.5f;
        if (!perspective || lighting.clusteredLights.empty())
        {
            return;
        }
        // 射影行列からnear/farと視野の広がりを取り出します
        // （XMMatrixPerspectiveFovRH: _33=f/(n-f), _43=n*f/(n-f)）。
        const float nearPlane = projection._43 / projection._33;
        const float farPlane = projection._43 / (projection._33 + 1.0f);
        if (!(nearPlane > 0.0f) || !(farPlane > nearPlane))
        {
            return;
        }
        const float tanHalfX = 1.0f / projection._11;
        const float tanHalfY = 1.0f / projection._22;
        const auto lightCount = static_cast<std::uint32_t>(
            std::min(
                lighting.clusteredLights.size(),
                MaximumClusteredLights));
        const auto lightBinding =
            TryResolveShaderResource(state->m_lightView);
        if (!lightBinding)
        {
            return;
        }

        OpenCommandList();
        auto* const commandList = m_commandList.Get();
        const std::uint64_t lightBytes =
            static_cast<std::uint64_t>(sizeof(GpuLight)) * lightCount;
        const auto lightUpload =
            AllocateFrameUpload(lightBytes, alignof(GpuLight));
        std::memcpy(
            lightUpload.data,
            lighting.clusteredLights.data(),
            static_cast<std::size_t>(lightBytes));
        TransitionResource(
            commandList,
            state->lightBuffer.Get(),
            state->lightBufferState,
            D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyBufferRegion(
            state->lightBuffer.Get(),
            0,
            lightUpload.resource,
            lightUpload.offset,
            lightBytes);

        ClusterCullingConstants constants{};
        constants.view = view;
        constants.gridParameters = {
            static_cast<float>(ClusteredLights::GridWidth),
            static_cast<float>(ClusteredLights::GridHeight),
            static_cast<float>(ClusteredLights::GridDepth),
            static_cast<float>(lightCount)
        };
        constants.depthParameters = {
            nearPlane,
            farPlane,
            std::log(farPlane / nearPlane),
            static_cast<float>(ClusteredLights::MaximumLightsPerCluster)
        };
        constants.frustumParameters = { tanHalfX, tanHalfY, 0.0f, 0.0f };
        const auto constantUpload = AllocateFrameUpload(
            sizeof(constants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        std::memcpy(constantUpload.data, &constants, sizeof(constants));

        TransitionResource(
            commandList,
            state->lightBuffer.Get(),
            state->lightBufferState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            state->indexListBuffer.Get(),
            state->indexListState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(
            commandList,
            state->countBuffer.Get(),
            state->countState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{ ShaderResourceDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(state->rootSignature.Get());
        commandList->SetPipelineState(state->pipeline.Get());
        commandList->SetComputeRootConstantBufferView(
            0,
            constantUpload.gpuAddress);
        commandList->SetComputeRootDescriptorTable(
            1,
            lightBinding->descriptor);
        commandList->SetComputeRootDescriptorTable(
            2,
            m_resourceDomain->ShaderResourceGpuHandle(
                *state->indexListAccessSlot));
        commandList->SetComputeRootDescriptorTable(
            3,
            m_resourceDomain->ShaderResourceGpuHandle(
                *state->countAccessSlot));
        commandList->Dispatch(
            (ClusteredLights::ClusterCount + 63u) / 64u,
            1u,
            1u);
        // 描画のpixel shaderが読めるstateへ戻します。
        TransitionResource(
            commandList,
            state->lightBuffer.Get(),
            state->lightBufferState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            state->indexListBuffer.Get(),
            state->indexListState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            state->countBuffer.Get(),
            state->countState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        lighting.clustered.lights = state->m_lightView;
        lighting.clustered.lightIndices = state->m_indexListView;
        lighting.clustered.clusterCounts = state->m_countView;
        lighting.clustered.nearPlane = nearPlane;
        lighting.clustered.farPlane = farPlane;
        lighting.clustered.inverseWidth =
            1.0f / static_cast<float>(std::max(width, 1u));
        lighting.clustered.inverseHeight =
            1.0f / static_cast<float>(std::max(height, 1u));
        lighting.clustered.lightCount = lightCount;
        lighting.clustered.enabled = true;
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
        const auto* const volumePayload = payload == nullptr
            ? TryTexture3DPayload(texture, m_resourceDomain.get())
            : nullptr;
        if (payload == nullptr && volumePayload == nullptr)
        {
            throw std::invalid_argument(
                "CreateShaderResourceView requires a texture from this "
                "backend generation.");
        }
        return CreateShaderResourceView(
            texture,
            GraphicsTextureViewDescription{
                0,
                payload != nullptr
                    ? payload->description.mipLevels
                    : volumePayload->description.mipLevels
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

    std::pair<GraphicsTextureHandle, GraphicsViewHandle>
        D3D12Backend::CreateTextureCube(
            const GraphicsTexture2DDescription& faceDescription,
            const std::span<const GraphicsTextureSubresourceData>
                subresources)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateTextureCube requires an initialized D3D12 backend.");
        }
        constexpr std::uint32_t FaceCount = 6u;
        if (faceDescription.width == 0
            || faceDescription.width != faceDescription.height
            || faceDescription.width > D3D12_REQ_TEXTURECUBE_DIMENSION
            || faceDescription.mipLevels == 0
            || faceDescription.mipLevels > Detail::MaximumTextureMipLevels(
                faceDescription.width,
                faceDescription.height)
            || faceDescription.updateMode
                != GraphicsTextureUpdateMode::Immutable
            || subresources.size()
                != static_cast<std::size_t>(faceDescription.mipLevels)
                    * FaceCount)
        {
            throw std::invalid_argument(
                "CreateTextureCube received an invalid description or "
                "subresource count.");
        }
        const auto format = Detail::ToDxgiTextureFormat(
            faceDescription.format);
        for (std::size_t index{}; index < subresources.size(); ++index)
        {
            Detail::ValidateTexture2DSubresourceData(
                format,
                faceDescription.width,
                faceDescription.height,
                faceDescription.mipLevels,
                static_cast<std::uint32_t>(
                    index % faceDescription.mipLevels),
                subresources[index]);
        }

        D3D12_RESOURCE_DESC nativeDescription{};
        nativeDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        nativeDescription.Width = faceDescription.width;
        nativeDescription.Height = faceDescription.height;
        nativeDescription.DepthOrArraySize = FaceCount;
        nativeDescription.MipLevels =
            static_cast<UINT16>(faceDescription.mipLevels);
        nativeDescription.Format = format;
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        nativeDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

        const auto domain = m_resourceDomain;
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &nativeDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(native.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(texture cube)",
            m_device.Get());
        // 転送完了時に全面・全ミップをPIXEL_SHADER_RESOURCEへ移します。
        SubmitTextureUpload(native, 0u, subresources, false);

        auto texture = Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D12ShadowTexturePayload>(domain, native));
        D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = format;
        viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        viewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        viewDescription.TextureCube.MostDetailedMip = 0;
        viewDescription.TextureCube.MipLevels = faceDescription.mipLevels;
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            m_device->CreateShaderResourceView(
                native.Get(),
                &viewDescription,
                domain->ShaderResourceCpuHandle(slot));
            auto view = Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    texture,
                    slot,
                    faceDescription.width,
                    faceDescription.height,
                    D3D12_SRV_DIMENSION_TEXTURECUBE));
            return { std::move(texture), std::move(view) };
        }
        catch (...)
        {
            domain->ReleaseUnpublishedShaderResourceSlot(slot);
            throw;
        }
    }

    D3D12Backend::ComputeCubeTarget D3D12Backend::CreateComputeCubeTarget(
        const std::uint32_t size,
        const std::uint32_t mipLevels)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateComputeCubeTarget requires an initialized D3D12 "
                "backend.");
        }
        constexpr std::uint16_t FaceCount = 6u;
        constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (size == 0u
            || size > D3D12_REQ_TEXTURECUBE_DIMENSION
            || mipLevels == 0u
            || mipLevels > Detail::MaximumTextureMipLevels(size, size))
        {
            throw std::invalid_argument(
                "CreateComputeCubeTarget received an invalid size or mip "
                "count.");
        }

        D3D12_RESOURCE_DESC nativeDescription{};
        nativeDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        nativeDescription.Width = size;
        nativeDescription.Height = size;
        nativeDescription.DepthOrArraySize = FaceCount;
        nativeDescription.MipLevels = static_cast<UINT16>(mipLevels);
        nativeDescription.Format = Format;
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        nativeDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto domain = m_resourceDomain;
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &nativeDescription,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(native.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(compute cube)",
            m_device.Get());

        // payloadが受け取るまでは、作ったUAV slotをここで戻します。
        std::vector<std::uint32_t> accessSlots;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mipAccess;
        std::shared_ptr<D3D12ComputeCubeTexturePayload> payload;
        try
        {
            accessSlots.reserve(mipLevels);
            mipAccess.reserve(mipLevels);
            for (std::uint32_t mip{}; mip < mipLevels; ++mip)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC accessDescription{};
                accessDescription.Format = Format;
                accessDescription.ViewDimension =
                    D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                accessDescription.Texture2DArray.MipSlice = mip;
                accessDescription.Texture2DArray.FirstArraySlice = 0;
                accessDescription.Texture2DArray.ArraySize = FaceCount;
                accessSlots.push_back(domain->AllocateShaderResourceSlot());
                m_device->CreateUnorderedAccessView(
                    native.Get(),
                    nullptr,
                    &accessDescription,
                    domain->ShaderResourceCpuHandle(accessSlots.back()));
                mipAccess.push_back(
                    domain->ShaderResourceGpuHandle(accessSlots.back()));
            }
            payload = std::make_shared<D3D12ComputeCubeTexturePayload>(
                domain,
                native,
                accessSlots);
        }
        catch (...)
        {
            for (const auto slot : accessSlots)
            {
                domain->ReleaseUnpublishedShaderResourceSlot(slot);
            }
            throw;
        }

        ComputeCubeTarget target;
        target.texture = Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::move(payload));
        target.mipAccess = std::move(mipAccess);
        D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = Format;
        viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        viewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        viewDescription.TextureCube.MostDetailedMip = 0;
        viewDescription.TextureCube.MipLevels = mipLevels;
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            m_device->CreateShaderResourceView(
                native.Get(),
                &viewDescription,
                domain->ShaderResourceCpuHandle(slot));
            target.view = Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    target.texture,
                    slot,
                    size,
                    size,
                    D3D12_SRV_DIMENSION_TEXTURECUBE));
        }
        catch (...)
        {
            domain->ReleaseUnpublishedShaderResourceSlot(slot);
            throw;
        }
        return target;
    }

    D3D12Backend::ShaderResourceBinding D3D12Backend::BeginCubeCompute(
        const GraphicsViewHandle& source,
        const GraphicsTextureHandle& target)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "BeginCubeCompute requires an initialized D3D12 backend.");
        }
        const auto binding = TryResolveShaderResource(source);
        const auto* const sourceTexture =
            Detail::GraphicsResourceHandleAccess::TextureResource(source);
        auto* const sourceNative = sourceTexture != nullptr
            ? TryNativeTexture(*sourceTexture, m_resourceDomain.get())
            : nullptr;
        const auto* const targetPayload =
            TryComputeCubePayload(target, m_resourceDomain.get());
        if (!binding
            || (binding->dimension != D3D12_SRV_DIMENSION_TEXTURECUBE
                && binding->dimension != D3D12_SRV_DIMENSION_TEXTURE2D)
            || sourceNative == nullptr
            || targetPayload == nullptr
            || targetPayload->native.Get() == sourceNative)
        {
            throw std::invalid_argument(
                "BeginCubeCompute requires a current TextureCube or Texture2D "
                "source and a separate compute cube target from this "
                "backend generation.");
        }

        OpenCommandList();
        TransitionComputeInputs(
            m_commandList.Get(),
            std::array<ID3D12Resource*, 2>{ sourceNative, nullptr },
            true);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = targetPayload->native.Get();
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_commandList->ResourceBarrier(1, &barrier);
        return *binding;
    }

    void D3D12Backend::EndCubeCompute(
        const GraphicsViewHandle& source,
        const GraphicsTextureHandle& target) noexcept
    {
        if (!IsInitialized() || !m_commandListOpen)
        {
            return;
        }
        const auto* const targetPayload =
            TryComputeCubePayload(target, m_resourceDomain.get());
        if (targetPayload != nullptr)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = targetPayload->native.Get();
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &barrier);
        }
        const auto* const sourceTexture =
            Detail::GraphicsResourceHandleAccess::TextureResource(source);
        TransitionComputeInputs(
            m_commandList.Get(),
            std::array<ID3D12Resource*, 2>{
                sourceTexture != nullptr
                    ? TryNativeTexture(*sourceTexture, m_resourceDomain.get())
                    : nullptr,
                nullptr },
            false);
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
        const auto* const volumePayload = payload == nullptr
            ? TryTexture3DPayload(texture, m_resourceDomain.get())
            : nullptr;
        const auto availableMipLevels = payload != nullptr
            ? payload->description.mipLevels
            : volumePayload != nullptr
                ? volumePayload->description.mipLevels
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

        // D3D11と同じく、Texture3Dは3D次元のviewで読みます。
        D3D12_SHADER_RESOURCE_VIEW_DESC nativeDescription{};
        nativeDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (payload != nullptr)
        {
            nativeDescription.Format = payload->format;
            nativeDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nativeDescription.Texture2D.MostDetailedMip =
                description.mostDetailedMip;
            nativeDescription.Texture2D.MipLevels = description.mipLevels;
        }
        else
        {
            nativeDescription.Format = volumePayload->format;
            nativeDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            nativeDescription.Texture3D.MostDetailedMip =
                description.mostDetailedMip;
            nativeDescription.Texture3D.MipLevels = description.mipLevels;
        }
        auto* const native = payload != nullptr
            ? payload->native.Get()
            : volumePayload->native.Get();
        const auto width = payload != nullptr
            ? payload->description.width
            : volumePayload->description.width;
        const auto height = payload != nullptr
            ? payload->description.height
            : volumePayload->description.height;

        const auto domain = m_resourceDomain;
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            m_device->CreateShaderResourceView(
                native,
                &nativeDescription,
                domain->ShaderResourceCpuHandle(slot));
            return Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    texture,
                    slot,
                    width,
                    height,
                    nativeDescription.ViewDimension));
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
        const GraphicsTexture3DDescription& description,
        const std::span<const GraphicsTextureSubresourceData> initialData)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateTexture3D requires an initialized D3D12 backend.");
        }
        if (description.width == 0
            || description.height == 0
            || description.depth == 0
            || description.width > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.height > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.depth > D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
            || description.mipLevels == 0
            || description.mipLevels > Detail::MaximumTextureMipLevels(
                description.width,
                description.height,
                description.depth)
            || initialData.size() != description.mipLevels)
        {
            throw std::invalid_argument(
                "CreateTexture3D received an invalid description or "
                "subresource count.");
        }

        // D3D11と同じく、shaderから読めるTexture3D形式だけを受け付けます。
        const auto format = Detail::ToDxgiTextureFormat(description.format);
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{ format };
        const auto requiredSupport =
            D3D12_FORMAT_SUPPORT1_TEXTURE3D
            | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
        if (FAILED(m_device->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT,
                &formatSupport,
                sizeof(formatSupport)))
            || (formatSupport.Support1 & requiredSupport) != requiredSupport)
        {
            throw std::invalid_argument(
                "CreateTexture3D requires a shader-readable 3D texture "
                "format supported by the active backend.");
        }
        for (std::size_t mipLevel{};
            mipLevel < initialData.size();
            ++mipLevel)
        {
            Detail::ValidateTexture3DSubresourceData(
                format,
                description.width,
                description.height,
                description.depth,
                description.mipLevels,
                static_cast<std::uint32_t>(mipLevel),
                initialData[mipLevel]);
        }

        D3D12_RESOURCE_DESC nativeDescription{};
        nativeDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        nativeDescription.Width = description.width;
        nativeDescription.Height = description.height;
        nativeDescription.DepthOrArraySize =
            static_cast<UINT16>(description.depth);
        nativeDescription.MipLevels =
            static_cast<UINT16>(description.mipLevels);
        nativeDescription.Format = format;
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        nativeDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

        const auto domain = m_resourceDomain;
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &nativeDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(texture.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(texture 3D)",
            m_device.Get());
        // D3D11のimmutable Texture3Dと同じく全ミップを転送し、完了時に
        // PIXEL_SHADER_RESOURCEへ移します。
        SubmitTextureUpload(texture, 0u, initialData, false);
        return Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D12Texture3DPayload>(
                domain,
                std::move(texture),
                description,
                format));
    }

    bool D3D12Backend::IsViewCurrent(
        const GraphicsViewHandle& view) const noexcept
    {
        return TryShaderResourceViewPayload(
            view,
            m_resourceDomain.get()) != nullptr;
    }

    void D3D12Backend::InitializeClusteredLights(
        ClusteredLights& clusteredLights,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "InitializeClusteredLights requires an initialized D3D12 "
                "backend.");
        }

        // native資源と3本のneutral viewを一時stateへ全て作り、完成した
        // 世代だけをfacadeへ公開します。途中失敗時は既存stateを保ちます。
        auto state = std::make_unique<D3D12ClusteredLightsState>();
        state->resourceDomain = m_resourceDomain;

        // D3D11と同じLamaPonLightCulling.hlslのCSMainを、b0の定数、t0の
        // ライト一覧、u0の番号表、u1のクラスタごとの数で実行します。
        const auto byteCode = CompileShaderCached(
            assets,
            shaderPath,
            "CSMain",
            "cs_5_0");
        std::array<D3D12_DESCRIPTOR_RANGE, 3> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 1;
        std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (std::size_t index{}; index < ranges.size(); ++index)
        {
            auto& parameter = parameters[index + 1u];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            std::string message =
                "D3D12SerializeRootSignature(light culling) failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            m_device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(state->rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(light culling)",
            m_device.Get());
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
        pipelineDescription.pRootSignature = state->rootSignature.Get();
        pipelineDescription.CS = {
            byteCode->GetBufferPointer(),
            byteCode->GetBufferSize()
        };
        ThrowIfFailed(
            m_device->CreateComputePipelineState(
                &pipelineDescription,
                IID_PPV_ARGS(state->pipeline.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateComputePipelineState(light culling)",
            m_device.Get());

        const auto createBuffer = [this](
            const std::uint64_t bytes,
            const D3D12_RESOURCE_FLAGS flags,
            const char* const operation)
        {
            const auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
            auto description = BufferDescription(bytes);
            description.Flags = flags;
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            ThrowIfFailed(
                m_device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &description,
                    D3D12_RESOURCE_STATE_COMMON,
                    nullptr,
                    IID_PPV_ARGS(buffer.GetAddressOf())),
                operation,
                m_device.Get());
            return buffer;
        };
        const auto createStructuredView = [this](
            const Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
            const std::uint32_t elementCount,
            const std::uint32_t stride)
        {
            auto bufferHandle =
                Detail::GraphicsResourceHandleAccess::MakeBuffer(
                    std::make_shared<D3D12BufferPayload>(
                        m_resourceDomain,
                        buffer));
            D3D12_SHADER_RESOURCE_VIEW_DESC description{};
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            description.Buffer.NumElements = elementCount;
            description.Buffer.StructureByteStride = stride;
            const auto slot = m_resourceDomain->AllocateShaderResourceSlot();
            try
            {
                m_device->CreateShaderResourceView(
                    buffer.Get(),
                    &description,
                    m_resourceDomain->ShaderResourceCpuHandle(slot));
                return Detail::GraphicsResourceHandleAccess::MakeView(
                    std::make_shared<D3D12ShaderResourceViewPayload>(
                        m_resourceDomain,
                        std::move(bufferHandle),
                        slot,
                        elementCount));
            }
            catch (...)
            {
                m_resourceDomain->ReleaseUnpublishedShaderResourceSlot(slot);
                throw;
            }
        };
        const auto createAccessSlot = [this](
            const Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
            const std::uint32_t elementCount)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            description.Buffer.NumElements = elementCount;
            description.Buffer.StructureByteStride =
                static_cast<UINT>(sizeof(std::uint32_t));
            const auto slot = m_resourceDomain->AllocateShaderResourceSlot();
            m_device->CreateUnorderedAccessView(
                buffer.Get(),
                nullptr,
                &description,
                m_resourceDomain->ShaderResourceCpuHandle(slot));
            return slot;
        };

        const std::uint32_t indexCount = ClusteredLights::ClusterCount
            * ClusteredLights::MaximumLightsPerCluster;
        const auto uintStride = static_cast<std::uint32_t>(
            sizeof(std::uint32_t));
        state->lightBuffer = createBuffer(
            static_cast<std::uint64_t>(sizeof(GpuLight))
                * MaximumClusteredLights,
            D3D12_RESOURCE_FLAG_NONE,
            "ID3D12Device::CreateCommittedResource(cluster lights)");
        state->indexListBuffer = createBuffer(
            static_cast<std::uint64_t>(uintStride) * indexCount,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            "ID3D12Device::CreateCommittedResource(cluster index list)");
        state->countBuffer = createBuffer(
            static_cast<std::uint64_t>(uintStride)
                * ClusteredLights::ClusterCount,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            "ID3D12Device::CreateCommittedResource(cluster counts)");
        state->indexListAccessSlot =
            createAccessSlot(state->indexListBuffer, indexCount);
        state->countAccessSlot = createAccessSlot(
            state->countBuffer,
            ClusteredLights::ClusterCount);
        state->m_lightView = createStructuredView(
            state->lightBuffer,
            static_cast<std::uint32_t>(MaximumClusteredLights),
            static_cast<std::uint32_t>(sizeof(GpuLight)));
        state->m_indexListView = createStructuredView(
            state->indexListBuffer,
            indexCount,
            uintStride);
        state->m_countView = createStructuredView(
            state->countBuffer,
            ClusteredLights::ClusterCount,
            uintStride);
        state->m_initialized = true;
        Detail::ClusteredLightsBackendAccess::Publish(
            clusteredLights,
            std::move(state));
    }
}
