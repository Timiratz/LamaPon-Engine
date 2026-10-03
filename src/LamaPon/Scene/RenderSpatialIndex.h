#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <cstddef>
#include <span>
#include <vector>

namespace LamaPon
{
    class GameObject;

    // 境界のBVHで描画候補を絞り、更新と検索は呼び出し側で直列化します。
    // 使用前にUpdateし、識別用の非所有ポインターを破棄する前にClearします。
    class RenderSpatialIndex final
    {
    public:
        struct Entry final
        {
            // 識別用の非所有オブジェクト
            GameObject* object{};
            // 描画対象のワールド境界
            Bounds3D bounds{};
            // カリング用のワールド境界
            Bounds3D cullingBounds{};
        };

        struct QueryResult final
        {
            // 登録順の候補フラグ
            std::vector<unsigned char> candidates;
            // 境界を判定したノード数
            std::size_t nodeTests{};
        };

        // 対象・順序・境界が同一なら再利用してtrueを返し、異なる場合は再構築します(next: 次の描画候補)。
        [[nodiscard]] bool Update(std::vector<Entry> next);
        // 登録した描画候補と索引を消去します。
        void Clear() noexcept;
        // 登録した候補を次の更新・消去まで有効な読み取りビューで返します。
        [[nodiscard]] std::span<const Entry> Entries() const noexcept { return m_entries; }
        // BVHのノード数を返します。
        [[nodiscard]] std::size_t NodeCount() const noexcept { return m_nodes.size(); }

        // 境界に触れる葉の候補を返します(intersects: 境界との交差判定)。
        // 個々の候補の厳密な可視・LOD・遮蔽判定は呼び出し側で行います。
        template<class Intersects>
        [[nodiscard]] QueryResult Query(const Intersects& intersects) const
        {
            // 登録順の候補と判定回数
            QueryResult result{ std::vector<unsigned char>(m_entries.size()), 0 };
            if (m_nodes.empty()) return result;
            // 境界に触れる子孫を走査します(self: 再帰用の自身, nodeIndex: 検索するノード添字)。
            const auto visit = [this, &intersects, &result](
                auto&& self, const std::size_t nodeIndex) -> void
            {
                // 検索するBVHノード
                const auto& node = m_nodes[nodeIndex];
                ++result.nodeTests;
                if (!intersects(node.bounds)) return;
                if (node.count > 0)
                {
                    // 葉に属する候補の順序添字
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
            // 子孫を囲むカリング境界
            Bounds3D bounds{};
            // 葉の候補順序の開始添字
            std::size_t first{};
            // 葉の候補数、枝は0
            std::size_t count{};
            // 左の子ノードの添字
            std::size_t left{};
            // 右の子ノードの添字
            std::size_t right{};
        };

        // 候補の境界からBVHを再構築します(next: 次の描画候補)。
        void Rebuild(std::vector<Entry> next);
        // 登録順の描画候補
        std::vector<Entry> m_entries;
        // BVHの葉に並べた候補添字
        std::vector<std::size_t> m_order;
        // 描画候補を囲むBVHノード
        std::vector<Node> m_nodes;
    };
}
