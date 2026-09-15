#pragma once

// EditorのD3D12 rendererだけが使うSDK非公開bridgeです。公開
// GraphicsDevice APIへnative Device、Queue、CommandListを出しません。
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
        [[nodiscard]] static D3D12Backend* Backend(
            GraphicsDevice& graphics) noexcept;
    };
}
