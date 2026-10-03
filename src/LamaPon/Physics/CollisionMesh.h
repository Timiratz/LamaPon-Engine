#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    struct CollisionMeshHit final
    {
        // ローカル始点からの接触距離
        float distance{};
        // 接触点のローカル座標
        DirectX::XMFLOAT3 point{};
        // 入射側へ向けたローカル法線
        DirectX::XMFLOAT3 normal{};
        // 検出した三角形番号
        std::uint32_t triangleIndex{};
    };

    // 静的コライダー向けに、ローカル頂点とBVHを所有します。
    class CollisionMesh final
    {
    public:
        // 三角形メッシュとBVHを構築します(vertices: 所有権を渡す頂点列, indices: 所有権を渡す頂点番号列)。
        // 三角形に満たない末尾番号は除き、範囲外番号や空頂点ではメッシュ全体を空にします。
        void Build(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices);

        // 三角形の頂点番号が空か返します。
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_indices.empty();
        }
        // 三角形の総数を返します。
        [[nodiscard]] std::size_t
            TriangleCount() const noexcept
        {
            return m_indices.size() / 3;
        }
        // 全体のローカルAABBを参照します。
        [[nodiscard]] const Bounds3D&
            LocalBounds() const noexcept
        {
            return m_localBounds;
        }
        // 所有するローカル頂点列を参照します。
        [[nodiscard]] const std::vector<DirectX::XMFLOAT3>&
            Vertices() const noexcept
        {
            return m_vertices;
        }
        // 三角形ごとの頂点番号列を参照します。
        [[nodiscard]] const std::vector<std::uint32_t>&
            Indices() const noexcept
        {
            return m_indices;
        }
        // 三角形のローカル頂点を取得します(triangleIndex: TriangleCount未満の番号, a: 第1頂点の出力, b: 第2頂点の出力, c: 第3頂点の出力)。
        void GetTriangle(
            std::uint32_t triangleIndex,
            DirectX::XMFLOAT3& a,
            DirectX::XMFLOAT3& b,
            DirectX::XMFLOAT3& c) const noexcept;

        // ローカルAABBに重なる三角形番号を追加します(localBounds: 検索範囲, results: 追加先)。
        // 既存のresultsは消去しません。
        void QueryOverlaps(
            const Bounds3D& localBounds,
            std::vector<std::uint32_t>& results) const;

        // 最も近いローカル接触を求めます(origin: レイ始点, direction: 正規化済み方向, maximumDistance: 距離上限, hit: 検出結果の出力)。
        // 距離上限未満を両面判定し、法線は入射側へ向け、未検出ではhitを変更しません。
        [[nodiscard]] bool Raycast(
            const DirectX::XMFLOAT3& origin,
            const DirectX::XMFLOAT3& direction,
            float maximumDistance,
            CollisionMeshHit& hit) const;

    private:
        struct Node final
        {
            // 配下の三角形を含む境界
            Bounds3D bounds{};
            // 左の子ノード番号か葉の-1
            std::int32_t leftChild{ -1 };
            // 右の子ノード番号か葉の-1
            std::int32_t rightChild{ -1 };
            // 葉が参照する並べ替え列の先頭
            std::uint32_t firstTriangle{};
            // 葉が参照する三角形の個数
            std::uint32_t triangleCount{};
        };

        // 最長軸の重心中央値でBVHを再帰構築します(firstTriangle: 並べ替え列の先頭, triangleCount: 対象数, triangleBounds: 三角形境界列, centroids: 重心列)。
        [[nodiscard]] std::int32_t BuildNode(
            std::uint32_t firstTriangle,
            std::uint32_t triangleCount,
            std::vector<Bounds3D>& triangleBounds,
            std::vector<DirectX::XMFLOAT3>& centroids);

        // 所有するローカル頂点列
        std::vector<DirectX::XMFLOAT3> m_vertices;
        // 三角形ごとの頂点番号列
        std::vector<std::uint32_t> m_indices;
        // BVHのノード配列
        std::vector<Node> m_nodes;
        // BVH用に並べ替えた三角形番号
        std::vector<std::uint32_t> m_triangleOrder;
        // メッシュ全体のローカル境界
        Bounds3D m_localBounds{};
    };

    // 13軸のSATで三角形とAABBの重なりを調べます(a: 第1頂点, b: 第2頂点, c: 第3頂点, bounds: 同じ座標系の境界)。
    [[nodiscard]] bool TriangleIntersectsBounds(
        const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b,
        const DirectX::XMFLOAT3& c,
        const Bounds3D& bounds) noexcept;
}
