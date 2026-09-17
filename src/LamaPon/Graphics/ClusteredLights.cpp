#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstddef>
#include <cstring>
#include <utility>

// API 66以前のGame Moduleはversion検査より先に旧constructor importを
// 解決します。このx64 loader互換thunkは旧layout全体を空状態にして、
// 静的初期化された旧objectも安全に破棄できるようにします。実際の
// ClusteredLights利用はAPI version不一致で拒否されます。
extern "C" void* LamaPonConstructLegacyClusteredLights(
    void* const storage,
    void*,
    void*,
    const void*) noexcept
{
    // API 66 layout: 10 ComPtr + 3 GraphicsViewHandle(shared_ptr)。
    static_assert(sizeof(LamaPon::GraphicsViewHandle)
        == 2u * sizeof(void*));
    constexpr std::size_t LegacyLayoutSize =
        10u * sizeof(void*)
        + 3u * sizeof(LamaPon::GraphicsViewHandle);
    if (storage != nullptr)
    {
        std::memset(storage, 0, LegacyLayoutSize);
    }
    return storage;
}

namespace LamaPon
{
    Detail::ClusteredLightsBackendState*
        Detail::ClusteredLightsBackendAccess::Get(
            ClusteredLights& clusteredLights) noexcept
    {
        return clusteredLights.m_backendState.get();
    }

    const Detail::ClusteredLightsBackendState*
        Detail::ClusteredLightsBackendAccess::Get(
            const ClusteredLights& clusteredLights) noexcept
    {
        return clusteredLights.m_backendState.get();
    }

    void Detail::ClusteredLightsBackendAccess::Publish(
        ClusteredLights& clusteredLights,
        std::unique_ptr<ClusteredLightsBackendState> state) noexcept
    {
        clusteredLights.m_backendState = std::move(state);
    }

    ClusteredLights::ClusteredLights() noexcept = default;

    ClusteredLights::~ClusteredLights() noexcept = default;
}
