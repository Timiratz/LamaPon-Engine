#include "LamaPon/Physics/CollisionMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    using DirectX::XMFLOAT3;
    using LamaPon::Bounds3D;

    constexpr std::uint32_t LeafTriangleLimit = 8;

    Bounds3D MergeBounds(
        const Bounds3D& left,
        const Bounds3D& right) noexcept
    {
        return {
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
    }

    bool BoundsOverlap(
        const Bounds3D& left,
        const Bounds3D& right) noexcept
    {
        return left.minimum.x <= right.maximum.x
            && left.maximum.x >= right.minimum.x
            && left.minimum.y <= right.maximum.y
            && left.maximum.y >= right.minimum.y
            && left.minimum.z <= right.maximum.z
            && left.maximum.z >= right.minimum.z;
    }

    // スラブ法。ヒットしない場合はfalse。
    bool RayIntersectsBoundsLocal(
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const Bounds3D& bounds,
        const float maximumDistance) noexcept
    {
        float entry = 0.0f;
        float exit = maximumDistance;
        const float origins[3]{
            origin.x, origin.y, origin.z };
        const float directions[3]{
            direction.x, direction.y, direction.z };
        const float minimums[3]{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z };
        const float maximums[3]{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z };
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(directions[axis]) < 1e-8f)
            {
                if (origins[axis] < minimums[axis]
                    || origins[axis] > maximums[axis])
                {
                    return false;
                }
                continue;
            }
            const float inverse =
                1.0f / directions[axis];
            float near =
                (minimums[axis] - origins[axis])
                * inverse;
            float far =
                (maximums[axis] - origins[axis])
                * inverse;
            if (near > far)
            {
                std::swap(near, far);
            }
            entry = std::max(entry, near);
            exit = std::min(exit, far);
            if (entry > exit)
            {
                return false;
            }
        }
        return true;
    }

    // Möller–Trumboreの両面判定。tは正方向のみ。
    bool RayIntersectsTriangle(
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c,
        float& distance) noexcept
    {
        const XMFLOAT3 edge1{
            b.x - a.x, b.y - a.y, b.z - a.z };
        const XMFLOAT3 edge2{
            c.x - a.x, c.y - a.y, c.z - a.z };
        const XMFLOAT3 perpendicular{
            direction.y * edge2.z
                - direction.z * edge2.y,
            direction.z * edge2.x
                - direction.x * edge2.z,
            direction.x * edge2.y
                - direction.y * edge2.x };
        const float determinant =
            edge1.x * perpendicular.x
            + edge1.y * perpendicular.y
            + edge1.z * perpendicular.z;
        if (std::abs(determinant) < 1e-10f)
        {
            return false;
        }
        const float inverseDeterminant =
            1.0f / determinant;
        const XMFLOAT3 toOrigin{
            origin.x - a.x,
            origin.y - a.y,
            origin.z - a.z };
        const float u =
            (toOrigin.x * perpendicular.x
                + toOrigin.y * perpendicular.y
                + toOrigin.z * perpendicular.z)
            * inverseDeterminant;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        const XMFLOAT3 crossQ{
            toOrigin.y * edge1.z - toOrigin.z * edge1.y,
            toOrigin.z * edge1.x - toOrigin.x * edge1.z,
            toOrigin.x * edge1.y - toOrigin.y * edge1.x };
        const float v =
            (direction.x * crossQ.x
                + direction.y * crossQ.y
                + direction.z * crossQ.z)
            * inverseDeterminant;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        const float t =
            (edge2.x * crossQ.x
                + edge2.y * crossQ.y
                + edge2.z * crossQ.z)
            * inverseDeterminant;
        if (t < 0.0f)
        {
            return false;
        }
        distance = t;
        return true;
    }
}

namespace LamaPon
{
    void CollisionMesh::Build(
        std::vector<DirectX::XMFLOAT3> vertices,
        std::vector<std::uint32_t> indices)
    {
        m_vertices = std::move(vertices);
        m_indices = std::move(indices);
        m_nodes.clear();
        m_triangleOrder.clear();
        m_localBounds = {};

        // 3の倍数に満たない末尾インデックスは切り捨てます。
        m_indices.resize(m_indices.size() / 3 * 3);
        const std::size_t triangleCount =
            m_indices.size() / 3;
        if (triangleCount == 0 || m_vertices.empty())
        {
            m_vertices.clear();
            m_indices.clear();
            return;
        }
        for (const auto index : m_indices)
        {
            if (index >= m_vertices.size())
            {
                m_vertices.clear();
                m_indices.clear();
                return;
            }
        }

        std::vector<Bounds3D> triangleBounds(
            triangleCount);
        std::vector<DirectX::XMFLOAT3> centroids(
            triangleCount);
        m_triangleOrder.resize(triangleCount);
        for (std::uint32_t triangle = 0;
            triangle < triangleCount;
            ++triangle)
        {
            m_triangleOrder[triangle] = triangle;
            DirectX::XMFLOAT3 a{};
            DirectX::XMFLOAT3 b{};
            DirectX::XMFLOAT3 c{};
            GetTriangle(triangle, a, b, c);
            triangleBounds[triangle] = {
                {
                    std::min({ a.x, b.x, c.x }),
                    std::min({ a.y, b.y, c.y }),
                    std::min({ a.z, b.z, c.z })
                },
                {
                    std::max({ a.x, b.x, c.x }),
                    std::max({ a.y, b.y, c.y }),
                    std::max({ a.z, b.z, c.z })
                }
            };
            centroids[triangle] = {
                (a.x + b.x + c.x) / 3.0f,
                (a.y + b.y + c.y) / 3.0f,
                (a.z + b.z + c.z) / 3.0f
            };
        }

        m_nodes.reserve(triangleCount * 2);
        const auto root = BuildNode(
            0,
            static_cast<std::uint32_t>(triangleCount),
            triangleBounds,
            centroids);
        m_localBounds = m_nodes[root].bounds;
    }

    std::int32_t CollisionMesh::BuildNode(
        const std::uint32_t firstTriangle,
        const std::uint32_t triangleCount,
        std::vector<Bounds3D>& triangleBounds,
        std::vector<DirectX::XMFLOAT3>& centroids)
    {
        Node node;
        node.bounds =
            triangleBounds[m_triangleOrder[firstTriangle]];
        for (std::uint32_t offset = 1;
            offset < triangleCount;
            ++offset)
        {
            node.bounds = MergeBounds(
                node.bounds,
                triangleBounds[
                    m_triangleOrder[
                        firstTriangle + offset]]);
        }

        const auto nodeIndex =
            static_cast<std::int32_t>(m_nodes.size());
        m_nodes.push_back(node);
        if (triangleCount <= LeafTriangleLimit)
        {
            m_nodes[nodeIndex].firstTriangle =
                firstTriangle;
            m_nodes[nodeIndex].triangleCount =
                triangleCount;
            return nodeIndex;
        }

        // 最長軸を重心の中央値で分割します。
        const DirectX::XMFLOAT3 size{
            node.bounds.maximum.x
                - node.bounds.minimum.x,
            node.bounds.maximum.y
                - node.bounds.minimum.y,
            node.bounds.maximum.z
                - node.bounds.minimum.z };
        int axis = 0;
        if (size.y > size.x)
        {
            axis = 1;
        }
        if (size.z > (axis == 0 ? size.x : size.y))
        {
            axis = 2;
        }
        const auto centroidValue =
            [&centroids, axis](
                const std::uint32_t triangle) noexcept
        {
            const auto& centroid = centroids[triangle];
            return axis == 0
                ? centroid.x
                : axis == 1
                    ? centroid.y
                    : centroid.z;
        };
        const auto begin =
            m_triangleOrder.begin() + firstTriangle;
        const auto middle = begin + triangleCount / 2;
        const auto end = begin + triangleCount;
        std::nth_element(
            begin,
            middle,
            end,
            [&centroidValue](
                const std::uint32_t left,
                const std::uint32_t right) noexcept
            {
                return centroidValue(left)
                    < centroidValue(right);
            });

        const std::uint32_t leftCount =
            triangleCount / 2;
        const auto leftChild = BuildNode(
            firstTriangle,
            leftCount,
            triangleBounds,
            centroids);
        const auto rightChild = BuildNode(
            firstTriangle + leftCount,
            triangleCount - leftCount,
            triangleBounds,
            centroids);
        m_nodes[nodeIndex].leftChild = leftChild;
        m_nodes[nodeIndex].rightChild = rightChild;
        return nodeIndex;
    }

    void CollisionMesh::GetTriangle(
        const std::uint32_t triangleIndex,
        DirectX::XMFLOAT3& a,
        DirectX::XMFLOAT3& b,
        DirectX::XMFLOAT3& c) const noexcept
    {
        const std::size_t base =
            static_cast<std::size_t>(triangleIndex) * 3;
        a = m_vertices[m_indices[base]];
        b = m_vertices[m_indices[base + 1]];
        c = m_vertices[m_indices[base + 2]];
    }

    void CollisionMesh::QueryOverlaps(
        const Bounds3D& localBounds,
        std::vector<std::uint32_t>& results) const
    {
        if (m_nodes.empty())
        {
            return;
        }
        // Reuse the traversal stack so overlap queries do not allocate on
        // every physics step.
        thread_local std::vector<std::int32_t> stack;
        stack.reserve(256);
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            const auto nodeIndex = stack.back();
            stack.pop_back();
            const auto& node = m_nodes[nodeIndex];
            if (!BoundsOverlap(node.bounds, localBounds))
            {
                continue;
            }
            if (node.leftChild < 0)
            {
                for (std::uint32_t offset = 0;
                    offset < node.triangleCount;
                    ++offset)
                {
                    const auto triangle =
                        m_triangleOrder[
                            node.firstTriangle + offset];
                    DirectX::XMFLOAT3 a{};
                    DirectX::XMFLOAT3 b{};
                    DirectX::XMFLOAT3 c{};
                    GetTriangle(triangle, a, b, c);
                    if (TriangleIntersectsBounds(
                            a,
                            b,
                            c,
                            localBounds))
                    {
                        results.push_back(triangle);
                    }
                }
                continue;
            }
            stack.push_back(node.leftChild);
            stack.push_back(node.rightChild);
        }
    }

    bool CollisionMesh::Raycast(
        const DirectX::XMFLOAT3& origin,
        const DirectX::XMFLOAT3& direction,
        const float maximumDistance,
        CollisionMeshHit& hit) const
    {
        if (m_nodes.empty() || maximumDistance <= 0.0f)
        {
            return false;
        }
        float closest = maximumDistance;
        std::uint32_t closestTriangle{};
        bool found = false;
        // Raycast is used every fixed step by controller-style gameplay.
        // Reusing the traversal stack avoids a heap allocation per query,
        // which otherwise shows up as an occasional frame hitch when a car
        // starts moving or begins drifting.
        thread_local std::vector<std::int32_t> stack;
        stack.reserve(256);
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            const auto nodeIndex = stack.back();
            stack.pop_back();
            const auto& node = m_nodes[nodeIndex];
            if (!RayIntersectsBoundsLocal(
                    origin,
                    direction,
                    node.bounds,
                    closest))
            {
                continue;
            }
            if (node.leftChild < 0)
            {
                for (std::uint32_t offset = 0;
                    offset < node.triangleCount;
                    ++offset)
                {
                    const auto triangle =
                        m_triangleOrder[
                            node.firstTriangle + offset];
                    DirectX::XMFLOAT3 a{};
                    DirectX::XMFLOAT3 b{};
                    DirectX::XMFLOAT3 c{};
                    GetTriangle(triangle, a, b, c);
                    float distance{};
                    if (RayIntersectsTriangle(
                            origin,
                            direction,
                            a,
                            b,
                            c,
                            distance)
                        && distance < closest)
                    {
                        closest = distance;
                        closestTriangle = triangle;
                        found = true;
                    }
                }
                continue;
            }
            stack.push_back(node.leftChild);
            stack.push_back(node.rightChild);
        }
        if (!found)
        {
            return false;
        }

        hit.distance = closest;
        hit.triangleIndex = closestTriangle;
        hit.point = {
            origin.x + direction.x * closest,
            origin.y + direction.y * closest,
            origin.z + direction.z * closest };
        DirectX::XMFLOAT3 a{};
        DirectX::XMFLOAT3 b{};
        DirectX::XMFLOAT3 c{};
        GetTriangle(closestTriangle, a, b, c);
        const DirectX::XMFLOAT3 edge1{
            b.x - a.x, b.y - a.y, b.z - a.z };
        const DirectX::XMFLOAT3 edge2{
            c.x - a.x, c.y - a.y, c.z - a.z };
        DirectX::XMFLOAT3 normal{
            edge1.y * edge2.z - edge1.z * edge2.y,
            edge1.z * edge2.x - edge1.x * edge2.z,
            edge1.x * edge2.y - edge1.y * edge2.x };
        const float length = std::sqrt(
            normal.x * normal.x
            + normal.y * normal.y
            + normal.z * normal.z);
        if (length > 1e-8f)
        {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        }
        // 法線はレイの入射側へ向けます（両面判定のため）。
        const float facing =
            normal.x * direction.x
            + normal.y * direction.y
            + normal.z * direction.z;
        if (facing > 0.0f)
        {
            normal = {
                -normal.x,
                -normal.y,
                -normal.z };
        }
        hit.normal = normal;
        return true;
    }

    bool TriangleIntersectsBounds(
        const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b,
        const DirectX::XMFLOAT3& c,
        const Bounds3D& bounds) noexcept
    {
        // AABB中心基準へ移動して標準の13軸SATを行います。
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x)
                * 0.5f,
            (bounds.minimum.y + bounds.maximum.y)
                * 0.5f,
            (bounds.minimum.z + bounds.maximum.z)
                * 0.5f };
        const DirectX::XMFLOAT3 extents{
            (bounds.maximum.x - bounds.minimum.x)
                * 0.5f,
            (bounds.maximum.y - bounds.minimum.y)
                * 0.5f,
            (bounds.maximum.z - bounds.minimum.z)
                * 0.5f };
        const float vertices[3][3]{
            { a.x - center.x,
                a.y - center.y,
                a.z - center.z },
            { b.x - center.x,
                b.y - center.y,
                b.z - center.z },
            { c.x - center.x,
                c.y - center.y,
                c.z - center.z } };
        const float halves[3]{
            extents.x, extents.y, extents.z };

        // 1) AABBの3軸
        for (int axis = 0; axis < 3; ++axis)
        {
            const float minimum = std::min({
                vertices[0][axis],
                vertices[1][axis],
                vertices[2][axis] });
            const float maximum = std::max({
                vertices[0][axis],
                vertices[1][axis],
                vertices[2][axis] });
            if (minimum > halves[axis]
                || maximum < -halves[axis])
            {
                return false;
            }
        }

        const float edges[3][3]{
            { vertices[1][0] - vertices[0][0],
                vertices[1][1] - vertices[0][1],
                vertices[1][2] - vertices[0][2] },
            { vertices[2][0] - vertices[1][0],
                vertices[2][1] - vertices[1][1],
                vertices[2][2] - vertices[1][2] },
            { vertices[0][0] - vertices[2][0],
                vertices[0][1] - vertices[2][1],
                vertices[0][2] - vertices[2][2] } };

        // 2) 三角形法線
        const float normal[3]{
            edges[0][1] * edges[1][2]
                - edges[0][2] * edges[1][1],
            edges[0][2] * edges[1][0]
                - edges[0][0] * edges[1][2],
            edges[0][0] * edges[1][1]
                - edges[0][1] * edges[1][0] };
        {
            const float distance =
                normal[0] * vertices[0][0]
                + normal[1] * vertices[0][1]
                + normal[2] * vertices[0][2];
            const float radius =
                halves[0] * std::abs(normal[0])
                + halves[1] * std::abs(normal[1])
                + halves[2] * std::abs(normal[2]);
            if (std::abs(distance) > radius)
            {
                return false;
            }
        }

        // 3) 9本のクロス積軸
        for (int edge = 0; edge < 3; ++edge)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                const int nextAxis = (axis + 1) % 3;
                const int lastAxis = (axis + 2) % 3;
                float testAxis[3]{};
                testAxis[nextAxis] =
                    -edges[edge][lastAxis];
                testAxis[lastAxis] =
                    edges[edge][nextAxis];

                float minimum =
                    std::numeric_limits<float>::max();
                float maximum =
                    -std::numeric_limits<float>::max();
                for (int vertex = 0; vertex < 3;
                    ++vertex)
                {
                    const float projection =
                        testAxis[0]
                            * vertices[vertex][0]
                        + testAxis[1]
                            * vertices[vertex][1]
                        + testAxis[2]
                            * vertices[vertex][2];
                    minimum =
                        std::min(minimum, projection);
                    maximum =
                        std::max(maximum, projection);
                }
                const float radius =
                    halves[0] * std::abs(testAxis[0])
                    + halves[1] * std::abs(testAxis[1])
                    + halves[2] * std::abs(testAxis[2]);
                if (minimum > radius
                    || maximum < -radius)
                {
                    return false;
                }
            }
        }
        return true;
    }
}
