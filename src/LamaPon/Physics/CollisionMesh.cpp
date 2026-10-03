#include "LamaPon/Physics/CollisionMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    using DirectX::XMFLOAT3;
    using LamaPon::Bounds3D;

    // BVHの葉へ格納する三角形上限
    constexpr std::uint32_t LeafTriangleLimit = 8;

    // 二つのAABBを囲む境界を求めます(left: 第1境界, right: 第2境界)。
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

    // 二つのAABBの接触を含む重なりを調べます(left: 第1境界, right: 第2境界)。
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

    // スラブ法でレイとAABBを判定します(origin: 始点, direction: 方向, bounds: 境界, maximumDistance: 距離上限)。
    bool RayIntersectsBoundsLocal(
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const Bounds3D& bounds,
        const float maximumDistance) noexcept
    {
        // 全軸を通る進入距離の下限
        float entry = 0.0f;
        // 全軸を通る退出距離の上限
        float exit = maximumDistance;
        // レイ始点の軸別成分
        const float origins[3]{
            origin.x, origin.y, origin.z };
        // レイ方向の軸別成分
        const float directions[3]{
            direction.x, direction.y, direction.z };
        // AABBの軸別最小座標
        const float minimums[3]{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z };
        // AABBの軸別最大座標
        const float maximums[3]{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z };
        // 判定するAABBの主軸番号
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
            // レイ方向成分の逆数
            const float inverse =
                1.0f / directions[axis];
            // 現在軸の近い境界までの距離
            float near =
                (minimums[axis] - origins[axis])
                * inverse;
            // 現在軸の遠い境界までの距離
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

    // 三角形の両面とレイの正方向を判定します(origin: 始点, direction: 方向, a: 第1頂点, b: 第2頂点, c: 第3頂点, distance: 接触距離の出力)。
    bool RayIntersectsTriangle(
        const XMFLOAT3& origin,
        const XMFLOAT3& direction,
        const XMFLOAT3& a,
        const XMFLOAT3& b,
        const XMFLOAT3& c,
        float& distance) noexcept
    {
        // 第1頂点から第2頂点への辺
        const XMFLOAT3 edge1{
            b.x - a.x, b.y - a.y, b.z - a.z };
        // 第1頂点から第3頂点への辺
        const XMFLOAT3 edge2{
            c.x - a.x, c.y - a.y, c.z - a.z };
        // レイ方向と第2辺の外積
        const XMFLOAT3 perpendicular{
            direction.y * edge2.z
                - direction.z * edge2.y,
            direction.z * edge2.x
                - direction.x * edge2.z,
            direction.x * edge2.y
                - direction.y * edge2.x };
        // レイと三角形の向きを示す行列式
        const float determinant =
            edge1.x * perpendicular.x
            + edge1.y * perpendicular.y
            + edge1.z * perpendicular.z;
        if (std::abs(determinant) < 1e-10f)
        {
            return false;
        }
        // 行列式の逆数
        const float inverseDeterminant =
            1.0f / determinant;
        // 第1頂点からレイ始点への差
        const XMFLOAT3 toOrigin{
            origin.x - a.x,
            origin.y - a.y,
            origin.z - a.z };
        // 第2頂点の重心座標成分
        const float u =
            (toOrigin.x * perpendicular.x
                + toOrigin.y * perpendicular.y
                + toOrigin.z * perpendicular.z)
            * inverseDeterminant;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        // 始点との差と第1辺の外積
        const XMFLOAT3 crossQ{
            toOrigin.y * edge1.z - toOrigin.z * edge1.y,
            toOrigin.z * edge1.x - toOrigin.x * edge1.z,
            toOrigin.x * edge1.y - toOrigin.y * edge1.x };
        // 第3頂点の重心座標成分
        const float v =
            (direction.x * crossQ.x
                + direction.y * crossQ.y
                + direction.z * crossQ.z)
            * inverseDeterminant;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        // 正方向のレイ接触距離
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

        m_indices.resize(m_indices.size() / 3 * 3);
        // 構築する三角形の個数
        const std::size_t triangleCount =
            m_indices.size() / 3;
        if (triangleCount == 0 || m_vertices.empty())
        {
            m_vertices.clear();
            m_indices.clear();
            return;
        }
        // 範囲を検証する頂点番号
        for (const auto index : m_indices)
        {
            if (index >= m_vertices.size())
            {
                m_vertices.clear();
                m_indices.clear();
                return;
            }
        }

        // 三角形ごとのローカル境界
        std::vector<Bounds3D> triangleBounds(
            triangleCount);
        // 三角形ごとのローカル重心
        std::vector<DirectX::XMFLOAT3> centroids(
            triangleCount);
        m_triangleOrder.resize(triangleCount);
        // 境界と重心を求める三角形番号
        for (std::uint32_t triangle = 0;
            triangle < triangleCount;
            ++triangle)
        {
            m_triangleOrder[triangle] = triangle;
            // 三角形の第1ローカル頂点
            DirectX::XMFLOAT3 a{};
            // 三角形の第2ローカル頂点
            DirectX::XMFLOAT3 b{};
            // 三角形の第3ローカル頂点
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
        // 構築したBVHのルート番号
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
        // 構築するBVHノード
        Node node;
        node.bounds =
            triangleBounds[m_triangleOrder[firstTriangle]];
        // 境界を結合する三角形の相対番号
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

        // 構築するBVHノード番号
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

        // ノード境界の軸別の長さ
        const DirectX::XMFLOAT3 size{
            node.bounds.maximum.x
                - node.bounds.minimum.x,
            node.bounds.maximum.y
                - node.bounds.minimum.y,
            node.bounds.maximum.z
                - node.bounds.minimum.z };
        // 重心を分割する最長軸番号
        int axis = 0;
        if (size.y > size.x)
        {
            axis = 1;
        }
        if (size.z > (axis == 0 ? size.x : size.y))
        {
            axis = 2;
        }
        // 分割軸の重心成分を取得します(triangle: 三角形番号)。
        const auto centroidValue =
            [&centroids, axis](
                const std::uint32_t triangle) noexcept
        {
            // 参照する三角形の重心
            const auto& centroid = centroids[triangle];
            return axis == 0
                ? centroid.x
                : axis == 1
                    ? centroid.y
                    : centroid.z;
        };
        // 並べ替える三角形範囲の先頭
        const auto begin =
            m_triangleOrder.begin() + firstTriangle;
        // 並べ替える三角形範囲の中央
        const auto middle = begin + triangleCount / 2;
        // 並べ替える三角形範囲の終端
        const auto end = begin + triangleCount;
        // 分割軸の重心を比較します(left: 左の三角形番号, right: 右の三角形番号)。
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

        // 左の部分木へ渡す三角形数
        const std::uint32_t leftCount =
            triangleCount / 2;
        // 構築した左の子ノード番号
        const auto leftChild = BuildNode(
            firstTriangle,
            leftCount,
            triangleBounds,
            centroids);
        // 構築した右の子ノード番号
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
        // 三角形の頂点番号列の先頭位置
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
        // 再利用するスレッド専用探索スタック
        thread_local std::vector<std::int32_t> stack;
        stack.reserve(256);
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            // 探索対象のBVHノード番号
            const auto nodeIndex = stack.back();
            stack.pop_back();
            // 探索対象のBVHノード
            const auto& node = m_nodes[nodeIndex];
            if (!BoundsOverlap(node.bounds, localBounds))
            {
                continue;
            }
            if (node.leftChild < 0)
            {
                // 葉の中の三角形の相対番号
                for (std::uint32_t offset = 0;
                    offset < node.triangleCount;
                    ++offset)
                {
                    // 判定する三角形の番号
                    const auto triangle =
                        m_triangleOrder[
                            node.firstTriangle + offset];
                    // 三角形の第1ローカル頂点
                    DirectX::XMFLOAT3 a{};
                    // 三角形の第2ローカル頂点
                    DirectX::XMFLOAT3 b{};
                    // 三角形の第3ローカル頂点
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
        // 現時点で最も近い検出距離
        float closest = maximumDistance;
        // 最も近い検出三角形の番号
        std::uint32_t closestTriangle{};
        // 三角形の接触を検出済みか
        bool found = false;
        // 再利用するスレッド専用探索スタック
        thread_local std::vector<std::int32_t> stack;
        stack.reserve(256);
        stack.clear();
        stack.push_back(0);
        while (!stack.empty())
        {
            // 探索対象のBVHノード番号
            const auto nodeIndex = stack.back();
            stack.pop_back();
            // 探索対象のBVHノード
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
                // 葉の中の三角形の相対番号
                for (std::uint32_t offset = 0;
                    offset < node.triangleCount;
                    ++offset)
                {
                    // 判定する三角形の番号
                    const auto triangle =
                        m_triangleOrder[
                            node.firstTriangle + offset];
                    // 三角形の第1ローカル頂点
                    DirectX::XMFLOAT3 a{};
                    // 三角形の第2ローカル頂点
                    DirectX::XMFLOAT3 b{};
                    // 三角形の第3ローカル頂点
                    DirectX::XMFLOAT3 c{};
                    GetTriangle(triangle, a, b, c);
                    // 今回の三角形との接触距離
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
        // 検出した三角形の第1頂点
        DirectX::XMFLOAT3 a{};
        // 検出した三角形の第2頂点
        DirectX::XMFLOAT3 b{};
        // 検出した三角形の第3頂点
        DirectX::XMFLOAT3 c{};
        GetTriangle(closestTriangle, a, b, c);
        // 第1頂点から第2頂点への辺
        const DirectX::XMFLOAT3 edge1{
            b.x - a.x, b.y - a.y, b.z - a.z };
        // 第1頂点から第3頂点への辺
        const DirectX::XMFLOAT3 edge2{
            c.x - a.x, c.y - a.y, c.z - a.z };
        // 検出三角形の法線
        DirectX::XMFLOAT3 normal{
            edge1.y * edge2.z - edge1.z * edge2.y,
            edge1.z * edge2.x - edge1.x * edge2.z,
            edge1.x * edge2.y - edge1.y * edge2.x };
        // 正規化前の三角形法線の長さ
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
        // 法線とレイ方向の内積
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
        // SATの基準となるAABB中心
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x)
                * 0.5f,
            (bounds.minimum.y + bounds.maximum.y)
                * 0.5f,
            (bounds.minimum.z + bounds.maximum.z)
                * 0.5f };
        // AABBの軸別の半径
        const DirectX::XMFLOAT3 extents{
            (bounds.maximum.x - bounds.minimum.x)
                * 0.5f,
            (bounds.maximum.y - bounds.minimum.y)
                * 0.5f,
            (bounds.maximum.z - bounds.minimum.z)
                * 0.5f };
        // AABB中心基準の三角形頂点
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
        // AABBの軸別の半径
        const float halves[3]{
            extents.x, extents.y, extents.z };

        // AABBの主軸で分離を検査します。
        // 判定するAABBの主軸番号
        for (int axis = 0; axis < 3; ++axis)
        {
            // 頂点を投影した区間の下限
            const float minimum = std::min({
                vertices[0][axis],
                vertices[1][axis],
                vertices[2][axis] });
            // 頂点を投影した区間の上限
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

        // 三角形の三辺の軸別成分
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

        // 三角形法線の分離軸
        const float normal[3]{
            edges[0][1] * edges[1][2]
                - edges[0][2] * edges[1][1],
            edges[0][2] * edges[1][0]
                - edges[0][0] * edges[1][2],
            edges[0][0] * edges[1][1]
                - edges[0][1] * edges[1][0] };
        {
            // 三角形面と中心の符号付き距離
            const float distance =
                normal[0] * vertices[0][0]
                + normal[1] * vertices[0][1]
                + normal[2] * vertices[0][2];
            // 分離軸に投影したAABB半径
            const float radius =
                halves[0] * std::abs(normal[0])
                + halves[1] * std::abs(normal[1])
                + halves[2] * std::abs(normal[2]);
            if (std::abs(distance) > radius)
            {
                return false;
            }
        }

        // 外積の分離軸を作る三角形辺番号
        for (int edge = 0; edge < 3; ++edge)
        {
            // 三角形辺と外積を作る主軸番号
            for (int axis = 0; axis < 3; ++axis)
            {
                // 外積の次の主軸番号
                const int nextAxis = (axis + 1) % 3;
                // 外積の最後の主軸番号
                const int lastAxis = (axis + 2) % 3;
                // 辺と主軸の外積による分離軸
                float testAxis[3]{};
                testAxis[nextAxis] =
                    -edges[edge][lastAxis];
                testAxis[lastAxis] =
                    edges[edge][nextAxis];

                // 三角形投影区間の下限
                float minimum =
                    std::numeric_limits<float>::max();
                // 三角形投影区間の上限
                float maximum =
                    -std::numeric_limits<float>::max();
                // 分離軸へ投影する頂点番号
                for (int vertex = 0; vertex < 3;
                    ++vertex)
                {
                    // 分離軸上の頂点の投影値
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
                // 外積軸に投影したAABB半径
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
