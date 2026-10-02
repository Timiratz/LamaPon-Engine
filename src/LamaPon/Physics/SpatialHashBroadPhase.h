#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <compare>
#include <cstddef>
#include <span>
#include <vector>

namespace LamaPon
{
    struct BroadPhasePair final
    {
        // 組の小さい入力境界番号
        std::size_t left{};
        // 組の大きい入力境界番号
        std::size_t right{};

        // 二つの入力番号の辞書順で組を比較します。
        auto operator<=>(const BroadPhasePair&) const = default;
    };

    struct BroadPhaseResult final
    {
        // 重複を除いた接触候補の組
        std::vector<BroadPhasePair> pairs;
        // 境界を登録したセル数
        std::size_t occupiedCellCount{};
        // セル展開を省いた境界数
        std::size_t oversizedColliderCount{};
    };

    // 2D境界を空間セルへ分けて接触候補を列挙します(bounds: 境界配列, cellSize: 有限のセル幅)。
    // セル幅は0.25〜100へ制限し、過大または逆転した境界は全対象との候補にします。
    // 結果は入力番号の組で重複なく昇順とし、実際の重なりは別途判定します。
    [[nodiscard]] BroadPhaseResult BuildSpatialHashPairs(
        std::span<const Bounds2D> bounds,
        float cellSize);
    // 3D境界を空間セルへ分けて接触候補を列挙します(bounds: 境界配列, cellSize: 有限のセル幅)。
    // セル幅と候補の規則は2D版と同じです。
    [[nodiscard]] BroadPhaseResult BuildSpatialHashPairs(
        std::span<const Bounds3D> bounds,
        float cellSize);
}
