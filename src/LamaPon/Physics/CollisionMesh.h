#pragma once

#include "LamaPon/Physics/CollisionTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace LamaPon
{
    struct CollisionMeshHit final
    {
        float distance{};
        DirectX::XMFLOAT3 point{};
        DirectX::XMFLOAT3 normal{};
        std::uint32_t triangleIndex{};
    };

    // 静的コライダー用の三角形メッシュ。ローカル空間の頂点と
    // BVH（AABBツリー）を保持し、AABB重なり列挙とレイキャストを
    // 提供します。
    class CollisionMesh final
    {
    public:
        void Build(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices);

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_indices.empty();
        }
        [[nodiscard]] std::size_t
            TriangleCount() const noexcept
        {
            return m_indices.size() / 3;
        }
        [[nodiscard]] const Bounds3D&
            LocalBounds() const noexcept
        {
            return m_localBounds;
        }
        [[nodiscard]] const std::vector<DirectX::XMFLOAT3>&
            Vertices() const noexcept
        {
            return m_vertices;
        }
        [[nodiscard]] const std::vector<std::uint32_t>&
            Indices() const noexcept
        {
            return m_indices;
        }
        void GetTriangle(
            std::uint32_t triangleIndex,
            DirectX::XMFLOAT3& a,
            DirectX::XMFLOAT3& b,
            DirectX::XMFLOAT3& c) const noexcept;

        // ローカルAABBに重なる三角形インデックスを列挙します。
        void QueryOverlaps(
            const Bounds3D& localBounds,
            std::vector<std::uint32_t>& results) const;

        // ローカル空間のレイキャスト。最も近いヒットを返します。
        // directionは正規化済みであること。
        [[nodiscard]] bool Raycast(
            const DirectX::XMFLOAT3& origin,
            const DirectX::XMFLOAT3& direction,
            float maximumDistance,
            CollisionMeshHit& hit) const;

    private:
        struct Node final
        {
            Bounds3D bounds{};
            // 内部ノードは子インデックス、葉は-1。
            std::int32_t leftChild{ -1 };
            std::int32_t rightChild{ -1 };
            std::uint32_t firstTriangle{};
            std::uint32_t triangleCount{};
        };

        [[nodiscard]] std::int32_t BuildNode(
            std::uint32_t firstTriangle,
            std::uint32_t triangleCount,
            std::vector<Bounds3D>& triangleBounds,
            std::vector<DirectX::XMFLOAT3>& centroids);

        std::vector<DirectX::XMFLOAT3> m_vertices;
        std::vector<std::uint32_t> m_indices;
        std::vector<Node> m_nodes;
        // BVH構築で並び替えた三角形番号（葉が参照）。
        std::vector<std::uint32_t> m_triangleOrder;
        Bounds3D m_localBounds{};
    };

    // 三角形とAABBのSAT判定（13軸）。
    [[nodiscard]] bool TriangleIntersectsBounds(
        const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b,
        const DirectX::XMFLOAT3& c,
        const Bounds3D& bounds) noexcept;
}
