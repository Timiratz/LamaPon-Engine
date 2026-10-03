#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <utility>

namespace LamaPon
{
    bool RenderSpatialIndex::Update(std::vector<Entry> next)
    {
        // 境界の全成分が一致するか返します(left: 比較元の境界, right: 比較先の境界)。
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
        // 既存の索引を再利用できる状態
        bool unchanged = next.size()
            == m_entries.size();
        // 照合または集約する候補添字
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
        // 先に容量を確保し、確保失敗時に既存の索引を維持します。
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

        // 両方を囲む境界を返します(left: 一方の境界, right: もう一方の境界)。
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
        // 指定軸の中心座標の2倍を返します(entry: 候補の添字, axis: X・Y・Zの軸番号)。
        const auto centerAt = [this](
            const std::size_t entry,
            const std::size_t axis) noexcept
        {
            // 集約するカリング境界
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

        // 候補区間を分割してノード添字を返します(self: 再帰用の自身, begin: 開始添字, end: 終端の次の添字)。
        const auto buildNode = [this,
            &mergeBounds,
            &centerAt](
            auto&& self,
            const std::size_t begin,
            const std::size_t end) -> std::size_t
        {
            // 構築するノードの添字
            const std::size_t nodeIndex =
                m_nodes.size();
            m_nodes.emplace_back();
            // 集約するカリング境界
            Bounds3D bounds = m_entries[
                m_order[begin]].cullingBounds;
            // 照合または集約する候補添字
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

            // 分割する候補の数
            const std::size_t count = end - begin;
            if (count <= 8u)
            {
                m_nodes[nodeIndex].first = begin;
                m_nodes[nodeIndex].count = count;
                return nodeIndex;
            }

            // 境界の各軸方向の長さ
            const std::array<float, 3> extents{
                bounds.maximum.x - bounds.minimum.x,
                bounds.maximum.y - bounds.minimum.y,
                bounds.maximum.z - bounds.minimum.z
            };
            // 長さが最大の分割軸
            const std::size_t axis = static_cast<std::size_t>(
                std::distance(
                    extents.begin(),
                    std::max_element(
                        extents.begin(),
                        extents.end())));
            // 左右へ分割する中央添字
            const std::size_t middle = begin + count / 2u;
            // 選んだ軸の中心位置を比較します(left: 一方の候補添字, right: もう一方の候補添字)。
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
            // 左の子ノードの添字
            const auto left = self(self, begin, middle);
            // 右の子ノードの添字
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
