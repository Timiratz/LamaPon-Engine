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
    inline constexpr std::size_t LevelCount = 2;

    namespace Detail
    {
        struct ClusterKey final
        {
            std::uint32_t x{};
            std::uint32_t y{};
            std::uint32_t z{};

            [[nodiscard]] bool operator==(
                const ClusterKey&) const noexcept = default;
        };

        struct ClusterKeyHash final
        {
            [[nodiscard]] std::size_t operator()(
                const ClusterKey& key) const noexcept
            {
                std::size_t hash = key.x;
                hash ^= static_cast<std::size_t>(key.y)
                    + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                hash ^= static_cast<std::size_t>(key.z)
                    + 0x9e3779b9u + (hash << 6) + (hash >> 2);
                return hash;
            }
        };
    }

    // 頂点属性は元の頂点バッファに残し、近い頂点を代表頂点へ
    // リマップしたインデックスだけを作ります。UVや法線を作り直さない
    // ため、高品質なLOD生成器より保守的ですが、インポート時の追加依存が
    // 無く、実行時にはインデックスバッファを切り替えるだけで済みます。
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
        const float maximumExtent = *std::max_element(
            extents.begin(),
            extents.end());
        if (!(maximumExtent > 0.000001f))
        {
            return {};
        }

        std::size_t activeDimensions{};
        for (const float extent : extents)
        {
            activeDimensions += extent > maximumExtent * 0.0001f
                ? 1u
                : 0u;
        }
        activeDimensions = std::max<std::size_t>(
            activeDimensions,
            1u);
        const auto desiredClusters = std::max<std::size_t>(
            8u,
            static_cast<std::size_t>(
                static_cast<double>(vertices.size())
                * std::clamp(vertexRatio, 0.05f, 0.95f)));
        const float cellsPerDimension = std::pow(
            static_cast<float>(desiredClusters),
            1.0f / static_cast<float>(activeDimensions));
        std::array<std::uint32_t, 3> resolution{};
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

        std::unordered_map<
            Detail::ClusterKey,
            std::uint32_t,
            Detail::ClusterKeyHash> representatives;
        representatives.reserve(desiredClusters);
        std::vector<std::uint32_t> remap(
            vertices.size(),
            0u);
        for (std::size_t index = 0; index < vertices.size(); ++index)
        {
            const auto& position = vertices[index].position;
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
                const float normalized = std::clamp(
                    (value - minimum) / extent,
                    0.0f,
                    0.999999f);
                return std::min(
                    cells - 1u,
                    static_cast<std::uint32_t>(
                        normalized * static_cast<float>(cells)));
            };
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
            const auto [iterator, inserted] =
                representatives.try_emplace(
                    key,
                    static_cast<std::uint32_t>(index));
            static_cast<void>(inserted);
            remap[index] = iterator->second;
        }

        std::vector<std::uint32_t> simplified;
        simplified.reserve(indices.size());
        for (std::size_t index = 0;
            index + 2 < indices.size();
            index += 3)
        {
            const auto sourceA = indices[index];
            const auto sourceB = indices[index + 1];
            const auto sourceC = indices[index + 2];
            if (sourceA >= remap.size()
                || sourceB >= remap.size()
                || sourceC >= remap.size())
            {
                continue;
            }
            const auto a = remap[sourceA];
            const auto b = remap[sourceB];
            const auto c = remap[sourceC];
            if (a == b || b == c || c == a)
            {
                continue;
            }
            simplified.push_back(a);
            simplified.push_back(b);
            simplified.push_back(c);
        }

        // 数%しか減らないLODは、切り替えと追加メモリの方が高くつきます。
        if (simplified.size() < 3
            || simplified.size() * 10u > indices.size() * 9u)
        {
            return {};
        }
        return simplified;
    }

    template<typename Vertex>
    [[nodiscard]] std::array<
        std::vector<std::uint32_t>,
        LevelCount> BuildLevels(
            const std::span<const Vertex> vertices,
            const std::span<const std::uint32_t> indices,
            const Bounds3D& bounds)
    {
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
