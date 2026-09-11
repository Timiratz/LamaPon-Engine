#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <Windows.h>
#include <objbase.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace
{
    class HiddenWindow final
    {
    public:
        HiddenWindow(
            const std::uint32_t width,
            const std::uint32_t height)
            : m_instance(GetModuleHandleW(nullptr))
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = m_instance;
            windowClass.lpszClassName = ClassName;
            m_class = RegisterClassExW(&windowClass);
            if (m_class == 0)
            {
                throw std::runtime_error(
                    "The D3D12 WARP test window class could not be registered");
            }

            m_window = CreateWindowExW(
                0,
                ClassName,
                L"LamaPonD3D12BackendTests",
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                static_cast<int>(width),
                static_cast<int>(height),
                nullptr,
                nullptr,
                m_instance,
                nullptr);
            if (m_window == nullptr)
            {
                UnregisterClassW(ClassName, m_instance);
                m_class = 0;
                throw std::runtime_error(
                    "The D3D12 WARP test window could not be created");
            }
        }

        ~HiddenWindow()
        {
            if (m_window != nullptr)
            {
                DestroyWindow(m_window);
            }
            if (m_class != 0)
            {
                UnregisterClassW(ClassName, m_instance);
            }
        }

        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        [[nodiscard]] HWND Get() const noexcept
        {
            return m_window;
        }

    private:
        static constexpr const wchar_t* ClassName =
            L"LamaPonGraphicsBackendSelectionD3D12Warp";

        HINSTANCE m_instance{};
        ATOM m_class{};
        HWND m_window{};
    };

    class TestGraphicsOutputState final
        : public LamaPon::GraphicsOutputState
    {
    };

    class RecordingDebugDrawingBackend final
        : public LamaPon::DebugDrawingBackend
    {
    public:
        void DrawLines(
            const std::span<const LamaPon::DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) override
        {
            ++drawCalls;
            lastLines.assign(lines.begin(), lines.end());
            lastView = view;
            lastProjection = projection;
        }

        std::size_t drawCalls{};
        std::vector<LamaPon::DebugLine> lastLines;
        DirectX::XMFLOAT4X4 lastView{};
        DirectX::XMFLOAT4X4 lastProjection{};
    };

    class RecordingGpuProfilerBackend final
        : public LamaPon::GpuProfilerBackend
    {
    public:
        [[nodiscard]] bool
            IsSupported() const noexcept override
        {
            return supported;
        }

        void OpenFrame() override
        {
            ++openFrameCalls;
        }

        [[nodiscard]] bool BeginSection(
            const std::string_view name,
            const std::uint32_t depth) override
        {
            ++beginSectionCalls;
            sectionNames.emplace_back(name);
            sectionDepths.push_back(depth);
            return acceptSections;
        }

        void EndSection() noexcept override
        {
            ++endSectionCalls;
        }

        void CloseFrame() override
        {
            endSectionCallsObservedAtClose = endSectionCalls;
            ++closeFrameCalls;
        }

        [[nodiscard]] const std::vector<LamaPon::GpuSectionTime>&
            LatestSections() const noexcept override
        {
            return latestSections;
        }

        [[nodiscard]] float
            LatestFrameMilliseconds() const noexcept override
        {
            return latestFrameMilliseconds;
        }

        [[nodiscard]] const LamaPon::GpuPipelineStatistics&
            LatestPipelineStatistics() const noexcept override
        {
            return latestPipelineStatistics;
        }

        bool supported{ true };
        bool acceptSections{ true };
        std::size_t openFrameCalls{};
        std::size_t beginSectionCalls{};
        std::size_t endSectionCalls{};
        std::size_t endSectionCallsObservedAtClose{};
        std::size_t closeFrameCalls{};
        std::vector<std::string> sectionNames;
        std::vector<std::uint32_t> sectionDepths;
        std::vector<LamaPon::GpuSectionTime> latestSections{
            { "resolved", 1.25f, 0u }
        };
        float latestFrameMilliseconds{ 2.5f };
        LamaPon::GpuPipelineStatistics latestPipelineStatistics{
            1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, true
        };
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void RequireClearColor(
        LamaPon::GraphicsBackend& backend,
        const std::uint32_t expectedWidth,
        const std::uint32_t expectedHeight,
        const std::array<float, 4>& color,
        const bool vSyncEnabled)
    {
        backend.BindAndClearBackBuffer(color.data());

        std::uint32_t capturedWidth{};
        std::uint32_t capturedHeight{};
        const auto pixels = backend.CaptureBackBuffer(
            capturedWidth,
            capturedHeight);
        Require(
            capturedWidth == expectedWidth
                && capturedHeight == expectedHeight,
            "D3D12 back-buffer capture returned unexpected dimensions");
        Require(
            pixels.size()
                == static_cast<std::size_t>(capturedWidth)
                    * capturedHeight * 4u,
            "D3D12 back-buffer capture returned an unexpected byte count");

        std::array<int, 4> expected{};
        for (std::size_t channel{}; channel < expected.size(); ++channel)
        {
            expected[channel] = static_cast<int>(std::lround(
                color[channel] * 255.0f));
        }
        for (std::size_t offset{}; offset < pixels.size(); offset += 4u)
        {
            for (std::size_t channel{}; channel < expected.size(); ++channel)
            {
                Require(
                    std::abs(
                        static_cast<int>(pixels[offset + channel])
                        - expected[channel]) <= 1,
                    "D3D12 WARP did not preserve the requested clear color");
            }
        }

        backend.DrainDebugMessages();
        backend.Present(vSyncEnabled);
    }

    template <typename Exception, typename Function>
    void RequireThrowsExactly(Function&& function, const char* message)
    {
        try
        {
            function();
        }
        catch (const Exception& exception)
        {
            if (typeid(exception) == typeid(Exception))
            {
                return;
            }
        }
        catch (...)
        {
        }
        throw std::runtime_error(message);
    }

    void CheckSelection(
        const LamaPon::RenderingApi requestedApi,
        const LamaPon::RenderingApi expectedRequestedApi,
        const LamaPon::RenderingApi expectedActiveApi,
        const LamaPon::RenderingApiFallbackReason expectedFallbackReason)
    {
        const auto selection = LamaPon::SelectGraphicsBackend(requestedApi);
        Require(selection.requestedApi == expectedRequestedApi,
            "Rendering backend selection returned an unexpected requested API");
        Require(selection.activeApi == expectedActiveApi,
            "Rendering backend selection returned an unexpected active API");
        Require(selection.fallbackReason == expectedFallbackReason,
            "Rendering backend selection returned an unexpected fallback reason");
    }

    void CheckSelection(
        const LamaPon::RenderingApi requestedApi,
        const LamaPon::GraphicsStartupProfile profile,
        const LamaPon::RenderingApi expectedRequestedApi,
        const LamaPon::RenderingApi expectedActiveApi,
        const LamaPon::RenderingApiFallbackReason expectedFallbackReason)
    {
        const auto selection = LamaPon::SelectGraphicsBackend(
            requestedApi,
            profile);
        Require(selection.requestedApi == expectedRequestedApi,
            "Profiled rendering backend selection returned an unexpected "
            "requested API");
        Require(selection.activeApi == expectedActiveApi,
            "Profiled rendering backend selection returned an unexpected "
            "active API");
        Require(selection.fallbackReason == expectedFallbackReason,
            "Profiled rendering backend selection returned an unexpected "
            "fallback reason");
    }

    class ScopedWarpAdapterPreference final
    {
    public:
        ScopedWarpAdapterPreference()
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        }

        ~ScopedWarpAdapterPreference()
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(false);
        }

        ScopedWarpAdapterPreference(const ScopedWarpAdapterPreference&) = delete;
        ScopedWarpAdapterPreference& operator=(
            const ScopedWarpAdapterPreference&) = delete;
    };
}

static_assert(noexcept(LamaPon::SelectGraphicsBackend(
    LamaPon::RenderingApi::DirectX11)));
static_assert(noexcept(LamaPon::SelectGraphicsBackend(
    LamaPon::RenderingApi::DirectX12Experimental,
    LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap)));
static_assert(noexcept(std::declval<
    const LamaPon::GraphicsBackend&>()
        .QueryVideoMemoryStatistics()));
static_assert(noexcept(std::declval<
    LamaPon::GraphicsBackend&>()
        .ProfilerBackend()));
static_assert(noexcept(std::declval<
    LamaPon::GraphicsBackend&>()
        .TryBindPixelShaderResources(
            0,
            std::span<const LamaPon::GraphicsViewHandle>{},
            std::declval<const LamaPon::GraphicsViewHandle&>())));

int main()
{
    // D3D12 bootstrapを含むGraphicsDeviceはAssetManagerのWIC factoryを
    // 作るため、COMを初期化してから検証します。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    int result = 0;
    try
    {
        CheckSelection(
            LamaPon::RenderingApi::Auto,
            LamaPon::RenderingApi::Auto,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApiFallbackReason::None);
        CheckSelection(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApiFallbackReason::None);
        CheckSelection(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApiFallbackReason::NotImplemented);
        CheckSelection(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApiFallbackReason::NotImplemented);
        CheckSelection(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::RenderingApiFallbackReason::None);
        CheckSelection(
            static_cast<LamaPon::RenderingApi>(-1),
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApi::DirectX11,
            LamaPon::RenderingApiFallbackReason::UnknownApi);

        // 共通facadeはAPI固有driverが未接続でも安全で、
        // 接続後は入れ子深度と確定結果だけを中継します。
        LamaPon::GpuProfiler profiler;
        Require(!profiler.IsSupported(),
            "A detached GPU profiler facade must be unsupported");
        profiler.OpenFrame();
        profiler.BeginSection("detached");
        profiler.EndSection();
        profiler.CloseFrame();
        Require(
            profiler.LatestSections().empty()
                && profiler.LatestFrameMilliseconds() == 0.0f
                && !profiler.LatestPipelineStatistics().valid,
            "A detached GPU profiler facade must expose empty results");

        RecordingGpuProfilerBackend unsupportedProfilerBackend;
        unsupportedProfilerBackend.supported = false;
        profiler.Attach(&unsupportedProfilerBackend);
        profiler.OpenFrame();
        profiler.BeginSection("unsupported");
        profiler.EndSection();
        profiler.CloseFrame();
        Require(
            unsupportedProfilerBackend.openFrameCalls == 0u
                && unsupportedProfilerBackend.beginSectionCalls == 0u
                && unsupportedProfilerBackend.endSectionCalls == 0u
                && unsupportedProfilerBackend.closeFrameCalls == 0u
                && profiler.LatestSections().empty()
                && profiler.LatestFrameMilliseconds() == 0.0f
                && !profiler.LatestPipelineStatistics().valid,
            "An unsupported GPU profiler backend must remain a safe no-op");

        RecordingGpuProfilerBackend profilerBackend;
        profiler.Attach(&profilerBackend);
        Require(profiler.IsSupported(),
            "The facade must expose backend support");
        Require(
            profiler.LatestSections().size() == 1u
                && profiler.LatestSections().front().name == "resolved"
                && profiler.LatestSections().front().milliseconds == 1.25f
                && profiler.LatestFrameMilliseconds() == 2.5f
                && profiler.LatestPipelineStatistics()
                    .computeShaderInvocations == 8u
                && profiler.LatestPipelineStatistics().valid,
            "The facade must forward resolved backend statistics");
        {
            LamaPon::GpuProfiler::SectionScope outer{
                profiler,
                "outer"
            };
            Require(outer.InitialDepth() == 0u,
                "The first profiler scope must begin at depth zero");
            profiler.BeginSection("inner");
        }
        Require(
            profilerBackend.sectionNames
                == std::vector<std::string>{ "outer", "inner" }
                && profilerBackend.sectionDepths
                    == std::vector<std::uint32_t>{ 0u, 1u }
                && profilerBackend.endSectionCalls == 2u,
            "A scope must unwind its nested sections through the backend");

        const auto endsBeforeManualScope =
            profilerBackend.endSectionCalls;
        {
            LamaPon::GpuProfiler::SectionScope manualScope{
                profiler,
                "manual"
            };
            manualScope.End();
            manualScope.End();
        }
        Require(
            profilerBackend.endSectionCalls
                == endsBeforeManualScope + 1u,
            "Explicitly ending a profiler scope must be idempotent");

        profilerBackend.acceptSections = false;
        const auto endsBeforeRejectedSection =
            profilerBackend.endSectionCalls;
        {
            LamaPon::GpuProfiler::SectionScope rejected{
                profiler,
                "rejected"
            };
            Require(rejected.InitialDepth() == 0u,
                "A rejected section must not retain an old depth");
        }
        profilerBackend.acceptSections = true;
        {
            LamaPon::GpuProfiler::SectionScope afterRejected{
                profiler,
                "after rejected"
            };
            Require(afterRejected.InitialDepth() == 0u,
                "A rejected section must not increment facade depth");
        }
        Require(
            profilerBackend.endSectionCalls
                == endsBeforeRejectedSection + 1u,
            "Only accepted profiler sections may be ended");

        profiler.BeginSection("close outer");
        profiler.BeginSection("close inner");
        const auto endsBeforeClose = profilerBackend.endSectionCalls;
        profiler.CloseFrame();
        Require(
            profilerBackend.endSectionCalls
                == endsBeforeClose + 2u
                && profilerBackend.closeFrameCalls == 1u
                && profilerBackend.endSectionCallsObservedAtClose
                    == profilerBackend.endSectionCalls,
            "Closing a frame must end every open section before the backend frame");

        LamaPon::GpuProfiler::SectionScope staleScope{
            profiler,
            "detach open"
        };
        const auto endsBeforeDetach = profilerBackend.endSectionCalls;
        profiler.Detach();
        Require(
            profilerBackend.endSectionCalls == endsBeforeDetach + 1u
                && !profiler.IsSupported()
                && profiler.LatestSections().empty(),
            "Detaching must unwind open sections and clear facade results");
        RecordingGpuProfilerBackend replacementProfilerBackend;
        replacementProfilerBackend.latestSections.front().name =
            "replacement";
        profiler.Attach(&replacementProfilerBackend);
        {
            LamaPon::GpuProfiler::SectionScope replacementScope{
                profiler,
                "replacement scope"
            };
            Require(replacementScope.InitialDepth() == 0u,
                "Attaching a new profiler backend must reset section depth");
            staleScope.End();
            Require(
                replacementProfilerBackend.endSectionCalls == 0u,
                "A stale scope must not close sections on a replacement backend");
        }
        Require(
            profiler.LatestSections().front().name == "replacement"
                && replacementProfilerBackend.sectionDepths.front() == 0u
                && replacementProfilerBackend.endSectionCalls == 1u,
            "A replacement profiler backend must not inherit old state");
        profiler.Detach();

        auto backend = LamaPon::CreateGraphicsBackend(
            LamaPon::RenderingApi::DirectX11);
        Require(backend != nullptr,
            "DirectX 11 backend factory must return a backend");
        Require(backend->Api() == LamaPon::RenderingApi::DirectX11,
            "DirectX 11 backend must report the DirectX 11 API");
        Require(!backend->IsInitialized(),
            "A newly created backend must not initialize graphics resources");
        Require(
            backend->ProfilerBackend() == nullptr,
            "An uninitialized backend must not expose a profiler driver");
        const auto emptyVideoMemory =
            backend->QueryVideoMemoryStatistics();
        Require(
            emptyVideoMemory.dedicatedBytes == 0
                && emptyVideoMemory.sharedSystemBytes == 0
                && emptyVideoMemory.localUsageBytes == 0
                && emptyVideoMemory.localBudgetBytes == 0
                && emptyVideoMemory.nonLocalUsageBytes == 0
                && emptyVideoMemory.nonLocalBudgetBytes == 0
                && !emptyVideoMemory.adapterAvailable
                && !emptyVideoMemory.descriptionAvailable
                && !emptyVideoMemory.localBudgetAvailable
                && !emptyVideoMemory.nonLocalBudgetAvailable,
            "An uninitialized backend must report empty video memory statistics");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    backend->CreateDebugDrawingBackend());
            },
            "Creating debug drawing resources requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->BindVertexBuffer(
                    {},
                    0,
                    0,
                    0);
            },
            "Binding a vertex buffer requires an initialized backend");
        const std::array<LamaPon::GraphicsViewHandle, 1>
            emptyShaderResources{};
        Require(
            !backend->TryBindPixelShaderResources(
                0,
                emptyShaderResources,
                {}),
            "An uninitialized backend accepted pixel shader resources");

        // DirectX 12はまだGraphicsDeviceの実描画経路へ選択しませんが、
        // bootstrap backend自体はWARPでdevice / swap chain / command
        // submissionを独立検証します。画面を表示する必要はありません。
        {
            constexpr std::uint32_t D3D12Width = 64u;
            constexpr std::uint32_t D3D12Height = 48u;
            HiddenWindow window{ D3D12Width, D3D12Height };
            auto d3d12Backend = LamaPon::CreateGraphicsBackend(
                LamaPon::RenderingApi::DirectX12Experimental);
            Require(
                d3d12Backend != nullptr
                    && d3d12Backend->Api()
                        == LamaPon::RenderingApi::DirectX12Experimental,
                "The DirectX 12 factory returned an incompatible backend");
            Require(
                !d3d12Backend->IsInitialized()
                    && !d3d12Backend->TearingAllowed(),
                "A new DirectX 12 backend exposed initialized state");
            const auto emptyD3D12Memory =
                d3d12Backend->QueryVideoMemoryStatistics();
            Require(
                !emptyD3D12Memory.adapterAvailable
                    && !emptyD3D12Memory.descriptionAvailable
                    && !emptyD3D12Memory.localBudgetAvailable
                    && !emptyD3D12Memory.nonLocalBudgetAvailable
                    && emptyD3D12Memory.dedicatedBytes == 0u
                    && emptyD3D12Memory.sharedSystemBytes == 0u,
                "An uninitialized DirectX 12 backend reported adapter state");

            const LamaPon::GraphicsBackendCreateInfo d3d12CreateInfo{
                static_cast<void*>(window.Get()),
                D3D12Width,
                D3D12Height,
                true,
                false
            };
            d3d12Backend->Initialize(d3d12CreateInfo);
            Require(
                d3d12Backend->IsInitialized(),
                "The DirectX 12 WARP backend did not initialize");
            const bool tearingAllowed =
                d3d12Backend->TearingAllowed();
            const auto d3d12Memory =
                d3d12Backend->QueryVideoMemoryStatistics();
            Require(
                d3d12Memory.adapterAvailable
                    && d3d12Memory.descriptionAvailable,
                "The DirectX 12 WARP adapter was not reported");
            Require(
                (d3d12Memory.localBudgetAvailable
                    || (d3d12Memory.localUsageBytes == 0u
                        && d3d12Memory.localBudgetBytes == 0u))
                    && (d3d12Memory.nonLocalBudgetAvailable
                        || (d3d12Memory.nonLocalUsageBytes == 0u
                            && d3d12Memory.nonLocalBudgetBytes == 0u)),
                "Unavailable DirectX 12 memory budgets contained values");

            RequireClearColor(
                *d3d12Backend,
                D3D12Width,
                D3D12Height,
                { 0.125f, 0.375f, 0.625f, 1.0f },
                false);

            // Captureは同期を伴うため、その後はcapture無しで両方の
            // frame allocatorを複数回再利用します。Resizeが直後の
            // outstanding submissionを待てなければWARP/debug layerで
            // allocator resetやResizeBuffersの事故になります。
            constexpr std::array<std::array<float, 4>, 6> FrameColors{
                std::array{ 0.10f, 0.20f, 0.30f, 1.0f },
                std::array{ 0.20f, 0.30f, 0.40f, 1.0f },
                std::array{ 0.30f, 0.40f, 0.50f, 1.0f },
                std::array{ 0.40f, 0.50f, 0.60f, 1.0f },
                std::array{ 0.50f, 0.60f, 0.70f, 1.0f },
                std::array{ 0.60f, 0.70f, 0.80f, 1.0f }
            };
            for (std::size_t frame{}; frame < FrameColors.size(); ++frame)
            {
                d3d12Backend->BindAndClearBackBuffer(
                    FrameColors[frame].data());
                d3d12Backend->DrainDebugMessages();
                d3d12Backend->Present(frame % 2u != 0u);
            }

            d3d12Backend->Resize(0u, D3D12Height);
            RequireClearColor(
                *d3d12Backend,
                D3D12Width,
                D3D12Height,
                { 0.75f, 0.25f, 0.50f, 1.0f },
                true);

            constexpr std::uint32_t ResizedWidth = 37u;
            constexpr std::uint32_t ResizedHeight = 19u;
            d3d12Backend->Resize(ResizedWidth, ResizedHeight);
            Require(
                d3d12Backend->IsInitialized()
                    && d3d12Backend->TearingAllowed() == tearingAllowed,
                "DirectX 12 resize changed device or tearing state");
            RequireClearColor(
                *d3d12Backend,
                ResizedWidth,
                ResizedHeight,
                { 0.80f, 0.40f, 0.20f, 1.0f },
                false);
            // 同じ寸法のResizeもback-buffer参照やallocator状態を壊しません。
            d3d12Backend->Resize(ResizedWidth, ResizedHeight);
            RequireClearColor(
                *d3d12Backend,
                ResizedWidth,
                ResizedHeight,
                { 0.20f, 0.70f, 0.35f, 1.0f },
                true);

            LamaPon::RenderTarget unsupportedTarget;
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    d3d12Backend->ResizeOffscreenTarget(
                        unsupportedTarget,
                        4u,
                        4u);
                },
                "The DirectX 12 bootstrap silently accepted an unsupported "
                "offscreen operation");

            d3d12Backend->PrepareForResourceRelease();
            d3d12Backend->PrepareForResourceRelease();
            Require(
                d3d12Backend->IsInitialized(),
                "Preparing DirectX 12 resources unexpectedly shut down the backend");
            d3d12Backend->Shutdown();
            d3d12Backend->Shutdown();
            Require(
                !d3d12Backend->IsInitialized()
                    && !d3d12Backend->TearingAllowed(),
                "DirectX 12 shutdown was not idempotent");
            const auto shutdownD3D12Memory =
                d3d12Backend->QueryVideoMemoryStatistics();
            Require(
                !shutdownD3D12Memory.adapterAvailable
                    && !shutdownD3D12Memory.descriptionAvailable,
                "DirectX 12 shutdown retained adapter diagnostics");

            constexpr std::uint32_t ReinitializedWidth = 23u;
            constexpr std::uint32_t ReinitializedHeight = 11u;
            d3d12Backend->Initialize({
                static_cast<void*>(window.Get()),
                ReinitializedWidth,
                ReinitializedHeight,
                true,
                false
            });
            Require(
                d3d12Backend->IsInitialized(),
                "The DirectX 12 backend could not reinitialize after shutdown");
            RequireClearColor(
                *d3d12Backend,
                ReinitializedWidth,
                ReinitializedHeight,
                { 0.05f, 0.45f, 0.85f, 1.0f },
                false);

            // Sprite描画が使うtexture資源は、D3D11と同じdescription /
            // subresource契約でD3D12の同期upload経路へ送ります。
            const auto solidTexture =
                d3d12Backend->CreateSolidRgba8Texture(
                    { 255u, 64u, 32u, 255u });
            const auto solidView =
                d3d12Backend->CreateShaderResourceView(solidTexture);
            Require(
                solidTexture
                    && solidView
                    && solidView.Kind()
                        == LamaPon::GraphicsViewKind::ShaderResource
                    && d3d12Backend->IsViewCurrent(solidView),
                "The DirectX 12 backend did not create a current texture view");
            RequireThrowsExactly<std::invalid_argument>(
                [&]
                {
                    static_cast<void>(d3d12Backend->CreateTexture2D(
                        LamaPon::GraphicsTexture2DDescription{ 2u, 2u, 1u },
                        {}));
                },
                "An immutable DirectX 12 texture was accepted without data");
            const std::array<std::uint8_t, 4> updatePixel{
                1u, 2u, 3u, 4u };
            RequireThrowsExactly<std::invalid_argument>(
                [&]
                {
                    d3d12Backend->UpdateTexture2D(
                        solidTexture,
                        0u,
                        { std::as_bytes(std::span{ updatePixel }), 4u, 4u });
                },
                "An immutable DirectX 12 texture accepted an update");

            const LamaPon::GraphicsTexture2DDescription progressiveDescription{
                4u,
                2u,
                3u,
                LamaPon::GraphicsTextureFormat::Rgba8Unorm,
                LamaPon::GraphicsTextureUpdateMode::PerMipUpdate
            };
            const auto progressiveTexture =
                d3d12Backend->CreateTexture2D(progressiveDescription, {});
            const std::array<std::uint8_t, 8> mip1Pixels{
                5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u };
            std::array<std::uint8_t, 32> mip0Pixels{};
            for (std::size_t index{}; index < mip0Pixels.size(); ++index)
            {
                mip0Pixels[index] = static_cast<std::uint8_t>(index);
            }
            // 段階uploadと同じく、粗いmipから描画用stateのまま更新します。
            d3d12Backend->UpdateTexture2D(
                progressiveTexture,
                2u,
                { std::as_bytes(std::span{ updatePixel }), 4u, 4u });
            d3d12Backend->UpdateTexture2D(
                progressiveTexture,
                1u,
                { std::as_bytes(std::span{ mip1Pixels }), 8u, 8u });
            d3d12Backend->UpdateTexture2D(
                progressiveTexture,
                0u,
                { std::as_bytes(std::span{ mip0Pixels }), 16u, 32u });
            const auto coarseView = d3d12Backend->CreateShaderResourceView(
                progressiveTexture,
                { 1u, 2u });
            Require(
                d3d12Backend->IsViewCurrent(coarseView),
                "A partial DirectX 12 mip view was not current");
            RequireThrowsExactly<std::invalid_argument>(
                [&]
                {
                    static_cast<void>(d3d12Backend->CreateShaderResourceView(
                        progressiveTexture,
                        { 2u, 2u }));
                },
                "A DirectX 12 view accepted an out-of-range mip span");
            RequireThrowsExactly<std::invalid_argument>(
                [&]
                {
                    d3d12Backend->UpdateTexture2D(
                        progressiveTexture,
                        3u,
                        { std::as_bytes(std::span{ updatePixel }), 4u, 4u });
                },
                "A DirectX 12 texture accepted an out-of-range mip update");

            // 記録中のframeより先にhandleを破棄しても、GPUの完了まで
            // resourceとdescriptorを遅延解放してPresentできます。
            {
                const std::array frameColor{ 0.3f, 0.2f, 0.1f, 1.0f };
                d3d12Backend->BindAndClearBackBuffer(frameColor.data());
                {
                    const auto transientView =
                        d3d12Backend->CreateShaderResourceView(
                            d3d12Backend->CreateSolidRgba8Texture(
                                { 0u, 255u, 0u, 255u }));
                    Require(
                        d3d12Backend->IsViewCurrent(transientView),
                        "A transient DirectX 12 view was not current");
                }
                d3d12Backend->DrainDebugMessages();
                d3d12Backend->Present(false);
            }

            d3d12Backend->PrepareForResourceRelease();
            d3d12Backend->Shutdown();
            // Shutdown後に残ったhandleは現在の世代ではなく、破棄も安全です。
            Require(
                !d3d12Backend->IsViewCurrent(solidView)
                    && !d3d12Backend->IsViewCurrent(coarseView),
                "A DirectX 12 view remained current after shutdown");
        }

        // Gameだけが明示的に許可するD3D12 bootstrapは、D3D11の
        // native rendererを初期化せず、clear / capture / present / resize、
        // Sprite用texture、空のscene facadeを安全に提供します。D3D12を
        // 初期化できない環境では同じprofileでもD3D11へ一度だけ
        // フォールバックします。
        {
            constexpr std::uint32_t BootstrapWidth = 53u;
            constexpr std::uint32_t BootstrapHeight = 31u;
            constexpr std::uint32_t ResizedBootstrapWidth = 29u;
            constexpr std::uint32_t ResizedBootstrapHeight = 47u;
            HiddenWindow window{ BootstrapWidth, BootstrapHeight };
            ScopedWarpAdapterPreference warpAdapterPreference;
            LamaPon::GraphicsDevice bootstrapGraphics;
            bootstrapGraphics.Initialize(
                window.Get(),
                BootstrapWidth,
                BootstrapHeight,
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);

            Require(bootstrapGraphics.IsInitialized(),
                "The profiled graphics device did not initialize");
            Require(
                bootstrapGraphics.StartupRenderingApi()
                    == LamaPon::RenderingApi::DirectX12Experimental,
                "The profiled graphics device did not retain the requested "
                "DirectX 12 API");

            if (bootstrapGraphics.ActiveRenderingApi()
                == LamaPon::RenderingApi::DirectX12Experimental)
            {
                Require(bootstrapGraphics.IsD3D12ExperimentalBootstrap(),
                    "An active DirectX 12 bootstrap was not identified");
                Require(
                    bootstrapGraphics.RenderingApiFallback()
                        == LamaPon::RenderingApiFallbackReason::None,
                    "An active DirectX 12 bootstrap reported a fallback");
                Require(
                    bootstrapGraphics.WhiteTextureHandle()
                        && bootstrapGraphics.WhiteTextureViewHandle(),
                    "The DirectX 12 bootstrap did not create its sprite "
                    "fallback white texture");
                Require(
                    !bootstrapGraphics.Shadows().IsValid()
                        && !bootstrapGraphics.SpotShadows().IsValid()
                        && !bootstrapGraphics.PointShadows().IsValid(),
                    "The DirectX 12 bootstrap did not expose safe empty "
                    "shadow facades");

                const std::array debugPoints{
                    DirectX::XMFLOAT3{ -1.0f, 0.0f, 0.0f },
                    DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f }
                };
                bootstrapGraphics.Debug().DrawLines(
                    debugPoints,
                    DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f),
                    DirectX::XMMatrixIdentity(),
                    DirectX::XMMatrixIdentity());

                constexpr std::array BootstrapColor{
                    0.15f, 0.35f, 0.55f, 1.0f };
                bootstrapGraphics.BeginFrame(BootstrapColor.data());
                std::uint32_t capturedWidth{};
                std::uint32_t capturedHeight{};
                const auto pixels = bootstrapGraphics.CaptureBackBuffer(
                    capturedWidth,
                    capturedHeight);
                Require(
                    capturedWidth == BootstrapWidth
                        && capturedHeight == BootstrapHeight
                        && pixels.size()
                            == static_cast<std::size_t>(BootstrapWidth)
                                * BootstrapHeight * 4u,
                    "The DirectX 12 bootstrap frame could not be captured");
                bootstrapGraphics.EndFrame();

                bootstrapGraphics.Resize(
                    ResizedBootstrapWidth,
                    ResizedBootstrapHeight);
                Require(
                    bootstrapGraphics.Width() == ResizedBootstrapWidth
                        && bootstrapGraphics.Height() == ResizedBootstrapHeight,
                    "The DirectX 12 bootstrap resize did not update the "
                    "graphics device dimensions");
                constexpr std::array ResizedBootstrapColor{
                    0.65f, 0.25f, 0.45f, 1.0f };
                bootstrapGraphics.BeginFrame(ResizedBootstrapColor.data());
                const auto resizedPixels = bootstrapGraphics.CaptureBackBuffer(
                    capturedWidth,
                    capturedHeight);
                Require(
                    capturedWidth == ResizedBootstrapWidth
                        && capturedHeight == ResizedBootstrapHeight
                        && resizedPixels.size()
                            == static_cast<std::size_t>(
                                ResizedBootstrapWidth)
                                * ResizedBootstrapHeight * 4u,
                    "The resized DirectX 12 bootstrap frame could not be "
                    "captured");
                bootstrapGraphics.EndFrame();
            }
            else
            {
                Require(
                    bootstrapGraphics.ActiveRenderingApi()
                        == LamaPon::RenderingApi::DirectX11,
                    "A failed DirectX 12 bootstrap did not fall back to "
                    "DirectX 11");
                Require(
                    !bootstrapGraphics.IsD3D12ExperimentalBootstrap()
                        && bootstrapGraphics.RenderingApiFallback()
                            == LamaPon::RenderingApiFallbackReason::
                                InitializationFailed,
                    "A failed DirectX 12 bootstrap did not report its "
                    "DirectX 11 fallback");
            }
        }

        RequireThrowsExactly<std::invalid_argument>(
            []
            {
                LamaPon::DebugRenderer renderer{
                    std::unique_ptr<LamaPon::DebugDrawingBackend>{} };
            },
            "Debug renderer must reject a missing drawing backend");
        auto recordingBackend =
            std::make_unique<RecordingDebugDrawingBackend>();
        auto* const recording = recordingBackend.get();
        LamaPon::DebugRenderer debugRenderer{
            std::move(recordingBackend) };
        const auto identity = DirectX::XMMatrixIdentity();
        const auto color =
            DirectX::XMVectorSet(0.2f, 0.4f, 0.6f, 0.8f);
        const auto requireLineCount =
            [recording](
                const std::size_t expected,
                const char* const message)
            {
                Require(
                    recording->lastLines.size() == expected,
                    message);
            };

        debugRenderer.DrawBounds(
            LamaPon::Bounds2D{
                { -1.0f, -2.0f },
                { 3.0f, 4.0f } },
            color,
            identity,
            identity);
        requireLineCount(4, "2D bounds must generate four debug lines");
        debugRenderer.DrawBounds(
            LamaPon::Bounds3D{
                { -1.0f, -2.0f, -3.0f },
                { 4.0f, 5.0f, 6.0f } },
            color,
            identity,
            identity);
        requireLineCount(12, "3D bounds must generate twelve debug lines");
        debugRenderer.DrawGridXZ(1.0f, 1.0f, identity, identity);
        requireLineCount(6, "XZ grid must generate both axes per index");
        debugRenderer.DrawGridXY(1.0f, 1.0f, identity, identity);
        requireLineCount(6, "XY grid must generate both axes per index");
        const std::array linePoints{
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 2.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 3.0f, 0.0f, 0.0f },
            DirectX::XMFLOAT3{ 4.0f, 0.0f, 0.0f }
        };
        debugRenderer.DrawLines(
            linePoints,
            color,
            identity,
            identity);
        requireLineCount(2, "An unmatched debug point must be ignored");
        debugRenderer.DrawFrustum(
            identity,
            DirectX::XM_PIDIV4,
            1.0f,
            0.1f,
            10.0f,
            color,
            identity,
            identity);
        requireLineCount(16, "Camera frustum must generate sixteen lines");
        debugRenderer.DrawDirectionalLight(
            identity,
            color,
            identity,
            identity);
        requireLineCount(5, "Directional light must generate five lines");
        debugRenderer.DrawPointLight(
            identity,
            1.0f,
            color,
            identity,
            identity);
        requireLineCount(72, "Point light must generate three rings");
        debugRenderer.DrawSpotLight(
            identity,
            2.0f,
            DirectX::XM_PIDIV4,
            color,
            identity,
            identity);
        requireLineCount(28, "Spot light must generate a ring and four rays");
        const auto callsBeforeNoOp = recording->drawCalls;
        const std::array singlePoint{
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f } };
        debugRenderer.DrawLines(
            singlePoint,
            color,
            identity,
            identity);
        debugRenderer.DrawGridXZ(0.0f, 1.0f, identity, identity);
        debugRenderer.DrawGridXY(1.0f, 0.0f, identity, identity);
        Require(
            recording->drawCalls == callsBeforeNoOp,
            "Empty debug geometry must not reach the drawing backend");
        Require(
            recording->lastView._11 == 1.0f
                && recording->lastView._22 == 1.0f
                && recording->lastProjection._33 == 1.0f
                && recording->lastProjection._44 == 1.0f,
            "Debug renderer must forward view and projection matrices");

        // 移行用facadeは未初期化でも例外や部分的なD3D11資源を
        // 返さず、安全に空の結果へ倒します。
        LamaPon::GraphicsDevice graphics;
        Require(
            !graphics.TryLoadCachedEnvironmentViews(0).IsValid(),
            "Environment cache restore must fail safely without a device");
        const std::array<std::uint16_t, 12> coefficients{};
        const auto neutralBakedGiViews =
            graphics.UploadBakedGlobalIlluminationViews(
                1,
                1,
                1,
                coefficients);
        Require(
            !neutralBakedGiViews[0]
                && !neutralBakedGiViews[1]
                && !neutralBakedGiViews[2],
            "Neutral Baked GI upload must return an empty result without a device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                const std::array initialData{
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ coefficients }),
                        8,
                        8
                    }
                };
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        1,
                        1,
                        1,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    initialData));
            },
            "Texture3D creation must require an initialized device");

        TestGraphicsOutputState outputState;
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    backend->CaptureOutputState());
            },
            "Capturing output state requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->RestoreOutputState(outputState);
            },
            "Restoring output state requires an initialized backend");

        LamaPon::ShadowMap shadowMap;
        Require(
            !shadowMap.IsValid() && !shadowMap.ViewHandle(),
            "A default shadow map must not expose a neutral view");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->InitializeShadowMap(
                    shadowMap,
                    1,
                    1,
                    false);
            },
            "Initializing a shadow map requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->BeginShadowMap(shadowMap, 0);
            },
            "Beginning a shadow map requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->EndShadowMap(shadowMap);
            },
            "Ending a shadow map requires an initialized backend");

        LamaPon::RenderTarget offscreenTarget;
        constexpr float clearColor[]{
            0.0f, 0.0f, 0.0f, 1.0f };
        const DirectX::XMFLOAT4X4 historyViewProjection{
            1.0f, 2.0f, 3.0f, 4.0f,
            5.0f, 6.0f, 7.0f, 8.0f,
            9.0f, 10.0f, 11.0f, 12.0f,
            13.0f, 14.0f, 15.0f, 16.0f };
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->ResizeOffscreenTarget(
                    offscreenTarget,
                    1,
                    1);
            },
            "Resizing an offscreen target requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->BeginOffscreenTarget(
                    offscreenTarget,
                    clearColor);
            },
            "Beginning an offscreen target requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->BindOffscreenTarget(
                    offscreenTarget);
            },
            "Binding an offscreen target requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->BindOffscreenTargetDepthOnly(
                    offscreenTarget);
            },
            "Binding an offscreen depth target requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->CaptureOffscreenTargetDepth(
                    offscreenTarget);
            },
            "Capturing offscreen depth requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->CaptureOffscreenTargetColorHistory(
                    offscreenTarget,
                    historyViewProjection);
            },
            "Capturing offscreen color history requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->CaptureOffscreenTargetTemporalHistory(
                    offscreenTarget,
                    historyViewProjection);
            },
            "Capturing offscreen temporal history requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    backend->TryReadOffscreenTargetLuminance(
                        offscreenTarget));
            },
            "Reading offscreen luminance requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->CaptureOffscreenTargetLuminance(
                    offscreenTarget);
            },
            "Capturing offscreen luminance requires an initialized backend");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                backend->PublishOffscreenTarget(
                    offscreenTarget);
            },
            "Publishing an offscreen target requires an initialized backend");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    // COM objectを借用するDevice / AssetManagerはtry内で破棄済みです。
    if (uninitialize)
    {
        CoUninitialize();
    }
    return result;
}
