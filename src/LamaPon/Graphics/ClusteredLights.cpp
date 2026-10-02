#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstddef>
#include <cstring>
#include <utility>

// 旧ABIの格納領域を空状態にする(storage: API66の旧領域)。
// 無名の引数3個は使用しない。
// x64の旧モジュールが版検査前にコンストラクターを解決するため、旧レイアウト全体を初期化する。
extern "C" void* LamaPonConstructLegacyClusteredLights(
    void* const storage,
    void*,
    void*,
    const void*) noexcept
{
    // API66の領域はComPtr10個と共有ビュー3個分を必要とする。
    static_assert(sizeof(LamaPon::GraphicsViewHandle)
        == 2u * sizeof(void*));
    // API66のオブジェクト領域サイズ
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
    // 所有する状態を借用し、未公開ならヌルを返す(clusteredLights: 参照する公開窓口)。
    Detail::ClusteredLightsBackendState*
        Detail::ClusteredLightsBackendAccess::Get(
            ClusteredLights& clusteredLights) noexcept
    {
        return clusteredLights.m_backendState.get();
    }

    // 所有する状態を読取専用で借用し、未公開ならヌルを返す(clusteredLights: 参照する公開窓口)。
    const Detail::ClusteredLightsBackendState*
        Detail::ClusteredLightsBackendAccess::Get(
            const ClusteredLights& clusteredLights) noexcept
    {
        return clusteredLights.m_backendState.get();
    }

    // 状態の所有権を公開窓口へ移し、既存状態を解放する(clusteredLights: 公開する窓口, state: 完成したバックエンド状態)。
    void Detail::ClusteredLightsBackendAccess::Publish(
        ClusteredLights& clusteredLights,
        std::unique_ptr<ClusteredLightsBackendState> state) noexcept
    {
        clusteredLights.m_backendState = std::move(state);
    }

    ClusteredLights::ClusteredLights() noexcept = default;

    ClusteredLights::~ClusteredLights() noexcept = default;
}
