#include "LamaPon/Graphics/D3D12Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/D3D12DebugDrawingBackend.h"
#include "LamaPon/Graphics/D3D12GpuProfilerBackend.h"
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
    // 失敗理由を例外にする(result: 結果コード, operation: 操作名, device: 原因照会先)。
    [[noreturn]] void ThrowHResult(
        const HRESULT result,
        const char* const operation,
        ID3D12Device* const device = nullptr)
    {
        // 操作名と失敗理由
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

    // 失敗時に例外を送出する(result: 結果コード, operation: 操作名, device: 原因照会先)。
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

    // ティアリング対応を調べる(factory: DXGI生成元)。
    [[nodiscard]] bool QueryTearingSupport(
        IDXGIFactory4* const factory) noexcept
    {
        // 対応確認用DXGI生成元
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        if (factory == nullptr
            || FAILED(factory->QueryInterface(
                IID_PPV_ARGS(factory5.GetAddressOf()))))
        {
            return false;
        }
        // ティアリング許可
        BOOL allowed = FALSE;
        return SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowed,
                sizeof(allowed)))
            && allowed != FALSE;
    }

    // 記述子の位置を進める(handle: 基点, index: 枠番号, increment: 枠のバイト幅)。
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE OffsetDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        const std::size_t index,
        const std::uint32_t increment) noexcept
    {
        handle.ptr += static_cast<SIZE_T>(index) * increment;
        return handle;
    }

    // 単一GPU用ヒープを設定する(type: メモリー種別)。
    [[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(
        const D3D12_HEAP_TYPE type) noexcept
    {
        // ヒープの生成設定
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = type;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    // バッファーの生成設定を作る(size: バイト数)。
    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::uint64_t size) noexcept
    {
        // バッファーの生成設定
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

    // 非ASCII文字を置換してGPU名を得る(description: GPU情報)。
    [[nodiscard]] std::string AdapterName(
        const DXGI_ADAPTER_DESC1& description)
    {
        // ログ表示用GPU名
        std::string name;
        // GPU名の文字
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

}

namespace LamaPon::Detail
{
    // 任意スレッドで手放した資源を、この世代の次回フェンス完了まで保持する。
    class D3D12ResourceDomain final
        : public GraphicsResourceDomain
    {
    public:
        // SRVとUAVの共有枠数
        static constexpr std::uint32_t ShaderResourceCapacity = 16384u;

        // 世代別の記述子を管理する(shaderResourceHeap: SRV・UAVヒープ, descriptorSize: 枠のバイト幅)。
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

        // この世代のSRV・UAVヒープを返す。
        [[nodiscard]] ID3D12DescriptorHeap*
            ShaderResourceHeap() const noexcept
        {
            return m_shaderResourceHeap.Get();
        }

        // 未使用または回収済みの枠を確保する。
        [[nodiscard]] std::uint32_t AllocateShaderResourceSlot()
        {
            // 枠管理の排他ロック
            std::scoped_lock lock(m_mutex);
            if (m_closed)
            {
                throw std::logic_error(
                    "The D3D12 resource domain has been shut down.");
            }
            if (!m_freeSlots.empty())
            {
                // 再利用する枠番号
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

        // 未公開の枠を即時に戻す(slot: 枠番号)。
        void ReleaseUnpublishedShaderResourceSlot(
            const std::uint32_t slot) noexcept
        {
            try
            {
                // 枠管理の排他ロック
                std::scoped_lock lock(m_mutex);
                m_freeSlots.push_back(slot);
            }
            catch (...)
            {
                // 戻せないslotは再利用しないだけで、安全性は損ないません。
            }
        }

        // CPU側の記述子位置を得る(slot: 枠番号)。
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE
            ShaderResourceCpuHandle(
                const std::uint32_t slot) const noexcept
        {
            // CPU側の記述子位置
            auto handle = m_cpuStart;
            handle.ptr += static_cast<SIZE_T>(slot) * m_descriptorSize;
            return handle;
        }

        // GPU側の記述子位置を得る(slot: 枠番号)。
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE
            ShaderResourceGpuHandle(
                const std::uint32_t slot) const noexcept
        {
            // GPU側の記述子位置
            auto handle = m_gpuStart;
            handle.ptr += static_cast<UINT64>(slot) * m_descriptorSize;
            return handle;
        }

        // 退避後に待つフェンス値を更新する(value: 次回の発行値)。
        void PublishNextFrameFenceValue(
            const std::uint64_t value) noexcept
        {
            m_nextFrameFenceValue.store(value);
        }

        // 次回フェンス完了まで解放を遅らせる(resource: GPU資源, slot: 任意の記述子枠)。
        void Retire(
            Microsoft::WRL::ComPtr<ID3D12Resource> resource,
            const std::optional<std::uint32_t> slot) noexcept
        {
            // 次回フェンス待ちの退避項目
            RetiredResource retired{
                m_nextFrameFenceValue.load(),
                std::move(resource),
                slot.value_or(0u),
                slot.has_value()
            };
            try
            {
                // 退避一覧の排他ロック
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
                // 退避先を確保できなければ、GPUが参照中かもしれないresourceを解放せず意図的に保持し続けます。
                if (retired.resource != nullptr)
                {
                    retired.resource->AddRef();
                }
            }
        }

        // ヒープを次回フェンスまで保持する(heap: 退避するヒープ)。
        void RetireDescriptorHeap(
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap) noexcept
        {
            if (heap == nullptr)
            {
                return;
            }
            // 次回フェンス待ちのヒープ
            RetiredResource retired{
                m_nextFrameFenceValue.load(),
                nullptr,
                0u,
                false,
                std::move(heap)
            };
            try
            {
                // 退避一覧の排他ロック
                std::scoped_lock lock(m_mutex);
                if (!m_closed)
                {
                    m_retired.push_back(std::move(retired));
                    return;
                }
            }
            catch (...)
            {
                // GPUがdescriptorを参照中かもしれないため、退避できない場合は意図的に解放しません。
                if (retired.descriptorHeap != nullptr)
                {
                    retired.descriptorHeap->AddRef();
                }
            }
        }

        // 完了済み資源をロック外で解放する(completedValue: 完了フェンス値)。
        void CollectCompleted(
            const std::uint64_t completedValue) noexcept
        {
            // ロック外で解放する資源
            std::vector<RetiredResource> released;
            try
            {
                // 退避一覧の排他ロック
                std::scoped_lock lock(m_mutex);
                // 回収対象を先頭へ分ける(entry: 退避項目)。
                const auto pending = std::partition(
                    m_retired.begin(),
                    m_retired.end(),
                    [completedValue](
                        const RetiredResource& entry) noexcept
                    {
                        return entry.fenceValue <= completedValue;
                    });
                // 完了済みの退避項目数
                const auto completedCount = static_cast<std::size_t>(
                    std::distance(m_retired.begin(), pending));
                if (completedCount == 0)
                {
                    return;
                }
                // 途中で失敗して同じslotを二重に戻さないよう、確保を先に済ませてから状態を変更します。
                released.reserve(completedCount);
                m_freeSlots.reserve(
                    m_freeSlots.size() + completedCount);
                // 回収中の退避項目
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

        // GPU停止後に退避資源を解放し、以後の退避を即時解放に切り替える。
        void Close() noexcept
        {
            // ロック外で解放する資源
            std::vector<RetiredResource> released;
            try
            {
                // 終了状態の排他ロック
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
            // 解放を待つフェンス値
            std::uint64_t fenceValue{};
            // 完了まで保持するGPU資源
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            // 回収する記述子枠
            std::uint32_t slot{};
            // 記述子枠の有無
            bool hasSlot{};
            // 完了まで保持するヒープ
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
                descriptorHeap;
        };

        // この世代のSRV・UAVヒープ
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shaderResourceHeap;
        // CPU側のヒープ先頭
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        // GPU側のヒープ先頭
        D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};
        // 記述子枠のバイト幅
        std::uint32_t m_descriptorSize{};
        // 枠・退避一覧の排他制御
        std::mutex m_mutex;
        // 再利用できる枠番号
        std::vector<std::uint32_t> m_freeSlots;
        // GPU完了待ちの退避一覧
        std::vector<RetiredResource> m_retired;
        // 退避後に待つフェンス値
        std::atomic<std::uint64_t> m_nextFrameFenceValue{ 1u };
        // 次の未使用枠番号
        std::uint32_t m_nextSlot{};
        // GPU停止後の閉鎖状態
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
        // 2Dテクスチャーを所有する(resourceDomain: 所有するGPU世代, texture: GPU資源, textureDescription: 公開設定, textureFormat: ネイティブ形式)。
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

        // テクスチャーの解放をGPU完了後へ遅らせる。
        ~D3D12TexturePayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        // 所有する2Dテクスチャー
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        // 2Dテクスチャーの公開設定
        LamaPon::GraphicsTexture2DDescription description;
        // ネイティブの画素形式
        DXGI_FORMAT format{};
    };

    // ベイクした間接光などが読む、初期dataから作る変更不可のTexture3Dです。
    class D3D12Texture3DPayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        // 3Dテクスチャーを所有する(resourceDomain: 所有するGPU世代, texture: GPU資源, textureDescription: 公開設定, textureFormat: ネイティブ形式)。
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

        // 3Dテクスチャーの解放をGPU完了後へ遅らせる。
        ~D3D12Texture3DPayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        // 所有する3Dテクスチャー
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        // 3Dテクスチャーの公開設定
        LamaPon::GraphicsTexture3DDescription description;
        // ネイティブの画素形式
        DXGI_FORMAT format{};
    };

    // 配列・キューブを専用実体で所有し、公開側には参照ビューを渡す。
    class D3D12ShadowTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        // 配列・キューブ資源を所有する(resourceDomain: 所有するGPU世代, texture: GPU資源)。
        D3D12ShadowTexturePayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> texture)
            : GraphicsTexturePayload(resourceDomain)
            , native(std::move(texture))
        {
        }

        // 配列・キューブの解放をGPU完了後へ遅らせる。
        ~D3D12ShadowTexturePayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        // 所有する配列・キューブ
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
    };

    // IBL計算先と各ミップのUAV枠を同じ寿命で所有する。
    class D3D12ComputeCubeTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        // 計算用キューブとUAVを所有する(resourceDomain: 所有するGPU世代, texture: GPU資源, accessSlots: ミップごとのUAV枠)。
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

        // キューブと各UAV枠の解放をGPU完了後へ遅らせる。
        ~D3D12ComputeCubeTexturePayload() noexcept override
        {
            // 解放時期を管理するGPU世代
            auto& domain = static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain());
            // 回収するミップのUAV枠
            for (const auto slot : unorderedAccessSlots)
            {
                domain.Retire(nullptr, slot);
            }
            domain.Retire(std::move(native), std::nullopt);
        }

        // 所有する計算用キューブ
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        // 所有する各ミップのUAV枠
        std::vector<std::uint32_t> unorderedAccessSlots;
    };

    // 共通Buffer handleが所有するDirectX 12側のbufferです。
    class D3D12BufferPayload final
        : public LamaPon::Detail::GraphicsBufferPayload
    {
    public:
        // バッファーを所有する(resourceDomain: 所有するGPU世代, buffer: GPU資源, dynamicVertexCapacity: 頂点容量、0は非頂点)。
        D3D12BufferPayload(
            const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>&
                resourceDomain,
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer,
            const std::uint32_t dynamicVertexCapacity = 0u)
            : GraphicsBufferPayload(resourceDomain)
            , native(std::move(buffer))
            , vertexCapacity(dynamicVertexCapacity)
        {
        }

        // バッファーの解放をGPU完了後へ遅らせる。
        ~D3D12BufferPayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(std::move(native), std::nullopt);
        }

        // 所有するGPUバッファー
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        // 頂点用の容量、0は非頂点
        std::uint32_t vertexCapacity{};
    };

    // SRV枠と参照先を所有し、基底クラスが参照先の寿命を保つ。
    class D3D12ShaderResourceViewPayload final
        : public LamaPon::Detail::GraphicsViewPayload
    {
    public:
        // テクスチャーとSRV枠を所有する(resourceDomain: 所有するGPU世代, texture: 参照先の所有ハンドル, descriptorSlot: SRV枠, textureWidth: 幅, textureHeight: 高さ, viewDimension: 参照次元)。
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

        // バッファーとSRV枠を所有する(resourceDomain: 所有するGPU世代, buffer: 参照先の所有ハンドル, descriptorSlot: SRV枠, elementCount: widthに格納する要素数)。
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

        // SRV枠の再利用をGPU完了後へ遅らせる。
        ~D3D12ShaderResourceViewPayload() noexcept override
        {
            static_cast<LamaPon::Detail::D3D12ResourceDomain&>(
                *Domain()).Retire(nullptr, slot);
        }

        // 所有するSRV枠
        std::uint32_t slot{};
        // 幅、バッファーでは要素数
        std::uint32_t width{};
        // 高さ、バッファーでは1
        std::uint32_t height{};
        // 参照する資源の次元
        D3D12_SRV_DIMENSION dimension{ D3D12_SRV_DIMENSION_TEXTURE2D };
    };

    // LamaPonLightCulling.hlslのb0と同じ112バイト配置を保つ。
    struct ClusterCullingConstants final
    {
        // ワールドから視点への変換
        DirectX::XMFLOAT4X4 view{};
        // XYZ格子数・Wライト数
        DirectX::XMFLOAT4 gridParameters{};
        // 近平面・遠平面・対数幅・上限
        DirectX::XMFLOAT4 depthParameters{};
        // 縦横の半画角の正接
        DirectX::XMFLOAT4 frustumParameters{};
    };

    // ライトを転送し、計算で作った番号表と件数をピクセルシェーダーへ渡す。
    struct D3D12ClusteredLightsState final
        : LamaPon::Detail::ClusteredLightsBackendState
    {
        // 計算用UAV枠の再利用をGPU完了後へ遅らせる。
        ~D3D12ClusteredLightsState() noexcept override
        {
            if (resourceDomain == nullptr)
            {
                return;
            }
            // 回収する計算用UAV枠
            for (const auto& slot : { indexListAccessSlot, countAccessSlot })
            {
                if (slot.has_value())
                {
                    resourceDomain->Retire(nullptr, *slot);
                }
            }
        }

        // 計算に必要な資源とUAVがそろっているか調べる。
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

        // 所有するGPU世代
        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain> resourceDomain;
        // ライト分類の入力配置
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        // ライト分類の計算PSO
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
        // GPU側のライト一覧
        Microsoft::WRL::ComPtr<ID3D12Resource> lightBuffer;
        // クラスタ別ライト番号表
        Microsoft::WRL::ComPtr<ID3D12Resource> indexListBuffer;
        // クラスタ別ライト数
        Microsoft::WRL::ComPtr<ID3D12Resource> countBuffer;
        // ライト一覧の資源状態
        D3D12_RESOURCE_STATES lightBufferState{ D3D12_RESOURCE_STATE_COMMON };
        // ライト番号表の資源状態
        D3D12_RESOURCE_STATES indexListState{ D3D12_RESOURCE_STATE_COMMON };
        // ライト数表の資源状態
        D3D12_RESOURCE_STATES countState{ D3D12_RESOURCE_STATE_COMMON };
        // 番号表のUAV枠
        std::optional<std::uint32_t> indexListAccessSlot;
        // ライト数表のUAV枠
        std::optional<std::uint32_t> countAccessSlot;
    };

    struct D3D12ShadowMapState final
        : LamaPon::Detail::ShadowMapBackendState
    {
        // 描画に必要な影資源がそろっているか調べる。
        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && texture != nullptr
                && depthStencilHeap != nullptr
                && descriptorSize != 0u
                && m_cascadeCount != 0u;
        }

        // カスケード・六面の影深度
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        // 各影面の深度記述子
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>
            depthStencilHeap;
        // 影深度の資源状態
        D3D12_RESOURCE_STATES resourceState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // DSV枠のバイト幅
        std::uint32_t descriptorSize{};
        // 六面キューブの指定
        bool cube{};
    };

    // 四分の一寸法から1×1までの測定段を作る(width: 元の幅, height: 元の高さ)。
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>>
        LuminanceLevelSizes(
            const std::uint32_t width,
            const std::uint32_t height)
    {
        // 各測定段の幅と高さ
        std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes;
        // 追加する測定段の寸法
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
        // 最後の段を1×1に縮小する輝度測定用資源。
        struct LuminanceLevel final
        {
            // この段の対数平均輝度
            Microsoft::WRL::ComPtr<ID3D12Resource> texture;
            // この段の輝度参照
            LamaPon::GraphicsViewHandle view;
            // 測定テクスチャーの状態
            D3D12_RESOURCE_STATES state{
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
            // 測定段の描画範囲
            D3D12_VIEWPORT viewport{};
            // 測定段の切り取り範囲
            D3D12_RECT scissor{};
        };

        // 未平滑化SSAOのRTV枠
        static constexpr std::uint32_t OcclusionRenderTargetSlot = 2u;
        // 平滑化SSAOのRTV枠
        static constexpr std::uint32_t OcclusionBlurRenderTargetSlot = 3u;
        // ストリーク第一描画先の枠
        static constexpr std::uint32_t LensFlareFirstRenderTargetSlot = 4u;
        // ストリーク第二描画先の枠
        static constexpr std::uint32_t LensFlareSecondRenderTargetSlot = 5u;
        // 輝度測定用RTVの先頭枠
        static constexpr std::uint32_t LuminanceRenderTargetSlot = 6u;

        // 描画先の全資源と記述子の解放をGPU完了後へ遅らせる。
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
                // 深度・読戻しを含む退避資源
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
                // 退避するGPU資源
                for (auto* const resource : resources)
                {
                    if (*resource != nullptr)
                    {
                        resourceDomain->Retire(
                            std::move(*resource),
                            std::nullopt);
                    }
                }
                // 退避する輝度測定段
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

        // 描画・測定に必要な全資源がそろっているか調べる。
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

        // 所有するGPU世代
        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>
            resourceDomain;
        // 描画中のHDRカラー
        Microsoft::WRL::ComPtr<ID3D12Resource> color;
        // 後処理の書き込み先
        Microsoft::WRL::ComPtr<ID3D12Resource> postColor;
        // 公開表示用カラー
        Microsoft::WRL::ComPtr<ID3D12Resource> displayColor;
        // SSR用の前回カラー
        Microsoft::WRL::ComPtr<ID3D12Resource> colorHistory;
        // TAA用の前回カラー
        Microsoft::WRL::ComPtr<ID3D12Resource> temporalHistory;
        // 描画先の深度・ステンシル
        Microsoft::WRL::ComPtr<ID3D12Resource> depth;
        // シェーダー参照用深度コピー
        Microsoft::WRL::ComPtr<ID3D12Resource> depthCopy;
        // 各描画先のRTVヒープ
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> renderTargetHeap;
        // 深度描画先のDSVヒープ
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depthStencilHeap;
        // 現在カラーの資源状態
        D3D12_RESOURCE_STATES colorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 後処理カラーの資源状態
        D3D12_RESOURCE_STATES postColorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 表示カラーの資源状態
        D3D12_RESOURCE_STATES displayColorState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // SSR履歴の資源状態
        D3D12_RESOURCE_STATES colorHistoryState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // TAA履歴の資源状態
        D3D12_RESOURCE_STATES temporalHistoryState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 描画用深度の資源状態
        D3D12_RESOURCE_STATES depthState{
            D3D12_RESOURCE_STATE_DEPTH_WRITE };
        // 深度コピーの資源状態
        D3D12_RESOURCE_STATES depthCopyState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 内部参照用の半解像度SSAO
        Microsoft::WRL::ComPtr<ID3D12Resource> occlusion;
        // 公開用の平滑化SSAO
        Microsoft::WRL::ComPtr<ID3D12Resource> occlusionBlur;
        // 未平滑化SSAOの資源状態
        D3D12_RESOURCE_STATES occlusionState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 平滑化SSAOの資源状態
        D3D12_RESOURCE_STATES occlusionBlurState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 内部参照用のSSAOビュー
        LamaPon::GraphicsViewHandle occlusionView;
        // SSAO用の半解像度描画範囲
        D3D12_VIEWPORT occlusionViewport{};
        // SSAO用の切り取り範囲
        D3D12_RECT occlusionScissor{};
        // 第一・第三段のストリーク
        Microsoft::WRL::ComPtr<ID3D12Resource> lensFlareFirst;
        // 第二段のストリーク
        Microsoft::WRL::ComPtr<ID3D12Resource> lensFlareSecond;
        // 第一ストリークの資源状態
        D3D12_RESOURCE_STATES lensFlareFirstState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 第二ストリークの資源状態
        D3D12_RESOURCE_STATES lensFlareSecondState{
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        // 第一ストリークの参照
        LamaPon::GraphicsViewHandle lensFlareFirstView;
        // 第二ストリークの参照
        LamaPon::GraphicsViewHandle lensFlareSecondView;
        // 四分の一寸法の描画範囲
        D3D12_VIEWPORT lensFlareViewport{};
        // ストリークの切り取り範囲
        D3D12_RECT lensFlareScissor{};
        // 全ミップを持つR32F深度階層
        Microsoft::WRL::ComPtr<ID3D12Resource> reflectionDepthPyramid;
        // 深度階層の各ミップ状態
        std::vector<D3D12_RESOURCE_STATES> reflectionDepthPyramidStates;
        // 深度階層の各ミップ参照
        std::vector<LamaPon::GraphicsViewHandle>
            reflectionDepthPyramidMipViews;
        // 深度階層の各描画範囲
        std::vector<D3D12_VIEWPORT> reflectionDepthPyramidViewports;
        // 深度階層用RTVの先頭枠
        std::uint32_t reflectionDepthPyramidRenderTargetSlot{};
        // 対数平均輝度の縮小段
        std::vector<LuminanceLevel> luminanceLevels;
        // 最終1×1輝度のCPU読戻し先
        Microsoft::WRL::ComPtr<ID3D12Resource> luminanceReadback;
        // 最終輝度のコピー配置
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT luminanceFootprint{};
        // 測定フェンス、0は未測定
        std::uint64_t luminanceFenceValue{};
        // RTV枠のバイト幅
        std::uint32_t renderTargetDescriptorSize{};
        // 元寸法の描画範囲
        D3D12_VIEWPORT viewport{};
        // 元寸法の切り取り範囲
        D3D12_RECT scissor{};
        // 表示カラーの計算書き込み
        bool computeWritable{};
        // 表示カラー用のUAV枠
        std::optional<std::uint32_t> displayUnorderedAccessSlot;
    };

    class D3D12OutputState final : public LamaPon::GraphicsOutputState
    {
    public:
        // 復元時の世代識別用所有参照
        std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>
            resourceDomain;
        // 短命な復元範囲で借りる描画先
        LamaPon::RenderTarget* offscreenTarget{};
        // 深度のみを描画する状態
        bool depthOnly{};
    };

    // 同じ世代の2D資源だけを返す(texture: 所有ハンドル, domain: 必要なGPU世代)。
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

    // 同じ世代の3D資源だけを返す(texture: 所有ハンドル, domain: 必要なGPU世代)。
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

    // 同じ世代のSRVだけを返す(view: 参照ハンドル, domain: 必要なGPU世代)。
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

    // 同じ世代の対応テクスチャー実体を返す(texture: 所有ハンドル, domain: 必要なGPU世代)。
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
        // 共通ハンドルが所有する実体
        const auto* const payload =
            GraphicsResourceHandleAccess::Payload(texture);
        // 2Dテクスチャーの実体
        if (const auto* const texture2D =
                dynamic_cast<const D3D12TexturePayload*>(payload))
        {
            return texture2D->native.Get();
        }
        // 3Dテクスチャーの実体
        if (const auto* const texture3D =
                dynamic_cast<const D3D12Texture3DPayload*>(payload))
        {
            return texture3D->native.Get();
        }
        // 配列・キューブ資源の実体
        if (const auto* const shadow =
                dynamic_cast<const D3D12ShadowTexturePayload*>(payload))
        {
            return shadow->native.Get();
        }
        // 計算用キューブの実体
        if (const auto* const computeCube =
                dynamic_cast<const D3D12ComputeCubeTexturePayload*>(payload))
        {
            return computeCube->native.Get();
        }
        return nullptr;
    }

    // 同じ世代の計算用キューブだけを返す(texture: 所有ハンドル, domain: 必要なGPU世代)。
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

    // 2D入力の実体を得て重複を除く(inputs: 二つの入力参照, domain: 必要なGPU世代)。
    [[nodiscard]] std::array<ID3D12Resource*, 2> ComputeInputTextures(
        const std::array<LamaPon::GraphicsViewHandle, 2>& inputs,
        const LamaPon::Detail::GraphicsResourceDomain* const domain) noexcept
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;
        // 遷移対象の入力資源
        std::array<ID3D12Resource*, 2> resources{};
        // 入力参照の番号
        for (std::size_t index{}; index < inputs.size(); ++index)
        {
            // 入力参照の所有テクスチャー
            const auto* const texture =
                GraphicsResourceHandleAccess::TextureResource(inputs[index]);
            // 入力テクスチャーの実体
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

    // 計算中だけ非ピクセル参照を許可する(commandList: 記録先, resources: 重複を除いた入力, reading: 計算参照の開始指定)。
    void TransitionComputeInputs(
        ID3D12GraphicsCommandList* const commandList,
        const std::array<ID3D12Resource*, 2>& resources,
        const bool reading) noexcept
    {
        // 計算中の共通参照状態
        const auto shaderStates = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // 状態を遷移する入力資源
        for (auto* const resource : resources)
        {
            if (resource == nullptr)
            {
                continue;
            }
            // 計算入力の状態遷移
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

    // 資源を所有する2D参照を作る(device: GPUデバイス, domain: 所有するGPU世代, resource: 参照先資源, nativeDescription: SRV設定, width: 幅, height: 高さ, publicFormat: 公開する画素形式)。
    [[nodiscard]] LamaPon::GraphicsViewHandle CreateTextureView(
        ID3D12Device* const device,
        const std::shared_ptr<LamaPon::Detail::D3D12ResourceDomain>& domain,
        const Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& nativeDescription,
        const std::uint32_t width,
        const std::uint32_t height,
        const LamaPon::GraphicsTextureFormat publicFormat)
    {
        // 確保した未公開SRV枠
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            device->CreateShaderResourceView(
                resource.Get(),
                &nativeDescription,
                domain->ShaderResourceCpuHandle(slot));
            // 参照ハンドルの公開設定
            LamaPon::GraphicsTexture2DDescription description;
            description.width = width;
            description.height = height;
            description.format = publicFormat;
            // 参照先の所有ハンドル
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

    // 全サブ資源の状態をそろえて追跡する(commandList: 記録先, resource: 遷移対象, currentState: 追跡中の状態, requiredState: 必要な状態)。
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
        // 全サブ資源の状態遷移
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

    // 指定サブ資源の状態だけを遷移する(commandList: 記録先, resource: 遷移対象, subresource: サブ資源番号, currentState: 追跡中の状態, requiredState: 必要な状態)。
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
        // 指定サブ資源の状態遷移
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = requiredState;
        commandList->ResourceBarrier(1, &barrier);
        currentState = requiredState;
    }

    // あふれを検出して切り上げる(value: 元のバイト数, alignment: 非ゼロの2の累乗幅)。
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

    // デバイス消失を検出しつつGPU完了を待つ(device: GPUデバイス, fence: 完了確認先, event: 通知イベント, value: 待つフェンス値)。
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
        // 待機前の完了フェンス値
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
        // 待機後の完了フェンス値
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
            // 描画先のWindowsウィンドウ
            const HWND window = static_cast<HWND>(
                createInfo.nativeWindow);
            if (window == nullptr)
            {
                throw std::invalid_argument(
                    "D3D12Backend requires a native window.");
            }
            // 初期バックバッファー幅
            const std::uint32_t width = std::max(
                createInfo.width,
                1u);
            // 初期バックバッファー高さ
            const std::uint32_t height = std::max(
                createInfo.height,
                1u);

            // デバッグ層の要求
            const bool wantDebugLayer =
#if defined(_DEBUG)
                true;
#else
                createInfo.enableDebugLayer;
#endif
            // DXGI生成時のフラグ
            UINT factoryFlags{};
            if (wantDebugLayer)
            {
                // 有効化するデバッグ層
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

            // DXGI生成の結果
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

            // 対応デバイスを試して採用する(adapter: 候補GPU)。
            const auto createDevice =
                [this](IDXGIAdapter1* const adapter)
            {
                // 候補GPUのデバイス
                Microsoft::WRL::ComPtr<ID3D12Device> device;
                // デバイス生成の結果
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

            // ソフトウェア描画の使用
            bool usingWarp = createInfo.preferWarpAdapter;
            if (!createInfo.preferWarpAdapter)
            {
                // GPU優先度を指定する生成元
                Microsoft::WRL::ComPtr<IDXGIFactory6> factory6;
                static_cast<void>(m_factory.As(&factory6));
                if (factory6)
                {
                    // 高性能GPUの候補番号
                    for (UINT index{};; ++index)
                    {
                        // 高性能順の候補GPU
                        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
                        // 候補GPUの取得結果
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
                        // 候補GPUの属性
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
                    // 通常列挙の候補番号
                    for (UINT index{};; ++index)
                    {
                        // 通常列挙の候補GPU
                        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
                        // 候補GPUの取得結果
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
                        // 候補GPUの属性
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
                // WARPの基底インターフェイス
                Microsoft::WRL::ComPtr<IDXGIAdapter> warpBase;
                ThrowIfFailed(
                    m_factory->EnumWarpAdapter(
                        IID_PPV_ARGS(warpBase.GetAddressOf())),
                    "IDXGIFactory4::EnumWarpAdapter");
                // 採用するWARPアダプター
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
            // 採用したGPUの属性
            DXGI_ADAPTER_DESC1 adapterDescription{};
            if (SUCCEEDED(m_adapter->GetDesc1(&adapterDescription)))
            {
                // 専用VRAMのMiB値
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

            // 描画コマンドキューの設定
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
            // 画面提示用バッファーの設定
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

            // 生成した画面提示先
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

            // バックバッファーRTVの設定
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

            // 画面深度用DSVの設定
            D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDescription{};
            dsvHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvHeapDescription.NumDescriptors = 1;
            ThrowIfFailed(
                m_device->CreateDescriptorHeap(
                    &dsvHeapDescription,
                    IID_PPV_ARGS(m_dsvHeap.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateDescriptorHeap(DSV)",
                m_device.Get());

            // 各フレームのコマンド記録領域
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

            // 描画で共有するSRVヒープ設定
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
            m_gpuProfilerBackend =
                std::make_unique<D3D12GpuProfilerBackend>(*this);
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
        m_gpuProfilerBackend.reset();
        m_activeShadowMap = nullptr;
        // GPU停止後に世代を閉じ、以後のハンドル破棄を即時解放に切り替える。
        if (m_resourceDomain != nullptr)
        {
            m_resourceDomain->Close();
            m_resourceDomain.reset();
        }
        m_nullShaderResourceSlots.fill(std::nullopt);
        // 各フレームの転送領域一覧
        for (auto& arena : m_frameUploadArenas)
        {
            arena.clear();
        }
        ReleaseUploadContext();
        ReleaseSizeDependentResources();
        m_infoQueue.Reset();
        m_commandList.Reset();
        // 各フレームのコマンド記録領域
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

        // GPU完了を確認できた場合だけ旧画面資源を解放する。
        try
        {
            DrainGpu();
        }
        catch (...)
        {
            m_terminalFailure = true;
            throw;
        }
        // 変更前の画面幅
        const std::uint32_t previousWidth = m_width;
        // 変更前の画面高さ
        const std::uint32_t previousHeight = m_height;
        ReleaseSizeDependentResources();
        // 画面寸法の変更結果
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
        // クリア対象の画面RTV
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
        // 蓄積した診断メッセージ数
        const auto stored = m_infoQueue->GetNumStoredMessages();
        // 診断メッセージの番号
        for (std::uint64_t index{}; index < stored; ++index)
        {
            // 診断データのバイト数
            SIZE_T length{};
            if (FAILED(m_infoQueue->GetMessage(index, nullptr, &length))
                || length == 0)
            {
                continue;
            }
            // 診断データの受け取り領域
            std::vector<std::byte> storage(length);
            // 受け取った診断メッセージ
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(
                storage.data());
            if (FAILED(m_infoQueue->GetMessage(
                    index,
                    message,
                    &length)))
            {
                continue;
            }
            // ログ出力する診断文
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
            // 提示したバックバッファー番号
            const std::uint32_t submittedIndex =
                m_currentBackBufferIndex;
            // 提示前にコマンドを記録したか
            const bool submittedCommands = m_commandListOpen;
            if (submittedCommands)
            {
                TransitionCurrentBackBuffer(
                    D3D12_RESOURCE_STATE_PRESENT);
                CloseAndExecuteOpenCommands();
            }

            // 垂直同期待ちの省略
            const bool immediate = !vSyncEnabled;
            // 画面提示の結果
            const HRESULT presented = m_swapChain->Present(
                immediate ? 0u : 1u,
                immediate && m_tearingAllowed
                    ? DXGI_PRESENT_ALLOW_TEARING
                    : 0u);
            if (submittedCommands)
            {
                // 提示後に発行するフェンス値
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
        // 取得できたGPUメモリー統計
        GraphicsVideoMemoryStatistics statistics;
        if (m_adapter == nullptr)
        {
            return statistics;
        }
        statistics.adapterAvailable = true;

        // GPUのメモリー容量情報
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

        // 使用量を問い合わせるGPU
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(m_adapter.As(&adapter3)))
        {
            return statistics;
        }
        // ローカルメモリー使用量
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        // 非ローカルメモリー使用量
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
        // 各画面提示用バッファー
        for (auto& backBuffer : m_backBuffers)
        {
            backBuffer.Reset();
        }
        m_depthBuffer.Reset();

        // 画面RTVヒープの先頭
        const auto rtvStart =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        // 初期化する画面バッファー番号
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

        // 画面深度の生成設定
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
        // 深度・ステンシルの初期値
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthClear.DepthStencil.Depth = 1.0f;
        // 画面深度のGPU専用ヒープ
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
        // 画面深度のDSV設定
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
        // 各画面提示用バッファー
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
        // このフレームのGPU完了後に限り、記録領域と転送領域を再利用する。
        ResetFrameUploadArena(m_currentBackBufferIndex);
        CollectRetiredResources();
        // GPU完了済みの記録領域
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
        // 画面バッファーの追跡状態
        auto& currentState =
            m_backBufferStates[m_currentBackBufferIndex];
        if (currentState == state)
        {
            return;
        }
        // 画面バッファーの状態遷移
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
        // 描画する画面RTV
        const auto rtv = OffsetDescriptor(
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
            m_currentBackBufferIndex,
            m_rtvDescriptorSize);
        // 描画する画面DSV
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
        if (m_gpuProfilerBackend != nullptr)
        {
            m_gpuProfilerBackend->BeforeCommandListClose(
                m_commandList.Get());
        }
        ThrowIfFailed(
            m_commandList->Close(),
            "ID3D12GraphicsCommandList::Close",
            m_device.Get());
        // 実行する描画コマンド列
        ID3D12CommandList* commandLists[]{ m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, commandLists);
        m_commandListOpen = false;
    }

    std::uint64_t D3D12Backend::SignalCurrentBackBuffer()
    {
        // 画面処理の完了フェンス値
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
        // GPU待機用のフェンス値
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
        // 今回発行するフェンス値
        const std::uint64_t value = m_nextFenceValue++;
        // この予約後の退避資源は、次に発行するフェンスの完了まで保持する。
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
        // 回収時の完了フェンス値
        const std::uint64_t completedValue =
            m_fence->GetCompletedValue();
        // デバイス消失時は完了を判定できないため、終了処理まで保持する。
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
        // 再利用前に初期化する転送領域
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
        // 転送先テクスチャーの設定
        const auto description = texture->GetDesc();
        // 連続転送するサブ資源数
        const auto subresourceCount = static_cast<UINT>(data.size());
        // 各サブ資源のGPU転送配置
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(
            subresourceCount);
        // 各サブ資源の転送行数
        std::vector<UINT> rowCounts(subresourceCount);
        // 各行の有効バイト数
        std::vector<UINT64> rowSizes(subresourceCount);
        // 転送領域の総バイト数
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

        // CPU書き込み用の転送ヒープ
        const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        // 転送バッファーの生成設定
        const auto stagingDescription = BufferDescription(totalBytes);
        // GPUへ渡す転送バッファー
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
        // 転送バッファーのCPU位置
        void* mapped{};
        // CPU読み取りなしの指定
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            staging->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(texture upload)",
            m_device.Get());
        try
        {
            // 詰め直すサブ資源の番号
            for (UINT index{}; index < subresourceCount; ++index)
            {
                // 面ごとのミップ段番号
                const auto mipLevel =
                    (firstMipLevel + index) % description.MipLevels;
                // ミップの行・ブロック配置
                const auto layout = Detail::RequiredTextureLayout(
                    description.Format,
                    std::max(
                        static_cast<std::uint32_t>(
                            description.Width >> mipLevel),
                        1u),
                    std::max(description.Height >> mipLevel, 1u));
                // サブ資源のGPU転送配置
                const auto& footprint = footprints[index];
                if (rowCounts[index] < layout.rowCount
                    || footprint.Footprint.RowPitch
                        < layout.minimumRowBytes)
                {
                    throw std::runtime_error(
                        "The D3D12 texture footprint is smaller than its "
                        "upload layout.");
                }
                // 呼び出し元の転送データ
                const auto& subresource = data[index];
                // 3Dミップの奥行き段数
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
                // GPU側の奥行き段間隔
                const auto destinationSlicePitch =
                    static_cast<std::size_t>(footprint.Footprint.RowPitch)
                    * rowCounts[index];
                // 奥行き方向の転送段番号
                for (std::uint32_t slice{}; slice < sliceCount; ++slice)
                {
                    // 詰め直す行番号
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

        // 転送記録と待機の排他ロック
        std::scoped_lock lock(m_uploadMutex);
        if (m_uploadCommandList == nullptr)
        {
            throw std::logic_error(
                "Texture upload requires an initialized D3D12 backend.");
        }
        // 転送コマンドの送信済み状態
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
            // 転送先の状態を遷移する(subresource: サブ資源番号, before: 遷移前の状態, after: 遷移後の状態)。
            const auto transition = [this, &texture](
                const UINT subresource,
                const D3D12_RESOURCE_STATES before,
                const D3D12_RESOURCE_STATES after)
            {
                // 転送用の資源状態遷移
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = texture.Get();
                barrier.Transition.Subresource = subresource;
                barrier.Transition.StateBefore = before;
                barrier.Transition.StateAfter = after;
                m_uploadCommandList->ResourceBarrier(1, &barrier);
            };
            // 既存資源は更新するミップだけを参照状態からコピー先へ切り替える。
            if (updateExistingTexture)
            {
                // 更新対象のサブ資源番号
                for (UINT index{}; index < subresourceCount; ++index)
                {
                    transition(
                        firstMipLevel + index,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                }
            }
            // コピーするサブ資源番号
            for (UINT index{}; index < subresourceCount; ++index)
            {
                // GPU側のコピー先
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = texture.Get();
                destination.Type =
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = firstMipLevel + index;
                // 転送バッファー内のコピー元
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
                // 参照状態へ戻すサブ資源番号
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
            // 実行する転送コマンド列
            ID3D12CommandList* commandLists[]{
                m_uploadCommandList.Get() };
            m_commandQueue->ExecuteCommandLists(1, commandLists);
            executed = true;
            // 転送完了を待つフェンス値
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
                // 送信後の失敗では転送元・転送先を終了処理まで保持する。
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

    D3D12Backend::ExternalShaderResourceDescriptor
        D3D12Backend::AllocateExternalShaderResourceDescriptor()
    {
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "Allocating a DirectX 12 shader resource descriptor "
                "requires an initialized backend.");
        }
        // 外部UI用に確保したSRV枠
        const auto slot = m_resourceDomain->AllocateShaderResourceSlot();
        return ExternalShaderResourceDescriptor{
            m_resourceDomain->ShaderResourceCpuHandle(slot),
            m_resourceDomain->ShaderResourceGpuHandle(slot),
            slot
        };
    }

    void D3D12Backend::ReleaseExternalShaderResourceDescriptor(
        const std::uint32_t slot) noexcept
    {
        if (m_resourceDomain != nullptr
            && slot < Detail::D3D12ResourceDomain::ShaderResourceCapacity)
        {
            // UIが参照した記述子枠は、次回フェンス完了まで再利用しない。
            m_resourceDomain->Retire(nullptr, slot);
        }
    }

    std::optional<D3D12Backend::ShaderResourceBinding>
        D3D12Backend::TryResolveShaderResource(
            const GraphicsViewHandle& view) const noexcept
    {
        // 現在世代のSRV実体
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

        // 現在フレームの転送領域一覧
        auto& arena = m_frameUploadArenas[m_currentBackBufferIndex];
        // 再利用候補の転送領域
        for (auto& chunk : arena)
        {
            // 整列後の書き込み位置
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

        // 新規転送領域の最小バイト数
        constexpr std::uint64_t MinimumChunkBytes = 1024u * 1024u;
        // 追加するフレーム転送領域
        FrameUploadChunk chunk;
        chunk.capacity = std::max(
            AlignUp(bytes, 64u * 1024u),
            MinimumChunkBytes);
        // 常時マップする転送ヒープ
        const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        // 追加転送領域の生成設定
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
        // 常時マップしたCPU位置
        void* mapped{};
        // CPU読み取りなしの指定
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            chunk.resource->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(frame upload)",
            m_device.Get());
        chunk.data = static_cast<std::byte*>(mapped);
        chunk.used = bytes;
        arena.push_back(std::move(chunk));
        // 追加済みのフレーム転送領域
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
        // 読戻し後の画面描画復元
        const bool resumeRenderTarget =
            m_commandListOpen
            && m_backBufferStates[m_currentBackBufferIndex]
                == D3D12_RESOURCE_STATE_RENDER_TARGET;
        // 読戻すバックバッファー
        auto* const backBuffer =
            m_backBuffers[m_currentBackBufferIndex].Get();
        // 読戻す画面の資源設定
        const auto backBufferDescription = backBuffer->GetDesc();

        // 画面のGPUコピー配置
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        // コピーする行数
        UINT rowCount{};
        // 各行の有効バイト数
        UINT64 rowSize{};
        // 読戻し領域の総バイト数
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

        // CPU読戻し用のヒープ
        const auto readbackHeap =
            HeapProperties(D3D12_HEAP_TYPE_READBACK);
        // 読戻しバッファーの設定
        const auto readbackDescription =
            BufferDescription(totalBytes);
        // 画面のCPU読戻し先
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
            // CPU読戻し用のコピー先
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type =
                D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            // 読戻す画面のコピー元
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
            // 読戻し完了のフェンス値
            const auto fenceValue = SignalCurrentBackBuffer();
            WaitForFence(fenceValue);

            // 取得画像の横ピクセル数
            const auto capturedWidth = static_cast<std::uint32_t>(
                backBufferDescription.Width);
            // 取得画像の縦ピクセル数
            const auto capturedHeight = backBufferDescription.Height;
            // 取得画像の行バイト数
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
            // 詰め直したRGBA画像
            std::vector<std::uint8_t> pixels(
                rowBytes * capturedHeight);
            // 読戻し領域のCPU位置
            void* mapped{};
            // CPUが読むバイト範囲
            const D3D12_RANGE readRange{ 0, totalBytes };
            ThrowIfFailed(
                readback->Map(0, &readRange, &mapped),
                "ID3D12Resource::Map(readback)",
                m_device.Get());
            // 画像へ詰め直す行番号
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
            // CPU書き込みなしの指定
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
        // 確保する描画先の幅
        const auto requestedWidth = std::max(width, 1u);
        // 確保する描画先の高さ
        const auto requestedHeight = std::max(height, 1u);
        if (requestedWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || requestedHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        {
            throw std::invalid_argument(
                "The offscreen target dimensions exceed the DirectX 12 "
                "limit.");
        }
        // 表示資源への計算書き込み
        const bool computeWritable =
            Detail::RenderTargetBackendAccess::ComputeWritable(target);
        // 再利用候補の既存描画先
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

        // 全資源完成後に公開する描画先
        auto pending = std::make_unique<D3D12RenderTargetState>();
        pending->resourceDomain = m_resourceDomain;
        pending->m_width = requestedWidth;
        pending->m_height = requestedHeight;
        pending->computeWritable = computeWritable;
        // 輝度測定段の幅と高さ
        const auto luminanceSizes = LuminanceLevelSizes(
            requestedWidth,
            requestedHeight);
        // 長辺が1になる全ミップ数
        std::uint32_t reflectionMipCount = 1u;
        // 深度階層の残り長辺
        for (std::uint32_t size = std::max(requestedWidth, requestedHeight);
            size > 1u;
            size >>= 1u)
        {
            ++reflectionMipCount;
        }
        pending->reflectionDepthPyramidRenderTargetSlot =
            D3D12RenderTargetState::LuminanceRenderTargetSlot
            + static_cast<std::uint32_t>(luminanceSizes.size());

        // 全描画先用RTVヒープの設定
        D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription{};
        renderTargetHeapDescription.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        // カラー・SSAO・ストリーク・輝度段・深度ミップの順にRTV枠を確保する。
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

        // 輝度測定段のGPUヒープ
        const auto luminanceHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 輝度測定のクリア設定
        D3D12_CLEAR_VALUE luminanceClear{};
        luminanceClear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        // 各輝度測定段のSRV設定
        D3D12_SHADER_RESOURCE_VIEW_DESC luminanceViewDescription{};
        luminanceViewDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        luminanceViewDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        luminanceViewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        luminanceViewDescription.Texture2D.MipLevels = 1u;
        // 各輝度測定段の資源設定
        D3D12_RESOURCE_DESC luminanceDescription{};
        luminanceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        luminanceDescription.DepthOrArraySize = 1u;
        luminanceDescription.MipLevels = 1u;
        luminanceDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        luminanceDescription.SampleDesc.Count = 1u;
        luminanceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        luminanceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        pending->luminanceLevels.reserve(luminanceSizes.size());
        // 生成する輝度測定段の番号
        for (std::size_t index{}; index < luminanceSizes.size(); ++index)
        {
            // levelWidth: 測定段の幅、levelHeight: 測定段の高さ
            const auto [levelWidth, levelHeight] = luminanceSizes[index];
            luminanceDescription.Width = levelWidth;
            luminanceDescription.Height = levelHeight;
            // 作成中の輝度測定段
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
        // 最終1×1輝度の転送設定
        auto readbackSource = luminanceDescription;
        readbackSource.Width = 1u;
        readbackSource.Height = 1u;
        // 最終輝度の読戻し容量
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
        // 輝度のCPU読戻しヒープ
        const auto readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        // 輝度読戻し資源の設定
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

        // 描画先のDSVヒープ設定
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

        // 描画先のGPU専用ヒープ
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // HDRカラーの生成設定
        D3D12_RESOURCE_DESC colorDescription{};
        colorDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        colorDescription.Width = requestedWidth;
        colorDescription.Height = requestedHeight;
        colorDescription.DepthOrArraySize = 1u;
        colorDescription.MipLevels = 1u;
        // ブルームや色調補正の前に、1を超えるHDRカラーを保持する。
        colorDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        colorDescription.SampleDesc.Count = 1u;
        colorDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        colorDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // HDRカラーのクリア設定
        D3D12_CLEAR_VALUE colorClear{};
        colorClear.Format = colorDescription.Format;

        // HDRカラーを確保する(destination: 生成先, operation: 診断用の操作名, flags: 読み書きの用途)。
        const auto createColor =
            [&](Microsoft::WRL::ComPtr<ID3D12Resource>& destination,
                const char* const operation,
                const D3D12_RESOURCE_FLAGS flags)
        {
            // 用途ごとのカラー生成設定
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

        // 描画先のRTVヒープ先頭
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

        // 半解像度SSAOの幅
        const auto occlusionWidth = std::max(requestedWidth / 2u, 1u);
        // 半解像度SSAOの高さ
        const auto occlusionHeight = std::max(requestedHeight / 2u, 1u);
        // 半解像度R8の生成設定
        auto occlusionDescription = colorDescription;
        occlusionDescription.Width = occlusionWidth;
        occlusionDescription.Height = occlusionHeight;
        occlusionDescription.Format = DXGI_FORMAT_R8_UNORM;
        // SSAOのクリア設定
        D3D12_CLEAR_VALUE occlusionClear{};
        occlusionClear.Format = occlusionDescription.Format;
        // SSAO資源とRTVを作る(destination: 生成先, slot: RTV枠番号, operation: 診断用の操作名)。
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

        // 四分の一寸法のストリーク幅
        const auto lensFlareWidth = std::max(requestedWidth / 4u, 1u);
        // 四分の一寸法のストリーク高さ
        const auto lensFlareHeight = std::max(requestedHeight / 4u, 1u);
        // HDRストリークの生成設定
        auto lensFlareDescription = colorDescription;
        lensFlareDescription.Width = lensFlareWidth;
        lensFlareDescription.Height = lensFlareHeight;
        // ストリーク資源とRTVを作る(destination: 生成先, slot: RTV枠番号, operation: 診断用の操作名)。
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

        // 深度・ステンシルの生成設定
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
        // 深度・ステンシルの初期値
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
        // 参照用深度コピーの設定
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
        // 描画先深度のDSV設定
        D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format = PrimaryDepthFormat;
        depthViewDescription.ViewDimension =
            D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(
            pending->depth.Get(),
            &depthViewDescription,
            pending->depthStencilHeap
                ->GetCPUDescriptorHandleForHeapStart());

        // HDRカラーのSRV設定
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
            // 表示カラーのUAV設定
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

        // 深度コピーのSRV設定
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
        // R8を公開形式Rgba8で扱う設定
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

        // 各ミップに描画できるR32F設定
        auto reflectionDescription = colorDescription;
        reflectionDescription.MipLevels =
            static_cast<UINT16>(reflectionMipCount);
        reflectionDescription.Format = DXGI_FORMAT_R32_FLOAT;
        // 深度階層のクリア設定
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
        // 全ミップ参照用SRVの設定
        D3D12_SHADER_RESOURCE_VIEW_DESC reflectionViewDescription{};
        reflectionViewDescription.Format = DXGI_FORMAT_R32_FLOAT;
        reflectionViewDescription.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        reflectionViewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        reflectionViewDescription.Texture2D.MipLevels = reflectionMipCount;
        // 公開形式はRgba16Floatで代用し、シェーダーは実体R32FのRだけを読む。
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
        // 生成する深度ミップ番号
        for (std::uint32_t mip{}; mip < reflectionMipCount; ++mip)
        {
            // 深度ミップの幅
            const auto mipWidth = std::max(requestedWidth >> mip, 1u);
            // 深度ミップの高さ
            const auto mipHeight = std::max(requestedHeight >> mip, 1u);
            // 単一ミップ用RTVの設定
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
            // 単一ミップ用SRVの設定
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
        // クリアする描画先の実体
        auto* const state = dynamic_cast<D3D12RenderTargetState*>(
            Detail::RenderTargetBackendAccess::Get(target));
        // クリアするHDRカラーのRTV
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
        // バインドする描画先の実体
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
        // 描画するHDRカラーのRTV
        const auto renderTarget = state->renderTargetHeap
            ->GetCPUDescriptorHandleForHeapStart();
        // 描画する深度のDSV
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
        // 表示コピーを公開する描画先
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
        // 後処理する描画先の実体
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
        // 後処理の書き込み先RTV
        const auto postTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            1u,
            state->renderTargetDescriptorSize);
        // PSO形式と合わせるDSV
        const auto depthTarget = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        // 深度テストをしない後処理でも、PSOの形式に合わせてDSVをバインドする。
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
        // カラーを交換する描画先
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
        // 記録済みOM設定は記述子を読み終えているため、カラー交換後のRTVを再作成できる。
        std::swap(state->color, state->postColor);
        std::swap(state->colorState, state->postColorState);
        std::swap(state->m_currentColorView, state->m_postColorView);
        // 交換後RTVを再構築する先頭
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
        // 深度のみを描画する先の実体
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
        // 深度のみを描画するDSV
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
        // 深度コピーを作る描画先
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
        // コピー後に描画先を復元するか
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
        // 計算書き込み先の実体
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

        // 計算に渡す入出力の記述子
        ComputeBindings bindings;
        bindings.output = m_resourceDomain->ShaderResourceGpuHandle(
            *state->displayUnorderedAccessSlot);
        bindings.width = state->m_width;
        bindings.height = state->m_height;
        // 重複を除いた入力テクスチャー
        const auto inputTextures =
            ComputeInputTextures(inputs, m_resourceDomain.get());
        // 検証する入力の番号
        for (std::size_t index{}; index < inputs.size(); ++index)
        {
            // 検証する入力SRVの実体
            const auto* const payload = TryShaderResourceViewPayload(
                inputs[index],
                m_resourceDomain.get());
            // 入力ビューの所有テクスチャー
            const auto* const texture =
                Detail::GraphicsResourceHandleAccess::TextureResource(
                    inputs[index]);
            // 入力テクスチャーの実体
            const auto* const texturePayload = texture != nullptr
                ? TryTexturePayload(*texture, m_resourceDomain.get())
                : nullptr;
            // 出力と同じ資源を入力する要求は、コマンド記録前に拒否する。
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
        // 計算書き込みを終える描画先
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
        // 次元別の空SRV設定
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension = dimension;
        description.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        // 空SRVのキャッシュ番号
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
        // この次元の空SRV枠
        auto& slot = m_nullShaderResourceSlots[index];
        if (!slot.has_value())
        {
            // 新しく確保した空SRV枠
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
        // SSR履歴を保存する描画先
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
        // 履歴コピー後の描画先復元
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
        // TAA履歴を保存する描画先
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
        // 履歴コピー後の描画先復元
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
        // 輝度測定段を調べる描画先
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
        // 輝度測定する描画先
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
        // 書き込む輝度測定段
        auto& destination = state->luminanceLevels[level];
        // 今回縮小する参照元
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
            // 一つ前の輝度測定段
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
        // 輝度縮小先のRTV
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12RenderTargetState::LuminanceRenderTargetSlot + level,
            state->renderTargetDescriptorSize);
        // 縮小先と深度の寸法が異なるため、DSVをバインドしない。
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
        // SSAO処理する描画先
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
        // 両パスとも、事前の深度キャプチャーで確定したコピーを読む。
        TransitionResource(
            m_commandList.Get(),
            state->depthCopy.Get(),
            state->depthCopyState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 深度コピーまたはSSAO入力
        GraphicsViewHandle source = state->m_depthView;
        // 未平滑化または平滑化先
        ID3D12Resource* destination = state->occlusion.Get();
        // 書き込み先の追跡状態
        D3D12_RESOURCE_STATES* destinationState = &state->occlusionState;
        // SSAO書き込み先のRTV枠
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
        // SSAO書き込み先のRTV
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            slot,
            state->renderTargetDescriptorSize);
        // SSAOと深度の寸法が異なるため、DSVをバインドしない。
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
        // SSAOを参照状態へ戻す描画先
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
        // 後続の描画が参照できるよう、SSAOの両資源を参照状態へ戻す。
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
        // ストリーク処理する描画先
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

        // 今回のストリーク入力
        GraphicsViewHandle source;
        // 今回のストリーク出力
        ID3D12Resource* destination{};
        // 出力資源の追跡状態
        D3D12_RESOURCE_STATES* destinationState{};
        // ストリーク出力のRTV枠
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
        // ストリーク出力のRTV
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
        // 最終ストリークを返す描画先
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
        // 深度階層を作る描画先
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
        // 深度コピーまたは前段ミップ
        GraphicsViewHandle source;
        if (mip == 0u)
        {
            // 事前にキャプチャーした深度コピーを、視点からの距離へ変換する。
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
        // 単一ミップの描画先RTV
        const auto renderTarget = OffsetDescriptor(
            state->renderTargetHeap->GetCPUDescriptorHandleForHeapStart(),
            state->reflectionDepthPyramidRenderTargetSlot + mip,
            state->renderTargetDescriptorSize);
        // 縮小先と深度の寸法が異なるため、DSVをバインドしない。
        m_commandList->OMSetRenderTargets(
            1,
            &renderTarget,
            FALSE,
            nullptr);
        // 単一ミップの描画範囲
        const auto& viewport = state->reflectionDepthPyramidViewports[mip];
        // 単一ミップの切り取り範囲
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
        // 深度階層を参照状態へ戻す先
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
        // 参照状態へ戻すミップ番号
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
        // 輝度の読戻し予約を捨てる先
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
        // 輝度を読み出す描画先
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
        // 読み出し時のGPU完了値
        const std::uint64_t completedValue = m_fence->GetCompletedValue();
        // GPU未完了・デバイス消失時は、CPUを待たせず未取得を返す。
        if (state->luminanceFenceValue == 0u
            || completedValue == std::numeric_limits<std::uint64_t>::max()
            || completedValue < state->luminanceFenceValue)
        {
            return std::nullopt;
        }

        // 1×1輝度の読戻し位置
        const auto offset =
            static_cast<SIZE_T>(state->luminanceFootprint.Offset);
        // 輝度読戻し領域のCPU位置
        void* mapped{};
        // 平均輝度一要素の読取範囲
        const D3D12_RANGE readRange{
            offset,
            offset + sizeof(DirectX::PackedVector::HALF) };
        ThrowIfFailed(
            state->luminanceReadback->Map(0, &readRange, &mapped),
            "ID3D12Resource::Map(luminance readback)",
            m_device.Get());
        // 半精度の対数平均輝度
        DirectX::PackedVector::HALF averageLogLuminance{};
        std::memcpy(
            &averageLogLuminance,
            static_cast<const std::uint8_t*>(mapped) + offset,
            sizeof(averageLogLuminance));
        // CPU書き込みなしの指定
        const D3D12_RANGE noWrite{};
        state->luminanceReadback->Unmap(0, &noWrite);
        // 対数平均を線形輝度へ戻して共通APIへ返す。
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
        // 測定結果をコピーする描画先
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
        // 前回コピーのGPU完了値
        const std::uint64_t completedValue = m_fence->GetCompletedValue();
        // 未完了の測定を上書きすると読み出せなくなるため、前回の完了を待つ。
        if (state->luminanceFenceValue != 0u
            && (completedValue == std::numeric_limits<std::uint64_t>::max()
                || completedValue < state->luminanceFenceValue))
        {
            return;
        }

        OpenCommandList();
        // コピーする最終1×1測定段
        auto& last = state->luminanceLevels.back();
        TransitionResource(
            m_commandList.Get(),
            last.texture.Get(),
            last.state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        // CPU読戻し領域のコピー先
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = state->luminanceReadback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = state->luminanceFootprint;
        // 最終輝度のコピー元
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
        // このコピーが実行された後の次回フェンス値で、CPU読戻しの可否を判定する。
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

        // 置換前の影バックエンド状態
        auto* const previousState =
            Detail::ShadowMapBackendAccess::Get(shadowMap);
        // 同じAPIの置換前の影状態
        auto* const previous = dynamic_cast<D3D12ShadowMapState*>(
            previousState);
        // 置換前の影描画を終了するか
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

        // 完成後に公開する新しい影状態
        auto state = std::make_unique<D3D12ShadowMapState>();
        state->m_resolution = std::max(resolution, 1u);
        state->m_cascadeCount = cube
            ? 6u
            : std::clamp(cascadeCount, 1u, 4u);
        state->cube = cube;

        // カスケード・六面深度の設定
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
        // 影深度のクリア値
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = ShadowDepthFormat;
        clearValue.DepthStencil.Depth = 1.0f;
        // 影深度のGPU専用ヒープ
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

        // 各影面用DSVヒープの設定
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
        // 単一影面のDSV設定
        D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
        depthViewDescription.Format = ShadowDepthFormat;
        depthViewDescription.ViewDimension =
            D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        depthViewDescription.Texture2DArray.ArraySize = 1;
        // 作成中の影面のDSV位置
        auto depthHandle = state->depthStencilHeap
            ->GetCPUDescriptorHandleForHeapStart();
        // 影面・カスケードの番号
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

        // 影参照用に確保したSRV枠
        const auto descriptorSlot =
            m_resourceDomain->AllocateShaderResourceSlot();
        try
        {
            // 全影面を参照するSRV設定
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
            // 影テクスチャーの所有参照
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
        // 描画を始める影状態
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
                // 影深度への書き込み遷移
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
            // 描画する影面のDSV
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
            // 影面の描画範囲
            const D3D12_VIEWPORT viewport{
                0.0f,
                0.0f,
                static_cast<float>(state->m_resolution),
                static_cast<float>(state->m_resolution),
                0.0f,
                1.0f };
            // 影面の切り取り範囲
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
        // 描画を終える影状態
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
            // 影深度の参照状態への遷移
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

        // 別世代・正射影・ライトなしの場合は、従来の固定配列で描画する。
        lighting.clustered = {};
        // ライト分類用の既存状態
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
        // 透視投影の判定
        const bool perspective = std::abs(projection._44) < 0.5f;
        if (!perspective || lighting.clusteredLights.empty())
        {
            return;
        }
        // 視点から近平面までの距離
        const float nearPlane = projection._43 / projection._33;
        // 視点から遠平面までの距離
        const float farPlane = projection._43 / (projection._33 + 1.0f);
        if (!(nearPlane > 0.0f) || !(farPlane > nearPlane))
        {
            return;
        }
        // 横方向の半画角の正接
        const float tanHalfX = 1.0f / projection._11;
        // 縦方向の半画角の正接
        const float tanHalfY = 1.0f / projection._22;
        // GPUへ送るライト数
        const auto lightCount = static_cast<std::uint32_t>(
            std::min(
                lighting.clusteredLights.size(),
                MaximumClusteredLights));
        // ライト一覧のSRV位置
        const auto lightBinding =
            TryResolveShaderResource(state->m_lightView);
        if (!lightBinding)
        {
            return;
        }

        OpenCommandList();
        // ライト分類の記録先
        auto* const commandList = m_commandList.Get();
        // ライト一覧の転送バイト数
        const std::uint64_t lightBytes =
            static_cast<std::uint64_t>(sizeof(GpuLight)) * lightCount;
        // ライト一覧のフレーム転送領域
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

        // ライト分類用の定数
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
        // ライト分類定数の転送領域
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
        // 分類に使うSRV・UAVヒープ
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
        // 分類結果を後続のピクセルシェーダーが読める状態へ戻す。
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
        // 世代を保持する描画先復元情報
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
        // 復元要求のD3D12描画先情報
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
        return std::make_unique<D3D12DebugDrawingBackend>(*this);
    }

    GpuProfilerBackend* D3D12Backend::ProfilerBackend() noexcept
    {
        return m_gpuProfilerBackend.get();
    }

    GraphicsTextureHandle D3D12Backend::CreateSolidRgba8Texture(
        const std::array<std::uint8_t, 4>& color)
    {
        // 単色1×1テクスチャーの設定
        const GraphicsTexture2DDescription description{
            1,
            1,
            1,
            GraphicsTextureFormat::Rgba8Unorm
        };
        // 単色RGBAの初期データ
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
        // 現在世代の2Dテクスチャー
        const auto* const payload = TryTexturePayload(
            texture,
            m_resourceDomain.get());
        // 現在世代の3Dテクスチャー
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
        GraphicsBufferHandle& buffer,
        const std::span<const std::byte> data)
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "UpdateDynamicVertexBuffer requires an initialized D3D12 "
                "backend.");
        }

        // 更新対象の頂点バッファー実体
        const auto* const payload = dynamic_cast<const D3D12BufferPayload*>(
            GraphicsResourceHandleAccess::Payload(buffer));
        if (buffer
            && (payload == nullptr
                || payload->vertexCapacity == 0u
                || GraphicsResourceHandleAccess::Domain(buffer)
                    != m_resourceDomain.get()))
        {
            throw std::invalid_argument(
                "UpdateDynamicVertexBuffer requires a dynamic vertex "
                "buffer from this backend generation.");
        }
        if (data.empty())
        {
            return false;
        }
        // 動的頂点の最小バイト容量
        constexpr std::uint64_t MinimumCapacity = 4096u;
        // 動的頂点の最大バイト容量
        constexpr std::uint64_t MaximumCapacity =
            std::numeric_limits<std::uint32_t>::max();
        if (data.size() > MaximumCapacity)
        {
            return false;
        }
        // 更新前の頂点バイト容量
        const std::uint64_t currentCapacity = payload != nullptr
            ? payload->vertexCapacity
            : 0u;
        // 上限内の倍増容量
        const std::uint64_t doubledCapacity =
            currentCapacity > MaximumCapacity / 2u
                ? MaximumCapacity
                : currentCapacity * 2u;
        // 必要量を満たす64ビット容量
        const auto newCapacity64 = data.size() <= currentCapacity
            ? currentCapacity
            : std::max({
                static_cast<std::uint64_t>(data.size()),
                doubledCapacity,
                MinimumCapacity });
        // 新しい頂点バイト容量
        const auto newCapacity = static_cast<std::uint32_t>(newCapacity64);

        // 頂点転送用UPLOADヒープ
        const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        // 新しい頂点資源の設定
        const auto description = BufferDescription(newCapacity);
        // 記録済み描画から隔離する資源
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        if (FAILED(m_device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(native.GetAddressOf()))))
        {
            return false;
        }

        // 頂点のCPU書き込み位置
        void* mapped{};
        // CPU読み取りなしの指定
        const D3D12_RANGE readRange{};
        if (FAILED(native->Map(0u, &readRange, &mapped)) || mapped == nullptr)
        {
            return false;
        }
        std::memcpy(mapped, data.data(), data.size());
        // 頂点を書き込んだバイト範囲
        const D3D12_RANGE writtenRange{ 0u, data.size() };
        native->Unmap(0u, &writtenRange);

        // 毎回別資源に置き換えて記録済み描画へのCPU書き込みを隔離し、旧資源はGPU完了まで保持する。
        buffer = GraphicsResourceHandleAccess::MakeBuffer(
            std::make_shared<D3D12BufferPayload>(
                m_resourceDomain,
                std::move(native),
                newCapacity));
        return true;
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
        // 2Dテクスチャーの画素形式
        const auto format = Detail::ToDxgiTextureFormat(
            description.format);
        // 初期データのミップ番号
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

        // 2Dテクスチャーの生成設定
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

        // テクスチャーを所有する世代
        const auto domain = m_resourceDomain;
        // 初期データの転送指定
        const bool uploadInitialData = !initialData.empty();
        // 2DテクスチャーのGPUヒープ
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 生成した2Dテクスチャー
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
        // 更新対象の2Dテクスチャー
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
        return CreateTextureArray(
            faceDescription,
            1u,
            true,
            subresources);
    }

    // 面ごとの全ミップから不変配列を作る(faceDescription: 一面の設定, arraySize: 配列数、キューブ単位, cubeArray: 六面キューブの指定, subresources: 面順・ミップ順のデータ)。
    std::pair<GraphicsTextureHandle, GraphicsViewHandle>
        D3D12Backend::CreateTextureArray(
            const GraphicsTexture2DDescription& faceDescription,
            const std::uint32_t arraySize,
            const bool cubeArray,
            const std::span<const GraphicsTextureSubresourceData>
                subresources)
    {
        if (!IsInitialized())
        {
            throw std::logic_error(
                "CreateTextureArray requires an initialized D3D12 backend.");
        }
        // キューブ一個の面数
        constexpr std::uint32_t FaceCount = 6u;
        // 六面展開後の64ビット配列数
        const std::uint64_t nativeArraySize64 = cubeArray
            ? static_cast<std::uint64_t>(arraySize) * FaceCount
            : arraySize;
        if (faceDescription.width == 0
            || faceDescription.height == 0
            || (cubeArray
                && faceDescription.width != faceDescription.height)
            || faceDescription.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || faceDescription.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || faceDescription.mipLevels == 0
            || faceDescription.mipLevels > Detail::MaximumTextureMipLevels(
                faceDescription.width,
                faceDescription.height)
            || faceDescription.updateMode
                != GraphicsTextureUpdateMode::Immutable
            || arraySize == 0u
            || (cubeArray
                && arraySize
                    > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / FaceCount)
            || nativeArraySize64
                > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION
            || subresources.size()
                != static_cast<std::size_t>(faceDescription.mipLevels)
                    * nativeArraySize64)
        {
            throw std::invalid_argument(
                "CreateTextureArray received an invalid description or "
                "subresource count.");
        }
        // 配列テクスチャーの画素形式
        const auto format = Detail::ToDxgiTextureFormat(
            faceDescription.format);
        // 六面展開後の配列数
        const auto nativeArraySize = static_cast<std::uint32_t>(
            nativeArraySize64);
        // 面順・ミップ順の検証番号
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

        // 配列テクスチャーの生成設定
        D3D12_RESOURCE_DESC nativeDescription{};
        nativeDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        nativeDescription.Width = faceDescription.width;
        nativeDescription.Height = faceDescription.height;
        nativeDescription.DepthOrArraySize =
            static_cast<UINT16>(nativeArraySize);
        nativeDescription.MipLevels =
            static_cast<UINT16>(faceDescription.mipLevels);
        nativeDescription.Format = format;
        nativeDescription.SampleDesc.Count = 1;
        nativeDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        nativeDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

        // 配列テクスチャーを所有する世代
        const auto domain = m_resourceDomain;
        // 配列テクスチャーのGPUヒープ
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 生成した配列テクスチャー
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        ThrowIfFailed(
            m_device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &nativeDescription,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(native.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(texture array)",
            m_device.Get());
        // 転送後は全ての面とミップをピクセル参照状態へ移す。
        SubmitTextureUpload(native, 0u, subresources, false);

        // 配列テクスチャーの所有参照
        auto texture = Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::make_shared<D3D12ShadowTexturePayload>(domain, native));
        // 配列・キューブのSRV設定
        D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = format;
        viewDescription.ViewDimension = cubeArray
            ? arraySize == 1u
                ? D3D12_SRV_DIMENSION_TEXTURECUBE
                : D3D12_SRV_DIMENSION_TEXTURECUBEARRAY
            : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        viewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (viewDescription.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE)
        {
            viewDescription.TextureCube.MostDetailedMip = 0;
            viewDescription.TextureCube.MipLevels =
                faceDescription.mipLevels;
        }
        else if (viewDescription.ViewDimension
            == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY)
        {
            viewDescription.TextureCubeArray.MostDetailedMip = 0;
            viewDescription.TextureCubeArray.MipLevels =
                faceDescription.mipLevels;
            viewDescription.TextureCubeArray.First2DArrayFace = 0;
            viewDescription.TextureCubeArray.NumCubes = arraySize;
        }
        else
        {
            viewDescription.Texture2DArray.MostDetailedMip = 0;
            viewDescription.Texture2DArray.MipLevels =
                faceDescription.mipLevels;
            viewDescription.Texture2DArray.FirstArraySlice = 0;
            viewDescription.Texture2DArray.ArraySize = arraySize;
        }
        // 未公開の配列SRV枠
        const auto slot = domain->AllocateShaderResourceSlot();
        try
        {
            m_device->CreateShaderResourceView(
                native.Get(),
                &viewDescription,
                domain->ShaderResourceCpuHandle(slot));
            // 配列・キューブの所有ビュー
            auto view = Detail::GraphicsResourceHandleAccess::MakeView(
                std::make_shared<D3D12ShaderResourceViewPayload>(
                    domain,
                    texture,
                    slot,
                    faceDescription.width,
                    faceDescription.height,
                    viewDescription.ViewDimension));
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
        // 計算用キューブの面数
        constexpr std::uint16_t FaceCount = 6u;
        // 計算用キューブのHDR形式
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

        // 計算用キューブの生成設定
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
        // 計算用キューブを所有する世代
        const auto domain = m_resourceDomain;
        // 計算用キューブのGPUヒープ
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 生成した計算用キューブ
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

        // 所有移譲前の各ミップUAV枠
        std::vector<std::uint32_t> accessSlots;
        // 各ミップのGPU書き込み位置
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mipAccess;
        // UAV枠を引き受ける所有実体
        std::shared_ptr<D3D12ComputeCubeTexturePayload> payload;
        try
        {
            accessSlots.reserve(mipLevels);
            mipAccess.reserve(mipLevels);
            // 書き込み先のミップ番号
            for (std::uint32_t mip{}; mip < mipLevels; ++mip)
            {
                // 六面共通のミップUAV設定
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
            // 生成失敗時に即時返すUAV枠
            for (const auto slot : accessSlots)
            {
                domain->ReleaseUnpublishedShaderResourceSlot(slot);
            }
            throw;
        }

        // 完成した計算用キューブ
        ComputeCubeTarget target;
        target.texture = Detail::GraphicsResourceHandleAccess::MakeTexture(
            std::move(payload));
        target.mipAccess = std::move(mipAccess);
        // 全ミップ参照用キューブ設定
        D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = Format;
        viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        viewDescription.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        viewDescription.TextureCube.MostDetailedMip = 0;
        viewDescription.TextureCube.MipLevels = mipLevels;
        // 未公開のキューブSRV枠
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
        // 計算入力のSRV位置
        const auto binding = TryResolveShaderResource(source);
        // 計算入力の所有テクスチャー
        const auto* const sourceTexture =
            Detail::GraphicsResourceHandleAccess::TextureResource(source);
        // 計算入力のGPU実体
        auto* const sourceNative = sourceTexture != nullptr
            ? TryNativeTexture(*sourceTexture, m_resourceDomain.get())
            : nullptr;
        // 計算出力のキューブ実体
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
        // 計算出力への書き込み遷移
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
        // 計算を終えるキューブ実体
        const auto* const targetPayload =
            TryComputeCubePayload(target, m_resourceDomain.get());
        if (targetPayload != nullptr)
        {
            // 計算出力の参照状態への遷移
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
        // 計算入力の所有テクスチャー
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
        // 現在世代の2D実体
        const auto* const payload = TryTexturePayload(
            texture,
            m_resourceDomain.get());
        // 現在世代の3D実体
        const auto* const volumePayload = payload == nullptr
            ? TryTexture3DPayload(texture, m_resourceDomain.get())
            : nullptr;
        // 参照可能なミップ総数
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

        // 2Dまたは3D用のSRV設定
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
        // 参照先のGPUテクスチャー
        auto* const native = payload != nullptr
            ? payload->native.Get()
            : volumePayload->native.Get();
        // 参照元の基底ミップ幅
        const auto width = payload != nullptr
            ? payload->description.width
            : volumePayload->description.width;
        // 参照元の基底ミップ高さ
        const auto height = payload != nullptr
            ? payload->description.height
            : volumePayload->description.height;

        // 参照ビューを所有する世代
        const auto domain = m_resourceDomain;
        // 未公開のテクスチャーSRV枠
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
        // 公開表示ビューの描画先状態
        const auto* const state = dynamic_cast<
            const D3D12RenderTargetState*>(
                Detail::RenderTargetBackendAccess::Get(target));
        // 取得する公開表示ビュー
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
        const GraphicsBufferHandle& buffer,
        const std::uint32_t slot,
        const std::uint32_t stride,
        const std::uint32_t offset)
    {
        using Detail::GraphicsResourceHandleAccess;
        if (!IsInitialized() || m_resourceDomain == nullptr)
        {
            throw std::logic_error(
                "BindVertexBuffer requires an initialized D3D12 backend.");
        }
        // 現在世代の頂点バッファー
        const auto* const payload = dynamic_cast<const D3D12BufferPayload*>(
            GraphicsResourceHandleAccess::Payload(buffer));
        if (!buffer
            || payload == nullptr
            || payload->vertexCapacity == 0u
            || GraphicsResourceHandleAccess::Domain(buffer)
                != m_resourceDomain.get()
            || slot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
            || stride == 0u
            || offset >= payload->vertexCapacity)
        {
            throw std::invalid_argument(
                "BindVertexBuffer requires a current dynamic vertex buffer, "
                "a valid slot and offset, and a non-zero stride.");
        }

        OpenCommandList();
        // 容量内の頂点参照範囲
        const D3D12_VERTEX_BUFFER_VIEW view{
            payload->native->GetGPUVirtualAddress() + offset,
            payload->vertexCapacity - offset,
            stride
        };
        m_commandList->IASetVertexBuffers(slot, 1u, &view);
    }

    bool D3D12Backend::TryBindPixelShaderResources(
        const std::uint32_t firstSlot,
        const std::span<const GraphicsViewHandle> resources,
        const GraphicsViewHandle&) noexcept
    {
        // 互換APIのSRVスロット上限
        constexpr std::size_t MaximumSlots =
            D3D12_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        if (!IsInitialized()
            || firstSlot >= MaximumSlots
            || resources.size() > MaximumSlots - firstSlot)
        {
            return false;
        }
        if (resources.empty())
        {
            return true;
        }
        // SRV設定は各ルート配置の描画サービスが担うため、直接設定の非空範囲は拒否する。
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

        // 3Dテクスチャーの画素形式
        const auto format = Detail::ToDxgiTextureFormat(description.format);
        // 3D画素形式の対応情報
        D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport{ format };
        // 3D資源と参照に必要な対応
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
        // 初期データの3Dミップ番号
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

        // 3Dテクスチャーの生成設定
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

        // 3Dテクスチャーを所有する世代
        const auto domain = m_resourceDomain;
        // 3DテクスチャーのGPUヒープ
        const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        // 生成した3Dテクスチャー
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
        // 全ミップを転送後、ピクセル参照状態へ移す。
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

        // 完成後に公開するライト分類状態
        auto state = std::make_unique<D3D12ClusteredLightsState>();
        state->resourceDomain = m_resourceDomain;

        // ライト分類用CSのバイトコード
        const auto byteCode = CompileShaderCached(
            assets,
            shaderPath,
            "CSMain",
            "cs_5_0");
        // ライトSRVと二つのUAV配置
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
        // 定数と記述子表のルート配置
        std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // 記述子表のルート番号
        for (std::size_t index{}; index < ranges.size(); ++index)
        {
            // 設定する記述子表のルート
            auto& parameter = parameters[index + 1u];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // ライト分類のルート設定
        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();
        // 直列化したルート配置
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        // ルート配置の診断データ
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // ルート配置の直列化結果
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            // 直列化失敗の診断文
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
        // ライト分類用の計算PSO設定
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

        // 分類用バッファーを作る(bytes: バイト容量, flags: 用途フラグ, operation: 診断用の操作名)。
        const auto createBuffer = [this](
            const std::uint64_t bytes,
            const D3D12_RESOURCE_FLAGS flags,
            const char* const operation)
        {
            // 分類用バッファーのGPUヒープ
            const auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
            // 分類用バッファーの生成設定
            auto description = BufferDescription(bytes);
            description.Flags = flags;
            // 生成した分類用バッファー
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
        // バッファーを所有するSRVを作る(buffer: 参照先のGPU資源, elementCount: 要素数, stride: 要素のバイト幅)。
        const auto createStructuredView = [this](
            const Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
            const std::uint32_t elementCount,
            const std::uint32_t stride)
        {
            // 参照先バッファーの所有参照
            auto bufferHandle =
                Detail::GraphicsResourceHandleAccess::MakeBuffer(
                    std::make_shared<D3D12BufferPayload>(
                        m_resourceDomain,
                        buffer));
            // 構造化バッファーのSRV設定
            D3D12_SHADER_RESOURCE_VIEW_DESC description{};
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            description.Buffer.NumElements = elementCount;
            description.Buffer.StructureByteStride = stride;
            // 未公開のバッファーSRV枠
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
        // 分類出力のUAV枠を確保する(buffer: 書き込み先のGPU資源, elementCount: 要素数)。
        const auto createAccessSlot = [this](
            const Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,
            const std::uint32_t elementCount)
        {
            // ライト分類出力のUAV設定
            D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
            description.Format = DXGI_FORMAT_UNKNOWN;
            description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            description.Buffer.NumElements = elementCount;
            description.Buffer.StructureByteStride =
                static_cast<UINT>(sizeof(std::uint32_t));
            // 分類出力用のUAV枠
            const auto slot = m_resourceDomain->AllocateShaderResourceSlot();
            m_device->CreateUnorderedAccessView(
                buffer.Get(),
                nullptr,
                &description,
                m_resourceDomain->ShaderResourceCpuHandle(slot));
            return slot;
        };

        // 全クラスタのライト番号容量
        const std::uint32_t indexCount = ClusteredLights::ClusterCount
            * ClusteredLights::MaximumLightsPerCluster;
        // ライト番号一要素のバイト数
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
        // 全資源の生成に成功した世代だけを公開し、途中失敗では既存状態を保つ。
        Detail::ClusteredLightsBackendAccess::Publish(
            clusteredLights,
            std::move(state));
    }
}
