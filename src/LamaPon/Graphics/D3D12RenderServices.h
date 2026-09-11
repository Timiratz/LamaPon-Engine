#pragma once

#include <memory>

namespace LamaPon
{
    class D3D12Backend;
    class GraphicsRenderServices;

    [[nodiscard]] std::unique_ptr<GraphicsRenderServices>
        CreateD3D12GraphicsRenderServices(D3D12Backend& backend);
}
