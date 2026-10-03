#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace LamaPon::ModelLod
{
    // 生成する追加LODの段数
    inline constexpr std::size_t LevelCount = 2;

    namespace Detail
    {
        struct ClusterKey final
        {
            // 格子のXセル番号
            std::uint32_t x{};
            // 格子のYセル番号
            std::uint32_t y{};
            // 格子のZセル番号
            std::uint32_t z{};

            // 格子のXYZセル番号が全て一致するか比べる。
            [[nodiscard]] bool operator==(
                const ClusterKey&) const noexcept = default;
        };

        struct ClusterKeyHash final
        {
            // XYZセル番号を混ぜてハッシュにする(key: 格子セルの組)。
            [[nodiscard]] std::size_t operator()(
                const ClusterKey& key) const noexcept
            {
                // XYZを混ぜるセル識別ハッシュ
                std::size_t hash = key.x;
                hash ^= static_cast<std::size_t>(key.y)
                    + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= static_cast<std::size_t>(key.z)
                    + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };
    }

    // 元頂点の属性を保ち、格子内の代表へ索引だけを集約する(vertices: positionを持つ頂点列, indices: 元の三角形索引列, bounds: 量子化する頂点境界, vertexRatio: 目標の代表頂点比率)。
    template<typename Vertex>
    [[nodiscard]] std::vector<std::uint32_t>
        BuildClusteredIndices(
            const std::span<const Vertex> vertices,
            const std::span<const std::uint32_t> indices,
            const Bounds3D& bounds,
            const float vertexRatio)
    {
        if (vertices.size() < 24 || indices.size() < 96)
        {
            return {};
        }

        // 頂点境界のXYZの長さ
        const std::array<float, 3> extents{
            std::max(
                bounds.maximum.x - bounds.minimum.x,
                0.0f),
            std::max(
                bounds.maximum.y - bounds.minimum.y,
                0.0f),
            std::max(
                bounds.maximum.z - bounds.minimum.z,
                0.0f)
        };
        // 頂点境界の最大辺の長さ
        const float maximumExtent = *std::max_element(
            extents.begin(),
            extents.end());
        if (!(maximumExtent > 0.000001f))
        {
            return {};
        }

        // 最大辺に比べて厚みのある軸数
        std::size_t activeDimensions{};
        // 厚みを調べる軸の長さ
        for (const float extent : extents)
        {
            activeDimensions += extent > maximumExtent * 0.0001f
                ? 1u
                : 0u;
        }
        activeDimensions = std::max<std::size_t>(
            activeDimensions,
            1u);
        // 目標とする代表セル数
        const auto desiredClusters = std::max<std::size_t>(
            8u,
            static_cast<std::size_t>(
                static_cast<double>(vertices.size())
                * std::clamp(vertexRatio, 0.05f, 0.95f)));
        // 有効次元ごとの目標セル数
        const float cellsPerDimension = std::pow(
            static_cast<float>(desiredClusters),
            1.0f / static_cast<float>(activeDimensions));
        // XYZ軸ごとのセル数
        std::array<std::uint32_t, 3> resolution{};
        // セル数を設定する軸番号
        for (std::size_t axis = 0; axis < resolution.size(); ++axis)
        {
            resolution[axis] = extents[axis]
                    > maximumExtent * 0.0001f
                ? std::max(
                    1u,
                    static_cast<std::uint32_t>(std::ceil(
                        cellsPerDimension
                        * extents[axis]
                        / maximumExtent)))
                : 1u;
        }

        // 格子セルと最初の代表頂点の索引
        std::unordered_map<
            Detail::ClusterKey,
            std::uint32_t,
            Detail::ClusterKeyHash> representatives;
        representatives.reserve(desiredClusters);
        // 元頂点から代表頂点への対応表
        std::vector<std::uint32_t> remap(
            vertices.size(),
            0u);
        // 代表へ対応させる元頂点番号
        for (std::size_t index = 0; index < vertices.size(); ++index)
        {
            // 代表セルを求める元頂点位置
            const auto& position = vertices[index].position;
            // 座標をセル番号へ切り詰める(value: 座標値, minimum: 軸の最小座標, extent: 軸の長さ, cells: 軸のセル数)。
            const auto quantize = [](
                const float value,
                const float minimum,
                const float extent,
                const std::uint32_t cells) noexcept
            {
                if (cells <= 1u || extent <= 0.000001f)
                {
                    return 0u;
                }
                // 最大セルを超えない正規化座標
                const float normalized = std::clamp(
                    (value - minimum) / extent,
                    0.0f,
                    0.999999f);
                return std::min(
                    cells - 1u,
                    static_cast<std::uint32_t>(
                        normalized * static_cast<float>(cells)));
            };
            // 元頂点が所属するセルの組
            const Detail::ClusterKey key{
                quantize(
                    position.x,
                    bounds.minimum.x,
                    extents[0],
                    resolution[0]),
                quantize(
                    position.y,
                    bounds.minimum.y,
                    extents[1],
                    resolution[1]),
                quantize(
                    position.z,
                    bounds.minimum.z,
                    extents[2],
                    resolution[2])
            };
            // iterator: セルの代表頂点、inserted: 新しいセルの登録
            const auto [iterator, inserted] =
                representatives.try_emplace(
                    key,
                    static_cast<std::uint32_t>(index));
            static_cast<void>(inserted);
            remap[index] = iterator->second;
        }

        // 折り畳まれた三角形を除く索引
        std::vector<std::uint32_t> simplified;
        simplified.reserve(indices.size());
        // 処理する三角形の索引列位置
        for (std::size_t index = 0;
            index + 2 < indices.size();
            index += 3)
        {
            // 元三角形の第一頂点索引
            const auto sourceA = indices[index];
            // 元三角形の第二頂点索引
            const auto sourceB = indices[index + 1];
            // 元三角形の第三頂点索引
            const auto sourceC = indices[index + 2];
            if (sourceA >= remap.size()
                || sourceB >= remap.size()
                || sourceC >= remap.size())
            {
                continue;
            }
            // 第一頂点の代表索引
            const auto a = remap[sourceA];
            // 第二頂点の代表索引
            const auto b = remap[sourceB];
            // 第三頂点の代表索引
            const auto c = remap[sourceC];
            if (a == b || b == c || c == a)
            {
                continue;
            }
            simplified.push_back(a);
            simplified.push_back(b);
            simplified.push_back(c);
        }

        // 三角形が残らない結果や、索引数が10%以上減らないLODは採用しない。
        if (simplified.size() < 3
            || simplified.size() * 10u > indices.size() * 9u)
        {
            return {};
        }
        return simplified;
    }

    // 代表頂点の目標比率50%・20%で作り、両段が有効で減らない後段は無効にする(vertices: positionを持つ頂点列, indices: 元の三角形索引列, bounds: 量子化する頂点境界)。
    template<typename Vertex>
    [[nodiscard]] std::array<
        std::vector<std::uint32_t>,
        LevelCount> BuildLevels(
            const std::span<const Vertex> vertices,
            const std::span<const std::uint32_t> indices,
            const Bounds3D& bounds)
    {
        // 二段階の追加LOD索引列
        std::array<
            std::vector<std::uint32_t>,
            LevelCount> result;
        result[0] = BuildClusteredIndices(
            vertices,
            indices,
            bounds,
            0.50f);
        result[1] = BuildClusteredIndices(
            vertices,
            indices,
            bounds,
            0.20f);
        if (!result[0].empty()
            && !result[1].empty()
            && result[1].size() >= result[0].size())
        {
            result[1].clear();
        }
        return result;
    }
}
