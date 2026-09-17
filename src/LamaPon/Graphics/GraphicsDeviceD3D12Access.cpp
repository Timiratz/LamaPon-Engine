#include "LamaPon/Graphics/GraphicsDeviceD3D12Access.h"

#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

namespace LamaPon::Detail
{
    D3D12Backend* GraphicsDeviceD3D12Access::Backend(
        GraphicsDevice& graphics) noexcept
    {
        return graphics.m_state != nullptr
            ? dynamic_cast<D3D12Backend*>(
                graphics.m_state->m_backend.get())
            : nullptr;
    }
}
