#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <utility>

namespace LamaPon
{
    bool RenderSpatialIndex::Update(std::vector<Entry> next)
    {
        const auto boundsEqual = [](
            const Bounds3D& left,
            const Bounds3D& right) noexcept
        {
            return left.minimum.x == right.minimum.x
                && left.minimum.y == right.minimum.y
                && left.minimum.z == right.minimum.z
                && left.maximum.x == right.maximum.x
                && left.maximum.y == right.maximum.y
                && left.maximum.z == right.maximum.z;
        };
        bool unchanged = next.size()
            == m_entries.size();
        for (std::size_t index = 0;
            unchanged && index < next.size();
            ++index)
        {
            unchanged = next[index].object
                    == m_entries[index].object
                && boundsEqual(
                    next[index].bounds,
                    m_entries[index].bounds)
                && boundsEqual(
                    next[index].cullingBounds,
                    m_entries[index].cullingBounds);
        }
        if (unchanged)
        {
            return true;
        }

        Rebuild(std::move(next));
        return false;
    }

    void RenderSpatialIndex::Rebuild(std::vector<Entry> next)
    {
        // 失敗し得る確保を先に済ませ、途中で古い索引の内容を壊しません。
        // 同じ規模の移動では既存容量を再利用し、毎フレームの再確保を避けます。
        m_order.reserve(next.size());
        m_nodes.reserve(next.size() * 2u);
        m_entries = std::move(next);
        m_order.resize(
            m_entries.size());
        std::iota(
            m_order.begin(),
            m_order.end(),
            std::size_t{});
        m_nodes.clear();
        if (m_entries.empty())
        {
            return;
        }

        const auto mergeBounds = [](
            const Bounds3D& left,
            const Bounds3D& right) noexcept
        {
            return Bounds3D{
                {
                    std::min(left.minimum.x, right.minimum.x),
                    std::min(left.minimum.y, right.minimum.y),
                    std::min(left.minimum.z, right.minimum.z)
                },
                {
                    std::max(left.maximum.x, right.maximum.x),
                    std::max(left.maximum.y, right.maximum.y),
                    std::max(left.maximum.z, right.maximum.z)
                }
            };
        };
        const auto centerAt = [this](
            const std::size_t entry,
            const std::size_t axis) noexcept
        {
            const auto& bounds =
                m_entries[entry].cullingBounds;
            switch (axis)
            {
            case 0:
                return bounds.minimum.x + bounds.maximum.x;
            case 1:
                return bounds.minimum.y + bounds.maximum.y;
            default:
                return bounds.minimum.z + bounds.maximum.z;
            }
        };

        const auto buildNode = [this,
            &mergeBounds,
            &centerAt](
            auto&& self,
            const std::size_t begin,
            const std::size_t end) -> std::size_t
        {
            const std::size_t nodeIndex =
                m_nodes.size();
            m_nodes.emplace_back();
            Bounds3D bounds = m_entries[
                m_order[begin]].cullingBounds;
            for (std::size_t index = begin + 1;
                index < end;
                ++index)
            {
                bounds = mergeBounds(
                    bounds,
                    m_entries[
                        m_order[index]]
                        .cullingBounds);
            }
            m_nodes[nodeIndex].bounds = bounds;

            const std::size_t count = end - begin;
            if (count <= 8u)
            {
                m_nodes[nodeIndex].first = begin;
                m_nodes[nodeIndex].count = count;
                return nodeIndex;
            }

            const std::array<float, 3> extents{
                bounds.maximum.x - bounds.minimum.x,
                bounds.maximum.y - bounds.minimum.y,
                bounds.maximum.z - bounds.minimum.z
            };
            const std::size_t axis = static_cast<std::size_t>(
                std::distance(
                    extents.begin(),
                    std::max_element(
                        extents.begin(),
                        extents.end())));
            const std::size_t middle = begin + count / 2u;
            std::nth_element(
                m_order.begin()
                    + static_cast<std::ptrdiff_t>(begin),
                m_order.begin()
                    + static_cast<std::ptrdiff_t>(middle),
                m_order.begin()
                    + static_cast<std::ptrdiff_t>(end),
                [&centerAt, axis](
                    const std::size_t left,
                    const std::size_t right)
                {
                    return centerAt(left, axis)
                        < centerAt(right, axis);
                });
            const auto left = self(self, begin, middle);
            const auto right = self(self, middle, end);
            m_nodes[nodeIndex].left = left;
            m_nodes[nodeIndex].right = right;
            return nodeIndex;
        };
        static_cast<void>(buildNode(
            buildNode,
            0,
            m_order.size()));
    }

    void RenderSpatialIndex::Clear() noexcept
    {
        m_entries.clear();
        m_order.clear();
        m_nodes.clear();
    }
}
