#include "LamaPon/Graphics/GraphicsBackend.h"

#include "LamaPon/Graphics/D3D11Backend.h"
#include "LamaPon/Graphics/D3D12Backend.h"

#include <stdexcept>

namespace LamaPon
{
    GraphicsBackendSelection SelectGraphicsBackend(
        const RenderingApi requestedApi) noexcept
    {
        return SelectGraphicsBackend(
            requestedApi,
            GraphicsStartupProfile::FullRenderer);
    }

    GraphicsBackendSelection SelectGraphicsBackend(
        const RenderingApi requestedApi,
        const GraphicsStartupProfile profile) noexcept
    {
        // Startup profiles were introduced while DirectX 12 only exposed a
        // bootstrap renderer.  Keep accepting them for source compatibility,
        // but the completed experimental renderer no longer needs an opt-in
        // path beyond selecting DirectX12Experimental itself.
        static_cast<void>(profile);
        switch (requestedApi)
        {
        case RenderingApi::Auto:
            return {
                RenderingApi::Auto,
                RenderingApi::DirectX11,
                RenderingApiFallbackReason::None
            };
        case RenderingApi::DirectX11:
            return {
                RenderingApi::DirectX11,
                RenderingApi::DirectX11,
                RenderingApiFallbackReason::None
            };
        case RenderingApi::DirectX12Experimental:
            return {
                RenderingApi::DirectX12Experimental,
                RenderingApi::DirectX12Experimental,
                RenderingApiFallbackReason::None
            };
        }
        return {
            RenderingApi::DirectX11,
            RenderingApi::DirectX11,
            RenderingApiFallbackReason::UnknownApi
        };
    }

    std::unique_ptr<GraphicsBackend>
        CreateGraphicsBackend(const RenderingApi activeApi)
    {
        switch (activeApi)
        {
        case RenderingApi::DirectX11:
            return std::make_unique<D3D11Backend>();
        case RenderingApi::DirectX12Experimental:
            return std::make_unique<D3D12Backend>();
        case RenderingApi::Auto:
        default:
            throw std::logic_error(
                "The requested graphics backend is not implemented.");
        }
    }
}
