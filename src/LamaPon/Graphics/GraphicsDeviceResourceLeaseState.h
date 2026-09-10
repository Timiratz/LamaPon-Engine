#pragma once

#include <cstddef>
#include <mutex>

namespace LamaPon
{
    class GraphicsDevice;

    namespace Detail
    {
        // GraphicsDevice、resource lease、SpriteRenderPassが共有する寿命gate
        // です。公開handleへは露出せず、Device破棄後のpass操作がraw ownerへ
        // 触れないことも同じmutexで保証します。
        struct GraphicsDeviceResourceLeaseState final
        {
            explicit GraphicsDeviceResourceLeaseState(
                GraphicsDevice* const deviceOwner) noexcept
                : owner(deviceOwner)
            {
            }

            std::mutex mutex;
            std::size_t activeLeases{};
            bool transitionInProgress{};
            bool closed{};
            GraphicsDevice* owner{};
        };
    }
}
