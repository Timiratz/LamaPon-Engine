#include "LamaPon/Graphics/D3D12Backend.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/DebugRenderer.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
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
                const std::uint64_t value = m_nextFenceValue++;
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
        const std::uint64_t value = m_nextFenceValue++;
        ThrowIfFailed(
            m_commandQueue->Signal(m_fence.Get(), value),
            "ID3D12CommandQueue::Signal",
            m_device.Get());
        m_frameFenceValues[m_currentBackBufferIndex] = value;
        return value;
    }

    void D3D12Backend::WaitForFence(const std::uint64_t value)
    {
        if (value == 0)
        {
            return;
        }
        const std::uint64_t completedValue =
            m_fence->GetCompletedValue();
        if (completedValue
            == std::numeric_limits<std::uint64_t>::max())
        {
            ThrowHResult(
                DXGI_ERROR_DEVICE_REMOVED,
                "ID3D12Fence::GetCompletedValue",
                m_device.Get());
        }
        if (completedValue >= value)
        {
            return;
        }
        ThrowIfFailed(
            m_fence->SetEventOnCompletion(value, m_fenceEvent),
            "ID3D12Fence::SetEventOnCompletion",
            m_device.Get());
        if (WaitForSingleObject(m_fenceEvent, INFINITE)
            != WAIT_OBJECT_0)
        {
            throw std::runtime_error(
                "Waiting for the D3D12 fence failed.");
        }
        const std::uint64_t completedAfterWait =
            m_fence->GetCompletedValue();
        if (completedAfterWait
            == std::numeric_limits<std::uint64_t>::max())
        {
            ThrowHResult(
                DXGI_ERROR_DEVICE_REMOVED,
                "ID3D12Fence::GetCompletedValue",
                m_device.Get());
        }
        if (completedAfterWait < value)
        {
            throw std::runtime_error(
                "The D3D12 fence event fired before the requested "
                "value completed.");
        }
    }

    void D3D12Backend::WaitForGpu()
    {
        const std::uint64_t value = m_nextFenceValue++;
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
        RenderTarget&,
        std::uint32_t,
        std::uint32_t)
    {
        ThrowUnsupported("ResizeOffscreenTarget");
    }

    void D3D12Backend::BeginOffscreenTarget(
        RenderTarget&,
        const float[4])
    {
        ThrowUnsupported("BeginOffscreenTarget");
    }

    void D3D12Backend::BindOffscreenTarget(RenderTarget&)
    {
        ThrowUnsupported("BindOffscreenTarget");
    }

    void D3D12Backend::PublishOffscreenTarget(RenderTarget&)
    {
        ThrowUnsupported("PublishOffscreenTarget");
    }

    void D3D12Backend::BindOffscreenTargetDepthOnly(RenderTarget&)
    {
        ThrowUnsupported("BindOffscreenTargetDepthOnly");
    }

    void D3D12Backend::CaptureOffscreenTargetDepth(RenderTarget&)
    {
        ThrowUnsupported("CaptureOffscreenTargetDepth");
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
        ShadowMap&,
        std::uint32_t,
        std::uint32_t,
        bool)
    {
        ThrowUnsupported("InitializeShadowMap");
    }

    void D3D12Backend::BeginShadowMap(ShadowMap&, std::uint32_t)
    {
        ThrowUnsupported("BeginShadowMap");
    }

    void D3D12Backend::EndShadowMap(ShadowMap&)
    {
        ThrowUnsupported("EndShadowMap");
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
        ThrowUnsupported("CaptureOutputState");
    }

    void D3D12Backend::RestoreOutputState(const GraphicsOutputState&)
    {
        ThrowUnsupported("RestoreOutputState");
    }

    std::unique_ptr<DebugDrawingBackend>
        D3D12Backend::CreateDebugDrawingBackend()
    {
        ThrowUnsupported("CreateDebugDrawingBackend");
    }

    GraphicsTextureHandle D3D12Backend::CreateSolidRgba8Texture(
        const std::array<std::uint8_t, 4>&)
    {
        ThrowUnsupported("CreateSolidRgba8Texture");
    }

    GraphicsViewHandle D3D12Backend::CreateShaderResourceView(
        const GraphicsTextureHandle&)
    {
        ThrowUnsupported("CreateShaderResourceView");
    }

    bool D3D12Backend::UpdateDynamicVertexBuffer(
        GraphicsBufferHandle&,
        std::span<const std::byte>)
    {
        ThrowUnsupported("UpdateDynamicVertexBuffer");
    }

    GraphicsTextureHandle D3D12Backend::CreateTexture2D(
        const GraphicsTexture2DDescription&,
        std::span<const GraphicsTextureSubresourceData>)
    {
        ThrowUnsupported("CreateTexture2D");
    }

    void D3D12Backend::UpdateTexture2D(
        const GraphicsTextureHandle&,
        std::uint32_t,
        const GraphicsTextureSubresourceData&)
    {
        ThrowUnsupported("UpdateTexture2D");
    }

    GraphicsViewHandle D3D12Backend::CreateShaderResourceView(
        const GraphicsTextureHandle&,
        const GraphicsTextureViewDescription&)
    {
        ThrowUnsupported("CreateShaderResourceView");
    }

    GraphicsViewHandle D3D12Backend::CreateOffscreenDisplayView(
        const RenderTarget&)
    {
        ThrowUnsupported("CreateOffscreenDisplayView");
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
        const GraphicsViewHandle&) const noexcept
    {
        return false;
    }

    void D3D12Backend::InitializeClusteredLights(
        ClusteredLights&,
        AssetManager&,
        const std::filesystem::path&)
    {
        ThrowUnsupported("InitializeClusteredLights");
    }
}
