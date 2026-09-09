#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/DebugRenderer.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

        auto backend = LamaPon::CreateGraphicsBackend(
            LamaPon::RenderingApi::DirectX11);
        Require(backend != nullptr,
            "DirectX 11 backend factory must return a backend");
        Require(backend->Api() == LamaPon::RenderingApi::DirectX11,
            "DirectX 11 backend must report the DirectX 11 API");
        Require(!backend->IsInitialized(),
            "A newly created backend must not initialize graphics resources");
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
            !graphics.TryLoadCachedEnvironment(0).IsValid(),
            "Environment cache restore must fail safely without a device");
        const std::array<std::uint16_t, 12> coefficients{};
        const auto bakedGiViews =
            graphics.UploadBakedGlobalIllumination(
                1,
                1,
                1,
                coefficients);
        Require(
            bakedGiViews[0] == nullptr
                && bakedGiViews[1] == nullptr
                && bakedGiViews[2] == nullptr,
            "Baked GI upload must return an empty result without a device");

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
