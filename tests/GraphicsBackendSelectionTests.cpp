#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <iostream>
#include <stdexcept>
#include <typeinfo>

namespace
{
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

        LamaPon::RenderTarget offscreenTarget;
        constexpr float clearColor[]{
            0.0f, 0.0f, 0.0f, 1.0f };
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
