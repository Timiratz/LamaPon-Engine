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
        std::size_t left{};
        std::size_t right{};

        auto operator<=>(const BroadPhasePair&) const = default;
    };

    struct BroadPhaseResult final
    {
        std::vector<BroadPhasePair> pairs;
        std::size_t occupiedCellCount{};
        std::size_t oversizedColliderCount{};
    };

    [[nodiscard]] BroadPhaseResult BuildSpatialHashPairs(
        std::span<const Bounds2D> bounds,
        float cellSize);
    [[nodiscard]] BroadPhaseResult BuildSpatialHashPairs(
        std::span<const Bounds3D> bounds,
        float cellSize);
}
