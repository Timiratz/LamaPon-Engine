#include "LamaPon/Physics/SpatialHashBroadPhase.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr std::int64_t MaximumCellsPerCollider = 4096;

    struct CellKey final
    {
        std::int32_t x{};
        std::int32_t y{};
        std::int32_t z{};

        bool operator==(const CellKey&) const = default;
    };

    struct CellKeyHash final
    {
        std::size_t operator()(
            const CellKey& key) const noexcept
        {
            std::size_t result =
                static_cast<std::uint32_t>(key.x)
                * 73856093u;
            result ^= static_cast<std::uint32_t>(key.y)
                * 19349663u;
            result ^= static_cast<std::uint32_t>(key.z)
                * 83492791u;
            return result;
        }
    };

    struct PairHash final
    {
        std::size_t operator()(
            const LamaPon::BroadPhasePair& pair) const noexcept
        {
            const auto leftHash =
                std::hash<std::size_t>{}(pair.left);
            const auto rightHash =
                std::hash<std::size_t>{}(pair.right);
            return leftHash
                ^ (rightHash
                    + 0x9e3779b97f4a7c15ull
                    + (leftHash << 6)
                    + (leftHash >> 2));
        }
    };

    std::int32_t CellCoordinate(
        const float value,
        const float cellSize) noexcept
    {
        if (!std::isfinite(value))
        {
            return 0;
        }
        const double coordinate = std::floor(
            static_cast<double>(value)
            / static_cast<double>(cellSize));
        return static_cast<std::int32_t>(std::clamp(
            coordinate,
            static_cast<double>(
                std::numeric_limits<std::int32_t>::min()),
            static_cast<double>(
                std::numeric_limits<std::int32_t>::max())));
    }

    void AddPair(
        std::unordered_set<
            LamaPon::BroadPhasePair,
            PairHash>& pairs,
        const std::size_t left,
        const std::size_t right)
    {
        if (left == right)
        {
            return;
        }
        pairs.insert({
            std::min(left, right),
            std::max(left, right)
        });
    }

    LamaPon::BroadPhaseResult BuildPairs(
        const std::span<const LamaPon::Bounds3D> bounds,
        const float requestedCellSize)
    {
        const float cellSize = std::clamp(
            requestedCellSize,
            0.25f,
            100.0f);
        std::unordered_map<
            CellKey,
            std::vector<std::size_t>,
            CellKeyHash> cells;
        std::vector<std::size_t> oversized;

        for (std::size_t index = 0;
            index < bounds.size();
            ++index)
        {
            const auto& value = bounds[index];
            const std::int32_t minimumX =
                CellCoordinate(value.minimum.x, cellSize);
            const std::int32_t minimumY =
                CellCoordinate(value.minimum.y, cellSize);
            const std::int32_t minimumZ =
                CellCoordinate(value.minimum.z, cellSize);
            const std::int32_t maximumX =
                CellCoordinate(value.maximum.x, cellSize);
            const std::int32_t maximumY =
                CellCoordinate(value.maximum.y, cellSize);
            const std::int32_t maximumZ =
                CellCoordinate(value.maximum.z, cellSize);

            const std::int64_t spanX =
                static_cast<std::int64_t>(maximumX)
                - minimumX + 1;
            const std::int64_t spanY =
                static_cast<std::int64_t>(maximumY)
                - minimumY + 1;
            const std::int64_t spanZ =
                static_cast<std::int64_t>(maximumZ)
                - minimumZ + 1;
            const bool invalidSpan =
                spanX <= 0
                || spanY <= 0
                || spanZ <= 0
                || spanX > MaximumCellsPerCollider
                || spanY > MaximumCellsPerCollider
                || spanZ > MaximumCellsPerCollider
                || spanX * spanY
                    > MaximumCellsPerCollider
                || spanX * spanY * spanZ
                    > MaximumCellsPerCollider;
            if (invalidSpan)
            {
                oversized.push_back(index);
                continue;
            }

            for (std::int64_t z = minimumZ;
                z <= maximumZ;
                ++z)
            {
                for (std::int64_t y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    for (std::int64_t x = minimumX;
                        x <= maximumX;
                        ++x)
                    {
                        cells[{
                            static_cast<std::int32_t>(x),
                            static_cast<std::int32_t>(y),
                            static_cast<std::int32_t>(z)
                        }].push_back(index);
                    }
                }
            }
        }

        std::unordered_set<
            LamaPon::BroadPhasePair,
            PairHash> uniquePairs;
        for (const auto& [cell, indices] : cells)
        {
            static_cast<void>(cell);
            for (std::size_t left = 0;
                left < indices.size();
                ++left)
            {
                for (std::size_t right = left + 1;
                    right < indices.size();
                    ++right)
                {
                    AddPair(
                        uniquePairs,
                        indices[left],
                        indices[right]);
                }
            }
        }
        for (const auto oversizedIndex : oversized)
        {
            for (std::size_t index = 0;
                index < bounds.size();
                ++index)
            {
                AddPair(
                    uniquePairs,
                    oversizedIndex,
                    index);
            }
        }

        LamaPon::BroadPhaseResult result;
        result.pairs.assign(
            uniquePairs.begin(),
            uniquePairs.end());
        std::ranges::sort(result.pairs);
        result.occupiedCellCount = cells.size();
        result.oversizedColliderCount =
            oversized.size();
        return result;
    }
}

namespace LamaPon
{
    BroadPhaseResult BuildSpatialHashPairs(
        const std::span<const Bounds2D> bounds,
        const float cellSize)
    {
        std::vector<Bounds3D> expanded;
        expanded.reserve(bounds.size());
        for (const auto& value : bounds)
        {
            expanded.push_back({
                {
                    value.minimum.x,
                    value.minimum.y,
                    0.0f
                },
                {
                    value.maximum.x,
                    value.maximum.y,
                    0.0f
                }
            });
        }
        return BuildPairs(expanded, cellSize);
    }

    BroadPhaseResult BuildSpatialHashPairs(
        const std::span<const Bounds3D> bounds,
        const float cellSize)
    {
        return BuildPairs(bounds, cellSize);
    }
}
