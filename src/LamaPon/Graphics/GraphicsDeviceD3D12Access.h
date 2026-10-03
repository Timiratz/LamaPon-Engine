#pragma once

#include "LamaPon/Graphics/GraphicsDevice.h"

namespace LamaPon
{
    class D3D12Backend;
}

namespace LamaPon::Detail
{
    class GraphicsDeviceD3D12Access final
    {
    public:
        // 現在のD3D12バックエンドを借用し、他APIまたは状態不在ならヌルを返す(graphics: 描画デバイス)。
        [[nodiscard]] static D3D12Backend* Backend(
            GraphicsDevice& graphics) noexcept;
    };
}
