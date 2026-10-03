#include "LamaPon/Physics/SpatialHashBroadPhase.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // 一つの境界を展開するセル数上限
    constexpr std::int64_t MaximumCellsPerCollider = 4096;

    struct CellKey final
    {
        // セルのX座標
        std::int32_t x{};
        // セルのY座標
        std::int32_t y{};
        // セルのZ座標
        std::int32_t z{};

        // セルの三軸座標が等しいか比較します。
        bool operator==(const CellKey&) const = default;
    };

    struct CellKeyHash final
    {
        // セル座標のハッシュを求めます(key: セル座標)。
        std::size_t operator()(
            const CellKey& key) const noexcept
        {
            // 三軸座標から作るセルハッシュ
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
        // 境界番号の組のハッシュを求めます(pair: 接触候補の組)。
        std::size_t operator()(
            const LamaPon::BroadPhasePair& pair) const noexcept
        {
            // 組の小さい入力番号のハッシュ
            const auto leftHash =
                std::hash<std::size_t>{}(pair.left);
            // 組の大きい入力番号のハッシュ
            const auto rightHash =
                std::hash<std::size_t>{}(pair.right);
            return leftHash
                ^ (rightHash
                    + 0x9e3779b97f4a7c15ull
                    + (leftHash << 6)
                    + (leftHash >> 2));
        }
    };

    // 座標を範囲内の整数セル番号へ変換します(value: 座標成分, cellSize: 正の有限セル幅)。
    // 非有限の座標はセル0にします。
    std::int32_t CellCoordinate(
        const float value,
        const float cellSize) noexcept
    {
        if (!std::isfinite(value))
        {
            return 0;
        }
        // 切り下げた整数セル座標
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

    // 番号順を揃えて自身を除く候補を追加します(pairs: 重複を除く候補集合, left: 第1境界番号, right: 第2境界番号)。
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

    // 同じセルの境界と過大境界から候補を作ります(bounds: 3D境界配列, requestedCellSize: 有限の要求セル幅)。
    LamaPon::BroadPhaseResult BuildPairs(
        const std::span<const LamaPon::Bounds3D> bounds,
        const float requestedCellSize)
    {
        // 許容範囲へ制限したセル幅
        const float cellSize = std::clamp(
            requestedCellSize,
            0.25f,
            100.0f);
        // セルごとの入力境界番号の一覧
        std::unordered_map<
            CellKey,
            std::vector<std::size_t>,
            CellKeyHash> cells;
        // セル展開を省く入力境界番号
        std::vector<std::size_t> oversized;

        // セルへ登録する入力境界番号
        for (std::size_t index = 0;
            index < bounds.size();
            ++index)
        {
            // セル範囲を求める入力境界
            const auto& value = bounds[index];
            // 境界を含む最小Xセル番号
            const std::int32_t minimumX =
                CellCoordinate(value.minimum.x, cellSize);
            // 境界を含む最小Yセル番号
            const std::int32_t minimumY =
                CellCoordinate(value.minimum.y, cellSize);
            // 境界を含む最小Zセル番号
            const std::int32_t minimumZ =
                CellCoordinate(value.minimum.z, cellSize);
            // 境界を含む最大Xセル番号
            const std::int32_t maximumX =
                CellCoordinate(value.maximum.x, cellSize);
            // 境界を含む最大Yセル番号
            const std::int32_t maximumY =
                CellCoordinate(value.maximum.y, cellSize);
            // 境界を含む最大Zセル番号
            const std::int32_t maximumZ =
                CellCoordinate(value.maximum.z, cellSize);

            // X軸方向へ展開するセル数
            const std::int64_t spanX =
                static_cast<std::int64_t>(maximumX)
                - minimumX + 1;
            // Y軸方向へ展開するセル数
            const std::int64_t spanY =
                static_cast<std::int64_t>(maximumY)
                - minimumY + 1;
            // Z軸方向へ展開するセル数
            const std::int64_t spanZ =
                static_cast<std::int64_t>(maximumZ)
                - minimumZ + 1;
            // セル範囲が逆転または過大か
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

            // 境界を登録するZセル番号
            for (std::int64_t z = minimumZ;
                z <= maximumZ;
                ++z)
            {
                // 境界を登録するYセル番号
                for (std::int64_t y = minimumY;
                    y <= maximumY;
                    ++y)
                {
                    // 境界を登録するXセル番号
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

        // 重複を除く接触候補の集合
        std::unordered_set<
            LamaPon::BroadPhasePair,
            PairHash> uniquePairs;
        // cell: 候補を作るセル座標
        // indices: セルへ登録した境界番号
        for (const auto& [cell, indices] : cells)
        {
            static_cast<void>(cell);
            // セル内の候補組の先頭番号
            for (std::size_t left = 0;
                left < indices.size();
                ++left)
            {
                // セル内の候補組の相手番号
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
        // 全対象と組を作る入力境界番号
        for (const auto oversizedIndex : oversized)
        {
            // 過大境界と組を作る入力番号
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

        // 組とセル数の集計結果
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
        // Zを0へ拡張した2D境界配列
        std::vector<Bounds3D> expanded;
        expanded.reserve(bounds.size());
        // 3Dへ拡張する2D境界
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
