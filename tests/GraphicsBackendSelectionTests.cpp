#include "LamaPon/Graphics/GraphicsBackend.h"

#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
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
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
