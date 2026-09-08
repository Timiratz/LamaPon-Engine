#include "LamaPon/Graphics/GraphicsBackend.h"

#include "LamaPon/Graphics/D3D11Backend.h"

#include <stdexcept>

namespace LamaPon
{
    GraphicsBackendSelection SelectGraphicsBackend(
        const RenderingApi requestedApi) noexcept
    {
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
                RenderingApi::DirectX11,
                RenderingApiFallbackReason::NotImplemented
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
        // TODO: D3D12Backend実装後は、ここでactiveApiに応じた
        // 具象Backendを生成する。未実装中は選択段階でD3D11へ倒す。
        if (activeApi == RenderingApi::DirectX11)
        {
            return std::make_unique<D3D11Backend>();
        }
        throw std::logic_error(
            "The requested graphics backend is not implemented.");
    }
}
