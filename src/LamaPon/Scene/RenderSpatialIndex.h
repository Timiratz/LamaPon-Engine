#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <cstddef>
#include <span>
#include <vector>

namespace LamaPon
{
    class GameObject;

    // 描画候補の境界からBVHを作ります。SceneやGPUには依存しません。
    // objectは識別用の借用ポインターで、このクラスは参照先を読みません。
    // 所有者はオブジェクトを使う前にUpdateし、Scene破棄時にClearします。
    class RenderSpatialIndex final
    {
    public:
        struct Entry final
        {
            GameObject* object{};
            Bounds3D bounds{};
            Bounds3D cullingBounds{};
        };

        struct QueryResult final
        {
            // 葉に残った候補。厳密な可視判定・LOD・遮蔽判定は呼び出し側。
            std::vector<unsigned char> candidates;
            std::size_t nodeTests{};
        };

        // 同じ順序・対象・境界なら再利用してtrue。それ以外は再構築。
        // 更新・検索は呼び出し側で直列化します。
        [[nodiscard]] bool Update(std::vector<Entry> next);
        void Clear() noexcept;
        [[nodiscard]] std::span<const Entry> Entries() const noexcept { return m_entries; }
        [[nodiscard]] std::size_t NodeCount() const noexcept { return m_nodes.size(); }

        // intersectsはノードの境界が検索範囲に触れるときtrueを返します。
        // 通常カメラと影で異なる視錐台判定を、同じ木の走査へ適用します。
        template<class Intersects>
        [[nodiscard]] QueryResult Query(const Intersects& intersects) const
        {
            QueryResult result{ std::vector<unsigned char>(m_entries.size()), 0 };
            if (m_nodes.empty()) return result;
            const auto visit = [this, &intersects, &result](
                auto&& self, const std::size_t nodeIndex) -> void
            {
                const auto& node = m_nodes[nodeIndex];
                ++result.nodeTests;
                if (!intersects(node.bounds)) return;
                if (node.count > 0)
                {
                    for (std::size_t index = node.first; index < node.first + node.count; ++index)
                    {
                        result.candidates[m_order[index]] = 1u;
                    }
                    return;
                }
                self(self, node.left);
                self(self, node.right);
            };
            visit(visit, 0);
            return result;
        }

    private:
        struct Node final
        {
            Bounds3D bounds{};
            std::size_t first{};
            std::size_t count{};
            std::size_t left{};
            std::size_t right{};
        };

        void Rebuild(std::vector<Entry> next);
        std::vector<Entry> m_entries;
        std::vector<std::size_t> m_order;
        std::vector<Node> m_nodes;
    };
}
