#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <array>
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
}

static_assert(noexcept(LamaPon::SelectGraphicsBackend(
    LamaPon::RenderingApi::DirectX11)));
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
        return 1;
    }
    return 0;
}
