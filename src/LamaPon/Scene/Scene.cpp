#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Reactive/Reactive.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/CharacterControllerComponent.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/InputMoverComponent.h"
#include "LamaPon/Components/JointComponent.h"
#include "LamaPon/Components/LODGroupComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NavMeshAgentComponent.h"
#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/CircleCollider2DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/Light2DComponent.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Components/SpriteMaskComponent.h"
#include "LamaPon/Components/ParallaxLayerComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/BillboardComponent.h"
#include "LamaPon/Components/RotatorComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Components/SpriteAnimatorComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/TextRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
// 自動露出の順応に実時間（timeScale非依存）が要ります。
#include "LamaPon/Core/Time.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/TemporalJitter.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/SpriteEffect.h"
#include "LamaPon/Physics/SpatialHashBroadPhase.h"
#include "LamaPon/Physics/Collision3D.h"
#include "LamaPon/Physics/PhysicsMaterial.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/SceneManager.h"

#include <algorithm>
#include <array>
#include <DirectXPackedVector.h>

#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace
{
    class BooleanStateScope final
    {
    public:
        BooleanStateScope(
            bool& target,
            const bool value) noexcept
            : m_target(target)
            , m_previous(target)
        {
            m_target = value;
        }

        ~BooleanStateScope()
        {
            m_target = m_previous;
        }

        BooleanStateScope(
            const BooleanStateScope&) = delete;
        BooleanStateScope& operator=(
            const BooleanStateScope&) = delete;

    private:
        bool& m_target;
        bool m_previous;
    };

    struct Contact final
    {
        DirectX::XMFLOAT3 normal;
        float penetration;
        DirectX::XMFLOAT3 point{};
        std::array<DirectX::XMFLOAT3, 4> points{};
        std::size_t pointCount{};
    };

    std::optional<Contact> IntersectBounds(
        const LamaPon::Bounds2D& left,
        const LamaPon::Bounds2D& right)
    {
        const float overlapX = std::min(left.maximum.x, right.maximum.x)
            - std::max(left.minimum.x, right.minimum.x);
        const float overlapY = std::min(left.maximum.y, right.maximum.y)
            - std::max(left.minimum.y, right.minimum.y);
        if (overlapX <= 0.0f || overlapY <= 0.0f)
        {
            return std::nullopt;
        }

        const float centerLeftX = (left.minimum.x + left.maximum.x) * 0.5f;
        const float centerLeftY = (left.minimum.y + left.maximum.y) * 0.5f;
        const float centerRightX = (right.minimum.x + right.maximum.x) * 0.5f;
        const float centerRightY = (right.minimum.y + right.maximum.y) * 0.5f;

        if (overlapX < overlapY)
        {
            const float contactX =
                (std::max(
                    left.minimum.x,
                    right.minimum.x)
                    + std::min(
                        left.maximum.x,
                        right.maximum.x))
                * 0.5f;
            const float minimumY =
                std::max(
                    left.minimum.y,
                    right.minimum.y);
            const float maximumY =
                std::min(
                    left.maximum.y,
                    right.maximum.y);
            return Contact{
                {
                    centerLeftX < centerRightX ? -1.0f : 1.0f,
                    0.0f,
                    0.0f
                },
                overlapX,
                {
                    contactX,
                    (minimumY + maximumY) * 0.5f,
                    0.0f
                },
                {
                    DirectX::XMFLOAT3{
                        contactX,
                        minimumY,
                        0.0f },
                    DirectX::XMFLOAT3{
                        contactX,
                        maximumY,
                        0.0f }
                },
                2
            };
        }

        const float contactY =
            (std::max(
                left.minimum.y,
                right.minimum.y)
                + std::min(
                    left.maximum.y,
                    right.maximum.y))
            * 0.5f;
        const float minimumX =
            std::max(
                left.minimum.x,
                right.minimum.x);
        const float maximumX =
            std::min(
                left.maximum.x,
                right.maximum.x);
        return Contact{
            {
                0.0f,
                centerLeftY < centerRightY ? -1.0f : 1.0f,
                0.0f
            },
            overlapY,
            {
                (minimumX + maximumX) * 0.5f,
                contactY,
                0.0f
            },
            {
                DirectX::XMFLOAT3{
                    minimumX,
                    contactY,
                    0.0f },
                DirectX::XMFLOAT3{
                    maximumX,
                    contactY,
                    0.0f }
            },
            2
        };
    }

    // 円×円の2D接触。法線はleft側を向きます。
    std::optional<Contact> IntersectCircles(
        const LamaPon::Circle2D& left,
        const LamaPon::Circle2D& right)
    {
        const float deltaX =
            left.center.x - right.center.x;
        const float deltaY =
            left.center.y - right.center.y;
        const float radiusSum =
            left.radius + right.radius;
        const float squaredDistance =
            deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance >= radiusSum * radiusSum)
        {
            return std::nullopt;
        }
        const float distance =
            std::sqrt(squaredDistance);
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        if (distance > 0.0001f)
        {
            normal = {
                deltaX / distance,
                deltaY / distance,
                0.0f };
        }
        const float penetration = radiusSum - distance;
        const DirectX::XMFLOAT3 point{
            right.center.x
                + normal.x
                    * (right.radius
                        - penetration * 0.5f),
            right.center.y
                + normal.y
                    * (right.radius
                        - penetration * 0.5f),
            0.0f };
        Contact contact{
            normal,
            penetration,
            point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    // 円×AABBの2D接触。法線は円（left）側を向きます。
    std::optional<Contact> IntersectCircleBounds(
        const LamaPon::Circle2D& circle,
        const LamaPon::Bounds2D& bounds)
    {
        const float closestX = std::clamp(
            circle.center.x,
            bounds.minimum.x,
            bounds.maximum.x);
        const float closestY = std::clamp(
            circle.center.y,
            bounds.minimum.y,
            bounds.maximum.y);
        const float deltaX = circle.center.x - closestX;
        const float deltaY = circle.center.y - closestY;
        const float squaredDistance =
            deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance
            >= circle.radius * circle.radius)
        {
            return std::nullopt;
        }

        DirectX::XMFLOAT3 normal{};
        float penetration{};
        if (squaredDistance > 0.0000001f)
        {
            // 円の中心がボックスの外側にあるケース。
            const float distance =
                std::sqrt(squaredDistance);
            normal = {
                deltaX / distance,
                deltaY / distance,
                0.0f };
            penetration = circle.radius - distance;
        }
        else
        {
            // 中心がボックス内部：最も近い面へ押し出します。
            const float toLeft =
                circle.center.x - bounds.minimum.x;
            const float toRight =
                bounds.maximum.x - circle.center.x;
            const float toBottom =
                circle.center.y - bounds.minimum.y;
            const float toTop =
                bounds.maximum.y - circle.center.y;
            const float smallest = std::min(
                { toLeft, toRight, toBottom, toTop });
            if (smallest == toLeft)
            {
                normal = { -1.0f, 0.0f, 0.0f };
            }
            else if (smallest == toRight)
            {
                normal = { 1.0f, 0.0f, 0.0f };
            }
            else if (smallest == toBottom)
            {
                normal = { 0.0f, -1.0f, 0.0f };
            }
            else
            {
                normal = { 0.0f, 1.0f, 0.0f };
            }
            penetration = circle.radius + smallest;
        }
        const DirectX::XMFLOAT3 point{
            closestX,
            closestY,
            0.0f };
        Contact contact{
            normal,
            penetration,
            point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    DirectX::XMFLOAT2 PolygonCentroid(
        const std::vector<DirectX::XMFLOAT2>& vertices)
    {
        DirectX::XMFLOAT2 centroid{ 0.0f, 0.0f };
        for (const auto& vertex : vertices)
        {
            centroid.x += vertex.x;
            centroid.y += vertex.y;
        }
        const float count = static_cast<float>(
            std::max<std::size_t>(vertices.size(), 1));
        centroid.x /= count;
        centroid.y /= count;
        return centroid;
    }

    // axis（faceの外向き法線）・facePoint（face上の1点）に対する、
    // otherVertices側の最深めり込み量（負＝めり込み、正＝分離）。
    // Box2Dのb2FindMaxSeparation等と同じ定義です。対称形状（例:
    // BoxCollider2Dの上面・底面）は単純な投影オーバーラップ量だと同値に
    // なってしまい、無関係な面が分離軸として選ばれてしまいます。
    // 「otherVertices側の最も浅い点」を見るこの定義なら、incident側が
    // 実際にどちら向きにあるかを区別できます。
    float FaceSeparation(
        const DirectX::XMFLOAT2& axis,
        const DirectX::XMFLOAT2& facePoint,
        const std::vector<DirectX::XMFLOAT2>& otherVertices)
    {
        float minimum = std::numeric_limits<float>::max();
        for (const auto& vertex : otherVertices)
        {
            const float distance =
                axis.x * (vertex.x - facePoint.x)
                    + axis.y * (vertex.y - facePoint.y);
            minimum = std::min(minimum, distance);
        }
        return minimum;
    }

    // BoxCollider2Dは既存どおりAABBのまま扱うため、WorldBounds()の4頂点を
    // 多角形としてPolygon×Polygon判定へ渡します（Box2D自体の挙動は不変）。
    std::vector<DirectX::XMFLOAT2> BoxBoundsToPolygon(
        const LamaPon::Bounds2D& bounds)
    {
        return {
            { bounds.minimum.x, bounds.minimum.y },
            { bounds.maximum.x, bounds.minimum.y },
            { bounds.maximum.x, bounds.maximum.y },
            { bounds.minimum.x, bounds.maximum.y }
        };
    }

    // 多角形の分離軸候補（各エッジの外向き法線）。頂点の巻き順（CW/CCW）に
    // 依存しないよう、重心から外側を向くよう符号を補正します。
    struct PolygonAxis final
    {
        DirectX::XMFLOAT2 axis;
        std::size_t edgeStart;
        bool fromLeft;
    };

    std::vector<PolygonAxis> PolygonAxisCandidates(
        const std::vector<DirectX::XMFLOAT2>& vertices,
        const bool fromLeft)
    {
        std::vector<PolygonAxis> axes;
        if (vertices.size() < 2)
        {
            return axes;
        }
        const auto centroid = PolygonCentroid(vertices);
        axes.reserve(vertices.size());
        for (std::size_t index{}; index < vertices.size(); ++index)
        {
            const auto& a = vertices[index];
            const auto& b = vertices[(index + 1) % vertices.size()];
            const float edgeX = b.x - a.x;
            const float edgeY = b.y - a.y;
            const float length =
                std::sqrt(edgeX * edgeX + edgeY * edgeY);
            if (length <= 0.0001f)
            {
                continue;
            }
            DirectX::XMFLOAT2 normal{ edgeY / length, -edgeX / length };
            const DirectX::XMFLOAT2 midpoint{
                (a.x + b.x) * 0.5f,
                (a.y + b.y) * 0.5f };
            const float towardCentroid =
                (centroid.x - midpoint.x) * normal.x
                    + (centroid.y - midpoint.y) * normal.y;
            if (towardCentroid > 0.0f)
            {
                normal.x = -normal.x;
                normal.y = -normal.y;
            }
            axes.push_back({ normal, index, fromLeft });
        }
        return axes;
    }

    // 線分[a,b]を、direction・p >= offsetを満たす半平面でクリップします。
    // 戻り値は残った頂点数（0〜2）。
    std::size_t ClipSegment(
        const DirectX::XMFLOAT2& a,
        const DirectX::XMFLOAT2& b,
        const DirectX::XMFLOAT2& direction,
        const float offset,
        std::array<DirectX::XMFLOAT2, 2>& output)
    {
        std::size_t count{};
        const float distanceA =
            direction.x * a.x + direction.y * a.y - offset;
        const float distanceB =
            direction.x * b.x + direction.y * b.y - offset;
        if (distanceA >= 0.0f)
        {
            output[count++] = a;
        }
        if (distanceB >= 0.0f)
        {
            output[count++] = b;
        }
        if (distanceA * distanceB < 0.0f && count < 2)
        {
            const float t = distanceA / (distanceA - distanceB);
            output[count++] = {
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t
            };
        }
        return count;
    }

    // 凸多角形×凸多角形のSAT判定。頂点は3個以上、CW/CCWいずれの巻き順でも
    // 構いません。参照エッジに入射エッジをクリップし、最大2点の接触
    // マニフォールドを生成します（2D Box×Boxと同格の精度）。法線は他の2D関数と
    // 同様にleft側（rightから見て押し出される向き）を向きます。
    std::optional<Contact> IntersectPolygons(
        const std::vector<DirectX::XMFLOAT2>& left,
        const std::vector<DirectX::XMFLOAT2>& right)
    {
        if (left.size() < 3 || right.size() < 3)
        {
            return std::nullopt;
        }

        auto axes = PolygonAxisCandidates(left, true);
        const auto rightAxes = PolygonAxisCandidates(right, false);
        axes.insert(axes.end(), rightAxes.begin(), rightAxes.end());
        if (axes.empty())
        {
            return std::nullopt;
        }

        float bestSeparation = std::numeric_limits<float>::lowest();
        std::size_t bestIndex{};
        bool found{};
        for (std::size_t index{}; index < axes.size(); ++index)
        {
            const auto& candidate = axes[index];
            const auto& facePolygon = candidate.fromLeft ? left : right;
            const auto& otherPolygon = candidate.fromLeft ? right : left;
            const float separation = FaceSeparation(
                candidate.axis,
                facePolygon[candidate.edgeStart],
                otherPolygon);
            if (separation > 0.0f)
            {
                // 分離軸が見つかったため接触していません。
                return std::nullopt;
            }
            if (separation > bestSeparation)
            {
                bestSeparation = separation;
                bestIndex = index;
                found = true;
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        const auto winner = axes[bestIndex];
        const auto& referencePolygon = winner.fromLeft ? left : right;
        const auto& incidentPolygon = winner.fromLeft ? right : left;

        const auto referenceA = referencePolygon[winner.edgeStart];
        const auto referenceB = referencePolygon[
            (winner.edgeStart + 1) % referencePolygon.size()];

        // 入射エッジ：分離軸と最も逆向き（内積が最小）の辺を選びます。
        const auto incidentAxes =
            PolygonAxisCandidates(incidentPolygon, winner.fromLeft);
        std::size_t incidentStart{};
        float lowestDot = std::numeric_limits<float>::max();
        for (const auto& candidate : incidentAxes)
        {
            const float dot =
                candidate.axis.x * winner.axis.x
                    + candidate.axis.y * winner.axis.y;
            if (dot < lowestDot)
            {
                lowestDot = dot;
                incidentStart = candidate.edgeStart;
            }
        }
        const auto incidentA = incidentPolygon[incidentStart];
        const auto incidentB = incidentPolygon[
            (incidentStart + 1) % incidentPolygon.size()];

        const float referenceEdgeX = referenceB.x - referenceA.x;
        const float referenceEdgeY = referenceB.y - referenceA.y;
        const float referenceLength = std::sqrt(
            referenceEdgeX * referenceEdgeX
                + referenceEdgeY * referenceEdgeY);
        if (referenceLength <= 0.0001f)
        {
            return std::nullopt;
        }
        const DirectX::XMFLOAT2 referenceDirection{
            referenceEdgeX / referenceLength,
            referenceEdgeY / referenceLength };
        const DirectX::XMFLOAT2 negativeDirection{
            -referenceDirection.x, -referenceDirection.y };

        std::array<DirectX::XMFLOAT2, 2> stage{};
        const std::size_t stageCount = ClipSegment(
            incidentA,
            incidentB,
            referenceDirection,
            referenceDirection.x * referenceA.x
                + referenceDirection.y * referenceA.y,
            stage);
        if (stageCount < 2)
        {
            return std::nullopt;
        }
        std::array<DirectX::XMFLOAT2, 2> clipped{};
        const std::size_t clippedCount = ClipSegment(
            stage[0],
            stage[1],
            negativeDirection,
            negativeDirection.x * referenceB.x
                + negativeDirection.y * referenceB.y,
            clipped);
        if (clippedCount < 2)
        {
            return std::nullopt;
        }

        const float referenceOffset =
            winner.axis.x * referenceA.x + winner.axis.y * referenceA.y;

        Contact contact{};
        contact.pointCount = 0;
        float totalPenetration{};
        for (const auto& point : clipped)
        {
            const float separation =
                winner.axis.x * point.x + winner.axis.y * point.y
                    - referenceOffset;
            if (separation > 0.0f)
            {
                // 参照面より外側（めり込んでいない）は除外します。
                continue;
            }
            contact.points[contact.pointCount++] =
                { point.x, point.y, 0.0f };
            totalPenetration += -separation;
        }
        if (contact.pointCount == 0)
        {
            return std::nullopt;
        }
        contact.penetration = std::max(
            totalPenetration
                / static_cast<float>(contact.pointCount),
            0.0001f);

        // 法線は他の2D関数と同じ規約（left側を向く）へ補正します。
        const auto leftCentroid = PolygonCentroid(left);
        const auto rightCentroid = PolygonCentroid(right);
        DirectX::XMFLOAT2 normal = winner.axis;
        const float towardLeft =
            normal.x * (leftCentroid.x - rightCentroid.x)
                + normal.y * (leftCentroid.y - rightCentroid.y);
        if (towardLeft < 0.0f)
        {
            normal.x = -normal.x;
            normal.y = -normal.y;
        }
        contact.normal = { normal.x, normal.y, 0.0f };

        DirectX::XMFLOAT3 average{};
        for (std::size_t index{}; index < contact.pointCount; ++index)
        {
            average.x += contact.points[index].x;
            average.y += contact.points[index].y;
        }
        const float scale =
            1.0f / static_cast<float>(contact.pointCount);
        average.x *= scale;
        average.y *= scale;
        contact.point = average;

        return contact;
    }

    // 凸多角形×円の2D接触。法線はleft側（多角形）を向きます。
    // 各辺の外向き法線を分離軸として最大分離量を持つ辺を探し、その辺との
    // Voronoi領域（辺上／端点近傍）で最近接点を求める標準的な手法です。
    std::optional<Contact> IntersectPolygonCircle(
        const std::vector<DirectX::XMFLOAT2>& polygon,
        const LamaPon::Circle2D& circle)
    {
        if (polygon.size() < 3)
        {
            return std::nullopt;
        }

        const auto axes = PolygonAxisCandidates(polygon, true);
        if (axes.empty())
        {
            return std::nullopt;
        }

        float maxSeparation = std::numeric_limits<float>::lowest();
        DirectX::XMFLOAT2 faceNormal{ 0.0f, 1.0f };
        std::size_t edgeStart{};
        for (const auto& candidate : axes)
        {
            const auto& vertex = polygon[candidate.edgeStart];
            const float separation =
                candidate.axis.x * (circle.center.x - vertex.x)
                    + candidate.axis.y * (circle.center.y - vertex.y);
            if (separation > maxSeparation)
            {
                maxSeparation = separation;
                faceNormal = candidate.axis;
                edgeStart = candidate.edgeStart;
            }
        }

        if (maxSeparation > circle.radius)
        {
            return std::nullopt;
        }

        if (maxSeparation <= 0.0001f)
        {
            // 中心が多角形の内部：最も近い面から押し出します。
            const float penetration = circle.radius - maxSeparation;
            const DirectX::XMFLOAT3 point{
                circle.center.x - faceNormal.x * maxSeparation,
                circle.center.y - faceNormal.y * maxSeparation,
                0.0f };
            Contact contact{
                { faceNormal.x, faceNormal.y, 0.0f },
                std::max(penetration, 0.0001f),
                point };
            contact.points[0] = point;
            contact.pointCount = 1;
            return contact;
        }

        // 中心が多角形の外側：最も分離した辺の上（または端点近傍）の
        // 最近接点を求めます。tのクランプにより、隣接する2辺のどちらを
        // 選んでも角付近では同じ頂点へ収束します。
        const auto& v1 = polygon[edgeStart];
        const auto& v2 = polygon[(edgeStart + 1) % polygon.size()];
        const float edgeX = v2.x - v1.x;
        const float edgeY = v2.y - v1.y;
        const float lengthSquared = edgeX * edgeX + edgeY * edgeY;
        float t = lengthSquared > 0.0001f
            ? ((circle.center.x - v1.x) * edgeX
                + (circle.center.y - v1.y) * edgeY) / lengthSquared
            : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const DirectX::XMFLOAT2 closest{
            v1.x + edgeX * t, v1.y + edgeY * t };

        const float deltaX = circle.center.x - closest.x;
        const float deltaY = circle.center.y - closest.y;
        const float squaredDistance = deltaX * deltaX + deltaY * deltaY;
        if (squaredDistance >= circle.radius * circle.radius)
        {
            return std::nullopt;
        }
        const float distance = std::sqrt(squaredDistance);
        DirectX::XMFLOAT3 normal{ faceNormal.x, faceNormal.y, 0.0f };
        if (distance > 0.0001f)
        {
            normal = { deltaX / distance, deltaY / distance, 0.0f };
        }
        const float penetration = circle.radius - distance;
        const DirectX::XMFLOAT3 point{ closest.x, closest.y, 0.0f };
        Contact contact{ normal, penetration, point };
        contact.points[0] = point;
        contact.pointCount = 1;
        return contact;
    }

    std::optional<Contact> IntersectBounds(
        const LamaPon::Bounds3D& left,
        const LamaPon::Bounds3D& right)
    {
        const float overlapX = std::min(left.maximum.x, right.maximum.x)
            - std::max(left.minimum.x, right.minimum.x);
        const float overlapY = std::min(left.maximum.y, right.maximum.y)
            - std::max(left.minimum.y, right.minimum.y);
        const float overlapZ = std::min(left.maximum.z, right.maximum.z)
            - std::max(left.minimum.z, right.minimum.z);
        if (overlapX <= 0.0f || overlapY <= 0.0f || overlapZ <= 0.0f)
        {
            return std::nullopt;
        }

        const DirectX::XMFLOAT3 leftCenter{
            (left.minimum.x + left.maximum.x) * 0.5f,
            (left.minimum.y + left.maximum.y) * 0.5f,
            (left.minimum.z + left.maximum.z) * 0.5f
        };
        const DirectX::XMFLOAT3 rightCenter{
            (right.minimum.x + right.maximum.x) * 0.5f,
            (right.minimum.y + right.maximum.y) * 0.5f,
            (right.minimum.z + right.maximum.z) * 0.5f
        };

        if (overlapX <= overlapY && overlapX <= overlapZ)
        {
            return Contact{
                { leftCenter.x < rightCenter.x ? -1.0f : 1.0f, 0.0f, 0.0f },
                overlapX
            };
        }
        if (overlapY <= overlapZ)
        {
            return Contact{
                { 0.0f, leftCenter.y < rightCenter.y ? -1.0f : 1.0f, 0.0f },
                overlapY
            };
        }

        return Contact{
            { 0.0f, 0.0f, leftCenter.z < rightCenter.z ? -1.0f : 1.0f },
            overlapZ
        };
    }

    struct DirectionalShadowCascade final
    {
        DirectX::XMMATRIX view;
        DirectX::XMMATRIX projection;
        float splitDistance{};
    };

    struct DirectionalShadowMatrices final
    {
        std::array<
            DirectionalShadowCascade,
            LamaPon::MaximumShadowCascades> cascades;
        std::size_t count{};
    };

    DirectionalShadowMatrices BuildDirectionalShadowMatrices(
        DirectX::FXMMATRIX cameraView,
        DirectX::CXMMATRIX cameraProjection,
        const DirectX::XMFLOAT3& lightDirection,
        const float shadowDistance,
        const std::size_t requestedCascadeCount,
        const float splitLambda,
        const std::uint32_t shadowResolution) noexcept
    {
        using namespace DirectX;

        XMVECTOR determinant{};
        const XMMATRIX inverseView =
            XMMatrixInverse(&determinant, cameraView);
        const XMMATRIX inverseProjection =
            XMMatrixInverse(&determinant, cameraProjection);
        const XMVECTOR direction = XMVector3Normalize(
            XMLoadFloat3(&lightDirection));

        const float verticalAlignment = std::abs(
            XMVectorGetX(XMVector3Dot(
                direction,
                XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f))));
        const XMVECTOR up = verticalAlignment > 0.95f
            ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        const XMVECTOR nearCenter = XMVector3TransformCoord(
            XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
            inverseProjection);
        const XMVECTOR farCenter = XMVector3TransformCoord(
            XMVectorSet(0.0f, 0.0f, 1.0f, 1.0f),
            inverseProjection);
        const float cameraNear = std::max(
            -XMVectorGetZ(nearCenter),
            0.01f);
        const float cameraFar = std::max(
            -XMVectorGetZ(farCenter),
            cameraNear + 0.01f);
        const float maximumDistance = std::clamp(
            shadowDistance,
            cameraNear + 0.01f,
            cameraFar);
        const std::size_t cascadeCount = std::clamp(
            requestedCascadeCount,
            std::size_t{ 1 },
            LamaPon::MaximumShadowCascades);
        const float lambda = std::clamp(
            splitLambda,
            0.0f,
            1.0f);

        DirectionalShadowMatrices result;
        result.count = cascadeCount;
        float previousSplit = cameraNear;
        for (std::size_t cascadeIndex = 0;
            cascadeIndex < cascadeCount;
            ++cascadeIndex)
        {
            const float fraction =
                static_cast<float>(cascadeIndex + 1)
                / static_cast<float>(cascadeCount);
            const float logarithmicSplit =
                cameraNear * std::pow(
                    maximumDistance / cameraNear,
                    fraction);
            const float uniformSplit =
                cameraNear
                + (maximumDistance - cameraNear)
                    * fraction;
            const float splitDistance = std::lerp(
                uniformSplit,
                logarithmicSplit,
                lambda);

            std::array<XMVECTOR, 8> corners;
            std::size_t cornerIndex{};
            for (const float ndcY : { -1.0f, 1.0f })
            {
                for (const float ndcX : { -1.0f, 1.0f })
                {
                    const XMVECTOR nearPoint =
                        XMVector3TransformCoord(
                            XMVectorSet(
                                ndcX,
                                ndcY,
                                0.0f,
                                1.0f),
                            inverseProjection);
                    const XMVECTOR farPoint =
                        XMVector3TransformCoord(
                            XMVectorSet(
                                ndcX,
                                ndcY,
                                1.0f,
                                1.0f),
                            inverseProjection);
                    const XMVECTOR ray =
                        XMVectorSubtract(
                            farPoint,
                            nearPoint);
                    const float rayDepth =
                        XMVectorGetZ(farPoint)
                        - XMVectorGetZ(nearPoint);
                    const float nearFraction =
                        (-previousSplit
                            - XMVectorGetZ(nearPoint))
                        / rayDepth;
                    const float farFraction =
                        (-splitDistance
                            - XMVectorGetZ(nearPoint))
                        / rayDepth;
                    corners[cornerIndex++] =
                        XMVector3TransformCoord(
                            XMVectorMultiplyAdd(
                                XMVectorReplicate(
                                    nearFraction),
                                ray,
                                nearPoint),
                            inverseView);
                    corners[cornerIndex++] =
                        XMVector3TransformCoord(
                            XMVectorMultiplyAdd(
                                XMVectorReplicate(
                                    farFraction),
                                ray,
                                nearPoint),
                            inverseView);
                }
            }

            XMVECTOR center = XMVectorZero();
            for (const auto& corner : corners)
            {
                center = XMVectorAdd(center, corner);
            }
            center = XMVectorScale(
                center,
                1.0f
                    / static_cast<float>(corners.size()));

            float radius{};
            for (const auto& corner : corners)
            {
                radius = std::max(
                    radius,
                    XMVectorGetX(XMVector3Length(
                        XMVectorSubtract(
                            corner,
                            center))));
            }
            radius = std::max(
                std::ceil(radius * 16.0f) / 16.0f,
                0.5f);

            const float depthPadding = std::max(
                shadowDistance * 0.5f,
                10.0f);
            const XMVECTOR eye = XMVectorMultiplyAdd(
                XMVectorNegate(direction),
                XMVectorReplicate(
                    radius + depthPadding),
                center);
            auto view = XMMatrixLookToRH(
                eye,
                direction,
                up);
            auto projection = XMMatrixOrthographicRH(
                radius * 2.0f,
                radius * 2.0f,
                0.1f,
                radius * 2.0f
                    + depthPadding * 2.0f);

            const float halfResolution =
                static_cast<float>(
                    std::max(shadowResolution, 1u))
                * 0.5f;
            const XMVECTOR shadowOrigin =
                XMVectorScale(
                    XMVector3TransformCoord(
                        XMVectorZero(),
                        view * projection),
                    halfResolution);
            const XMVECTOR roundedOrigin =
                XMVectorRound(shadowOrigin);
            XMVECTOR roundingOffset =
                XMVectorScale(
                    XMVectorSubtract(
                        roundedOrigin,
                        shadowOrigin),
                    1.0f / halfResolution);
            roundingOffset = XMVectorSetZ(
                roundingOffset,
                0.0f);
            roundingOffset = XMVectorSetW(
                roundingOffset,
                0.0f);
            projection.r[3] = XMVectorAdd(
                projection.r[3],
                roundingOffset);

            result.cascades[cascadeIndex] = {
                view,
                projection,
                splitDistance
            };
            previousSplit = splitDistance;
        }

        return result;
    }

    // 祖先のScrollViewを探します（最も近いもの）。
    const LamaPon::UIScrollViewComponent*
        FindAncestorScrollView(
            const LamaPon::GameObject& gameObject)
    {
        for (const auto* ancestor = gameObject.Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            if (const auto* scrollView =
                ancestor->GetComponent<
                    LamaPon::UIScrollViewComponent>();
                scrollView != nullptr
                && scrollView->IsEnabled())
            {
                return scrollView;
            }
        }
        return nullptr;
    }

    struct Light2DEntry final
    {
        DirectX::XMFLOAT2 position;
        float radius;
        float intensity;
        DirectX::XMFLOAT3 color;
        bool affectsUI;
    };

    // Light2Dは加算式（暗くはしない）で、ワールド空間のSprite
    // RendererとTilemapに影響します（UIは、UIも照らす設定のLight2Dが
    // あるときだけ）。このエンジンの2DはワールドXY＝画面ピクセルの
    // 1対1対応（SpriteRendererComponentの位置計算と同じ）なので、
    // 灯りの座標はそのまま画面座標として渡せます。
    //
    // 以前は「近い2灯をCustomParametersへ詰める」方式でしたが、
    // 画面いっぱいに広がるTilemapでは「オブジェクトの原点に近い2灯」
    // が地図の反対側の画素にも使われてしまい成立しません。灯りは
    // Sprite単位ではなく画面単位のものなので、b1の専用バッファへ
    // 一覧ごと載せて、画素ごとに全灯を評価します。
    LamaPon::Sprite2DLighting BuildSprite2DLighting(
        const std::vector<Light2DEntry>& lights,
        const bool uiOnly,
        const DirectX::XMFLOAT2& worldOffset)
    {
        LamaPon::Sprite2DLighting lighting{};
        std::uint32_t count{};
        for (const auto& light : lights)
        {
            if (count >= LamaPon::MaximumSprite2DLights)
            {
                break;
            }
            // UIを描いている間は、UIを照らす設定の灯りだけを渡します。
            if (uiOnly && !light.affectsUI)
            {
                continue;
            }
            auto& destination = lighting.lights[count];
            destination.positionRadiusIntensity = {
                light.position.x
                    + (uiOnly ? 0.0f : worldOffset.x),
                light.position.y
                    + (uiOnly ? 0.0f : worldOffset.y),
                light.radius,
                light.intensity };
            destination.color = {
                light.color.x,
                light.color.y,
                light.color.z,
                0.0f };
            ++count;
        }
        lighting.counts = { count, 0u, 0u, 0u };
        return lighting;
    }

    const std::filesystem::path SpriteLit2DShaderPath{
        L"shaders/LamaPonSpriteLit.hlsl" };

    struct SpriteMaskEntry final
    {
        DirectX::XMFLOAT2 position;
        LamaPon::SpriteMaskShape shape;
        DirectX::XMFLOAT2 size;
    };

    const std::filesystem::path SpriteMaskShaderPath{
        L"shaders/LamaPonSpriteMask.hlsl" };

    // SpriteMaskは最も近いマスク1つだけを使うクッキリしたクリップです
    // （半透明フェードなし）。座標系や[5]/[6]予約枠の考え方は
    // BuildLight2DParametersと同じです。
    std::array<DirectX::XMFLOAT4, 8> BuildSpriteMaskParameters(
        const DirectX::XMFLOAT2& spritePosition,
        const DirectX::XMFLOAT4& spriteTint,
        const DirectX::XMFLOAT4& spriteDrawRect,
        const LamaPon::SpriteMaskInteraction interaction,
        const std::vector<SpriteMaskEntry>& masks)
    {
        std::array<DirectX::XMFLOAT4, 8> parameters{};
        parameters[5] = spriteTint;
        parameters[6] = spriteDrawRect;

        std::size_t nearestIndex = masks.size();
        float nearestDistanceSquared =
            std::numeric_limits<float>::max();
        for (std::size_t index{}; index < masks.size(); ++index)
        {
            const float deltaX =
                masks[index].position.x - spritePosition.x;
            const float deltaY =
                masks[index].position.y - spritePosition.y;
            const float distanceSquared =
                deltaX * deltaX + deltaY * deltaY;
            if (distanceSquared < nearestDistanceSquared)
            {
                nearestDistanceSquared = distanceSquared;
                nearestIndex = index;
            }
        }

        if (nearestIndex < masks.size())
        {
            const auto& mask = masks[nearestIndex];
            parameters[0].x =
                mask.shape == LamaPon::SpriteMaskShape::Rectangle
                    ? 0.0f
                    : 1.0f;
            parameters[0].y =
                interaction
                    == LamaPon::SpriteMaskInteraction::
                        VisibleOutsideMask
                    ? 1.0f
                    : 0.0f;
            parameters[1] = {
                mask.position.x,
                mask.position.y,
                mask.size.x * 0.5f,
                mask.size.y * 0.5f };
        }
        return parameters;
    }

    void RenderSprites2D(
        const std::vector<std::unique_ptr<LamaPon::GameObject>>&
            gameObjects,
        LamaPon::GraphicsDevice& graphics,
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        std::vector<LamaPon::GameObject*> ordered;
        ordered.reserve(gameObjects.size());
        for (const auto& gameObject : gameObjects)
        {
            ordered.push_back(gameObject.get());
        }
        std::vector<Light2DEntry> lights2D;
        for (const auto& gameObject : gameObjects)
        {
            if (const auto* light =
                    gameObject->GetComponent<
                        LamaPon::Light2DComponent>();
                light != nullptr
                && light->IsEnabled()
                && gameObject->IsActiveInHierarchy())
            {
                lights2D.push_back({
                    light->WorldPosition(),
                    light->Radius(),
                    light->Intensity(),
                    light->Color(),
                    light->AffectsUI() });
            }
        }
        // 灯りはSprite単位ではなく画面単位なので、一覧はループの外で
        // 1回だけ組み立てます。UI用はUIを照らす設定の灯りだけの版です。
        const auto worldLighting =
            BuildSprite2DLighting(
                lights2D,
                false,
                graphics.Sprite2DOffset());
        const auto uiLighting =
            BuildSprite2DLighting(
                lights2D,
                true,
                graphics.Sprite2DOffset());
        const bool anyLightAffectsUI =
            uiLighting.counts.x > 0u;
        std::vector<SpriteMaskEntry> masks2D;
        for (const auto& gameObject : gameObjects)
        {
            if (const auto* mask =
                    gameObject->GetComponent<
                        LamaPon::SpriteMaskComponent>();
                mask != nullptr
                && mask->IsEnabled()
                && gameObject->IsActiveInHierarchy())
            {
                masks2D.push_back({
                    {
                        mask->WorldPosition().x
                            + graphics.Sprite2DOffset().x,
                        mask->WorldPosition().y
                            + graphics.Sprite2DOffset().y
                    },
                    mask->Shape(),
                    mask->Size() });
            }
        }
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](
                const LamaPon::GameObject* left,
                const LamaPon::GameObject* right)
            {
                return left->Render2DSortOrder()
                    < right->Render2DSortOrder();
            });
        // ScrollView配下のオブジェクトは表示領域でクリッピング
        // します。
        const LamaPon::UIScrollViewComponent*
            activeScrollView{};
        for (auto* gameObject : ordered)
        {
            // SpriteRenderer にカスタムシェーダーが指定されている場合は、
            // そのオブジェクトの描画だけ SpriteBatch の PS を差し替えます。
            // パラメーターはオブジェクトごとに異なるため、カスタム描画の
            // 手前で一度バッチを確定させます（ScrollViewのクリッピングは
            // 通常バッチのみ対象で、切替時にシザーを畳んで整合させます）。
            if (gameObject->IsEnabled())
            {
                if (auto* sprite =
                        gameObject->GetComponent<
                            LamaPon::SpriteRendererComponent>();
                    sprite != nullptr
                    && sprite->IsEnabled()
                    && !sprite->ShaderPath().empty())
                {
                    if (activeScrollView != nullptr)
                    {
                        graphics.PopUIScissor();
                        activeScrollView = nullptr;
                    }
                    graphics.EndSprites();
                    sprite->BeginRenderBatch(graphics);
                    gameObject->Render2D(
                        graphics,
                        spriteBatch,
                        whiteTexture);
                    graphics.EndSprites();
                    graphics.BeginSprites();
                    continue;
                }
            }
            // SpriteMaskによるクリップ：MaskInteractionを設定した
            // Sprite Rendererだけに適用します。Light2Dと同時に必要な
            // Spriteでは、こちら（マスク）を優先します
            // （1回のバッチ切替で使えるカスタムShaderは1つのため）。
            if (!masks2D.empty()
                && gameObject->IsEnabled()
                && gameObject->GetComponent<
                    LamaPon::UIRectTransformComponent>()
                    == nullptr)
            {
                if (auto* maskedSprite =
                        gameObject->GetComponent<
                            LamaPon::SpriteRendererComponent>();
                    maskedSprite != nullptr
                    && maskedSprite->IsEnabled()
                    && maskedSprite->ShaderPath().empty()
                    && maskedSprite->MaskInteraction()
                        != LamaPon::SpriteMaskInteraction::None)
                {
                    DirectX::XMFLOAT4X4 spriteWorld{};
                    DirectX::XMStoreFloat4x4(
                        &spriteWorld,
                        gameObject->WorldMatrix());
                    const float worldScaleX = std::sqrt(
                        spriteWorld._11 * spriteWorld._11
                            + spriteWorld._12
                                * spriteWorld._12);
                    const float worldScaleY = std::sqrt(
                        spriteWorld._21 * spriteWorld._21
                            + spriteWorld._22
                                * spriteWorld._22);
                    const auto& spriteSize =
                        maskedSprite->Size();
                    const float drawWidth = std::max(
                        spriteSize.x * worldScaleX,
                        0.0001f);
                    const float drawHeight = std::max(
                        spriteSize.y * worldScaleY,
                        0.0001f);
                    // Pivotを動かしているとTransformの位置は左上では
                    // ないので、矩形の左上へ戻してから渡します。
                    const auto& spritePivot =
                        maskedSprite->Pivot();
                    const DirectX::XMFLOAT4 drawRect{
                        spriteWorld._41
                            - spritePivot.x * drawWidth
                            + graphics.Sprite2DOffset().x,
                        spriteWorld._42
                            - spritePivot.y * drawHeight
                            + graphics.Sprite2DOffset().y,
                        drawWidth,
                        drawHeight
                    };
                    const auto parameters =
                        BuildSpriteMaskParameters(
                            { spriteWorld._41
                                + graphics.Sprite2DOffset().x,
                                spriteWorld._42
                                + graphics.Sprite2DOffset().y },
                            maskedSprite->Color(),
                            drawRect,
                            maskedSprite->MaskInteraction(),
                            masks2D);

                    if (activeScrollView != nullptr)
                    {
                        graphics.PopUIScissor();
                        activeScrollView = nullptr;
                    }
                    graphics.EndSprites();
                    graphics.BeginSprites(
                        SpriteMaskShaderPath,
                        parameters);
                    gameObject->Render2D(
                        graphics,
                        spriteBatch,
                        whiteTexture);
                    graphics.EndSprites();
                    graphics.BeginSprites();
                    continue;
                }
            }
            // Light2Dの加算式ライティング。カスタムShaderの分岐と同じ、
            // バッチを一度確定させてから差し替える方式です。
            //
            // 対象は独自Shaderを持たないSprite RendererとTilemapです。
            // UIは既定では外します（文字が読めなくなるほうが困る）。
            // UIも照らす設定のLight2Dが1つでもあるときだけ、UI側は
            // その灯りだけの一覧で照らします。
            if (!lights2D.empty() && gameObject->IsEnabled())
            {
                const bool isUI =
                    gameObject->GetComponent<
                        LamaPon::UIRectTransformComponent>()
                        != nullptr;
                auto* litSprite =
                    gameObject->GetComponent<
                        LamaPon::SpriteRendererComponent>();
                const bool spriteIsLit =
                    litSprite != nullptr
                    && litSprite->IsEnabled()
                    && litSprite->ShaderPath().empty();
                const auto* tilemap =
                    gameObject->GetComponent<
                        LamaPon::TilemapComponent>();
                const bool tilemapIsLit =
                    !isUI
                    && tilemap != nullptr
                    && tilemap->IsEnabled();
                if ((spriteIsLit || tilemapIsLit)
                    && (!isUI || anyLightAffectsUI))
                {
                    if (activeScrollView != nullptr)
                    {
                        graphics.PopUIScissor();
                        activeScrollView = nullptr;
                    }
                    graphics.EndSprites();
                    // 色とUVは頂点から受け取るので、この経路では
                    // CustomParametersを使いません（灯りはb1）。
                    graphics.BeginSprites(
                        SpriteLit2DShaderPath,
                        std::array<DirectX::XMFLOAT4, 8>{},
                        nullptr,
                        nullptr,
                        isUI ? &uiLighting : &worldLighting);
                    gameObject->Render2D(
                        graphics,
                        spriteBatch,
                        whiteTexture);
                    graphics.EndSprites();
                    graphics.BeginSprites();
                    continue;
                }
            }
            const auto* scrollView =
                FindAncestorScrollView(*gameObject);
            if (scrollView != activeScrollView)
            {
                if (activeScrollView != nullptr)
                {
                    graphics.PopUIScissor();
                }
                activeScrollView = scrollView;
                if (activeScrollView != nullptr)
                {
                    const auto viewRect =
                        activeScrollView->ViewRect(
                            graphics);
                    graphics.PushUIScissor(
                        viewRect.minimum.x,
                        viewRect.minimum.y,
                        viewRect.maximum.x,
                        viewRect.maximum.y);
                }
            }
            gameObject->Render2D(
                graphics,
                spriteBatch,
                whiteTexture);
        }
        if (activeScrollView != nullptr)
        {
            graphics.PopUIScissor();
        }
    }
}

namespace LamaPon
{
    Scene::Scene(GraphicsDevice& graphics) noexcept
        : m_graphics(graphics)
        , m_sceneManager(
            std::make_unique<SceneManager>(
                *this,
                graphics))
    {
    }

    // リフレクションプローブのベイクに使う共有リソース。
    // どのプローブも同じキューブへ描いてから事前フィルタで
    // 自分用の結果を作るので、キューブ本体は1組で足ります。
    struct Scene::ReflectionProbeBakeResources final
    {
        static constexpr std::uint32_t FaceSize = 128;

        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            cubeTexture;
        std::array<
            Microsoft::WRL::ComPtr<
                ID3D11RenderTargetView>,
            6> faceTargets;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            cubeShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            depthView;
        // 面ごとの描画先（2D）。エンジンの右手系のまま描き、
        // キューブ面へは左右反転コピーで書き込みます（射影行列で
        // 鏡像にすると巻き方向が反転してカリングが崩れるため）。
        Microsoft::WRL::ComPtr<ID3D11Texture2D>
            scratchTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            scratchTarget;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            scratchShaderResourceView;

        explicit ReflectionProbeBakeResources(
            ID3D11Device* const device)
        {
            const auto throwIfFailed =
                [](const HRESULT result,
                    const char* operation)
                {
                    if (FAILED(result))
                    {
                        throw std::runtime_error(
                            std::string(operation)
                            + " failed");
                    }
                };

            D3D11_TEXTURE2D_DESC cubeDescription{};
            cubeDescription.Width = FaceSize;
            cubeDescription.Height = FaceSize;
            cubeDescription.MipLevels = 1;
            cubeDescription.ArraySize = 6;
            cubeDescription.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            cubeDescription.SampleDesc.Count = 1;
            cubeDescription.Usage = D3D11_USAGE_DEFAULT;
            cubeDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            cubeDescription.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE;
            throwIfFailed(
                device->CreateTexture2D(
                    &cubeDescription,
                    nullptr,
                    cubeTexture.ReleaseAndGetAddressOf()),
                "CreateTexture2D(probe cube)");

            for (std::uint32_t face = 0; face < 6; ++face)
            {
                D3D11_RENDER_TARGET_VIEW_DESC
                    targetDescription{};
                targetDescription.Format =
                    cubeDescription.Format;
                targetDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                targetDescription.Texture2DArray
                    .FirstArraySlice = face;
                targetDescription.Texture2DArray
                    .ArraySize = 1;
                throwIfFailed(
                    device->CreateRenderTargetView(
                        cubeTexture.Get(),
                        &targetDescription,
                        faceTargets[face]
                            .ReleaseAndGetAddressOf()),
                    "CreateRenderTargetView(probe face)");
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC
                viewDescription{};
            viewDescription.Format =
                cubeDescription.Format;
            viewDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURECUBE;
            viewDescription.TextureCube.MipLevels = 1;
            throwIfFailed(
                device->CreateShaderResourceView(
                    cubeTexture.Get(),
                    &viewDescription,
                    cubeShaderResourceView
                        .ReleaseAndGetAddressOf()),
                "CreateShaderResourceView(probe cube)");

            D3D11_TEXTURE2D_DESC depthDescription{};
            depthDescription.Width = FaceSize;
            depthDescription.Height = FaceSize;
            depthDescription.MipLevels = 1;
            depthDescription.ArraySize = 1;
            depthDescription.Format =
                DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDescription.SampleDesc.Count = 1;
            depthDescription.Usage = D3D11_USAGE_DEFAULT;
            depthDescription.BindFlags =
                D3D11_BIND_DEPTH_STENCIL;
            throwIfFailed(
                device->CreateTexture2D(
                    &depthDescription,
                    nullptr,
                    depthTexture
                        .ReleaseAndGetAddressOf()),
                "CreateTexture2D(probe depth)");
            throwIfFailed(
                device->CreateDepthStencilView(
                    depthTexture.Get(),
                    nullptr,
                    depthView.ReleaseAndGetAddressOf()),
                "CreateDepthStencilView(probe depth)");

            D3D11_TEXTURE2D_DESC scratchDescription{};
            scratchDescription.Width = FaceSize;
            scratchDescription.Height = FaceSize;
            scratchDescription.MipLevels = 1;
            scratchDescription.ArraySize = 1;
            scratchDescription.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            scratchDescription.SampleDesc.Count = 1;
            scratchDescription.Usage =
                D3D11_USAGE_DEFAULT;
            scratchDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE
                | D3D11_BIND_RENDER_TARGET;
            throwIfFailed(
                device->CreateTexture2D(
                    &scratchDescription,
                    nullptr,
                    scratchTexture
                        .ReleaseAndGetAddressOf()),
                "CreateTexture2D(probe scratch)");
            throwIfFailed(
                device->CreateRenderTargetView(
                    scratchTexture.Get(),
                    nullptr,
                    scratchTarget
                        .ReleaseAndGetAddressOf()),
                "CreateRenderTargetView(probe scratch)");
            throwIfFailed(
                device->CreateShaderResourceView(
                    scratchTexture.Get(),
                    nullptr,
                    scratchShaderResourceView
                        .ReleaseAndGetAddressOf()),
                "CreateShaderResourceView(probe scratch)");
        }
    };

    Scene::~Scene() = default;

    GameObject& Scene::CreateGameObject(std::string name)
    {
        auto gameObject = std::make_unique<GameObject>(m_nextId++, std::move(name));
        gameObject->m_scene = this;
        // 追加読み込み中なら、そのシーンの所属にします。
        gameObject->m_sourceScene = m_loadingScene;
        auto* result = gameObject.get();
        m_gameObjects.emplace_back(std::move(gameObject));
        return *result;
    }

    GameObject& Scene::CreateEditorPreviewGameObject(std::string name)
    {
        auto& gameObject = CreateGameObject(std::move(name));
        // UINT32_MAX は実際の追加シーンハンドルとして予約しない
        // エディター専用値です。SceneSerializationはPrimarySceneHandle
        // 以外を主シーンへ保存しないため、プレビューがシーンファイルへ
        // 混ざることもありません。
        gameObject.m_sourceScene = static_cast<SceneHandle>(-1);
        return gameObject;
    }

    GameObject& Scene::DuplicateGameObject(
        const GameObject& source,
        GameObject* targetParent,
        const bool appendCopySuffix)
    {
        if (targetParent != nullptr
            && FindGameObject(targetParent->Id()) != targetParent)
        {
            throw std::invalid_argument("The duplicate target parent is not in this scene.");
        }

        GameObject* duplicatedRoot{};
        std::unordered_map<
            GameObjectId,
            GameObjectId> duplicatedIds;

        // 複製は複製元と同じシーンの所属にします（追加シーンの中で
        // 複製したものが主シーンへ紛れ込まないようにするため）。
        // CreateGameObjectがm_loadingSceneを見るので、その間だけ
        // 差し替えます。
        const SceneHandle previousLoadingScene =
            m_loadingScene;
        m_loadingScene = source.SourceScene();
        struct LoadingSceneRestore final
        {
            Scene& scene;
            SceneHandle previous;
            ~LoadingSceneRestore()
            {
                scene.m_loadingScene = previous;
            }
        } restoreLoadingScene{
            *this,
            previousLoadingScene
        };

        const auto clone =
            [this,
                &duplicatedRoot,
                &duplicatedIds,
                appendCopySuffix](
                const auto& self,
                const GameObject& sourceObject,
                GameObject* parent,
                const bool isRoot) -> GameObject&
            {
                auto& duplicate = CreateGameObject(
                    sourceObject.Name()
                    + (isRoot && appendCopySuffix
                        ? " Copy"
                        : ""));

                if (isRoot)
                {
                    duplicatedRoot = &duplicate;
                }
                duplicatedIds.emplace(
                    sourceObject.Id(),
                    duplicate.Id());

                duplicate.SetEnabled(sourceObject.IsEnabled());
                duplicate.SetTag(sourceObject.Tag());
                duplicate.GetTransform() = sourceObject.GetTransform();
                duplicate.SetPrefabAssetPath(
                    sourceObject.PrefabAssetPath());
                duplicate.SetParent(parent);

                for (const auto& sourceComponent : sourceObject.Components())
                {
                    Component* duplicateComponent{};

                    if (const auto* camera =
                        dynamic_cast<const CameraComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<CameraComponent>(
                            camera->VerticalFieldOfView(),
                            camera->NearPlane(),
                            camera->FarPlane());
                    }
                    else if (const auto* directionalLight =
                        dynamic_cast<const DirectionalLightComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                DirectionalLightComponent>(
                                directionalLight->Color(),
                                directionalLight->Intensity(),
                                directionalLight->CastsShadows(),
                                directionalLight->ShadowDistance(),
                                directionalLight->ShadowBias(),
                                directionalLight->ShadowNormalBias(),
                                directionalLight->ShadowStrength(),
                                directionalLight->ShadowCascadeCount(),
                                directionalLight->ShadowSplitLambda());
                    }
                    else if (const auto* pointLight =
                        dynamic_cast<const PointLightComponent*>(
                            sourceComponent.get()))
                    {
                        auto& duplicatePointLight =
                            duplicate.AddComponent<
                                PointLightComponent>(
                                pointLight->Color(),
                                pointLight->Intensity(),
                                pointLight->Range());
                        duplicatePointLight.SetCastsShadows(
                            pointLight->CastsShadows());
                        duplicatePointLight.SetShadowBias(
                            pointLight->ShadowBias());
                        duplicatePointLight
                            .SetShadowStrength(
                                pointLight
                                    ->ShadowStrength());
                        duplicateComponent =
                            &duplicatePointLight;
                    }
                    else if (const auto* spotLight =
                        dynamic_cast<const SpotLightComponent*>(
                            sourceComponent.get()))
                    {
                        auto& duplicateSpotLight =
                            duplicate.AddComponent<
                                SpotLightComponent>(
                                spotLight->Color(),
                                spotLight->Intensity(),
                                spotLight->Range(),
                                spotLight->InnerConeAngle(),
                                spotLight->OuterConeAngle());
                        duplicateSpotLight.SetCastsShadows(
                            spotLight->CastsShadows());
                        duplicateSpotLight.SetShadowBias(
                            spotLight->ShadowBias());
                        duplicateSpotLight
                            .SetShadowNormalBias(
                                spotLight
                                    ->ShadowNormalBias());
                        duplicateSpotLight
                            .SetShadowStrength(
                                spotLight
                                    ->ShadowStrength());
                        duplicateComponent =
                            &duplicateSpotLight;
                    }
                    else if (const auto* light2D =
                        dynamic_cast<
                            const Light2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                Light2DComponent>(
                                    light2D->Color(),
                                    light2D->Intensity(),
                                    light2D->Radius());
                    }
                    else if (const auto* collider2D =
                        dynamic_cast<const BoxCollider2DComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<BoxCollider2DComponent>(
                            collider2D->Size(),
                            collider2D->Offset(),
                            collider2D->IsTrigger(),
                            collider2D->Layer(),
                            collider2D->CollisionMask(),
                            collider2D->Material());
                    }
                    else if (const auto* circle2D =
                        dynamic_cast<
                            const CircleCollider2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                CircleCollider2DComponent>(
                                    circle2D->Radius(),
                                    circle2D->Offset(),
                                    circle2D->IsTrigger(),
                                    circle2D->Layer(),
                                    circle2D
                                        ->CollisionMask(),
                                    circle2D->Material());
                    }
                    else if (const auto* polygon2D =
                        dynamic_cast<
                            const PolygonCollider2DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                PolygonCollider2DComponent>(
                                    polygon2D->Vertices(),
                                    polygon2D->Offset(),
                                    polygon2D->IsTrigger(),
                                    polygon2D->Layer(),
                                    polygon2D
                                        ->CollisionMask(),
                                    polygon2D->Material());
                    }
                    else if (const auto* collider3D =
                        dynamic_cast<const BoxCollider3DComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<BoxCollider3DComponent>(
                            collider3D->Size(),
                            collider3D->Offset(),
                            collider3D->IsTrigger(),
                            collider3D->Layer(),
                            collider3D->CollisionMask(),
                            collider3D->Material());
                    }
                    else if (const auto* capsule =
                        dynamic_cast<const CapsuleCollider3DComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                CapsuleCollider3DComponent>(
                                    capsule->Radius(),
                                    capsule->Height(),
                                    capsule->Offset(),
                                    capsule->IsTrigger(),
                                    capsule->Layer(),
                                    capsule->CollisionMask(),
                                    capsule->Material());
                    }
                    else if (const auto* sphere =
                        dynamic_cast<
                            const SphereCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                SphereCollider3DComponent>(
                                    sphere->Radius(),
                                    sphere->Offset(),
                                    sphere->IsTrigger(),
                                    sphere->Layer(),
                                    sphere->CollisionMask(),
                                    sphere->Material());
                    }
                    else if (const auto* hull =
                        dynamic_cast<
                            const ConvexHullCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                ConvexHullCollider3DComponent>(
                                    hull->Points(),
                                    hull->Offset(),
                                    hull->IsTrigger(),
                                    hull->Layer(),
                                    hull->CollisionMask(),
                                    hull->Material());
                    }
                    else if (const auto* meshCollider =
                        dynamic_cast<
                            const MeshCollider3DComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                MeshCollider3DComponent>(
                                    meshCollider
                                        ->ModelPath(),
                                    meshCollider->Offset(),
                                    meshCollider
                                        ->IsTrigger(),
                                    meshCollider->Layer(),
                                    meshCollider
                                        ->CollisionMask(),
                                    meshCollider
                                        ->Material());
                    }
                    else if (const auto* mesh =
                        dynamic_cast<const MeshRendererComponent*>(sourceComponent.get()))
                    {
                        auto& duplicateMesh =
                            duplicate.AddComponent<MeshRendererComponent>(
                            mesh->Shape(),
                            mesh->Color(),
                            mesh->AlbedoTexturePath(),
                            mesh->NormalTexturePath(),
                            mesh->Roughness(),
                            mesh->NormalStrength(),
                            mesh->MaterialAssetPath());
                        duplicateMesh.SetMetallic(
                            mesh->Metallic());
                        // PBRマップと発光も引き継ぎます。
                        duplicateMesh.SetRoughnessTexturePath(
                            mesh->RoughnessTexturePath());
                        duplicateMesh.SetMetallicTexturePath(
                            mesh->MetallicTexturePath());
                        duplicateMesh.SetOcclusionTexturePath(
                            mesh->OcclusionTexturePath());
                        duplicateMesh.SetEmissiveTexturePath(
                            mesh->EmissiveTexturePath());
                        duplicateMesh.SetOcclusionStrength(
                            mesh->OcclusionStrength());
                        duplicateMesh.SetEmissiveColor(
                            mesh->EmissiveColor());
                        if (mesh->HasProceduralMesh())
                        {
                            duplicateMesh.SetProceduralMesh(
                                mesh->ProceduralVertices(),
                                mesh->ProceduralIndices());
                        }
                        duplicateComponent =
                            &duplicateMesh;
                    }
                    else if (const auto* sprite =
                        dynamic_cast<const SpriteRendererComponent*>(sourceComponent.get()))
                    {
                        auto& duplicateSprite =
                            duplicate.AddComponent<SpriteRendererComponent>(
                                sprite->Size(),
                                sprite->Color(),
                                sprite->TexturePath());
                        duplicateSprite.SetSortOrder(
                            sprite->SortOrder());
                        duplicateSprite.SetSourceRect(
                            sprite->SourceRect());
                        duplicateSprite.SetShaderPath(
                            sprite->ShaderPath());
                        duplicateSprite.SetMaskInteraction(
                            sprite->MaskInteraction());
                        for (std::size_t index = 0;
                            index
                                < SpriteRendererComponent::
                                    CustomParameterCount;
                            ++index)
                        {
                            duplicateSprite.SetCustomParameter(
                                index,
                                sprite->CustomParameter(index));
                        }
                        duplicateComponent = &duplicateSprite;
                    }
                    else if (const auto* spriteMask =
                        dynamic_cast<
                            const SpriteMaskComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                SpriteMaskComponent>(
                                    spriteMask->Shape(),
                                    spriteMask->Size());
                    }
                    else if (const auto* renderCulling =
                        dynamic_cast<
                            const RenderCullingComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                RenderCullingComponent>(
                                    renderCulling
                                        ->AlwaysVisible(),
                                    renderCulling
                                        ->CullingMargin());
                    }
                    else if (const auto* reflectionProbe =
                        dynamic_cast<
                            const ReflectionProbeComponent*>(
                                sourceComponent.get()))
                    {
                        // ベイク結果は複製しません（新しい位置で
                        // 焼き直すのが正しいため。作成直後は
                        // ベイク待ちなので自動で焼かれます）。
                        auto& duplicateProbe =
                            duplicate.AddComponent<
                                ReflectionProbeComponent>(
                                    reflectionProbe
                                        ->Range(),
                                    reflectionProbe
                                        ->Intensity());
                        duplicateProbe.SetBoxExtents(
                            reflectionProbe->BoxExtents());
                        duplicateProbe.SetBlendDistance(
                            reflectionProbe
                                ->BlendDistance());
                        duplicateComponent =
                            &duplicateProbe;
                    }
                    else if (const auto* spriteAnimator =
                        dynamic_cast<
                            const SpriteAnimatorComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateAnimator =
                            duplicate.AddComponent<
                                SpriteAnimatorComponent>(
                                spriteAnimator->Columns(),
                                spriteAnimator->Rows());
                        duplicateAnimator.SetSpeed(
                            spriteAnimator->Speed());
                        duplicateAnimator.SetPlayOnStart(
                            spriteAnimator->PlayOnStart());
                        duplicateAnimator.SetDefaultClip(
                            spriteAnimator->DefaultClip());
                        for (const auto& clip :
                            spriteAnimator->Clips())
                        {
                            duplicateAnimator.AddClip(
                                clip);
                        }
                        duplicateComponent =
                            &duplicateAnimator;
                    }
                    else if (const auto* tilemap =
                        dynamic_cast<
                            const TilemapComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateTilemap =
                            duplicate.AddComponent<
                                TilemapComponent>(
                                    tilemap->
                                        TileSize(),
                                    tilemap->
                                        AtlasColumns(),
                                    tilemap->
                                        AtlasRows(),
                                    tilemap->Color(),
                                    tilemap->
                                        TexturePath());
                        for (const auto&
                            [coordinate, tileIndex] :
                            tilemap->Cells())
                        {
                            duplicateTilemap.SetCell(
                                coordinate.first,
                                coordinate.second,
                                tileIndex);
                        }
                        duplicateTilemap.SetSortOrder(
                            tilemap->SortOrder());
                        duplicateComponent =
                            &duplicateTilemap;
                    }
                    else if (const auto* parallax =
                        dynamic_cast<
                            const ParallaxLayerComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                ParallaxLayerComponent>(
                                    parallax->Factor(),
                                    parallax
                                        ->ReferenceId());
                    }
                    else if (const auto* navMesh =
                        dynamic_cast<
                            const NavMeshComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateNavMesh =
                            duplicate.AddComponent<
                                NavMeshComponent>(
                                    navMesh->
                                        SurfaceSize(),
                                    navMesh->
                                        CellSize(),
                                    navMesh->
                                        AgentRadius(),
                                    navMesh->
                                        AgentHeight());
                        if (navMesh->IsBaked())
                        {
                            std::vector<
                                NavMeshComponent::
                                    CellCoordinate>
                                blockedCells;
                            for (std::uint32_t z{};
                                z < navMesh->
                                    GridDepth();
                                ++z)
                            {
                                for (std::uint32_t x{};
                                    x < navMesh->
                                        GridWidth();
                                    ++x)
                                {
                                    if (navMesh->
                                        IsBlocked(x, z))
                                    {
                                        blockedCells.
                                            emplace_back(
                                                x,
                                                z);
                                    }
                                }
                            }
                            duplicateNavMesh.
                                RestoreBake(
                                    blockedCells);
                        }
                        duplicateComponent =
                            &duplicateNavMesh;
                    }
                    else if (const auto* agent =
                        dynamic_cast<
                            const
                                NavMeshAgentComponent*>(
                                    sourceComponent.get()))
                    {
                        auto& duplicateAgent =
                            duplicate.AddComponent<
                                NavMeshAgentComponent>(
                                    agent->Speed(),
                                    agent->
                                        StoppingDistance(),
                                    agent->
                                        RotateToPath());
                        duplicateAgent.SetPath(
                            agent->Destination(),
                            agent->Path());
                        duplicateComponent =
                            &duplicateAgent;
                    }
                    else if (const auto* particles =
                        dynamic_cast<
                            const
                                ParticleSystemComponent*>(
                                    sourceComponent.get()))
                    {
                        auto& duplicateParticles =
                            duplicate.AddComponent<
                                ParticleSystemComponent>(
                                    particles->
                                        MaxParticles(),
                                    particles->
                                        EmissionRate(),
                                    particles->
                                        Lifetime(),
                                    particles->
                                        StartSpeed(),
                                    particles->
                                        StartSize(),
                                    particles->
                                        StartColor(),
                                    particles->
                                        EndColor(),
                                    particles->
                                        EmitterShape(),
                                    particles->
                                        TexturePath());
                        duplicateParticles.
                            SetEndSizeMultiplier(
                                particles->
                                    EndSizeMultiplier());
                        duplicateParticles.SetRenderMode(
                            particles->RenderMode());
                        duplicateParticles.SetGravity(
                            particles->Gravity());
                        duplicateParticles.
                            SetEmitterSize(
                                particles->
                                    EmitterSize());
                        duplicateParticles.
                            SetConeAngle(
                                particles->
                                    ConeAngle());
                        duplicateParticles.
                            SetDuration(
                                particles->
                                    Duration());
                        duplicateParticles.
                            SetLooping(
                                particles->
                                    Looping());
                        duplicateParticles.
                            SetPlayOnStart(
                                particles->
                                    PlayOnStart());
                        duplicateParticles.
                            SetPreviewInEditor(
                                particles->
                                    PreviewInEditor());
                        duplicateParticles.
                            SetAdditive(
                                particles->
                                    Additive());
                        duplicateParticles.
                            SetShaderPath(
                                particles->
                                    ShaderPath());
                        duplicateParticles.
                            SetAuxiliaryTexturePath(
                                particles->
                                    AuxiliaryTexturePath());
                        for (std::size_t index = 0;
                            index
                                < ParticleSystemComponent::
                                    CustomParameterCount;
                            ++index)
                        {
                            duplicateParticles.
                                SetCustomParameter(
                                    index,
                                    particles->
                                        CustomParameter(index));
                        }
                        duplicateComponent =
                            &duplicateParticles;
                    }
                    else if (const auto* canvas =
                        dynamic_cast<
                            const UICanvasComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                UICanvasComponent>(
                                    canvas->
                                        ReferenceResolution(),
                                    canvas->
                                        MatchWidthOrHeight());
                    }
                    else if (const auto* uiTransform =
                        dynamic_cast<
                            const
                                UIRectTransformComponent*>(
                                    sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                UIRectTransformComponent>(
                                    uiTransform->AnchorMin(),
                                    uiTransform->AnchorMax(),
                                    uiTransform->Pivot(),
                                    uiTransform->
                                        AnchoredPosition(),
                                    uiTransform->SizeDelta());
                    }
                    else if (const auto* button =
                        dynamic_cast<
                            const UIButtonComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateButton =
                            duplicate.AddComponent<
                                UIButtonComponent>(
                                    button->Label(),
                                    button->FallbackSize(),
                                    button->TexturePath());
                        duplicateButton.SetFontFamily(
                            button->FontFamily());
                        duplicateButton.SetFontSize(
                            button->FontSize());
                        duplicateButton.SetNormalColor(
                            button->NormalColor());
                        duplicateButton.SetHoveredColor(
                            button->HoveredColor());
                        duplicateButton.SetPressedColor(
                            button->PressedColor());
                        duplicateButton.SetDisabledColor(
                            button->DisabledColor());
                        duplicateButton.SetTextColor(
                            button->TextColor());
                        duplicateButton.SetInteractable(
                            button->Interactable());
                        duplicateButton.SetTargetScene(
                            button->TargetScene());
                        duplicateButton.
                            SetReloadCurrentScene(
                                button->
                                    ReloadCurrentScene());
                        duplicateButton
                            .SetClickEventName(
                                button
                                    ->ClickEventName());
                        duplicateButton.SetSortOrder(
                            button->SortOrder());
                        duplicateComponent =
                            &duplicateButton;
                    }
                    else if (const auto* image =
                        dynamic_cast<
                            const UIImageComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateImage =
                            duplicate.AddComponent<
                                UIImageComponent>(
                                    image->TexturePath(),
                                    image->Color());
                        duplicateImage.SetBorder(
                            image->Border());
                        duplicateImage.SetFallbackSize(
                            image->FallbackSize());
                        duplicateImage.SetSortOrder(
                            image->SortOrder());
                        duplicateComponent =
                            &duplicateImage;
                    }
                    else if (const auto* toggle =
                        dynamic_cast<
                            const UIToggleComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateToggle =
                            duplicate.AddComponent<
                                UIToggleComponent>(
                                    toggle->Label(),
                                    toggle->IsOn());
                        duplicateToggle.SetFontFamily(
                            toggle->FontFamily());
                        duplicateToggle.SetFontSize(
                            toggle->FontSize());
                        duplicateToggle.SetInteractable(
                            toggle->Interactable());
                        duplicateToggle.SetBoxColor(
                            toggle->BoxColor());
                        duplicateToggle.SetCheckColor(
                            toggle->CheckColor());
                        duplicateToggle.SetTextColor(
                            toggle->TextColor());
                        duplicateToggle.SetFallbackSize(
                            toggle->FallbackSize());
                        duplicateToggle.SetSortOrder(
                            toggle->SortOrder());
                        static_cast<void>(
                            duplicateToggle.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateToggle;
                    }
                    else if (const auto* slider =
                        dynamic_cast<
                            const UISliderComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateSlider =
                            duplicate.AddComponent<
                                UISliderComponent>(
                                    slider->MinimumValue(),
                                    slider->MaximumValue(),
                                    slider->Value());
                        duplicateSlider.SetWholeNumbers(
                            slider->WholeNumbers());
                        duplicateSlider.SetInteractable(
                            slider->Interactable());
                        duplicateSlider.
                            SetBackgroundColor(
                                slider->
                                    BackgroundColor());
                        duplicateSlider.SetFillColor(
                            slider->FillColor());
                        duplicateSlider.SetHandleColor(
                            slider->HandleColor());
                        duplicateSlider.SetFallbackSize(
                            slider->FallbackSize());
                        duplicateSlider.SetSortOrder(
                            slider->SortOrder());
                        static_cast<void>(
                            duplicateSlider.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateSlider;
                    }
                    else if (const auto* inputField =
                        dynamic_cast<
                            const UIInputFieldComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateField =
                            duplicate.AddComponent<
                                UIInputFieldComponent>(
                                    inputField->Text(),
                                    inputField->
                                        Placeholder());
                        duplicateField.SetFontFamily(
                            inputField->FontFamily());
                        duplicateField.SetFontSize(
                            inputField->FontSize());
                        duplicateField.SetMaxLength(
                            inputField->MaxLength());
                        duplicateField.SetInteractable(
                            inputField->Interactable());
                        duplicateField.
                            SetBackgroundColor(
                                inputField->
                                    BackgroundColor());
                        duplicateField.SetFocusedColor(
                            inputField->FocusedColor());
                        duplicateField.SetTextColor(
                            inputField->TextColor());
                        duplicateField.
                            SetPlaceholderColor(
                                inputField->
                                    PlaceholderColor());
                        duplicateField.SetFallbackSize(
                            inputField->FallbackSize());
                        duplicateField.SetSortOrder(
                            inputField->SortOrder());
                        static_cast<void>(
                            duplicateField.
                                ConsumeValueChanged());
                        duplicateComponent =
                            &duplicateField;
                    }
                    else if (const auto* layoutGroup =
                        dynamic_cast<
                            const UILayoutGroupComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateLayout =
                            duplicate.AddComponent<
                                UILayoutGroupComponent>(
                                    layoutGroup->Axis(),
                                    layoutGroup->
                                        Spacing());
                        duplicateLayout.SetPadding(
                            layoutGroup->Padding());
                        duplicateLayout.
                            SetChildAlignment(
                                layoutGroup->
                                    ChildAlignment());
                        duplicateComponent =
                            &duplicateLayout;
                    }
                    else if (const auto* scrollView =
                        dynamic_cast<
                            const UIScrollViewComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateScroll =
                            duplicate.AddComponent<
                                UIScrollViewComponent>();
                        duplicateScroll.SetScrollSpeed(
                            scrollView->ScrollSpeed());
                        duplicateScroll.SetInteractable(
                            scrollView->Interactable());
                        duplicateScroll.
                            SetBackgroundColor(
                                scrollView->
                                    BackgroundColor());
                        duplicateScroll.
                            SetScrollbarColor(
                                scrollView->
                                    ScrollbarColor());
                        duplicateScroll.SetSortOrder(
                            scrollView->SortOrder());
                        duplicateComponent =
                            &duplicateScroll;
                    }
                    else if (dynamic_cast<
                        const AudioListenerComponent*>(
                            sourceComponent.get()) != nullptr)
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                AudioListenerComponent>();
                    }
                    else if (const auto* audio =
                        dynamic_cast<const AudioSourceComponent*>(sourceComponent.get()))
                    {
                        auto& duplicateAudio =
                            duplicate.AddComponent<AudioSourceComponent>(
                            audio->AudioPath(),
                            audio->Volume(),
                            audio->Pitch(),
                            audio->Pan(),
                            audio->Loop(),
                            audio->PlayOnStart(),
                            audio->IsSpatial(),
                            audio->MinimumDistance(),
                            audio->MaximumDistance());
                        duplicateAudio.SetBus(audio->Bus());
                        duplicateAudio.SetStreaming(
                            audio->IsStreaming());
                        duplicateComponent =
                            &duplicateAudio;
                    }
                    else if (const auto* model =
                        dynamic_cast<const ModelRendererComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<ModelRendererComponent>(
                            model->ModelPath(),
                            model->IsWireframe(),
                            model->IsMaterialOverrideEnabled(),
                            model->Color(),
                            model->AlbedoTexturePath(),
                            model->NormalTexturePath(),
                            model->Roughness(),
                            model->NormalStrength(),
                            model->MaterialAssetPath(),
                            model->AnimationIndex(),
                            model->AnimationSpeed(),
                            model->AnimationLoop(),
                            model->AnimationPlayOnStart(),
                            model->AnimationControllerPath(),
                            model->ApplyRootMotion(),
                            model->RootMotionNode(),
                            model->PreserveEmbeddedMaterialColor());
                        static_cast<ModelRendererComponent*>(
                            duplicateComponent)
                            ->SetMetallic(
                                model->Metallic());
                        auto* const duplicateModel =
                            static_cast<
                                ModelRendererComponent*>(
                                    duplicateComponent);
                        duplicateModel->SetUseLegacyShading(
                            model->UsesLegacyShading());
                        // PBRマップと発光も引き継ぎます。
                        duplicateModel->SetRoughnessTexturePath(
                            model->RoughnessTexturePath());
                        duplicateModel->SetMetallicTexturePath(
                            model->MetallicTexturePath());
                        duplicateModel->SetOcclusionTexturePath(
                            model->OcclusionTexturePath());
                        duplicateModel->SetEmissiveTexturePath(
                            model->EmissiveTexturePath());
                        duplicateModel->SetOcclusionStrength(
                            model->OcclusionStrength());
                        duplicateModel->SetEmissiveColor(
                            model->EmissiveColor());
                    }
                    else if (const auto* text =
                        dynamic_cast<const TextRendererComponent*>(sourceComponent.get()))
                    {
                        auto& duplicateText =
                            duplicate.AddComponent<TextRendererComponent>(
                                text->Text(),
                                text->FontFamily(),
                                text->FontSize(),
                                text->Color(),
                                text->LayoutSize(),
                                text->WordWrap(),
                                text->HorizontalAlignment(),
                                text->VerticalAlignment());
                        duplicateText.SetSortOrder(
                            text->SortOrder());
                        duplicateComponent = &duplicateText;
                    }
                    else if (const auto* rotator =
                        dynamic_cast<const RotatorComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<RotatorComponent>(
                            rotator->AngularVelocity());
                    }
                    else if (const auto* billboard =
                        dynamic_cast<
                            const BillboardComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateBillboard =
                            duplicate.AddComponent<
                                BillboardComponent>(
                                billboard->Mode(),
                                billboard->FacingAxis());
                        duplicateBillboard
                            .SetTargetPosition(
                                billboard
                                    ->TargetPosition());
                        duplicateComponent =
                            &duplicateBillboard;
                    }
                    else if (const auto* animator =
                        dynamic_cast<
                            const TransformAnimatorComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                TransformAnimatorComponent>(
                                    animator->ClipPath(),
                                    animator->Speed(),
                                    animator->Loop(),
                                    animator->PlayOnStart(),
                                    animator->ControllerPath());
                    }
                    else if (const auto* inputMover =
                        dynamic_cast<const InputMoverComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                InputMoverComponent>(
                                    inputMover->
                                        HorizontalAction(),
                                    inputMover->
                                        VerticalAction(),
                                    inputMover->Speed());
                    }
                    else if (const auto* controller =
                        dynamic_cast<
                            const CharacterControllerComponent*>(
                                sourceComponent.get()))
                    {
                        auto& duplicateController =
                            duplicate.AddComponent<
                                CharacterControllerComponent>(
                                    controller->Radius(),
                                    controller->Height(),
                                    controller->MoveSpeed(),
                                    controller->Gravity(),
                                    controller->JumpSpeed(),
                                    controller->StepOffset(),
                                    controller->SkinWidth(),
                                    controller->Layer(),
                                    controller->CollisionMask());
                        duplicateController.SetUseInput(
                            controller->UseInput());
                        duplicateController.SetHorizontalAction(
                            controller->HorizontalAction());
                        duplicateController.SetVerticalAction(
                            controller->VerticalAction());
                        duplicateController.SetJumpAction(
                            controller->JumpAction());
                        duplicateComponent = &duplicateController;
                    }
                    else if (const auto* rigidbody =
                        dynamic_cast<const RigidbodyComponent*>(sourceComponent.get()))
                    {
                        duplicateComponent = &duplicate.AddComponent<RigidbodyComponent>(
                            rigidbody->Velocity(),
                            rigidbody->UsesGravity(),
                            rigidbody->IsKinematic(),
                            rigidbody->CollisionDetection(),
                            rigidbody->Mass(),
                            rigidbody->AngularVelocity(),
                            rigidbody->CenterOfMass(),
                            rigidbody->LinearDrag(),
                            rigidbody->AngularDrag(),
                            rigidbody->Constraints(),
                            rigidbody->Interpolates());
                    }
                    else if (const auto* joint =
                        dynamic_cast<const JointComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<JointComponent>(
                                joint->Type(),
                                joint->ConnectedBodyId(),
                                joint->Anchor(),
                                joint->ConnectedAnchor(),
                                joint->Axis(),
                                joint->RestLength(),
                                joint->Stiffness(),
                                joint->Damping(),
                                joint->CollideConnected(),
                                joint->UseLimits(),
                                joint->Limits(),
                                joint->UseMotor(),
                                joint->Motor());
                    }
                    else if (const auto* lodGroup =
                        dynamic_cast<const LODGroupComponent*>(
                            sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                LODGroupComponent>(
                                    lodGroup->Levels(),
                                    lodGroup->
                                        CullDistance());
                    }
                    else if (const auto* nativeScript =
                        dynamic_cast<
                            const NativeScriptComponent*>(
                                sourceComponent.get()))
                    {
                        duplicateComponent =
                            &duplicate.AddComponent<
                                NativeScriptComponent>(
                                    nativeScript->ScriptType(),
                                    nativeScript->
                                        SerializedProperties());
                    }

                    if (duplicateComponent != nullptr)
                    {
                        duplicateComponent->SetEnabled(sourceComponent->IsEnabled());
                    }
                }

                for (const auto* sourceChild : sourceObject.Children())
                {
                    self(self, *sourceChild, &duplicate, false);
                }

                return duplicate;
            };

        try
        {
            auto& result =
                clone(clone, source, targetParent, true);
            for (const auto& [sourceId, duplicateId] :
                duplicatedIds)
            {
                static_cast<void>(sourceId);
                auto* duplicate = FindGameObject(
                    duplicateId);
                auto* joint = duplicate != nullptr
                    ? duplicate->GetComponent<JointComponent>()
                    : nullptr;
                if (joint != nullptr)
                {
                    if (const auto target =
                            duplicatedIds.find(
                                joint->ConnectedBodyId());
                        target != duplicatedIds.end())
                    {
                        joint->SetConnectedBodyId(
                            target->second);
                    }
                }
                if (auto* lodGroup =
                        duplicate != nullptr
                        ? duplicate->GetComponent<
                            LODGroupComponent>()
                        : nullptr)
                {
                    auto levels =
                        lodGroup->Levels();
                    for (auto& level : levels)
                    {
                        if (const auto target =
                                duplicatedIds.find(
                                    level.targetId);
                            target
                                != duplicatedIds.end())
                        {
                            level.targetId =
                                target->second;
                        }
                    }
                    lodGroup->SetLevels(
                        std::move(levels));
                }
            }
            return result;
        }
        catch (...)
        {
            if (duplicatedRoot != nullptr
                && FindGameObject(duplicatedRoot->Id()) == duplicatedRoot)
            {
                DestroyGameObject(*duplicatedRoot);
            }
            throw;
        }
    }

    GameObject* Scene::FindGameObject(const GameObjectId id) const noexcept
    {
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Id() == id)
            {
                return gameObject.get();
            }
        }

        return nullptr;
    }

    GameObject* Scene::FindGameObjectByName(
        const std::string_view name) const noexcept
    {
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Name() == name)
            {
                return gameObject.get();
            }
        }
        return nullptr;
    }

    std::vector<GameObject*> Scene::FindGameObjectsByName(
        const std::string_view name) const
    {
        std::vector<GameObject*> results;
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->Name() == name)
            {
                results.push_back(gameObject.get());
            }
        }
        return results;
    }

    GameObject* Scene::FindGameObjectByTag(
        const std::string_view tag) const noexcept
    {
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->CompareTag(tag))
            {
                return gameObject.get();
            }
        }
        return nullptr;
    }

    std::vector<GameObject*> Scene::FindGameObjectsByTag(
        const std::string_view tag) const
    {
        std::vector<GameObject*> results;
        for (const auto& gameObject : m_gameObjects)
        {
            if (gameObject->CompareTag(tag))
            {
                results.push_back(gameObject.get());
            }
        }
        return results;
    }

    bool Scene::IsTagRegistered(
        const std::string_view tag) const noexcept
    {
        for (const auto& registered : m_registeredTags)
        {
            if (registered == tag)
            {
                return true;
            }
        }
        return false;
    }

    void Scene::WarnUnregisteredTag(
        const GameObject& gameObject) const
    {
        if (m_registeredTags.empty()
            || gameObject.Tag().empty()
            || IsTagRegistered(gameObject.Tag()))
        {
            return;
        }
        Logger::Instance().Warning(
            "未登録のタグ「" + gameObject.Tag()
                + "」が「" + gameObject.Name()
                + "」で使われています。プロジェクト設定のタグ一覧へ"
                  "追加してください。",
            gameObject.Id());
    }

    bool Scene::ReorderGameObject(
        GameObject& moved,
        const GameObject& reference,
        const bool insertAfter)
    {
        if (&moved == &reference
            || FindGameObject(moved.Id()) != &moved
            || FindGameObject(reference.Id())
                != &reference)
        {
            return false;
        }
        // 自分の子孫を基準に動かすと、自分の中へ自分を入れる形に
        // なってしまいます。
        for (const auto* ancestor = reference.Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            if (ancestor == &moved)
            {
                return false;
            }
        }

        // 動かすのは自分と子孫のまとまりです。親だけ動かすと
        // 親子が配列上で離れ、保存したJSONの並びが階層表示と
        // 食い違って読みにくくなります。
        std::unordered_set<GameObjectId> movedIds;
        const auto collectSubtree =
            [&movedIds](
                const auto& self,
                const GameObject& current) -> void
            {
                movedIds.insert(current.Id());
                for (const auto* child : current.Children())
                {
                    self(self, *child);
                }
            };
        collectSubtree(collectSubtree, moved);

        // 基準が動かす側に含まれていたら何もしません。
        if (movedIds.contains(reference.Id()))
        {
            return false;
        }

        // まとまりを相対順序のまま取り出します。
        std::vector<std::unique_ptr<GameObject>> detached;
        detached.reserve(movedIds.size());
        for (auto& candidate : m_gameObjects)
        {
            if (candidate != nullptr
                && movedIds.contains(candidate->Id()))
            {
                detached.push_back(std::move(candidate));
            }
        }
        std::erase(
            m_gameObjects,
            std::unique_ptr<GameObject>{});

        // 取り出した後の位置で基準を探し直します（取り出しで
        // 添字がずれるため、先に求めた位置は使えません）。
        const auto referencePosition = std::ranges::find_if(
            m_gameObjects,
            [&reference](
                const std::unique_ptr<GameObject>& candidate)
            {
                return candidate.get() == &reference;
            });
        if (referencePosition == m_gameObjects.end())
        {
            // 起こらないはずですが、失った状態にはしません。
            for (auto& object : detached)
            {
                m_gameObjects.push_back(std::move(object));
            }
            return false;
        }

        const auto insertPosition = insertAfter
            ? std::next(referencePosition)
            : referencePosition;
        m_gameObjects.insert(
            insertPosition,
            std::make_move_iterator(detached.begin()),
            std::make_move_iterator(detached.end()));

        // 表示順の出どころは2つあります。ルートはm_gameObjectsの順、
        // 子は親のm_childrenの順です。片方だけ直すと、保存には
        // 反映されるのにエディターの表示が変わらない（再起動すると
        // 変わる）という食い違いになります。
        if (auto* const parent = moved.Parent();
            parent != nullptr
            && reference.Parent() == parent)
        {
            auto& siblings = parent->m_children;
            std::erase(siblings, &moved);
            const auto siblingPosition =
                std::ranges::find(siblings, &reference);
            if (siblingPosition == siblings.end())
            {
                siblings.push_back(&moved);
            }
            else
            {
                siblings.insert(
                    insertAfter
                        ? std::next(siblingPosition)
                        : siblingPosition,
                    &moved);
            }
        }
        return true;
    }

    bool Scene::DestroyGameObject(GameObject& gameObject)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            return false;
        }

        std::unordered_set<GameObjectId> idsToRemove;
        const auto collectChildren =
            [&idsToRemove](const auto& self, const GameObject& current) -> void
            {
                idsToRemove.insert(current.Id());
                for (const auto* child : current.Children())
                {
                    self(self, *child);
                }
            };
        collectChildren(collectChildren, gameObject);

        if (m_mainCamera != nullptr
            && idsToRemove.contains(m_mainCamera->Owner().Id()))
        {
            m_mainCamera = nullptr;
        }

        gameObject.SetParent(nullptr);
        std::erase_if(
            m_gameObjects,
            [&idsToRemove](const std::unique_ptr<GameObject>& candidate)
            {
                return idsToRemove.contains(candidate->Id());
            });
        for (const auto& remaining : m_gameObjects)
        {
            if (auto* joint =
                    remaining->GetComponent<JointComponent>();
                joint != nullptr
                && idsToRemove.contains(
                    joint->ConnectedBodyId()))
            {
                joint->SetConnectedBodyId(0);
            }
            if (auto* lodGroup =
                    remaining->GetComponent<
                        LODGroupComponent>())
            {
                auto levels =
                    lodGroup->Levels();
                bool changed{};
                for (auto& level : levels)
                {
                    if (idsToRemove.contains(
                        level.targetId))
                    {
                        level.targetId = 0;
                        changed = true;
                    }
                }
                if (changed)
                {
                    lodGroup->SetLevels(
                        std::move(levels));
                }
            }
        }
        return true;
    }

    void Scene::DontDestroyOnLoad(
        GameObject& gameObject,
        std::string persistenceKey)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            throw std::invalid_argument(
                "Persistent GameObject does not belong to this Scene.");
        }
        if (gameObject.Parent() != nullptr)
        {
            throw std::invalid_argument(
                "Only a root GameObject can persist across scene loads.");
        }
        if (persistenceKey.empty())
        {
            persistenceKey = gameObject.Name();
        }
        if (persistenceKey.empty())
        {
            persistenceKey =
                "PersistentGameObject:"
                + std::to_string(gameObject.Id());
        }
        for (const auto& candidate : m_gameObjects)
        {
            if (candidate.get() != &gameObject
                && candidate->m_persistent
                && candidate->m_persistenceKey
                    == persistenceKey)
            {
                throw std::invalid_argument(
                    "Persistence keys must be unique in a Scene.");
            }
        }
        gameObject.m_persistent = true;
        gameObject.m_persistenceKey =
            std::move(persistenceKey);
    }

    void Scene::DestroyOnLoad(GameObject& gameObject)
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            throw std::invalid_argument(
                "GameObject does not belong to this Scene.");
        }
        gameObject.m_persistent = false;
        gameObject.m_persistenceKey.clear();
    }

    SceneHandle Scene::FindAdditiveScene(
        const std::filesystem::path& path)
            const noexcept
    {
        if (path.empty())
        {
            return PrimarySceneHandle();
        }
        const auto normalized =
            path.lexically_normal();
        for (const auto& scene : m_additiveScenes)
        {
            if (scene.path == normalized)
            {
                return scene.handle;
            }
        }
        return PrimarySceneHandle();
    }

    const LoadedSceneInfo* Scene::FindAdditiveScene(
        const SceneHandle handle) const noexcept
    {
        for (const auto& scene : m_additiveScenes)
        {
            if (scene.handle == handle)
            {
                return &scene;
            }
        }
        return nullptr;
    }

    bool Scene::UnloadScene(const SceneHandle handle)
    {
        if (handle == PrimarySceneHandle())
        {
            Logger::Instance().Warning(
                "主シーンはUnloadSceneでは破棄できません。"
                "切り替えにはSceneManagerのRequestLoadを"
                "使ってください。");
            return false;
        }

        const auto entry = std::find_if(
            m_additiveScenes.begin(),
            m_additiveScenes.end(),
            [handle](const LoadedSceneInfo& scene)
            {
                return scene.handle == handle;
            });
        if (entry == m_additiveScenes.end())
        {
            return false;
        }
        const auto unloadedPath = entry->path;

        // 先にルートを消すと子孫もまとめて消えます。追加シーンの
        // GameObjectを主シーン側へ親付けし直していた場合の取り
        // こぼしを、2周目で回収します。
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<GameObject*> targets;
            for (const auto& object : m_gameObjects)
            {
                if (object->m_sourceScene != handle)
                {
                    continue;
                }
                if (pass == 0
                    && object->Parent() != nullptr)
                {
                    continue;
                }
                targets.push_back(object.get());
            }
            for (auto* target : targets)
            {
                if (FindGameObject(target->Id())
                    == target)
                {
                    static_cast<void>(
                        DestroyGameObject(*target));
                }
            }
        }

        m_additiveScenes.erase(entry);
        Logger::Instance().Info(
            "追加シーンを破棄しました: "
            + PathToUtf8(unloadedPath));
        return true;
    }

    bool Scene::UnloadScene(
        const std::filesystem::path& path)
    {
        const auto handle = FindAdditiveScene(path);
        if (handle == PrimarySceneHandle())
        {
            return false;
        }
        return UnloadScene(handle);
    }

    void Scene::UnloadAllAdditiveScenes()
    {
        while (!m_additiveScenes.empty())
        {
            const auto handle =
                m_additiveScenes.back().handle;
            if (!UnloadScene(handle))
            {
                // 想定外の状態でも無限ループさせません。
                m_additiveScenes.pop_back();
            }
        }
    }

    Scene::PersistentTransfer
        Scene::ExtractPersistentObjects()
    {
        PersistentTransfer transfer;
        std::unordered_set<GameObjectId>
            persistentIds;
        const auto collect =
            [&persistentIds](
                const auto& self,
                const GameObject& object) -> void
            {
                persistentIds.insert(object.Id());
                for (const auto* child : object.Children())
                {
                    self(self, *child);
                }
            };
        for (const auto& object : m_gameObjects)
        {
            if (object->m_persistent
                && object->Parent() == nullptr)
            {
                collect(collect, *object);
            }
        }
        if (persistentIds.empty())
        {
            return transfer;
        }

        if (m_mainCamera != nullptr
            && persistentIds.contains(
                m_mainCamera->Owner().Id()))
        {
            transfer.mainCamera = m_mainCamera;
            m_mainCamera = nullptr;
        }
        transfer.objects.reserve(
            persistentIds.size());
        for (auto& object : m_gameObjects)
        {
            if (persistentIds.contains(object->Id()))
            {
                transfer.objects.push_back(
                    std::move(object));
            }
        }
        std::erase(
            m_gameObjects,
            std::unique_ptr<GameObject>{});
        m_activeCollisions.clear();
        return transfer;
    }

    void Scene::MergePersistentObjects(
        PersistentTransfer transfer)
    {
        if (transfer.objects.empty())
        {
            return;
        }

        std::unordered_set<std::string>
            incomingKeys;
        std::unordered_set<GameObjectId>
            incomingIds;
        GameObjectId nextId = m_nextId;
        for (const auto& object : transfer.objects)
        {
            incomingIds.insert(object->Id());
            nextId = std::max(
                nextId,
                object->Id() + 1);
            if (object->m_persistent
                && object->Parent() == nullptr)
            {
                incomingKeys.insert(
                    object->m_persistenceKey);
            }
        }

        std::vector<GameObject*> duplicates;
        for (const auto& object : m_gameObjects)
        {
            if (object->m_persistent
                && object->Parent() == nullptr
                && incomingKeys.contains(
                    object->m_persistenceKey))
            {
                duplicates.push_back(object.get());
            }
        }
        for (auto* duplicate : duplicates)
        {
            if (FindGameObject(duplicate->Id())
                == duplicate)
            {
                DestroyGameObject(*duplicate);
            }
        }

        for (const auto& object : m_gameObjects)
        {
            nextId = std::max(
                nextId,
                object->Id() + 1);
        }
        for (const auto& object : m_gameObjects)
        {
            if (incomingIds.contains(object->Id()))
            {
                object->m_id = nextId++;
            }
        }

        for (auto& object : transfer.objects)
        {
            object->m_scene = this;
            // 追加シーン由来の永続オブジェクトは、シーンを跨いだ
            // 時点で主シーンの所属にします（元の追加シーンは
            // すでに存在せず、破棄も保存もできなくなるため）。
            object->m_sourceScene =
                PrimarySceneHandle();
            m_gameObjects.push_back(
                std::move(object));
        }
        if (transfer.mainCamera != nullptr)
        {
            m_mainCamera = transfer.mainCamera;
        }
        m_nextId = nextId;
        m_activeCollisions.clear();
    }

    bool Scene::RemoveComponent(GameObject& gameObject, Component& component)
    {
        if (m_mainCamera == &component)
        {
            m_mainCamera = nullptr;
        }

        return gameObject.RemoveComponent(component);
    }

    void Scene::SetAmbientLightColor(
        const DirectX::XMFLOAT3& color) noexcept
    {
        m_ambientLightColor = {
            std::clamp(color.x, 0.0f, 1.0f),
            std::clamp(color.y, 0.0f, 1.0f),
            std::clamp(color.z, 0.0f, 1.0f)
        };
    }

    void Scene::SetAmbientLightIntensity(
        const float intensity) noexcept
    {
        m_ambientLightIntensity =
            std::clamp(intensity, 0.0f, 4.0f);
    }

    void Scene::SetSkySettings(
        const SkySettings& settings) noexcept
    {
        m_sky = settings;
        const auto clampColor =
            [](DirectX::XMFLOAT3 color)
            {
                return DirectX::XMFLOAT3{
                    std::clamp(color.x, 0.0f, 8.0f),
                    std::clamp(color.y, 0.0f, 8.0f),
                    std::clamp(color.z, 0.0f, 8.0f)
                };
            };
        m_sky.topColor = clampColor(m_sky.topColor);
        m_sky.horizonColor =
            clampColor(m_sky.horizonColor);
        m_sky.groundColor =
            clampColor(m_sky.groundColor);
        m_sky.intensity =
            std::clamp(m_sky.intensity, 0.0f, 8.0f);
    }

    bool Scene::ResolveSkySun(
        DirectX::XMFLOAT3& directionToSun,
        DirectX::XMFLOAT3& color,
        float& angularRadius) const noexcept
    {
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            const auto* light =
                gameObject->GetComponent<
                    DirectionalLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }
            // WorldDirection()は光が進む向きなので、太陽へ向かう
            // 向きはその逆です。
            const auto travel = light->WorldDirection();
            directionToSun = {
                -travel.x,
                -travel.y,
                -travel.z
            };
            const auto lightColor = light->Color();
            const float intensity = light->Intensity();
            color = {
                lightColor.x * intensity,
                lightColor.y * intensity,
                lightColor.z * intensity
            };
            angularRadius = DirectX::XMConvertToRadians(
                light->AngularDiameterDegrees() * 0.5f);
            return true;
        }
        return false;
    }

    SkySettings Scene::ResolvedSky() const noexcept
    {
        if (!m_sky.sunDriven)
        {
            return m_sky;
        }
        DirectX::XMFLOAT3 directionToSun{};
        DirectX::XMFLOAT3 color{};
        float angularRadius{};
        if (!ResolveSkySun(directionToSun, color, angularRadius))
        {
            // 方向光が無いなら朝も夜も決まりません。手で設定した
            // 色のまま描きます（急に真っ暗にしない）。
            return m_sky;
        }
        const auto evaluated =
            EvaluateSunDrivenSky(directionToSun);
        SkySettings resolved = m_sky;
        resolved.topColor = evaluated.topColor;
        resolved.horizonColor = evaluated.horizonColor;
        resolved.groundColor = evaluated.groundColor;
        return resolved;
    }

    void Scene::SetFogSettings(
        const FogSettings& settings) noexcept
    {
        m_fog = settings;
        m_fog.color = {
            std::clamp(m_fog.color.x, 0.0f, 8.0f),
            std::clamp(m_fog.color.y, 0.0f, 8.0f),
            std::clamp(m_fog.color.z, 0.0f, 8.0f)
        };
        m_fog.startDistance =
            std::clamp(
                m_fog.startDistance,
                0.0f,
                100000.0f);
        m_fog.endDistance =
            std::clamp(
                m_fog.endDistance,
                m_fog.startDistance + 0.01f,
                100000.0f);
        m_fog.density =
            std::clamp(m_fog.density, 0.0f, 10.0f);
    }

    void Scene::SetAmbientOcclusionSettings(
        const AmbientOcclusionSettings& settings) noexcept
    {
        m_ambientOcclusion = settings;
        m_ambientOcclusion.radius =
            std::clamp(m_ambientOcclusion.radius, 0.01f, 10.0f);
        m_ambientOcclusion.strength =
            std::clamp(m_ambientOcclusion.strength, 0.0f, 1.0f);
    }

    void Scene::SetTemporalAntiAliasingSettings(
        const TemporalAntiAliasingSettings& settings)
        noexcept
    {
        m_temporalAntiAliasing = settings;
        // 0.98より上は履歴がほぼ抜けなくなり、動きに追従しません。
        m_temporalAntiAliasing.historyWeight = std::clamp(
            m_temporalAntiAliasing.historyWeight,
            0.0f,
            0.98f);
        m_temporalAntiAliasing.jitterScale = std::clamp(
            m_temporalAntiAliasing.jitterScale,
            0.0f,
            2.0f);
        m_temporalAntiAliasing.clampTolerance = std::clamp(
            m_temporalAntiAliasing.clampTolerance,
            0.0f,
            8.0f);
    }

    void Scene::SetScreenSpaceReflectionSettings(
        const ScreenSpaceReflectionSettings& settings)
        noexcept
    {
        m_screenSpaceReflection = settings;
        m_screenSpaceReflection.intensity = std::clamp(
            m_screenSpaceReflection.intensity,
            0.0f,
            1.0f);
        m_screenSpaceReflection.maximumDistance =
            std::clamp(
                m_screenSpaceReflection.maximumDistance,
                0.1f,
                1000.0f);
        m_screenSpaceReflection.stepCount =
            std::clamp<std::uint32_t>(
                m_screenSpaceReflection.stepCount,
                4u,
                128u);
        m_screenSpaceReflection.thickness = std::clamp(
            m_screenSpaceReflection.thickness,
            0.01f,
            100.0f);
        m_screenSpaceReflection.roughnessCutoff =
            std::clamp(
                m_screenSpaceReflection.roughnessCutoff,
                0.0f,
                1.0f);
    }

    void Scene::SetVolumetricLightSettings(
        const VolumetricLightSettings& settings) noexcept
    {
        m_volumetricLight = settings;
        m_volumetricLight.intensity = std::clamp(
            m_volumetricLight.intensity,
            0.0f,
            4.0f);
        m_volumetricLight.sampleCount =
            std::clamp<std::uint32_t>(
                m_volumetricLight.sampleCount,
                4u,
                128u);
        m_volumetricLight.maximumDistance = std::clamp(
            m_volumetricLight.maximumDistance,
            1.0f,
            10000.0f);
        m_volumetricLight.scattering = std::clamp(
            m_volumetricLight.scattering,
            0.0f,
            0.95f);
    }

    void Scene::SetBloomSettings(
        const BloomSettings& settings) noexcept
    {
        m_bloom = settings;
        m_bloom.threshold =
            std::clamp(m_bloom.threshold, 0.0f, 4.0f);
        m_bloom.intensity =
            std::clamp(m_bloom.intensity, 0.0f, 8.0f);
        m_bloom.radius =
            std::clamp(m_bloom.radius, 0.25f, 12.0f);
    }

    void Scene::SetScreenOutlineSettings(
        const ScreenOutlineSettings& settings) noexcept
    {
        m_screenOutline = settings;
        m_screenOutline.color = {
            std::clamp(m_screenOutline.color.x, 0.0f, 1.0f),
            std::clamp(m_screenOutline.color.y, 0.0f, 1.0f),
            std::clamp(m_screenOutline.color.z, 0.0f, 1.0f)
        };
        m_screenOutline.intensity = std::clamp(
            m_screenOutline.intensity,
            0.0f,
            1.0f);
        m_screenOutline.thickness = std::clamp(
            m_screenOutline.thickness,
            1.0f,
            4.0f);
        m_screenOutline.depthThreshold = std::clamp(
            m_screenOutline.depthThreshold,
            0.0001f,
            1.0f);
        m_screenOutline.normalThreshold = std::clamp(
            m_screenOutline.normalThreshold,
            0.0f,
            1.0f);
    }

    void Scene::SetScreenSpaceLensFlareSettings(
        const ScreenSpaceLensFlareSettings& settings) noexcept
    {
        m_screenSpaceLensFlare = settings;
        m_screenSpaceLensFlare.threshold = std::clamp(
            m_screenSpaceLensFlare.threshold,
            0.0f,
            16.0f);
        m_screenSpaceLensFlare.intensity = std::clamp(
            m_screenSpaceLensFlare.intensity,
            0.0f,
            8.0f);
        m_screenSpaceLensFlare.ghostDispersal = std::clamp(
            m_screenSpaceLensFlare.ghostDispersal,
            0.01f,
            2.0f);
        m_screenSpaceLensFlare.haloWidth = std::clamp(
            m_screenSpaceLensFlare.haloWidth,
            0.05f,
            1.5f);
        m_screenSpaceLensFlare.chromaticAberration = std::clamp(
            m_screenSpaceLensFlare.chromaticAberration,
            0.0f,
            1.0f);
        m_screenSpaceLensFlare.streakIntensity = std::clamp(
            m_screenSpaceLensFlare.streakIntensity,
            0.0f,
            4.0f);
        m_screenSpaceLensFlare.streakLength = std::clamp(
            m_screenSpaceLensFlare.streakLength,
            0.0f,
            1.0f);
        m_screenSpaceLensFlare.streakDirections = std::clamp(
            m_screenSpaceLensFlare.streakDirections,
            1u,
            4u);
    }

    void Scene::SetDepthOfFieldSettings(
        const DepthOfFieldSettings& settings) noexcept
    {
        m_depthOfField = settings;
        // ピントの合う距離は0にできません（0除算になるうえ、
        // カメラの直前にピントを合わせる意味もありません）。
        m_depthOfField.focusDistance = std::clamp(
            m_depthOfField.focusDistance,
            0.01f,
            10000.0f);
        m_depthOfField.focusRange = std::clamp(
            m_depthOfField.focusRange,
            0.0f,
            1000.0f);
        m_depthOfField.blurStrength = std::clamp(
            m_depthOfField.blurStrength,
            0.0f,
            8.0f);
        // 上限を32画素で止めているのは、サンプル数を増やさずに
        // 半径だけ広げるとぼけの粒が目立ってくるためです。
        m_depthOfField.maximumRadius = std::clamp(
            m_depthOfField.maximumRadius,
            0.0f,
            32.0f);
    }

    void Scene::SetMotionBlurSettings(
        const MotionBlurSettings& settings) noexcept
    {
        m_motionBlur = settings;
        m_motionBlur.intensity = std::clamp(
            m_motionBlur.intensity,
            0.0f,
            4.0f);
        // 上限を64画素で止めているのは、これ以上伸ばすとサンプルの
        // 間隔が空いて、ブレが縞に分かれて見えるためです。
        m_motionBlur.maximumRadius = std::clamp(
            m_motionBlur.maximumRadius,
            0.0f,
            64.0f);
    }

    void Scene::SetAutoExposureSettings(
        const AutoExposureSettings& settings) noexcept
    {
        m_autoExposure = settings;
        m_autoExposure.keyValue = std::clamp(
            m_autoExposure.keyValue,
            0.01f,
            2.0f);
        m_autoExposure.minimumLuminance = std::clamp(
            m_autoExposure.minimumLuminance,
            0.0001f,
            100.0f);
        // 上限は下限より下へ行けません。逆に入れられると露出が
        // どちらへ張り付くか分からなくなります。
        m_autoExposure.maximumLuminance = std::clamp(
            m_autoExposure.maximumLuminance,
            m_autoExposure.minimumLuminance,
            100.0f);
        m_autoExposure.speedToBright = std::clamp(
            m_autoExposure.speedToBright,
            0.0f,
            20.0f);
        m_autoExposure.speedToDark = std::clamp(
            m_autoExposure.speedToDark,
            0.0f,
            20.0f);
    }

    PostProcessFrame Scene::PostProcessFrameData() const
    {
        PostProcessFrame frame{};
        frame.bloom = m_bloom;
        frame.screenOutline = m_screenOutlineFrame;
        frame.screenOutline.settings = m_screenOutline;
        frame.lensFlare = m_screenSpaceLensFlare;
        frame.colorGrading = m_colorGrading;
        frame.volumetric = m_volumetricFrame;
        frame.temporal = m_temporalFrame;
        // 行列は直前の3D描画が控えたものを使い、設定は今の値を
        // その場で読みます。こうしないと、インスペクターで動かした
        // 値が1フレーム遅れて効きます。
        frame.depthOfField = m_depthOfFieldFrame;
        frame.depthOfField.settings = m_depthOfField;
        frame.motionBlur = m_motionBlurFrame;
        frame.motionBlur.settings = m_motionBlur;
        frame.autoExposure.settings = m_autoExposure;
        // 順応は実時間で進めます。timeScaleを0にしたときやエディター
        // で停止しているときも露出は落ち着いてほしいためです。
        frame.autoExposure.deltaSeconds =
            Time::UnscaledDeltaTime();
        return frame;
    }

    void Scene::SetColorGradingSettings(
        const ColorGradingSettings& settings) noexcept
    {
        m_colorGrading = settings;
        m_colorGrading.exposure =
            std::clamp(m_colorGrading.exposure, -8.0f, 8.0f);
        m_colorGrading.contrast =
            std::clamp(m_colorGrading.contrast, 0.0f, 4.0f);
        m_colorGrading.saturation =
            std::clamp(m_colorGrading.saturation, 0.0f, 4.0f);
        m_colorGrading.temperature =
            std::clamp(m_colorGrading.temperature, -2.0f, 2.0f);
        m_colorGrading.tint =
            std::clamp(m_colorGrading.tint, -2.0f, 2.0f);
        m_colorGrading.vignette =
            std::clamp(m_colorGrading.vignette, 0.0f, 1.0f);
    }

    void Scene::SetPhysicsBroadPhaseCellSize(
        const float size) noexcept
    {
        m_physicsBroadPhaseCellSize = std::clamp(
            size,
            0.25f,
            100.0f);
    }

    void Scene::Clear() noexcept
    {
        m_mainCamera = nullptr;
        m_gameObjects.clear();
        m_nextId = 1;
        m_loadingScene = PrimarySceneHandle();
        m_nextSceneHandle = 1;
        m_additiveScenes.clear();
        // Cameraが消えるので、名前付きレンダーテクスチャも
        // 手放します（次のフレームで必要な分だけ作り直されます）。
        m_graphics.ClearRenderTextures();
        m_activeCollisions.clear();
        m_ambientLightColor = { 0.65f, 0.72f, 0.85f };
        m_ambientLightIntensity = 0.35f;
        m_sky = {};
        m_fog = {};
        m_bloom = {};
        m_screenOutline = {};
        m_screenSpaceLensFlare = {};
        m_depthOfField = {};
        m_depthOfFieldFrame = {};
        m_screenOutlineFrame = {};
        m_motionBlur = {};
        m_motionBlurFrame = {};
        m_autoExposure = {};
        m_ambientOcclusion = {};
        // 通常ロードでは省略された設定も既定値に戻します。
        // 前シーンの行列・ジッター・描画対象への参照は引き継ぎません。
        m_screenSpaceReflection = {};
        m_temporalAntiAliasing = {};
        m_temporalFrame = {};
        m_temporalFrameIndex = 0;
        m_volumetricLight = {};
        m_volumetricFrame = {};
        m_colorGrading = {};
        m_bakedGiSettings = {};
        m_bakedGiBakedShape = {};
        m_bakedGiData.clear();
        m_bakedGiViews = {};
        m_bakedGiTexturesDirty = false;
        m_bakedGiBaking = false;
        m_bakedGiNextProbe = 0;
        m_bakedGiWorking.clear();
        m_physicsBroadPhaseCellSize = 4.0f;
        m_physicsStats = {};
        m_physicsClock = {};
        m_renderingInterpolatedTransforms = false;
        m_frustumCullingEnabled = true;
        m_occlusionCullingEnabled = true;
        m_visibilityStats = {};
        m_renderSpatialIndex.Clear();
        m_frameReflectionProbes.clear();
        m_skyPrefilterKeyPath.clear();
        m_skyPrefilterKey = 0;
    }

    void Scene::Update(const float deltaTime)
    {
        Reactive::Detail::AdvanceFrame(
            deltaTime,
            Time::UnscaledDeltaTime());
        static_cast<void>(
            m_sceneManager->ProcessPending());
        // ユーザーコード（ScriptのStart/Update/FixedUpdate/LateUpdate）を
        // 呼ぶ走査は、範囲forではなく添字で回します。
        //
        // Startは最初のUpdateから呼ばれるため、Startの中で
        // CreateGameObjectするとm_gameObjectsが再確保されます。範囲forだと
        // イテレーターが宙に浮き、次の要素（ムーブ済みでnullになった
        // unique_ptr）を触ってアクセス違反になります。添字なら毎回
        // size()と配列先頭を読み直すので、走査中に追加されても壊れません。
        for (std::size_t index = 0;
            index < m_gameObjects.size();
            ++index)
        {
            m_gameObjects[index]->Update(m_graphics, deltaTime);
        }
        for (const auto& gameObject : m_gameObjects)
        {
            const auto* rigidbody =
                gameObject->GetComponent<
                    RigidbodyComponent>();
            gameObject->
                SetPhysicsInterpolationActive(
                    rigidbody != nullptr
                    && rigidbody->IsEnabled()
                    && rigidbody->Interpolates());
            gameObject->
                SynchronizePhysicsInterpolation();
        }

        const auto& physicsSettings = ActivePhysicsSettings();
        m_physicsClock.BeginFrame(deltaTime, physicsSettings.fixedTimeStep,
            static_cast<std::size_t>(physicsSettings.maximumCatchUpSteps));
        const float safeDeltaTime =
            std::isfinite(deltaTime) ? std::clamp(deltaTime, 0.0f, 0.1f) : 0.0f;
        if (safeDeltaTime <= 0.0f)
        {
            // エディターのゼロ時間での重なり更新を維持します。物理姿勢が
            // 変わった物体だけ履歴を同期し、停止中の表示位置は保持します。
            StepPhysics(0.0f);
            for (const auto& gameObject :
                m_gameObjects)
            {
                gameObject->
                    SynchronizePhysicsInterpolation();
            }
            for (std::size_t index = 0;
                index < m_gameObjects.size();
                ++index)
            {
                m_gameObjects[index]->LateUpdate(
                    m_graphics,
                    deltaTime);
            }
            return;
        }

        // コールバック中に設定が変更されても、このフレームの刻み幅は
        // 時計・FixedUpdate・物理計算で同じ値を使います。
        const float fixedDeltaTime = PhysicsTiming().fixedDeltaTime;
        while (m_physicsClock.PendingStep())
        {
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->
                    BeginPhysicsInterpolationStep();
            }
            for (std::size_t index = 0;
                index < m_gameObjects.size();
                ++index)
            {
                m_gameObjects[index]->FixedUpdate(
                    m_graphics,
                    fixedDeltaTime);
            }
            StepPhysics(fixedDeltaTime);
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->
                    EndPhysicsInterpolationStep();
            }
            m_physicsClock.CompleteStep();
        }
        m_physicsClock.FinishFrame();

        for (std::size_t index = 0;
            index < m_gameObjects.size();
            ++index)
        {
            m_gameObjects[index]->LateUpdate(
                m_graphics,
                deltaTime);
        }
    }

    std::size_t Scene::PhysicsSubstepCount(
        const float deltaTime) const noexcept
    {
        // 薄い障害物のすり抜けはスイープCCD（StepPhysics内）が
        // 防ぐため、ここでは接触品質のためのサブステップ数だけを
        // 決めます。基準は「そのボディ自身のコライダー寸法」で、
        // シーン内の無関係な小コライダーには影響されません。
        const float absoluteDeltaTime = std::abs(deltaTime);
        std::size_t requiredSubsteps{ 1 };
        for (const auto& gameObject : m_gameObjects)
        {
            const auto* body =
                gameObject->GetComponent<RigidbodyComponent>();
            if (body == nullptr
                || !body->IsEnabled()
                || body->IsKinematic()
                || body->IsSleeping()
                || !body->UsesContinuousCollisionDetection()
                || !gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            // ボディ自身の最小コライダー寸法。
            float ownFeatureSize = 0.5f;
            if (const auto* box =
                    gameObject->GetComponent<
                        BoxCollider3DComponent>();
                box != nullptr && box->IsEnabled())
            {
                const auto bounds = box->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y),
                    std::abs(bounds.maximum.z - bounds.minimum.z)
                });
            }
            if (const auto* capsule =
                    gameObject->GetComponent<
                        CapsuleCollider3DComponent>();
                capsule != nullptr && capsule->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    capsule->Radius() * 2.0f);
            }
            if (const auto* sphere =
                    gameObject->GetComponent<
                        SphereCollider3DComponent>();
                sphere != nullptr && sphere->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    sphere->WorldSphere().radius * 2.0f);
            }
            if (const auto* box =
                    gameObject->GetComponent<
                        BoxCollider2DComponent>();
                box != nullptr && box->IsEnabled())
            {
                const auto bounds = box->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y)
                });
            }
            if (const auto* circle =
                    gameObject->GetComponent<
                        CircleCollider2DComponent>();
                circle != nullptr && circle->IsEnabled())
            {
                ownFeatureSize = std::min(
                    ownFeatureSize,
                    circle->WorldCircle().radius * 2.0f);
            }
            if (const auto* polygon =
                    gameObject->GetComponent<
                        PolygonCollider2DComponent>();
                polygon != nullptr && polygon->IsEnabled())
            {
                const auto bounds = polygon->WorldBounds();
                ownFeatureSize = std::min({
                    ownFeatureSize,
                    std::abs(bounds.maximum.x - bounds.minimum.x),
                    std::abs(bounds.maximum.y - bounds.minimum.y)
                });
            }

            const float maximumStepDistance =
                std::max(ownFeatureSize * 0.5f, 0.01f);
            const auto velocity = body->Velocity();
            const float speed = std::sqrt(
                velocity.x * velocity.x
                + velocity.y * velocity.y
                + velocity.z * velocity.z);
            const float distance =
                speed * absoluteDeltaTime
                + (body->UsesGravity()
                    ? 4.905f * absoluteDeltaTime * absoluteDeltaTime
                    : 0.0f);
            requiredSubsteps = std::max(
                requiredSubsteps,
                static_cast<std::size_t>(
                    std::ceil(distance / maximumStepDistance)));
        }
        return std::clamp<std::size_t>(requiredSubsteps, 1, 8);
    }

    bool Scene::CollisionSuppressedByJoint(
        const GameObject& left,
        const GameObject& right) const noexcept
    {
        const auto suppresses = [](
            const GameObject& owner,
            const GameObject& connected) noexcept
        {
            const auto* joint = owner.GetComponent<JointComponent>();
            return joint != nullptr
                && joint->IsEnabled()
                && !joint->CollideConnected()
                && joint->ConnectedBodyId() == connected.Id();
        };
        return suppresses(left, right)
            || suppresses(right, left);
    }

    void Scene::SolveJoints(
        const float deltaTime,
        const bool applyForces)
    {
        const auto worldPoint = [](
            const GameObject& object,
            const DirectX::XMFLOAT3& local) noexcept
        {
            DirectX::XMFLOAT3 result{};
            DirectX::XMStoreFloat3(
                &result,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&local),
                    object.WorldMatrix()));
            return result;
        };
        const auto isDynamic = [](const RigidbodyComponent* body) noexcept
        {
            return body != nullptr
                && body->IsEnabled()
                && !body->IsKinematic();
        };
        const auto dot = [](
            const DirectX::XMFLOAT3& left,
            const DirectX::XMFLOAT3& right) noexcept
        {
            return left.x * right.x
                + left.y * right.y
                + left.z * right.z;
        };
        const auto multiply = [](
            const DirectX::XMFLOAT3& value,
            const float scale) noexcept
        {
            return DirectX::XMFLOAT3{
                value.x * scale,
                value.y * scale,
                value.z * scale
            };
        };
        const auto subtract = [](
            const DirectX::XMFLOAT3& left,
            const DirectX::XMFLOAT3& right) noexcept
        {
            return DirectX::XMFLOAT3{
                left.x - right.x,
                left.y - right.y,
                left.z - right.z
            };
        };

        for (const auto& ownerPointer : m_gameObjects)
        {
            auto& owner = *ownerPointer;
            auto* joint = owner.GetComponent<JointComponent>();
            if (!owner.IsActiveInHierarchy()
                || joint == nullptr
                || !joint->IsEnabled())
            {
                continue;
            }
            auto* connected =
                FindGameObject(joint->ConnectedBodyId());
            if (connected == nullptr
                || connected == &owner
                || !connected->IsActiveInHierarchy())
            {
                continue;
            }

            auto* ownerBody = owner.GetComponent<RigidbodyComponent>();
            auto* connectedBody =
                connected->GetComponent<RigidbodyComponent>();
            const bool ownerDynamic = isDynamic(ownerBody);
            const bool connectedDynamic = isDynamic(connectedBody);
            if (!ownerDynamic && !connectedDynamic)
            {
                continue;
            }

            const auto ownerAnchor =
                worldPoint(owner, joint->Anchor());
            const auto connectedAnchor =
                worldPoint(*connected, joint->ConnectedAnchor());
            DirectX::XMFLOAT3 delta{
                connectedAnchor.x - ownerAnchor.x,
                connectedAnchor.y - ownerAnchor.y,
                connectedAnchor.z - ownerAnchor.z
            };
            const float distance = std::sqrt(
                delta.x * delta.x
                + delta.y * delta.y
                + delta.z * delta.z);

            const float ownerInverseMass =
                ownerDynamic
                ? ownerBody->InverseMass()
                : 0.0f;
            const float connectedInverseMass =
                connectedDynamic
                ? connectedBody->InverseMass()
                : 0.0f;
            const float totalInverseMass =
                ownerInverseMass + connectedInverseMass;
            const float ownerShare =
                ownerInverseMass / totalInverseMass;
            const float connectedShare =
                connectedInverseMass / totalInverseMass;

            if (joint->Type() == JointType::Spring)
            {
                if (distance <= 0.000001f)
                {
                    continue;
                }
                const DirectX::XMFLOAT3 direction{
                    delta.x / distance,
                    delta.y / distance,
                    delta.z / distance
                };
                const auto ownerVelocity = ownerDynamic
                    ? ownerBody->Velocity()
                    : DirectX::XMFLOAT3{};
                const auto connectedVelocity = connectedDynamic
                    ? connectedBody->Velocity()
                    : DirectX::XMFLOAT3{};
                const float relativeSpeed =
                    (ownerVelocity.x - connectedVelocity.x) * direction.x
                    + (ownerVelocity.y - connectedVelocity.y) * direction.y
                    + (ownerVelocity.z - connectedVelocity.z) * direction.z;
                const float error = distance - joint->RestLength();
                const float impulse =
                    (joint->Stiffness() * error
                        - joint->Damping() * relativeSpeed)
                    * deltaTime;

                if (applyForces && ownerDynamic)
                {
                    ownerBody->SetVelocity({
                        ownerVelocity.x + direction.x * impulse * ownerShare,
                        ownerVelocity.y + direction.y * impulse * ownerShare,
                        ownerVelocity.z + direction.z * impulse * ownerShare
                    });
                }
                if (applyForces && connectedDynamic)
                {
                    connectedBody->SetVelocity({
                        connectedVelocity.x
                            - direction.x * impulse * connectedShare,
                        connectedVelocity.y
                            - direction.y * impulse * connectedShare,
                        connectedVelocity.z
                            - direction.z * impulse * connectedShare
                    });
                }

                const float correctionScale = std::clamp(
                    joint->Stiffness() * deltaTime * deltaTime,
                    0.0f,
                    0.5f);
                delta = {
                    direction.x * error * correctionScale,
                    direction.y * error * correctionScale,
                    direction.z * error * correctionScale
                };
            }

            if (ownerDynamic)
            {
                owner.TranslateWorld({
                    delta.x * ownerShare,
                    delta.y * ownerShare,
                    delta.z * ownerShare
                });
            }
            if (connectedDynamic)
            {
                connected->TranslateWorld({
                    -delta.x * connectedShare,
                    -delta.y * connectedShare,
                    -delta.z * connectedShare
                });
            }

            if (joint->Type() != JointType::Spring)
            {
                const auto ownerVelocity = ownerDynamic
                    ? ownerBody->Velocity()
                    : DirectX::XMFLOAT3{};
                const auto connectedVelocity = connectedDynamic
                    ? connectedBody->Velocity()
                    : DirectX::XMFLOAT3{};
                const DirectX::XMFLOAT3 sharedVelocity{
                    ownerVelocity.x * (1.0f - ownerShare)
                        + connectedVelocity.x * ownerShare,
                    ownerVelocity.y * (1.0f - ownerShare)
                        + connectedVelocity.y * ownerShare,
                    ownerVelocity.z * (1.0f - ownerShare)
                        + connectedVelocity.z * ownerShare
                };
                if (ownerDynamic)
                {
                    ownerBody->SetVelocity(sharedVelocity);
                }
                if (connectedDynamic)
                {
                    connectedBody->SetVelocity(sharedVelocity);
                }

                if (joint->Type() == JointType::Hinge)
                {
                    joint->EnsureHingeReference(
                        *connected);
                    const auto axis =
                        joint->HingeWorldAxis(
                            *connected);
                    auto ownerAngularVelocity =
                        ownerDynamic
                        ? ownerBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    auto connectedAngularVelocity =
                        connectedDynamic
                        ? connectedBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    auto relativeAngularVelocity =
                        subtract(
                            ownerAngularVelocity,
                            connectedAngularVelocity);
                    float axisSpeed = dot(
                        relativeAngularVelocity,
                        axis);
                    const auto lockedAngularVelocity =
                        subtract(
                            relativeAngularVelocity,
                            multiply(axis, axisSpeed));
                    if (ownerDynamic)
                    {
                        ownerAngularVelocity =
                            subtract(
                                ownerAngularVelocity,
                                multiply(
                                    lockedAngularVelocity,
                                    ownerShare));
                        ownerBody->SetAngularVelocity(
                            ownerAngularVelocity);
                    }
                    if (connectedDynamic)
                    {
                        connectedAngularVelocity = {
                            connectedAngularVelocity.x
                                + lockedAngularVelocity.x
                                    * connectedShare,
                            connectedAngularVelocity.y
                                + lockedAngularVelocity.y
                                    * connectedShare,
                            connectedAngularVelocity.z
                                + lockedAngularVelocity.z
                                    * connectedShare
                        };
                        connectedBody->SetAngularVelocity(
                            connectedAngularVelocity);
                    }

                    const float angle =
                        joint->HingeAngleRadians(
                            *connected);
                    constexpr float degreesToRadians =
                        DirectX::XM_PI / 180.0f;
                    bool atLowerLimit{};
                    bool atUpperLimit{};
                    if (joint->UseLimits())
                    {
                        const float minimum =
                            joint->Limits()
                                .minimumAngleDegrees
                            * degreesToRadians;
                        const float maximum =
                            joint->Limits()
                                .maximumAngleDegrees
                            * degreesToRadians;
                        atLowerLimit =
                            angle <= minimum;
                        atUpperLimit =
                            angle >= maximum;
                        const float target =
                            std::clamp(
                                angle,
                                minimum,
                                maximum);
                        const float error =
                            angle - target;
                        if (std::abs(error)
                            > 0.000001f)
                        {
                            if (ownerDynamic)
                            {
                                owner.RotateWorld(
                                    multiply(
                                        axis,
                                        -error
                                            * ownerShare));
                            }
                            if (connectedDynamic)
                            {
                                connected->RotateWorld(
                                    multiply(
                                        axis,
                                        error
                                            * connectedShare));
                            }
                        }

                        const bool movingOutward =
                            (atLowerLimit
                                && axisSpeed < 0.0f)
                            || (atUpperLimit
                                && axisSpeed > 0.0f);
                        if (movingOutward)
                        {
                            if (ownerDynamic)
                            {
                                ownerBody->SetAngularVelocity(
                                    subtract(
                                        ownerBody
                                            ->AngularVelocity(),
                                        multiply(
                                            axis,
                                            axisSpeed
                                                * ownerShare)));
                            }
                            if (connectedDynamic)
                            {
                                const auto velocity =
                                    connectedBody
                                        ->AngularVelocity();
                                connectedBody
                                    ->SetAngularVelocity({
                                        velocity.x
                                            + axis.x
                                                * axisSpeed
                                                * connectedShare,
                                        velocity.y
                                            + axis.y
                                                * axisSpeed
                                                * connectedShare,
                                        velocity.z
                                            + axis.z
                                                * axisSpeed
                                                * connectedShare
                                    });
                            }
                        }
                    }

                    if (applyForces
                        && joint->UseMotor()
                        && joint->Motor()
                            .maximumTorque > 0.0f)
                    {
                        float targetSpeed =
                            joint->Motor()
                                .targetVelocityDegrees
                            * degreesToRadians;
                        if ((atLowerLimit
                                && targetSpeed < 0.0f)
                            || (atUpperLimit
                                && targetSpeed > 0.0f))
                        {
                            targetSpeed = 0.0f;
                        }
                        relativeAngularVelocity =
                            subtract(
                                ownerDynamic
                                ? ownerBody
                                    ->AngularVelocity()
                                : DirectX::XMFLOAT3{},
                                connectedDynamic
                                ? connectedBody
                                    ->AngularVelocity()
                                : DirectX::XMFLOAT3{});
                        axisSpeed = dot(
                            relativeAngularVelocity,
                            axis);
                        const float ownerResponse =
                            ownerDynamic
                            ? dot(
                                ownerBody
                                    ->ApplyInverseInertia(
                                        axis),
                                axis)
                            : 0.0f;
                        const float connectedResponse =
                            connectedDynamic
                            ? dot(
                                connectedBody
                                    ->ApplyInverseInertia(
                                        axis),
                                axis)
                            : 0.0f;
                        const float response =
                            ownerResponse
                            + connectedResponse;
                        if (response > 0.000001f)
                        {
                            const float maximumImpulse =
                                joint->Motor()
                                    .maximumTorque
                                * std::abs(deltaTime);
                            const float impulse =
                                std::clamp(
                                    (targetSpeed
                                        - axisSpeed)
                                        / response,
                                    -maximumImpulse,
                                    maximumImpulse);
                            if (ownerDynamic)
                            {
                                ownerBody->AddTorque(
                                    multiply(
                                        axis,
                                        impulse),
                                    ForceMode::Impulse);
                            }
                            if (connectedDynamic)
                            {
                                connectedBody->AddTorque(
                                    multiply(
                                        axis,
                                        -impulse),
                                    ForceMode::Impulse);
                            }
                        }
                    }
                }
                else if (joint->Type() == JointType::Fixed)
                {
                    const auto ownerAngularVelocity =
                        ownerDynamic
                        ? ownerBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    const auto connectedAngularVelocity =
                        connectedDynamic
                        ? connectedBody->AngularVelocity()
                        : DirectX::XMFLOAT3{};
                    const DirectX::XMFLOAT3
                        sharedAngularVelocity{
                            ownerAngularVelocity.x
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.x
                                    * ownerShare,
                            ownerAngularVelocity.y
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.y
                                    * ownerShare,
                            ownerAngularVelocity.z
                                * (1.0f - ownerShare)
                                + connectedAngularVelocity.z
                                    * ownerShare
                        };
                    if (ownerDynamic)
                    {
                        ownerBody->SetAngularVelocity(
                            sharedAngularVelocity);
                    }
                    if (connectedDynamic)
                    {
                        connectedBody->SetAngularVelocity(
                            sharedAngularVelocity);
                    }
                }
            }
        }
    }

    void Scene::StepPhysics(const float deltaTime)
    {
        std::map<CollisionKey, bool> currentCollisions;
        std::unordered_set<std::uint64_t>
            supportedBodies;
        const auto findRigidbodyObject =
            [](GameObject& colliderObject)
        {
            auto* current = &colliderObject;
            while (current != nullptr)
            {
                if (auto* body =
                    current->GetComponent<
                        RigidbodyComponent>())
                {
                    return std::pair{
                        current,
                        body
                    };
                }
                current = current->Parent();
            }
            return std::pair<
                GameObject*,
                RigidbodyComponent*>{
                    &colliderObject,
                    nullptr
                };
        };

        const auto processContact =
            [this,
                &currentCollisions,
                &supportedBodies,
                &findRigidbodyObject](
                GameObject& left,
                GameObject& right,
                const Contact& contact,
                const bool is3D,
                const bool isTrigger,
                const PhysicsMaterial leftMaterial,
                const PhysicsMaterial rightMaterial)
            {
                auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(left);
                auto [rightPhysicsObject, rightBody] =
                    findRigidbodyObject(right);
                if (leftBody != nullptr
                    && leftPhysicsObject
                        == rightPhysicsObject)
                {
                    return;
                }
                const CollisionKey key{
                    std::min(left.Id(), right.Id()),
                    std::max(left.Id(), right.Id()),
                    is3D
                };
                const bool firstContactThisFrame =
                    !currentCollisions.contains(key);
                currentCollisions[key] = isTrigger;

                const DirectX::XMFLOAT3 oppositeNormal{
                    -contact.normal.x,
                    -contact.normal.y,
                    -contact.normal.z
                };

                if (!firstContactThisFrame)
                {
                    // Contact resolution still runs for every CCD substep,
                    // but gameplay callbacks are emitted once per frame.
                }
                else if (m_activeCollisions.contains(key))
                {
                    left.NotifyCollisionStay(
                        right,
                        contact.normal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                    right.NotifyCollisionStay(
                        left,
                        oppositeNormal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                }
                else
                {
                    left.NotifyCollisionEnter(
                        right,
                        contact.normal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                    right.NotifyCollisionEnter(
                        left,
                        oppositeNormal,
                        contact.point,
                        contact.penetration,
                        isTrigger);
                }

                if (isTrigger)
                {
                    return;
                }

                const bool leftDynamic =
                    leftBody != nullptr
                    && leftBody->IsEnabled()
                    && !leftBody->IsKinematic();
                const bool rightDynamic =
                    rightBody != nullptr
                    && rightBody->IsEnabled()
                    && !rightBody->IsKinematic();

                if (!leftDynamic && !rightDynamic)
                {
                    return;
                }

                const float inverseMassLeft = leftDynamic
                    ? leftBody->InverseMass()
                    : 0.0f;
                const float inverseMassRight = rightDynamic
                    ? rightBody->InverseMass()
                    : 0.0f;
                const float inverseMassSum =
                    inverseMassLeft + inverseMassRight;
                if (inverseMassSum <= 0.0f)
                {
                    return;
                }
                const float leftShare =
                    inverseMassLeft / inverseMassSum;
                const float rightShare =
                    inverseMassRight / inverseMassSum;
                constexpr float penetrationSlop =
                    0.001f;
                constexpr float positionCorrection =
                    0.8f;
                const float correctionDepth =
                    std::max(
                        contact.penetration
                            - penetrationSlop,
                        0.0f)
                    * positionCorrection;

                if (leftDynamic)
                {
                    leftPhysicsObject->TranslateWorld({
                        contact.normal.x * correctionDepth * leftShare,
                        contact.normal.y * correctionDepth * leftShare,
                        contact.normal.z * correctionDepth * leftShare
                    });
                }

                if (rightDynamic)
                {
                    rightPhysicsObject->TranslateWorld({
                        oppositeNormal.x * correctionDepth * rightShare,
                        oppositeNormal.y * correctionDepth * rightShare,
                        oppositeNormal.z * correctionDepth * rightShare
                    });
                }

                const auto material = CombinePhysicsMaterials(
                    leftMaterial,
                    rightMaterial);
                const auto dot = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return a.x * b.x + a.y * b.y + a.z * b.z;
                };
                const auto subtract = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return DirectX::XMFLOAT3{
                        a.x - b.x,
                        a.y - b.y,
                        a.z - b.z
                    };
                };
                const auto multiply = [](
                    const DirectX::XMFLOAT3& value,
                    const float scale) noexcept
                {
                    return DirectX::XMFLOAT3{
                        value.x * scale,
                        value.y * scale,
                        value.z * scale
                    };
                };
                const auto cross = [](
                    const DirectX::XMFLOAT3& a,
                    const DirectX::XMFLOAT3& b) noexcept
                {
                    return DirectX::XMFLOAT3{
                        a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x
                    };
                };
                const std::size_t pointCount =
                    contact.pointCount > 0
                    ? std::min(
                        contact.pointCount,
                        contact.points.size())
                    : 1;
                std::array<float, 4>
                    accumulatedNormalImpulses{};
                // 接触の解決を繰り返す回数。プロジェクト設定から
                // （以前は8回で固定でした）。積み上げた箱が沈む
                // ときに増やします。
                const int impulseIterations =
                    static_cast<int>(
                        ActivePhysicsSettings()
                            .solverIterations);
                for (int iteration{};
                    iteration < impulseIterations;
                    ++iteration)
                {
                    std::array<float, 4>
                        iterationNormalImpulses{};
                    for (std::size_t pointIndex{};
                        pointIndex < pointCount;
                        ++pointIndex)
                    {
                        const std::size_t orderedIndex =
                            iteration % 2 == 0
                            ? pointIndex
                            : pointCount
                                - pointIndex - 1;
                        const auto point =
                            contact.pointCount > 0
                            ? contact.points[orderedIndex]
                            : contact.point;
                        const auto leftRadius =
                            leftDynamic
                            ? subtract(
                                point,
                                leftBody
                                    ->WorldCenterOfMass())
                            : DirectX::XMFLOAT3{};
                        const auto rightRadius =
                            rightDynamic
                            ? subtract(
                                point,
                                rightBody
                                    ->WorldCenterOfMass())
                            : DirectX::XMFLOAT3{};
                        const auto impulseDenominator =
                            [&](const DirectX::XMFLOAT3&
                                direction)
                        {
                            float result =
                                inverseMassSum;
                            if (leftDynamic)
                            {
                                result += dot(
                                    cross(
                                        leftBody
                                            ->ApplyInverseInertia(
                                                cross(
                                                    leftRadius,
                                                    direction)),
                                        leftRadius),
                                    direction);
                            }
                            if (rightDynamic)
                            {
                                result += dot(
                                    cross(
                                        rightBody
                                            ->ApplyInverseInertia(
                                                cross(
                                                    rightRadius,
                                                    direction)),
                                        rightRadius),
                                    direction);
                            }
                            return result;
                        };

                        auto relativeVelocity =
                            subtract(
                                leftDynamic
                                ? leftBody
                                    ->VelocityAtPoint(
                                        point)
                                : DirectX::XMFLOAT3{},
                                rightDynamic
                                ? rightBody
                                    ->VelocityAtPoint(
                                        point)
                                : DirectX::XMFLOAT3{});
                        const float inwardSpeed =
                            dot(
                                relativeVelocity,
                                contact.normal);
                        const float normalDenominator =
                            impulseDenominator(
                                contact.normal);
                        if (inwardSpeed >= -0.000001f
                            || normalDenominator
                                <= 0.000001f)
                        {
                            continue;
                        }

                        const float restitution =
                            iteration == 0
                            ? material.restitution
                            : 0.0f;
                        const float
                            normalImpulseMagnitude =
                                -(1.0f + restitution)
                                * inwardSpeed
                                / normalDenominator
                                / static_cast<float>(
                                    pointCount);
                        iterationNormalImpulses[
                            orderedIndex] =
                                normalImpulseMagnitude;
                    }
                    for (std::size_t pointIndex{};
                        pointIndex < pointCount;
                        ++pointIndex)
                    {
                        const float
                            normalImpulseMagnitude =
                                iterationNormalImpulses[
                                    pointIndex];
                        if (normalImpulseMagnitude
                            <= 0.0f)
                        {
                            continue;
                        }
                        accumulatedNormalImpulses[
                            pointIndex] +=
                                normalImpulseMagnitude;
                        const auto point =
                            contact.pointCount > 0
                            ? contact.points[pointIndex]
                            : contact.point;
                        const auto normalImpulse =
                            multiply(
                                contact.normal,
                                normalImpulseMagnitude);
                        if (leftDynamic)
                        {
                            leftBody
                                ->ApplyImpulseAtPoint(
                                    normalImpulse,
                                    point);
                        }
                        if (rightDynamic)
                        {
                            rightBody
                                ->ApplyImpulseAtPoint(
                                    multiply(
                                        normalImpulse,
                                        -1.0f),
                                    point);
                        }
                    }
                }
                for (std::size_t pointIndex{};
                    pointIndex < pointCount;
                    ++pointIndex)
                {
                    const auto point =
                        contact.pointCount > 0
                        ? contact.points[pointIndex]
                        : contact.point;
                    auto relativeVelocity =
                        subtract(
                            leftDynamic
                            ? leftBody
                                ->VelocityAtPoint(point)
                            : DirectX::XMFLOAT3{},
                            rightDynamic
                            ? rightBody
                                ->VelocityAtPoint(point)
                            : DirectX::XMFLOAT3{});
                    const float normalSpeed =
                        dot(
                            relativeVelocity,
                            contact.normal);
                    auto tangent = subtract(
                        relativeVelocity,
                        multiply(
                            contact.normal,
                            normalSpeed));
                    // 2D接触では摩擦を面内（XY）に限定します。
                    if (!is3D)
                    {
                        tangent.z = 0.0f;
                    }
                    const float tangentLength =
                        std::sqrt(
                            dot(tangent, tangent));
                    if (tangentLength <= 0.005f)
                    {
                        continue;
                    }
                    tangent.x /= tangentLength;
                    tangent.y /= tangentLength;
                    tangent.z /= tangentLength;

                    const auto leftRadius =
                        leftDynamic
                        ? subtract(
                            point,
                            leftBody
                                ->WorldCenterOfMass())
                        : DirectX::XMFLOAT3{};
                    const auto rightRadius =
                        rightDynamic
                        ? subtract(
                            point,
                            rightBody
                                ->WorldCenterOfMass())
                        : DirectX::XMFLOAT3{};
                    float tangentDenominator =
                        inverseMassSum;
                    if (leftDynamic)
                    {
                        tangentDenominator += dot(
                            cross(
                                leftBody
                                    ->ApplyInverseInertia(
                                        cross(
                                            leftRadius,
                                            tangent)),
                                leftRadius),
                            tangent);
                    }
                    if (rightDynamic)
                    {
                        tangentDenominator += dot(
                            cross(
                                rightBody
                                    ->ApplyInverseInertia(
                                        cross(
                                            rightRadius,
                                            tangent)),
                                rightRadius),
                            tangent);
                    }
                    const float desiredTangentImpulse =
                        -dot(
                            relativeVelocity,
                            tangent)
                        / std::max(
                            tangentDenominator,
                            0.000001f);
                    const float maximumFrictionImpulse =
                        accumulatedNormalImpulses[
                            pointIndex]
                        * material.friction;
                    const float tangentImpulseMagnitude =
                        std::clamp(
                            desiredTangentImpulse,
                            -maximumFrictionImpulse,
                            maximumFrictionImpulse);
                    const auto tangentImpulse =
                        multiply(
                            tangent,
                            tangentImpulseMagnitude);
                    if (leftDynamic)
                    {
                        leftBody->ApplyImpulseAtPoint(
                            tangentImpulse,
                            point);
                    }
                    if (rightDynamic)
                    {
                        rightBody->ApplyImpulseAtPoint(
                            multiply(
                                tangentImpulse,
                                -1.0f),
                            point);
                    }
                }
                if (pointCount > 1
                    && material.restitution
                        <= 0.0001f)
                {
                    if (leftDynamic)
                    {
                        leftBody->RemoveInwardVelocity(
                            contact.normal);
                    }
                    if (rightDynamic)
                    {
                        rightBody->RemoveInwardVelocity(
                            oppositeNormal);
                    }
                }
                if (material.restitution
                    <= 0.0001f)
                {
                    if (leftDynamic
                        && contact.normal.y > 0.5f)
                    {
                        supportedBodies.insert(
                            leftPhysicsObject->Id());
                    }
                    if (rightDynamic
                        && oppositeNormal.y > 0.5f)
                    {
                        supportedBodies.insert(
                            rightPhysicsObject->Id());
                    }
                }
            };

        PhysicsBroadPhaseStats aggregateStats{};
        const std::size_t substepCount =
            PhysicsSubstepCount(deltaTime);
        const float substepDeltaTime =
            deltaTime / static_cast<float>(substepCount);
        for (std::size_t substep{}; substep < substepCount; ++substep)
        {
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }

                if (auto* rigidbody =
                        gameObject->GetComponent<RigidbodyComponent>();
                    rigidbody != nullptr && rigidbody->IsEnabled())
                {
                    float integrateDelta = substepDeltaTime;
                    // Continuousモードは移動をスイープし、最初の
                    // 衝突位置までに制限します（トンネリング防止）。
                    if (rigidbody
                            ->UsesContinuousCollisionDetection()
                        && !rigidbody->IsKinematic()
                        && !rigidbody->IsSleeping())
                    {
                        const auto velocity =
                            rigidbody->Velocity();
                        const float speed = std::sqrt(
                            velocity.x * velocity.x
                            + velocity.y * velocity.y
                            + velocity.z * velocity.z);
                        const float travel =
                            speed * substepDeltaTime;

                        // 自身のコライダーの最小断面半径を
                        // スイープ球の半径にします。
                        float sweepRadius = 0.25f;
                        std::uint32_t sweepMask =
                            0xffffffffu;
                        const auto shrinkToBounds =
                            [&sweepRadius](
                                const Bounds3D& bounds)
                        {
                            sweepRadius = std::min({
                                sweepRadius,
                                std::abs(
                                    bounds.maximum.x
                                    - bounds.minimum.x)
                                    * 0.5f,
                                std::abs(
                                    bounds.maximum.y
                                    - bounds.minimum.y)
                                    * 0.5f,
                                std::abs(
                                    bounds.maximum.z
                                    - bounds.minimum.z)
                                    * 0.5f });
                        };
                        if (const auto* box =
                            gameObject->GetComponent<
                                BoxCollider3DComponent>();
                            box != nullptr
                            && box->IsEnabled())
                        {
                            shrinkToBounds(
                                box->WorldBounds());
                            sweepMask =
                                box->CollisionMask();
                        }
                        if (const auto* sphere =
                            gameObject->GetComponent<
                                SphereCollider3DComponent>();
                            sphere != nullptr
                            && sphere->IsEnabled())
                        {
                            sweepRadius = std::min(
                                sweepRadius,
                                sphere->WorldSphere()
                                    .radius);
                            sweepMask =
                                sphere->CollisionMask();
                        }
                        if (const auto* capsule =
                            gameObject->GetComponent<
                                CapsuleCollider3DComponent>();
                            capsule != nullptr
                            && capsule->IsEnabled())
                        {
                            sweepRadius = std::min(
                                sweepRadius,
                                capsule->Radius());
                            sweepMask =
                                capsule->CollisionMask();
                        }

                        // 1サブステップで自身の断面を超えて動く
                        // 場合だけスイープします。
                        if (speed > 0.0001f
                            && travel > sweepRadius)
                        {
                            PhysicsQueryFilter sweepFilter;
                            sweepFilter.layerMask =
                                sweepMask;
                            sweepFilter
                                .ignoredGameObjectId =
                                gameObject->Id();
                            const Ray sweepRay{
                                rigidbody
                                    ->WorldCenterOfMass(),
                                {
                                    velocity.x / speed,
                                    velocity.y / speed,
                                    velocity.z / speed
                                } };
                            PhysicsHit sweepHit{};
                            if (SphereCast(
                                    sweepRay,
                                    sweepRadius,
                                    travel,
                                    sweepHit,
                                    sweepFilter))
                            {
                                constexpr float skin =
                                    0.01f;
                                const float allowed =
                                    std::max(
                                        sweepHit.distance
                                            - skin,
                                        0.0f);
                                integrateDelta =
                                    substepDeltaTime
                                    * std::clamp(
                                        allowed / travel,
                                        0.0f,
                                        1.0f);
                                // 進入方向の速度を消して
                                // 次ステップの再貫通を防止。
                                rigidbody
                                    ->RemoveInwardVelocity(
                                        sweepHit.normal);
                            }
                        }
                    }
                    rigidbody->Integrate(
                        *gameObject,
                        integrateDelta);
                }
            }
            constexpr int jointSolverIterations = 4;
            for (int iteration{};
                iteration < jointSolverIterations;
                ++iteration)
            {
                SolveJoints(
                    substepDeltaTime,
                    iteration == 0);
            }

        struct ColliderEntry2D final
        {
            GameObject* gameObject{};
            BoxCollider2DComponent* box{};
            CircleCollider2DComponent* circle{};
            PolygonCollider2DComponent* polygon{};
            Bounds2D bounds{};
            bool isTrigger{};
            std::uint32_t layer{};
            std::uint32_t collisionMask{};
            PhysicsMaterial material{};
        };
        struct ColliderEntry3D final
        {
            GameObject* gameObject{};
            BoxCollider3DComponent* box{};
            CapsuleCollider3DComponent* capsule{};
            SphereCollider3DComponent* sphere{};
            ConvexHullCollider3DComponent* hull{};
            MeshCollider3DComponent* mesh{};
            Bounds3D bounds{};
            bool isTrigger{};
            std::uint32_t layer{};
            std::uint32_t collisionMask{};
            PhysicsMaterial material{};
        };

        std::vector<ColliderEntry2D> entries2D;
        std::vector<ColliderEntry3D> entries3D;
        std::vector<Bounds2D> bounds2D;
        std::vector<Bounds3D> bounds3D;
        entries2D.reserve(m_gameObjects.size());
        entries3D.reserve(m_gameObjects.size());
        bounds2D.reserve(m_gameObjects.size());
        bounds3D.reserve(m_gameObjects.size());

        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            if (auto* collider =
                gameObject->GetComponent<
                    BoxCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    collider,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    CircleCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    nullptr,
                    collider,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    PolygonCollider2DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds = collider->WorldBounds();
                entries2D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    collider,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds2D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    BoxCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds = collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    collider,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    CapsuleCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds = collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    collider,
                    nullptr,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    SphereCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    collider,
                    nullptr,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    ConvexHullCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled())
            {
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    collider,
                    nullptr,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
            if (auto* collider =
                gameObject->GetComponent<
                    MeshCollider3DComponent>();
                collider != nullptr
                && collider->IsEnabled()
                && collider->HasMesh())
            {
                const auto bounds =
                    collider->WorldBounds();
                entries3D.push_back({
                    gameObject.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    collider,
                    bounds,
                    collider->IsTrigger(),
                    collider->Layer(),
                    collider->CollisionMask(),
                    collider->Material()
                });
                bounds3D.push_back(bounds);
            }
        }

        const auto broadPhase3D = BuildSpatialHashPairs(
            bounds3D,
            m_physicsBroadPhaseCellSize);
        const auto broadPhase2D = BuildSpatialHashPairs(
            bounds2D,
            m_physicsBroadPhaseCellSize);
        PhysicsBroadPhaseStats stepStats{
            entries2D.size(),
            entries3D.size(),
            broadPhase2D.pairs.size(),
            broadPhase3D.pairs.size(),
            0,
            0,
            broadPhase2D.occupiedCellCount,
            broadPhase3D.occupiedCellCount,
            broadPhase2D.oversizedColliderCount,
            broadPhase3D.oversizedColliderCount,
            0
        };

        constexpr int collisionSolverIterations = 4;
        for (int collisionIteration{};
            collisionIteration
                < collisionSolverIterations;
            ++collisionIteration)
        {
            for (const auto& pair :
                broadPhase3D.pairs)
            {
                auto& left = entries3D[pair.left];
                auto& right = entries3D[pair.right];
                const auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(
                        *left.gameObject);
                const auto [
                    rightPhysicsObject,
                    rightBody] =
                        findRigidbodyObject(
                            *right.gameObject);
                if (left.gameObject == right.gameObject
                    || (leftBody != nullptr
                        && rightBody != nullptr
                        && leftPhysicsObject
                            == rightPhysicsObject)
                    || CollisionSuppressedByJoint(
                        *leftPhysicsObject,
                        *rightPhysicsObject)
                    || (left.collisionMask
                        & (1u << right.layer)) == 0
                    || (right.collisionMask
                        & (1u << left.layer)) == 0
                    // プロジェクト設定の衝突マトリクス
                    // （既定は全部当たる）。
                    || !LayersCanCollide(
                        left.layer,
                        right.layer))
                {
                    continue;
                }

                if (collisionIteration == 0)
                {
                    ++stepStats
                        .narrowPhaseTestCount3D;
                }

                // メッシュコライダーは三角形単位で接触を生成し、
                // 1三角形ごとにprocessContactを呼びます（コール
                // バックはキー単位で重複抑制済み）。
                if (left.mesh != nullptr
                    || right.mesh != nullptr)
                {
                    // 静的メッシュ同士は解決不要です。
                    if (left.mesh != nullptr
                        && right.mesh != nullptr)
                    {
                        continue;
                    }
                    const bool meshIsLeft =
                        left.mesh != nullptr;
                    auto& meshEntry =
                        meshIsLeft ? left : right;
                    auto& otherEntry =
                        meshIsLeft ? right : left;

                    constexpr float queryMargin = 0.05f;
                    Bounds3D query = otherEntry.bounds;
                    query.minimum.x -= queryMargin;
                    query.minimum.y -= queryMargin;
                    query.minimum.z -= queryMargin;
                    query.maximum.x += queryMargin;
                    query.maximum.y += queryMargin;
                    query.maximum.z += queryMargin;
                    std::vector<MeshColliderTriangle>
                        triangles;
                    meshEntry.mesh->CollectTriangles(
                        query,
                        triangles);

                    constexpr std::size_t
                        MaximumTriangleContacts = 8;
                    std::size_t triangleContacts{};
                    for (const auto& triangle : triangles)
                    {
                        ConvexHull3D triangleHull{
                            {
                                triangle.a,
                                triangle.b,
                                triangle.c
                            } };
                        std::optional<Contact3D> single;
                        if (otherEntry.box != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.box
                                    ->WorldBox());
                        }
                        else if (otherEntry.capsule
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.capsule
                                    ->WorldCapsule());
                        }
                        else if (otherEntry.sphere
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.sphere
                                    ->WorldSphere());
                        }
                        else if (otherEntry.hull
                            != nullptr)
                        {
                            single = LamaPon::Intersect(
                                triangleHull,
                                otherEntry.hull
                                    ->WorldHull());
                        }
                        if (!single)
                        {
                            continue;
                        }
                        // 法線は第1引数（三角形＝メッシュ側）を
                        // 向くため、leftへ向くよう調整します。
                        const float normalScale =
                            meshIsLeft ? 1.0f : -1.0f;
                        const Contact meshContact{
                            {
                                single->normal.x
                                    * normalScale,
                                single->normal.y
                                    * normalScale,
                                single->normal.z
                                    * normalScale
                            },
                            single->penetration,
                            single->point
                        };
                        processContact(
                            *left.gameObject,
                            *right.gameObject,
                            meshContact,
                            true,
                            left.isTrigger
                                || right.isTrigger,
                            left.material,
                            right.material);
                        if (++triangleContacts
                            >= MaximumTriangleContacts)
                        {
                            break;
                        }
                    }
                    continue;
                }

                std::optional<Contact> contact;
                if (left.box != nullptr
                    && right.box != nullptr)
                {
                    if (const auto manifold =
                        LamaPon::IntersectManifold(
                        left.box->WorldBox(),
                        right.box->WorldBox()))
                    {
                        DirectX::XMFLOAT3 average{};
                        for (std::size_t index{};
                            index < manifold->pointCount;
                            ++index)
                        {
                            average.x +=
                                manifold->points[index].x;
                            average.y +=
                                manifold->points[index].y;
                            average.z +=
                                manifold->points[index].z;
                        }
                        const float scale =
                            1.0f / static_cast<float>(
                                std::max<std::size_t>(
                                    manifold->pointCount,
                                    1));
                        average.x *= scale;
                        average.y *= scale;
                        average.z *= scale;
                        contact = Contact{
                            manifold->normal,
                            manifold->penetration,
                            average,
                            manifold->points,
                            manifold->pointCount
                        };
                    }
                }
                else if (
                    (left.capsule != nullptr
                        && right.box != nullptr)
                    || (left.box != nullptr
                        && right.capsule != nullptr))
                {
                    // カプセル×ボックスは両端球の2点マニフォールド。
                    const bool capsuleIsLeft =
                        left.capsule != nullptr;
                    const auto manifold =
                        LamaPon::IntersectManifold(
                            (capsuleIsLeft
                                ? left.capsule
                                : right.capsule)
                                ->WorldCapsule(),
                            (capsuleIsLeft
                                ? right.box
                                : left.box)
                                ->WorldBox());
                    if (manifold)
                    {
                        // 法線はカプセル側を向くため、leftへ
                        // 向くように調整します。
                        const float normalScale =
                            capsuleIsLeft ? 1.0f : -1.0f;
                        DirectX::XMFLOAT3 average{};
                        for (std::size_t index{};
                            index < manifold->pointCount;
                            ++index)
                        {
                            average.x +=
                                manifold->points[index].x;
                            average.y +=
                                manifold->points[index].y;
                            average.z +=
                                manifold->points[index].z;
                        }
                        const float scale =
                            1.0f / static_cast<float>(
                                std::max<std::size_t>(
                                    manifold->pointCount,
                                    1));
                        average.x *= scale;
                        average.y *= scale;
                        average.z *= scale;
                        contact = Contact{
                            {
                                manifold->normal.x
                                    * normalScale,
                                manifold->normal.y
                                    * normalScale,
                                manifold->normal.z
                                    * normalScale
                            },
                            manifold->penetration,
                            average,
                            manifold->points,
                            manifold->pointCount
                        };
                    }
                }
                else
                {
                    std::optional<Contact3D> single;
                    bool reverseNormal{};
                    if (left.capsule != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                        left.capsule->WorldCapsule(),
                        right.capsule
                            ->WorldCapsule());
                    }
                    else if (left.sphere != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.sphere
                                ->WorldSphere());
                    }
                    else if (left.sphere != nullptr
                        && right.box != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.box->WorldBox());
                    }
                    else if (left.box != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.sphere
                                ->WorldSphere(),
                            left.box->WorldBox());
                        reverseNormal = true;
                    }
                    else if (left.sphere != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.sphere->WorldSphere(),
                            right.capsule
                                ->WorldCapsule());
                    }
                    else if (left.capsule != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.sphere
                                ->WorldSphere(),
                            left.capsule
                                ->WorldCapsule());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.hull->WorldHull());
                    }
                    else if (left.hull != nullptr
                        && right.box != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.box->WorldBox());
                    }
                    else if (left.box != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.box->WorldBox());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.capsule != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.capsule
                                ->WorldCapsule());
                    }
                    else if (left.capsule != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.capsule
                                ->WorldCapsule());
                        reverseNormal = true;
                    }
                    else if (left.hull != nullptr
                        && right.sphere != nullptr)
                    {
                        single = LamaPon::Intersect(
                            left.hull->WorldHull(),
                            right.sphere->WorldSphere());
                    }
                    else if (left.sphere != nullptr
                        && right.hull != nullptr)
                    {
                        single = LamaPon::Intersect(
                            right.hull->WorldHull(),
                            left.sphere->WorldSphere());
                        reverseNormal = true;
                    }
                    if (single)
                    {
                        const float normalScale =
                            reverseNormal
                            ? -1.0f
                            : 1.0f;
                        contact = Contact{
                            {
                                single->normal.x
                                    * normalScale,
                                single->normal.y
                                    * normalScale,
                                single->normal.z
                                    * normalScale
                            },
                            single->penetration,
                            single->point
                        };
                    }
                }
                if (contact)
                {
                    processContact(
                        *left.gameObject,
                        *right.gameObject,
                        *contact,
                        true,
                        left.isTrigger
                            || right.isTrigger,
                        left.material,
                        right.material);
                }
            }

            for (const auto& pair :
                broadPhase2D.pairs)
            {
                auto& left = entries2D[pair.left];
                auto& right = entries2D[pair.right];
                const auto [leftPhysicsObject, leftBody] =
                    findRigidbodyObject(
                        *left.gameObject);
                const auto [
                    rightPhysicsObject,
                    rightBody] =
                        findRigidbodyObject(
                            *right.gameObject);
                if (left.gameObject == right.gameObject
                    || (leftBody != nullptr
                        && rightBody != nullptr
                        && leftPhysicsObject
                            == rightPhysicsObject)
                    || CollisionSuppressedByJoint(
                        *leftPhysicsObject,
                        *rightPhysicsObject)
                    || (left.collisionMask
                        & (1u << right.layer)) == 0
                    || (right.collisionMask
                        & (1u << left.layer)) == 0
                    // プロジェクト設定の衝突マトリクス
                    // （既定は全部当たる）。
                    || !LayersCanCollide(
                        left.layer,
                        right.layer))
                {
                    continue;
                }

                if (collisionIteration == 0)
                {
                    ++stepStats
                        .narrowPhaseTestCount2D;
                }
                std::optional<Contact> contact2D;
                if (left.box != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectBounds(
                        left.box->WorldBounds(),
                        right.box->WorldBounds());
                }
                else if (left.circle != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectCircles(
                        left.circle->WorldCircle(),
                        right.circle->WorldCircle());
                }
                else if (left.circle != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectCircleBounds(
                        left.circle->WorldCircle(),
                        right.box->WorldBounds());
                }
                else if (left.box != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectCircleBounds(
                        right.circle->WorldCircle(),
                        left.box->WorldBounds());
                    if (contact2D)
                    {
                        // 法線を left 側へ向け直します。
                        contact2D->normal.x =
                            -contact2D->normal.x;
                        contact2D->normal.y =
                            -contact2D->normal.y;
                    }
                }
                else if (left.polygon != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygons(
                        left.polygon->WorldPolygon().vertices,
                        right.polygon->WorldPolygon().vertices);
                }
                else if (left.polygon != nullptr
                    && right.circle != nullptr)
                {
                    contact2D = IntersectPolygonCircle(
                        left.polygon->WorldPolygon().vertices,
                        right.circle->WorldCircle());
                }
                else if (left.circle != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygonCircle(
                        right.polygon->WorldPolygon().vertices,
                        left.circle->WorldCircle());
                    if (contact2D)
                    {
                        // 法線を left 側へ向け直します。
                        contact2D->normal.x =
                            -contact2D->normal.x;
                        contact2D->normal.y =
                            -contact2D->normal.y;
                    }
                }
                else if (left.polygon != nullptr
                    && right.box != nullptr)
                {
                    contact2D = IntersectPolygons(
                        left.polygon->WorldPolygon().vertices,
                        BoxBoundsToPolygon(
                            right.box->WorldBounds()));
                }
                else if (left.box != nullptr
                    && right.polygon != nullptr)
                {
                    contact2D = IntersectPolygons(
                        BoxBoundsToPolygon(
                            left.box->WorldBounds()),
                        right.polygon->WorldPolygon().vertices);
                }
                if (contact2D)
                {
                    processContact(
                        *left.gameObject,
                        *right.gameObject,
                        *contact2D,
                        false,
                        left.isTrigger
                            || right.isTrigger,
                        left.material,
                        right.material);
                }
            }
        }
            aggregateStats.colliderCount2D =
                stepStats.colliderCount2D;
            aggregateStats.colliderCount3D =
                stepStats.colliderCount3D;
            aggregateStats.candidatePairCount2D +=
                stepStats.candidatePairCount2D;
            aggregateStats.candidatePairCount3D +=
                stepStats.candidatePairCount3D;
            aggregateStats.narrowPhaseTestCount2D +=
                stepStats.narrowPhaseTestCount2D;
            aggregateStats.narrowPhaseTestCount3D +=
                stepStats.narrowPhaseTestCount3D;
            aggregateStats.occupiedCellCount2D =
                stepStats.occupiedCellCount2D;
            aggregateStats.occupiedCellCount3D =
                stepStats.occupiedCellCount3D;
            aggregateStats.oversizedColliderCount2D =
                stepStats.oversizedColliderCount2D;
            aggregateStats.oversizedColliderCount3D =
                stepStats.oversizedColliderCount3D;
        }

        for (const auto& gameObject : m_gameObjects)
        {
            if (auto* rigidbody =
                    gameObject->GetComponent<RigidbodyComponent>();
                rigidbody != nullptr)
            {
                rigidbody->UpdateSleepState(
                    deltaTime,
                    supportedBodies.contains(
                        gameObject->Id()));
                rigidbody->ClearAccumulators();
            }
        }

        for (const auto& [key, wasTrigger] : m_activeCollisions)
        {
            if (currentCollisions.contains(key))
            {
                continue;
            }

            const auto [leftId, rightId, is3D] = key;
            static_cast<void>(is3D);
            auto* left = FindGameObject(leftId);
            auto* right = FindGameObject(rightId);
            if (left != nullptr && right != nullptr)
            {
                left->NotifyCollisionExit(*right, wasTrigger);
                right->NotifyCollisionExit(*left, wasTrigger);
            }
        }

        m_activeCollisions = std::move(currentCollisions);
        m_physicsStats = aggregateStats;
        m_physicsStats.activeContactCount =
            m_activeCollisions.size();
    }

    void Scene::Render()
    {
        RenderTargetTextures();
        RenderMainCamera(m_graphics.AspectRatio(), true);
    }

    void Scene::RenderTargetTextures()
    {
        // 再入（レンダーテクスチャ描画中の再呼び出し）は無視します。
        if (m_renderingTargetTextures)
        {
            return;
        }
        // エディターはRenderMainCameraを通らないフレームがあるので
        // （Scene Viewだけの表示）、ここでもベイクを拾います。
        BakePendingReflectionProbes();
        ProcessBakedGlobalIlluminationBake();

        std::vector<CameraComponent*> targetCameras;
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            auto* camera =
                gameObject->GetComponent<CameraComponent>();
            if (camera == nullptr
                || !camera->IsEnabled()
                || !camera->RendersToTexture())
            {
                continue;
            }
            targetCameras.push_back(camera);
        }
        if (targetCameras.empty())
        {
            return;
        }

        const BooleanStateScope targetScope{
            m_renderingTargetTextures,
            true
        };
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        // 描画先とUI基準サイズを書き換えるので、元の値へ戻します。
        // 復元を忘れると、この後のメインカメラの描画が最後の
        // レンダーテクスチャへ流れ込みます。
        const std::uint32_t previousUIWidth =
            m_graphics.UIWidth();
        const std::uint32_t previousUIHeight =
            m_graphics.UIHeight();
        auto* context = m_graphics.Context();
        Microsoft::WRL::ComPtr<
            ID3D11RenderTargetView> previousTargetView;
        Microsoft::WRL::ComPtr<
            ID3D11DepthStencilView> previousDepthView;
        context->OMGetRenderTargets(
            1,
            previousTargetView.GetAddressOf(),
            previousDepthView.GetAddressOf());
        UINT previousViewportCount = 1;
        D3D11_VIEWPORT previousViewport{};
        context->RSGetViewports(
            &previousViewportCount,
            &previousViewport);

        m_graphics.Gpu().BeginSection(
            "レンダーテクスチャ");
        for (auto* camera : targetCameras)
        {
            auto& target =
                m_graphics.AcquireRenderTexture(
                    camera->TargetTexture(),
                    camera->TargetTextureWidth(),
                    camera->TargetTextureHeight());
            if (!target.IsValid())
            {
                Logger::Instance().Warning(
                    "レンダーテクスチャを作成できませんでした: "
                    + camera->TargetTexture());
                continue;
            }

            m_graphics.SetUIViewportSize(
                target.Width(),
                target.Height());
            target.Bind(m_graphics.Context());
            const auto& clearColor =
                camera->TargetClearColor();
            const float clear[4]{
                clearColor.x,
                clearColor.y,
                clearColor.z,
                clearColor.w
            };
            target.Clear(
                m_graphics.Context(),
                clear);
            RenderWithMatrices(
                camera->ViewMatrix(),
                camera->ProjectionMatrix(
                    target.AspectRatio()),
                false,
                false,
                &target);

            RunPostProcess(
                m_graphics,
                target,
                PostProcessFrameData());
            // ポスト処理でテクスチャを入れ替えるため、表示用へ
            // コピーしてから参照側に渡します。
            target.CopyToDisplay(
                m_graphics.Context());
        }
        m_graphics.Gpu().EndSection();

        m_graphics.SetUIViewportSize(
            previousUIWidth,
            previousUIHeight);
        context->OMSetRenderTargets(
            1,
            previousTargetView.GetAddressOf(),
            previousDepthView.Get());
        if (previousViewportCount > 0)
        {
            context->RSSetViewports(
                1,
                &previousViewport);
        }
    }

    void Scene::RenderMainCamera(
        const float aspectRatio,
        const bool include2D,
        RenderTarget* target)
    {
        // ベイク待ちのプローブはフレームの頭で焼きます（この後の
        // 描画からすぐ反射に使えるように）。
        BakePendingReflectionProbes();
        ProcessBakedGlobalIlluminationBake();
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        if (m_mainCamera != nullptr && m_mainCamera->IsEnabled())
        {
            const auto view = m_mainCamera->ViewMatrix();
            const auto projection = m_mainCamera->ProjectionMatrix(aspectRatio);
            RenderWithMatrices(
                view,
                projection,
                include2D,
                false,
                target);
            return;
        }

        if (!include2D)
        {
            return;
        }

        Render2D();
    }

    void Scene::Render2D()
    {
        // UIは3D用ポストエフェクトの後から単独で描画できるよう分離する。
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        m_graphics.Gpu().BeginSection("2D／UI");
        auto& spriteBatch = m_graphics.BeginSprites();
        RenderSprites2D(
            m_gameObjects,
            m_graphics,
            spriteBatch,
            m_graphics.WhiteTexture());
        m_graphics.EndSprites();
        m_graphics.Gpu().EndSection();
    }

    void Scene::RenderWithMatrices(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX rawProjection,
        const bool include2D,
        const bool renderDebug,
        RenderTarget* target)
    {
        // TAAのサブピクセルずらし。深度プリパスも本描画も同じ行列で
        // なければ噛み合わないので、ここで1回だけ織り込みます。
        // プローブのベイク中はずらしません（キューブへ揺れが焼き
        // 付きます）。
        m_temporalFrame = {};
        const bool temporalWanted =
            m_temporalAntiAliasing.enabled
            && target != nullptr
            && !m_bakingReflectionProbes;
        DirectX::XMMATRIX projection = rawProjection;
        if (temporalWanted)
        {
            const auto jitter = TemporalJitterOffset(
                m_temporalFrameIndex);
            projection = ApplyTemporalJitter(
                rawProjection,
                DirectX::XMFLOAT2{
                    jitter.x
                        * m_temporalAntiAliasing
                            .jitterScale,
                    jitter.y
                        * m_temporalAntiAliasing
                            .jitterScale },
                target->Width(),
                target->Height());
            ++m_temporalFrameIndex;
        }

        // SSAOが深度をビュー空間へ戻すのに使うため、今回の射影を
        // 記録します（ゲーム実行時のポスト処理が参照します）。
        DirectX::XMFLOAT4X4 storedProjection{};
        DirectX::XMStoreFloat4x4(
            &storedProjection,
            projection);
        m_graphics.SetSceneProjection(storedProjection);
        // 被写界深度へ渡す情報。ビューごとに射影が違う（エディターは
        // Scene Viewとカメラプレビューを同じフレームで描く）ので、
        // 共有せずにここで詰め直します。ずらしを含んだ側の行列を
        // 渡すのが要点で、深度バッファはこの行列で書かれています。
        // ベイク中はぼかしません（プローブのキューブへぼけが焼き
        // 付くと、反射だけ二重にぼけます）。
        m_depthOfFieldFrame = {};
        if (!m_bakingReflectionProbes)
        {
            m_depthOfFieldFrame.settings = m_depthOfField;
            m_depthOfFieldFrame.projection = storedProjection;
        }
        m_screenOutlineFrame = {};
        if (target != nullptr && !m_bakingReflectionProbes)
        {
            m_screenOutlineFrame.projection = storedProjection;
        }
        const BooleanStateScope interpolationScope{
            m_renderingInterpolatedTransforms,
            true
        };
        // プローブのベイク中はカリングを使いません。128pxの
        // 一回きりの描画なので全描画で構わず、視錐台・遮蔽の
        // 取りこぼしがベイク結果に焼き付く事故も避けられます。
        const auto visibility = m_bakingReflectionProbes
            ? VisibilityResult{}
            : BuildRenderVisibility(view, projection);
        if (!m_bakingReflectionProbes)
        {
            m_visibilityStats = visibility.stats;
        }

        // 今フレームで使えるリフレクションプローブを集めます
        // （描画側がNearestReflectionProbeで引きます）。
        // ベイク中の再帰描画では空にして映り込みを防ぎます。
        // 前フレームの影テクスチャを持ち越さないよう毎回消します
        // （影が無いフレームで古いSRVを読むと筋が残ります）。
        m_volumetricFrame = {};
        m_frameReflectionProbes.clear();
        if (!m_bakingReflectionProbes)
        {
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                auto* probe = gameObject->GetComponent<
                    ReflectionProbeComponent>();
                if (probe != nullptr
                    && probe->IsEnabled()
                    && probe->IsBaked())
                {
                    m_frameReflectionProbes.push_back(
                        probe);
                }
            }
        }

        auto lighting = BuildLightingState();

        const DirectionalLightComponent* shadowLight{};
        std::size_t shadowLightIndex{};
        std::size_t directionalIndex{};
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            const auto* candidate =
                gameObject->GetComponent<
                    DirectionalLightComponent>();
            if (candidate == nullptr
                || !candidate->IsEnabled())
            {
                continue;
            }
            if (directionalIndex
                == MaximumDirectionalLights)
            {
                break;
            }
            if (candidate->CastsShadows())
            {
                shadowLight = candidate;
                shadowLightIndex = directionalIndex;
                break;
            }
            ++directionalIndex;
        }

        // シャドウマップ描画中は深度専用パスに切り替えます
        // （例外時も確実に解除するためRAIIで管理）。
        // 深度専用パスのRAII。GPU区間の計測も兼ねます。
        struct DepthOnlyPassScope final
        {
            GraphicsDevice& graphics;
            DepthOnlyPassScope(
                GraphicsDevice& device,
                const char* gpuSectionName) noexcept
                : graphics(device)
            {
                graphics.SetDepthPass(
                    DepthPassKind::Shadow);
                graphics.Gpu().BeginSection(
                    gpuSectionName);
            }
            ~DepthOnlyPassScope() noexcept
            {
                graphics.Gpu().EndSection();
                graphics.SetDepthPass(
                    DepthPassKind::None);
            }
        };

        if (m_graphics.Settings().shadowsEnabled
            && shadowLight != nullptr
            && m_graphics.Shadows().IsValid())
        {
            const DepthOnlyPassScope depthScope{
                m_graphics,
                "影(カスケード)" };
            const auto shadowMatrices =
                BuildDirectionalShadowMatrices(
                    view,
                    projection,
                    shadowLight->WorldDirection(),
                    shadowLight->ShadowDistance(),
                    std::min({
                        static_cast<std::size_t>(
                            shadowLight->
                                ShadowCascadeCount()),
                        static_cast<std::size_t>(
                            m_graphics.Settings().
                                shadowCascadeLimit),
                        static_cast<std::size_t>(
                            m_graphics.Shadows().
                                CascadeCount())
                    }),
                    shadowLight->ShadowSplitLambda(),
                    m_graphics.Shadows().Resolution());

            auto shadowPassLighting = lighting;
            shadowPassLighting.directionalShadow.enabled =
                false;
            shadowPassLighting.directionalShadow.texture =
                nullptr;
            m_graphics.SetLightingState(
                shadowPassLighting);

            auto& shadowMap = m_graphics.Shadows();
            for (std::size_t cascadeIndex = 0;
                cascadeIndex < shadowMatrices.count;
                ++cascadeIndex)
            {
                const auto& cascade =
                    shadowMatrices.cascades[cascadeIndex];
                const auto shadowHidden =
                    BuildShadowFrustumHidden(
                        cascade.view,
                        cascade.projection,
                        visibility.lodHidden);
                shadowMap.Begin(
                    m_graphics.Context(),
                    static_cast<std::uint32_t>(
                        cascadeIndex));
                try
                {
                    for (const auto& gameObject : m_gameObjects)
                    {
                        if (visibility.lodHidden.contains(
                                gameObject->Id())
                            || shadowHidden.contains(
                                gameObject->Id()))
                        {
                            continue;
                        }
                        gameObject->Render3D(
                            m_graphics,
                            cascade.view,
                            cascade.projection);
                    }
                }
                catch (...)
                {
                    shadowMap.End(m_graphics.Context());
                    throw;
                }
                shadowMap.End(m_graphics.Context());
            }

            auto& shadow =
                lighting.directionalShadow;
            shadow.cascadeCount =
                shadowMatrices.count;
            for (std::size_t cascadeIndex = 0;
                cascadeIndex < shadowMatrices.count;
                ++cascadeIndex)
            {
                const auto& cascade =
                    shadowMatrices.cascades[cascadeIndex];
                DirectX::XMStoreFloat4x4(
                    &shadow.lightViewProjections[
                        cascadeIndex],
                    cascade.view
                        * cascade.projection);
                shadow.cascadeSplits[cascadeIndex] =
                    cascade.splitDistance;
            }
            shadow.texture =
                shadowMap.ShaderResourceView();
            shadow.lightIndex = shadowLightIndex;
            shadow.bias = shadowLight->ShadowBias();
            shadow.normalBias =
                shadowLight->ShadowNormalBias();
            shadow.strength =
                shadowLight->ShadowStrength();
            shadow.enabled = true;

            // ボリュメトリックライト（光の筋）へ渡す情報。カスケードの
            // 行列と光の向きが揃うのはここだけなので、この場で組み立てて
            // ポスト処理まで持って行きます。ベイク中は作りません
            // （プローブのキューブに光の筋を焼き込まないため）。
            if (m_volumetricLight.enabled
                && !m_bakingReflectionProbes)
            {
                auto& frame = m_volumetricFrame;
                frame.settings = m_volumetricLight;
                // 届く距離は影のカスケードが覆う範囲までに抑えます。
                // シェーダーはカスケードの外を「光が当たっている」と
                // みなすため、影の距離（既定24m）より遠くまで進めると
                // 遮蔽の判定が無い区間がそのまま光として積まれ、
                // 屋内でも画面が白く霞みます。
                frame.settings.maximumDistance = std::min(
                    frame.settings.maximumDistance,
                    shadowLight->ShadowDistance());
                auto& inputs = frame.inputs;
                inputs.cascadeShadow = shadow.texture;
                inputs.cascadeCount =
                    static_cast<std::uint32_t>(
                        shadowMatrices.count);
                inputs.shadowBias =
                    shadowLight->ShadowBias();
                inputs.shadowResolution =
                    lighting.directionalShadowResolution;
                inputs.lightDirection =
                    shadowLight->WorldDirection();
                // 太陽を明るくしたら筋も明るくなるよう、光源の
                // 強度を掛けておきます。設定側のintensityは
                // 「空気の濃さ」としてこの上に乗ります。
                const auto& lightColor =
                    shadowLight->Color();
                const float lightIntensity =
                    shadowLight->Intensity();
                inputs.lightColor = {
                    lightColor.x * lightIntensity,
                    lightColor.y * lightIntensity,
                    lightColor.z * lightIntensity
                };
                for (std::size_t cascadeIndex = 0;
                    cascadeIndex < shadowMatrices.count
                        && cascadeIndex < 4;
                    ++cascadeIndex)
                {
                    inputs.cascadeViewProjections[
                        cascadeIndex] =
                        shadow.lightViewProjections[
                            cascadeIndex];
                }
                DirectX::XMVECTOR determinant{};
                const auto inverseViewProjection =
                    DirectX::XMMatrixInverse(
                        &determinant,
                        view * projection);
                DirectX::XMStoreFloat4x4(
                    &inputs.inverseViewProjection,
                    inverseViewProjection);
                const auto inverseView =
                    DirectX::XMMatrixInverse(
                        &determinant,
                        view);
                DirectX::XMStoreFloat3(
                    &inputs.cameraPosition,
                    inverseView.r[3]);
            }
        }

        // スポットライトの影パス（最大MaximumSpotShadows灯）。
        if (m_graphics.Settings().shadowsEnabled
            && m_graphics.SpotShadows().IsValid())
        {
            struct SpotShadowCandidate final
            {
                const SpotLightComponent* light{};
                std::size_t lightIndex{};
            };
            std::array<
                SpotShadowCandidate,
                MaximumSpotShadows> candidates{};
            std::size_t candidateCount = 0;
            std::size_t spotIndex = 0;
            const std::size_t spotLimit =
                std::min<std::size_t>(
                    m_graphics.Settings().spotLightLimit,
                    MaximumSpotLights);
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                const auto* candidate =
                    gameObject->GetComponent<
                        SpotLightComponent>();
                if (candidate == nullptr
                    || !candidate->IsEnabled())
                {
                    continue;
                }
                if (spotIndex >= spotLimit)
                {
                    break;
                }
                if (candidate->CastsShadows()
                    && candidateCount
                        < MaximumSpotShadows)
                {
                    candidates[candidateCount++] = {
                        candidate,
                        spotIndex
                    };
                }
                ++spotIndex;
            }

            if (candidateCount > 0)
            {
                const DepthOnlyPassScope depthScope{
                    m_graphics,
                    "影(スポット)" };
                m_graphics.SetLightingState(lighting);
                auto& spotShadowMap =
                    m_graphics.SpotShadows();
                for (std::size_t slot = 0;
                    slot < candidateCount;
                    ++slot)
                {
                    const auto* light =
                        candidates[slot].light;
                    const auto eye =
                        light->WorldPosition();
                    const auto direction =
                        light->WorldDirection();
                    const auto up =
                        std::abs(direction.y) > 0.99f
                        ? DirectX::XMFLOAT3{
                            0.0f, 0.0f, 1.0f }
                        : DirectX::XMFLOAT3{
                            0.0f, 1.0f, 0.0f };
                    const auto shadowView =
                        DirectX::XMMatrixLookToRH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &direction),
                            DirectX::XMLoadFloat3(&up));
                    const float fieldOfView = std::clamp(
                        light->OuterConeAngle() * 2.0f,
                        DirectX::XMConvertToRadians(5.0f),
                        DirectX::XMConvertToRadians(
                            170.0f));
                    const auto shadowProjection =
                        DirectX::
                            XMMatrixPerspectiveFovRH(
                                fieldOfView,
                                1.0f,
                                0.1f,
                                std::max(
                                    light->Range(),
                                    0.2f));
                    const auto shadowHidden =
                        BuildShadowFrustumHidden(
                            shadowView,
                            shadowProjection,
                            visibility.lodHidden);

                    spotShadowMap.Begin(
                        m_graphics.Context(),
                        static_cast<std::uint32_t>(slot));
                    try
                    {
                        for (const auto& gameObject :
                            m_gameObjects)
                        {
                            if (visibility.lodHidden
                                    .contains(
                                        gameObject->Id())
                                || shadowHidden.contains(
                                    gameObject->Id()))
                            {
                                continue;
                            }
                            gameObject->Render3D(
                                m_graphics,
                                shadowView,
                                shadowProjection);
                        }
                    }
                    catch (...)
                    {
                        spotShadowMap.End(
                            m_graphics.Context());
                        throw;
                    }
                    spotShadowMap.End(
                        m_graphics.Context());

                    auto& destination =
                        lighting.spotShadows[slot];
                    DirectX::XMStoreFloat4x4(
                        &destination.lightViewProjection,
                        shadowView * shadowProjection);
                    destination.lightIndex =
                        static_cast<std::ptrdiff_t>(
                            candidates[slot].lightIndex);
                    destination.bias =
                        light->ShadowBias();
                    destination.normalBias =
                        light->ShadowNormalBias();
                    destination.strength =
                        light->ShadowStrength();
                    destination.enabled = true;
                }
                lighting.spotShadowTexture =
                    spotShadowMap.ShaderResourceView();
            }
        }

        // ポイントライトの影パス（最初の1灯、キューブ6面）。
        if (m_graphics.Settings().shadowsEnabled
            && m_graphics.PointShadows().IsValid())
        {
            const PointLightComponent* pointShadowLight{};
            std::size_t pointShadowIndex{};
            std::size_t pointIndex = 0;
            const std::size_t pointLimit =
                std::min<std::size_t>(
                    m_graphics.Settings().pointLightLimit,
                    MaximumPointLights);
            for (const auto& gameObject : m_gameObjects)
            {
                if (!gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                const auto* candidate =
                    gameObject->GetComponent<
                        PointLightComponent>();
                if (candidate == nullptr
                    || !candidate->IsEnabled())
                {
                    continue;
                }
                if (pointIndex >= pointLimit)
                {
                    break;
                }
                if (candidate->CastsShadows())
                {
                    pointShadowLight = candidate;
                    pointShadowIndex = pointIndex;
                    break;
                }
                ++pointIndex;
            }

            if (pointShadowLight != nullptr)
            {
                const DepthOnlyPassScope depthScope{
                    m_graphics,
                    "影(ポイント)" };
                m_graphics.SetLightingState(lighting);
                auto& pointShadowMap =
                    m_graphics.PointShadows();
                const auto eye =
                    pointShadowLight->WorldPosition();
                const float farPlane = std::max(
                    pointShadowLight->Range(),
                    0.2f);
                // D3D標準のキューブ面向き（左手系）。シェーダー側の
                // 深度復元式（EvaluatePointShadow）と対になります。
                static constexpr DirectX::XMFLOAT3
                    FaceDirections[6]{
                        { 1.0f, 0.0f, 0.0f },
                        { -1.0f, 0.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, -1.0f, 0.0f },
                        { 0.0f, 0.0f, 1.0f },
                        { 0.0f, 0.0f, -1.0f }
                    };
                static constexpr DirectX::XMFLOAT3
                    FaceUps[6]{
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 0.0f, -1.0f },
                        { 0.0f, 0.0f, 1.0f },
                        { 0.0f, 1.0f, 0.0f },
                        { 0.0f, 1.0f, 0.0f }
                    };
                const auto faceProjection =
                    DirectX::XMMatrixPerspectiveFovLH(
                        DirectX::XM_PIDIV2,
                        1.0f,
                        0.1f,
                        farPlane);
                for (std::uint32_t face = 0;
                    face < 6u;
                    ++face)
                {
                    const auto faceView =
                        DirectX::XMMatrixLookToLH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &FaceDirections[face]),
                            DirectX::XMLoadFloat3(
                                &FaceUps[face]));
                    const auto shadowHidden =
                        BuildShadowFrustumHidden(
                            faceView,
                            faceProjection,
                            visibility.lodHidden);
                    pointShadowMap.Begin(
                        m_graphics.Context(),
                        face);
                    try
                    {
                        for (const auto& gameObject :
                            m_gameObjects)
                        {
                            if (visibility.lodHidden
                                    .contains(
                                        gameObject->Id())
                                || shadowHidden.contains(
                                    gameObject->Id()))
                            {
                                continue;
                            }
                            gameObject->Render3D(
                                m_graphics,
                                faceView,
                                faceProjection);
                        }
                    }
                    catch (...)
                    {
                        pointShadowMap.End(
                            m_graphics.Context());
                        throw;
                    }
                    pointShadowMap.End(
                        m_graphics.Context());
                }
                auto& destination = lighting.pointShadow;
                destination.texture =
                    pointShadowMap.ShaderResourceView();
                destination.lightIndex =
                    static_cast<std::ptrdiff_t>(
                        pointShadowIndex);
                destination.bias =
                    pointShadowLight->ShadowBias();
                destination.strength =
                    pointShadowLight->ShadowStrength();
                destination.enabled = true;
            }
        }

        // 深度プリパス＋SSAO。ライティングの前に遮蔽を用意して、
        // Litシェーダーが環境光／IBL項にだけ掛けられるようにします
        // （完成した色へ掛けると直接光や影の中まで暗くなります）。
        if (target != nullptr)
        {
            const auto prepass = RunDepthPrepass(
                m_graphics,
                *target,
                storedProjection,
                m_ambientOcclusion,
                m_screenSpaceReflection,
                // 参照で捕まえます（XMMATRIXは16バイト境界を要求する
                // ため、std::functionへ値で載せたくありません）。
                // 呼ばれるのはこの関数の中だけなので安全です。
                [this, &visibility, &view, &projection]()
                {
                    for (const auto& gameObject :
                        m_gameObjects)
                    {
                        if (visibility.renderHidden
                                .contains(
                                    gameObject->Id()))
                        {
                            continue;
                        }
                        gameObject->Render3D(
                            m_graphics,
                            view,
                            projection);
                    }
                });
            if (prepass.ambientOcclusionResolved)
            {
                auto& occlusion =
                    lighting.screenAmbientOcclusion;
                occlusion.texture =
                    target->
                        AmbientOcclusionShaderResourceView();
                occlusion.inverseWidth = 1.0f
                    / static_cast<float>(
                        std::max(target->Width(), 1u));
                occlusion.inverseHeight = 1.0f
                    / static_cast<float>(
                        std::max(target->Height(), 1u));
                occlusion.enabled = true;
            }

            // SSR（画面空間反射）。前フレームのカラーがまだ無い
            // 最初のフレームは無効のままで、2フレーム目から効きます。
            if (prepass.depthAvailable
                && m_screenSpaceReflection.enabled
                && !m_bakingReflectionProbes)
            {
                auto* const history =
                    target->ColorHistoryShaderResourceView();
                if (history != nullptr)
                {
                    auto& reflection =
                        lighting.screenSpaceReflection;
                    reflection.texture = history;
                    // 深度はコピーを読みます（本体はDSVとして
                    // 刺さっているためSRVにできません）。
                    target->CaptureDepthForReflections(
                        m_graphics.Context());
                    // Hi-Z: 深度を「距離のminミップピラミッド」へ
                    // 直します。シェーダーはこれを読んで、何も無い
                    // 空間を大股で飛びます。
                    m_graphics.Environment()
                        .BuildReflectionDepthPyramid(
                            *target,
                            storedProjection._33,
                            storedProjection._43);
                    reflection.depth =
                        target->
                            ReflectionDepthPyramidShaderResourceView();
                    reflection.depthPyramidMaximumMip =
                        target->ReflectionDepthPyramidMipCount()
                            > 0
                        ? target
                            ->ReflectionDepthPyramidMipCount()
                            - 1
                        : 0;
                    reflection.previousViewProjection =
                        target->
                            ColorHistoryViewProjection();
                    reflection.inverseWidth = 1.0f
                        / static_cast<float>(
                            std::max(target->Width(), 1u));
                    reflection.inverseHeight = 1.0f
                        / static_cast<float>(
                            std::max(
                                target->Height(), 1u));
                    // 深度をビュー空間のZへ戻すための値。
                    reflection.projectionZ =
                        storedProjection._33;
                    reflection.projectionW =
                        storedProjection._43;
                    reflection.intensity =
                        m_screenSpaceReflection.intensity;
                    reflection.maximumDistance =
                        m_screenSpaceReflection
                            .maximumDistance;
                    reflection.thickness =
                        m_screenSpaceReflection.thickness;
                    reflection.roughnessCutoff =
                        m_screenSpaceReflection
                            .roughnessCutoff;
                    reflection.stepCount =
                        m_screenSpaceReflection.stepCount;
                    reflection.enabled = true;
                }
            }
            // プリパスとSSAOで描画先が変わっているので、カラーへ
            // 戻します（深度は消さずにそのまま使います）。
            target->Bind(m_graphics.Context());
        }

        // キューブマップスカイとIBL（環境反射）。
        ID3D11ShaderResourceView* skyCubemap{};
        if (m_sky.enabled
            && !m_sky.cubemapPath.empty()
            && m_graphics.IsInitialized())
        {
            try
            {
                if (const auto texture =
                        m_graphics.Assets().LoadTexture(
                            m_sky.cubemapPath);
                    texture != nullptr && texture->isCube)
                {
                    skyCubemap = texture->view.Get();
                }
            }
            catch (...)
            {
            }
        }
        lighting.environment.texture = skyCubemap;
        lighting.environment.intensity =
            m_sky.iblIntensity;
        lighting.environment.enabled =
            skyCubemap != nullptr
            && m_sky.iblIntensity > 0.0f;
        if (lighting.environment.enabled)
        {
            // ディスクキャッシュの鍵（キューブマップの内容ハッシュ）。
            // パスが変わったときだけ読み直して計算します。読めない
            // ときは0＝キャッシュなしで、従来どおり毎回生成します。
            if (m_sky.cubemapPath != m_skyPrefilterKeyPath)
            {
                m_skyPrefilterKeyPath = m_sky.cubemapPath;
                m_skyPrefilterKey = 0;
                try
                {
                    const auto cubemapBytes =
                        m_graphics.Assets().ReadFileBytes(
                            m_sky.cubemapPath);
                    m_skyPrefilterKey =
                        EnvironmentCache::HashBytes(
                            cubemapBytes);
                }
                catch (...)
                {
                }
            }
            // GGX事前畳み込み（初回のみ生成、以降はキャッシュ）。
            // 失敗時はソース直接サンプリングへフォールバック。
            try
            {
                const auto prefiltered =
                    m_graphics.Environment()
                        .GetPrefilteredEnvironment(
                            skyCubemap,
                            m_skyPrefilterKey);
                lighting.environment.specular =
                    prefiltered.specular;
                lighting.environment.irradiance =
                    prefiltered.irradiance;
                lighting.environment.specularMaximumMip =
                    prefiltered.specularMaximumMip;
            }
            catch (const std::exception&)
            {
            }
        }

        // ベイクした間接光（照度ボリューム）。焼いたときの形
        // （m_bakedGiBakedShape）で表示します——設定の格子数を後から
        // 変えても、次にベイクするまで見た目が壊れないように。
        // ベイク中は自分の焼きかけを読まないよう無効にします
        // （焼き込みは常に「GIなしの絵」から＝1バウンスで確定）。
        EnsureBakedGlobalIlluminationTextures();
        {
            auto& bakedGi = lighting.bakedGlobalIllumination;
            const bool bakedGiActive =
                m_bakedGiSettings.enabled
                && !m_bakingReflectionProbes
                && m_bakedGiViews[0] != nullptr
                && m_bakedGiViews[1] != nullptr
                && m_bakedGiViews[2] != nullptr;
            bakedGi.enabled = bakedGiActive;
            if (bakedGiActive)
            {
                const auto& shape = m_bakedGiBakedShape;
                bakedGi.redCoefficients =
                    m_bakedGiViews[0].Get();
                bakedGi.greenCoefficients =
                    m_bakedGiViews[1].Get();
                bakedGi.blueCoefficients =
                    m_bakedGiViews[2].Get();
                bakedGi.volumeMinimum = {
                    shape.center.x - shape.size.x * 0.5f,
                    shape.center.y - shape.size.y * 0.5f,
                    shape.center.z - shape.size.z * 0.5f
                };
                bakedGi.volumeSize = shape.size;
                bakedGi.resolution = {
                    static_cast<float>(shape.resolutionX),
                    static_cast<float>(shape.resolutionY),
                    static_cast<float>(shape.resolutionZ)
                };
                // 強さだけはベイクし直さずに効きます。
                bakedGi.intensity =
                    m_bakedGiSettings.intensity;
            }
        }

        // クラスタライトカリング（Forward+）。ライトの全量を
        // クラスタごとの番号表にして、Litシェーダーが自分のクラスタの
        // 分だけ計算できるようにします。
        //
        // Forwardを選んでいるときは番号表を作らずに飛ばします。
        // lighting.clusteredが空のままなので、Litシェーダーは
        // 従来の16灯経路（品質設定のポイント／スポット上限が効く方）
        // へ落ちます。Compute Shaderのディスパッチごと省けます。
        const bool clusteredRequested =
            m_graphics.Settings().renderingPath
            == RenderingPath::ForwardPlus;
        if (clusteredRequested
            && !lighting.clusteredLights.empty()
            && !m_clusteredLightingUnavailable)
        {
            // 影スロットをクラスタ側のライトへも書き込みます。
            // クラスタ配列はポイント→スポットの順で、それぞれ
            // 固定配列と同じ並びです（BuildLightingState参照）。
            std::ptrdiff_t spotOrdinal = 0;
            for (auto& clusteredLight :
                lighting.clusteredLights)
            {
                if (clusteredLight.extraParameters.y
                    < 0.5f)
                {
                    continue;
                }
                for (std::size_t slot = 0;
                    slot < MaximumSpotShadows;
                    ++slot)
                {
                    const auto& spotShadow =
                        lighting.spotShadows[slot];
                    if (spotShadow.enabled
                        && spotShadow.lightIndex
                            == spotOrdinal)
                    {
                        clusteredLight.extraParameters.z =
                            static_cast<float>(slot + 1);
                    }
                }
                ++spotOrdinal;
            }

            try
            {
                m_graphics.Gpu().BeginSection(
                    "ライトカリング");
                m_graphics.Clusters().Update(
                    m_graphics.Context(),
                    lighting,
                    view,
                    projection,
                    target != nullptr
                        ? target->Width()
                        : m_graphics.RenderWidth(),
                    target != nullptr
                        ? target->Height()
                        : m_graphics.RenderHeight());
                m_graphics.Gpu().EndSection();
            }
            catch (const std::exception& exception)
            {
                m_graphics.Gpu().EndSection();
                // シェーダーが無い・コンパイルできない環境では
                // 従来の16灯経路で描き続けます。毎フレーム
                // 例外を投げ直さないよう、一度きりで諦めます。
                m_clusteredLightingUnavailable = true;
                lighting.clustered = {};
                Logger::Instance().Warning(
                    std::string{
                        "クラスタライトカリングを初期化"
                        "できないため、従来のライト上限で"
                        "描画します: " }
                    + exception.what());
            }
        }

        m_graphics.SetLightingState(lighting);
        m_graphics.Gpu().BeginSection("スカイ");
        EnvironmentRenderer::SkySun skySun{};
        const bool hasSkySun =
            m_sky.sunDriven
            && ResolveSkySun(
                skySun.directionToSun,
                skySun.color,
                skySun.angularRadius);
        m_graphics.Environment().DrawSky(
            view,
            projection,
            ResolvedSky(),
            skyCubemap,
            hasSkySun ? &skySun : nullptr);
        m_graphics.Gpu().EndSection();
        m_graphics.Gpu().BeginSection("3D描画");

        m_visibilityStats.modelInstanceBatchCount = 0;
        m_visibilityStats.modelInstancedRendererCount = 0;
        m_visibilityStats.meshInstanceBatchCount = 0;
        m_visibilityStats.meshInstancedRendererCount = 0;

        // 同一形状・同一マテリアルのMeshRendererをまとめて
        // インスタンス描画します（2個以上のグループのみ）。
        std::unordered_map<
            std::uint64_t,
            std::vector<MeshRendererComponent*>>
            instanceBatches;
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy()
                || visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                continue;
            }
            auto* meshRenderer =
                gameObject->GetComponent<
                    MeshRendererComponent>();
            if (meshRenderer != nullptr
                && meshRenderer->IsEnabled()
                && meshRenderer->CanBeInstanced()
                // アルファ合成は1つずつ並べ替えて描くので、
                // まとめてしまうと順番を作れません。加算合成は
                // 順番によらないのでまとめたままです。
                && !meshRenderer->IsAlphaBlended3D())
            {
                instanceBatches[
                    meshRenderer->InstanceBatchKey()]
                    .push_back(meshRenderer);
            }
        }
        for (auto& [batchKey, batch] : instanceBatches)
        {
            static_cast<void>(batchKey);
            if (batch.size() < 2)
            {
                continue;
            }
            batch.front()->RenderInstancedBatch(
                batch,
                view,
                projection);
            ++m_visibilityStats.meshInstanceBatchCount;
            m_visibilityStats.meshInstancedRendererCount +=
                batch.size();
        }

        std::unordered_map<
            std::uint64_t,
            std::vector<ModelRendererComponent*>>
            modelInstanceBatches;
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy()
                || visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                continue;
            }
            auto* modelRenderer =
                gameObject->GetComponent<
                    ModelRendererComponent>();
            if (modelRenderer != nullptr
                && modelRenderer->IsEnabled()
                && modelRenderer->CanBeInstanced()
                && !modelRenderer->IsAlphaBlended3D())
            {
                modelInstanceBatches[
                    modelRenderer->InstanceBatchKey()]
                    .push_back(modelRenderer);
            }
        }
        for (auto& [batchKey, batch] : modelInstanceBatches)
        {
            static_cast<void>(batchKey);
            if (batch.size() >= 2
                && batch.front()->RenderInstancedBatch(
                    batch,
                    view,
                    projection))
            {
                ++m_visibilityStats.modelInstanceBatchCount;
                m_visibilityStats.modelInstancedRendererCount +=
                    batch.size();
            }
        }

        // アルファ合成のオブジェクトは、不透明を全部描き終えてから
        // カメラの遠い順に描きます。手前から描くと、後ろのものが
        // 先に置かれた深度に阻まれて消えるためです。
        //
        // 遮蔽用の事前パスを持つものはここへ入れません。あちらは
        // 「配列上で連続していること」を前提に組を作るので、
        // 抜き出すと組が崩れます。
        struct AlphaBlendedDraw final
        {
            GameObject* object;
            float distanceSquared;
        };
        std::vector<AlphaBlendedDraw> alphaBlendedDraws;
        DirectX::XMVECTOR cameraPosition =
            DirectX::XMMatrixInverse(nullptr, view).r[3];

        for (std::size_t objectIndex = 0;
            objectIndex < m_gameObjects.size();)
        {
            const auto& gameObject = m_gameObjects[objectIndex];
            if (visibility.renderHidden.contains(
                    gameObject->Id()))
            {
                ++objectIndex;
                continue;
            }

            if (!gameObject->HasPreRender3DPass(m_graphics))
            {
                if (gameObject->HasAlphaBlended3DPass(
                        m_graphics))
                {
                    // 親子関係を含んだ位置が要るのでWorldMatrixの
                    // 平行移動成分を使います（Transform::positionは
                    // 親から見た位置なので、入れ子のオブジェクトで
                    // 順番が狂います）。
                    const auto offset =
                        DirectX::XMVectorSubtract(
                            gameObject->WorldMatrix().r[3],
                            cameraPosition);
                    alphaBlendedDraws.push_back({
                        gameObject.get(),
                        DirectX::XMVectorGetX(
                            DirectX::XMVector3LengthSq(
                                offset))
                    });
                    ++objectIndex;
                    continue;
                }
                gameObject->Render3D(
                    m_graphics,
                    view,
                    projection);
                ++objectIndex;
                continue;
            }

            // 連続する遮蔽対象を先にまとめて描画し、その後に通常描画します。
            // 分割モデルが直前の自分自身を障害物として扱う自己遮蔽を防ぎます。
            std::vector<GameObject*> preRenderGroup;
            std::size_t groupEnd = objectIndex;
            for (; groupEnd < m_gameObjects.size(); ++groupEnd)
            {
                auto* candidate = m_gameObjects[groupEnd].get();
                if (visibility.renderHidden.contains(candidate->Id()))
                {
                    continue;
                }
                if (!candidate->HasPreRender3DPass(m_graphics))
                {
                    break;
                }
                preRenderGroup.push_back(candidate);
            }
            for (auto* candidate : preRenderGroup)
            {
                candidate->RenderPre3D(
                    m_graphics,
                    view,
                    projection);
            }
            for (auto* candidate : preRenderGroup)
            {
                candidate->Render3D(
                    m_graphics,
                    view,
                    projection);
            }
            objectIndex = groupEnd;
        }

        // 遠い順。同距離のときは元の並びを保つよう安定ソートに
        // します（毎フレーム順番が入れ替わるとちらつきます）。
        std::stable_sort(
            alphaBlendedDraws.begin(),
            alphaBlendedDraws.end(),
            [](const AlphaBlendedDraw& left,
                const AlphaBlendedDraw& right)
            {
                return left.distanceSquared
                    > right.distanceSquared;
            });
        for (const auto& draw : alphaBlendedDraws)
        {
            draw.object->Render3D(
                m_graphics,
                view,
                projection);
        }

        if (renderDebug)
        {
            for (const auto& gameObject : m_gameObjects)
            {
                gameObject->RenderDebug3D(m_graphics, view, projection);
            }
        }
        m_graphics.Gpu().EndSection();

        // TAAへ渡す行列。ワールド復元にはずらし込みの逆行列を使い
        // （深度と噛み合わせるため）、次フレームの参照用にはずらしを
        // 含まない行列を渡します（履歴は積算された絵で、ずらしの
        // 位置には対応していないため）。
        //
        // 前フレームの行列はRenderTargetが自分で覚えます。ここで
        // Sceneが1つだけ持つと、エディターがシーンビューとゲーム
        // ビューを同じフレームで描いたときに互いを踏み合います。
        //
        // 設定は毎フレーム渡します。混ぜられないフレーム（履歴が
        // まだ無い最初の1枚）でも、履歴の作り直しは走らせたい
        // ためです。
        if (temporalWanted && target != nullptr)
        {
            auto& frame = m_temporalFrame;
            frame.settings = m_temporalAntiAliasing;
            // 再投影には「ずらし無し」の行列を使います。
            //
            // ずらし込みの逆行列で復元すると、履歴を読む位置が毎
            // フレーム±0.5pxだけ動きます。内部は平均されるので
            // 収束しますが、コントラストの高い輪郭では読む場所が
            // 揺れるぶんそのまま出力が振れ、エッジがちらつきます
            // （2026-08-05に連続フレームの差が最大95まで出ました）。
            // 深度側にサブピクセルのずれは残りますが、写像が毎
            // フレーム同じであることの方がずっと重要です。
            DirectX::XMVECTOR determinant{};
            DirectX::XMStoreFloat4x4(
                &frame.inputs.inverseViewProjection,
                DirectX::XMMatrixInverse(
                    &determinant,
                    view * rawProjection));
            DirectX::XMStoreFloat4x4(
                &frame.inputs.viewProjection,
                view * rawProjection);
        }

        // モーションブラーへ渡す行列。TAAとまったく同じ2本（ずらし
        // 無しの逆ビュー射影と、次フレーム用のビュー射影）です。
        // 前フレームの行列はここでもRenderTargetが覚えます。
        //
        // TAAが切られていてもモーションブラーは使えるべきなので、
        // TAAのフレーム情報には相乗りしません。ベイク中は作りません
        // （プローブのキューブにブレを焼き付けないため）。
        m_motionBlurFrame = {};
        if (target != nullptr && !m_bakingReflectionProbes)
        {
            DirectX::XMVECTOR determinant{};
            DirectX::XMStoreFloat4x4(
                &m_motionBlurFrame.inverseViewProjection,
                DirectX::XMMatrixInverse(
                    &determinant,
                    view * rawProjection));
            DirectX::XMStoreFloat4x4(
                &m_motionBlurFrame.viewProjection,
                view * rawProjection);
        }

        // 今のカラーをSSRの「前フレームのカラー」として控えます。
        // 3Dを描き終えた直後・ポスト処理の前でなければいけません。
        // ポスト処理の後だとBloomやトーンマップが乗った絵になり、
        // 反射だけ色が違って見えます。逆に3Dパスより前に控えると、
        // まだ読んでいない履歴を潰してしまいます。
        if (target != nullptr
            && m_screenSpaceReflection.enabled
            && !m_bakingReflectionProbes)
        {
            DirectX::XMFLOAT4X4 storedViewProjection{};
            DirectX::XMStoreFloat4x4(
                &storedViewProjection,
                view * projection);
            target->CaptureColorHistory(
                m_graphics.Context(),
                storedViewProjection);
        }

        if (!include2D)
        {
            return;
        }

        Render2D();
    }

    ReflectionProbeComponent*
        Scene::NearestReflectionProbe(
            const DirectX::XMFLOAT3& position)
            const noexcept
    {
        // ベイク中は無効（プローブがプローブを映す循環を防ぐ）。
        if (m_bakingReflectionProbes)
        {
            return nullptr;
        }
        ReflectionProbeComponent* nearest = nullptr;
        float nearestSquared =
            std::numeric_limits<float>::max();
        for (auto* probe : m_frameReflectionProbes)
        {
            const auto world =
                probe->Owner().WorldMatrix();
            DirectX::XMFLOAT3 center{};
            DirectX::XMStoreFloat3(&center, world.r[3]);
            const float dx = position.x - center.x;
            const float dy = position.y - center.y;
            const float dz = position.z - center.z;
            const float distanceSquared =
                dx * dx + dy * dy + dz * dz;
            if (distanceSquared
                    <= probe->Range() * probe->Range()
                && distanceSquared < nearestSquared)
            {
                nearest = probe;
                nearestSquared = distanceSquared;
            }
        }
        return nearest;
    }

    ReflectionProbeEnvironment
        Scene::ReflectionProbeEnvironmentAt(
            const DirectX::XMFLOAT3& position)
            const noexcept
    {
        ReflectionProbeEnvironment result{};
        // ベイク中は無効（プローブがプローブを映す循環を防ぐ）。
        if (m_bakingReflectionProbes)
        {
            return result;
        }

        // 影響度の上位2個を拾います。同じ影響度なら近い方を優先
        // します（範囲の内側が一律1になるブレンド距離0のとき、
        // 従来の「一番近いもの」と同じ選び方になります）。
        ReflectionProbeComponent* primary = nullptr;
        ReflectionProbeComponent* secondary = nullptr;
        float primaryInfluence = 0.0f;
        float secondaryInfluence = 0.0f;
        float primaryDistanceSquared =
            std::numeric_limits<float>::max();
        float secondaryDistanceSquared =
            std::numeric_limits<float>::max();
        for (auto* probe : m_frameReflectionProbes)
        {
            const float influence =
                probe->InfluenceAt(position);
            if (influence <= 0.0f)
            {
                continue;
            }
            const auto center = probe->WorldPosition();
            const float dx = position.x - center.x;
            const float dy = position.y - center.y;
            const float dz = position.z - center.z;
            const float distanceSquared =
                dx * dx + dy * dy + dz * dz;
            const bool beatsPrimary =
                influence > primaryInfluence
                || (influence == primaryInfluence
                    && distanceSquared
                        < primaryDistanceSquared);
            if (beatsPrimary)
            {
                secondary = primary;
                secondaryInfluence = primaryInfluence;
                secondaryDistanceSquared =
                    primaryDistanceSquared;
                primary = probe;
                primaryInfluence = influence;
                primaryDistanceSquared = distanceSquared;
                continue;
            }
            const bool beatsSecondary =
                influence > secondaryInfluence
                || (influence == secondaryInfluence
                    && distanceSquared
                        < secondaryDistanceSquared);
            if (beatsSecondary)
            {
                secondary = probe;
                secondaryInfluence = influence;
                secondaryDistanceSquared =
                    distanceSquared;
            }
        }

        if (primary == nullptr)
        {
            return result;
        }

        const auto& baked = primary->BakedEnvironment();
        // まだ焼けていないプローブは無いものとして扱います。
        // 空のキューブを「有効」として渡すと、Shaderがnullを読んで
        // 反射が真っ黒になります（ベイク前の1フレームで出ます）。
        if (baked.specular == nullptr
            || baked.irradiance == nullptr)
        {
            return result;
        }
        result.specular = baked.specular.Get();
        result.irradiance = baked.irradiance.Get();
        result.specularMaximumMip =
            baked.specularMaximumMip;
        result.intensity = primary->Intensity();
        result.boxCenter = primary->WorldPosition();
        result.boxExtents = primary->BoxExtents();

        // どちらもブレンド距離0なら混ぜません（境界で切り替わる
        // 従来の挙動をそのまま残すため）。片方でも指定があれば、
        // 影響度の比で混ぜます。
        const bool blendRequested =
            secondary != nullptr
            && (primary->BlendDistance() > 0.0f
                || secondary->BlendDistance() > 0.0f);
        if (!blendRequested)
        {
            return result;
        }

        const float total =
            primaryInfluence + secondaryInfluence;
        if (total <= 0.0f)
        {
            return result;
        }
        const auto& secondaryBaked =
            secondary->BakedEnvironment();
        if (secondaryBaked.specular == nullptr
            || secondaryBaked.irradiance == nullptr)
        {
            return result;
        }
        result.secondarySpecular =
            secondaryBaked.specular.Get();
        result.secondaryIrradiance =
            secondaryBaked.irradiance.Get();
        result.secondarySpecularMaximumMip =
            secondaryBaked.specularMaximumMip;
        result.secondaryBoxCenter =
            secondary->WorldPosition();
        result.secondaryBoxExtents =
            secondary->BoxExtents();
        result.secondaryWeight =
            secondaryInfluence / total;
        // 強度も比率で混ぜます（片方だけ強いプローブがあるとき、
        // 境界で明るさが飛ばないようにするため）。
        result.intensity =
            primary->Intensity()
                * (1.0f - result.secondaryWeight)
            + secondary->Intensity()
                * result.secondaryWeight;
        return result;
    }

    namespace
    {
        // プローブの環境キャッシュの鍵。「同じシーンの、同じ場所の、
        // 同じ範囲のプローブ」なら前回のベイク結果を使ってよい、
        // という意味の鍵です。シーンのパスを混ぜるのは、別のシーンの
        // 同じ座標に置かれたプローブが他所の景色を映さないためです。
        // 位置はfloatのビット列そのままで比べます（JSONから読み直した
        // 値は毎回同じビットになるため）。
        [[nodiscard]] std::uint64_t ProbeEnvironmentCacheKey(
            const std::filesystem::path& scenePath,
            const ReflectionProbeComponent& probe,
            const std::uint32_t faceSize)
        {
            std::vector<std::uint8_t> buffer;
            const auto pathUtf8 = PathToUtf8(scenePath);
            buffer.insert(
                buffer.end(),
                pathUtf8.begin(),
                pathUtf8.end());
            const auto appendBits =
                [&buffer](const auto value)
            {
                const auto* begin =
                    reinterpret_cast<const std::uint8_t*>(
                        &value);
                buffer.insert(
                    buffer.end(),
                    begin,
                    begin + sizeof(value));
            };
            appendBits(probe.WorldPosition().x);
            appendBits(probe.WorldPosition().y);
            appendBits(probe.WorldPosition().z);
            appendBits(probe.Range());
            appendBits(faceSize);
            return EnvironmentCache::HashBytes(buffer);
        }
    }

    void Scene::BakePendingReflectionProbes()
    {
        if (m_bakingReflectionProbes
            || !m_graphics.IsInitialized())
        {
            return;
        }

        // ベイク待ちのプローブを集めます（大半のフレームは空）。
        std::vector<ReflectionProbeComponent*> pending;
        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }
            auto* probe = gameObject->GetComponent<
                ReflectionProbeComponent>();
            if (probe == nullptr
                || !probe->IsEnabled()
                || !probe->IsBakeRequested())
            {
                continue;
            }
            // シーン由来のプローブの最初のベイク要求は、前回の
            // ベイク結果がディスクに残っていれば復元で済ませます
            // （シーンを開くたびの6面描画＋畳み込みを飛ばす）。
            // 復元できなければ普通にベイクします。実行中に作られた
            // プローブは対象外です（過去の別の絵を映さないため）。
            if (probe->IsLoadedFromScene()
                && !probe->RestoreAttempted())
            {
                probe->MarkRestoreAttempted();
                auto restored = EnvironmentCache::TryLoad(
                    m_graphics.Device(),
                    ProbeEnvironmentCacheKey(
                        m_sceneManager != nullptr
                            ? m_sceneManager
                                ->CurrentScenePath()
                            : std::filesystem::path{},
                        *probe,
                        ReflectionProbeBakeResources::
                            FaceSize));
                if (restored.IsValid())
                {
                    probe->SetBakedEnvironment(
                        std::move(restored));
                    continue;
                }
            }
            pending.push_back(probe);
        }
        if (pending.empty())
        {
            return;
        }

        try
        {
            if (!m_probeBakeResources)
            {
                m_probeBakeResources = std::make_unique<
                    ReflectionProbeBakeResources>(
                    m_graphics.Device());
            }
        }
        catch (const std::exception& exception)
        {
            // リソースが作れない環境では諦めます（毎フレーム
            // 例外にしないため、要求だけ下ろします）。
            for (auto* probe : pending)
            {
                probe->SetBakedEnvironment({});
            }
            Logger::Instance().Warning(
                std::string{
                    "リフレクションプローブを初期化"
                    "できませんでした: " }
                + exception.what());
            return;
        }

        auto* const context = m_graphics.Context();

        // 現在の描画先を退避します（フレーム途中で呼ばれるため）。
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            previousTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            previousDepth;
        context->OMGetRenderTargets(
            1,
            previousTarget.ReleaseAndGetAddressOf(),
            previousDepth.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT previousViewport{};
        UINT previousViewportCount = 1;
        context->RSGetViewports(
            &previousViewportCount,
            &previousViewport);

        // D3D標準のキューブ面向き（左手系）。ポイント影と同じ
        // 並びで、ワールド方向ベクトルでのサンプリングと一致します。
        static constexpr DirectX::XMFLOAT3
            FaceDirections[6]{
                { 1.0f, 0.0f, 0.0f },
                { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, -1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, 0.0f, -1.0f }
            };
        static constexpr DirectX::XMFLOAT3 FaceUps[6]{
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }
        };

        m_graphics.Gpu().BeginSection("プローブベイク");
        m_bakingReflectionProbes = true;
        try
        {
            for (auto* probe : pending)
            {
                const auto world =
                    probe->Owner().WorldMatrix();
                DirectX::XMFLOAT3 eye{};
                DirectX::XMStoreFloat3(&eye, world.r[3]);
                // エンジンの右手系のまま、鏡像なしで描きます。
                // D3Dのキューブ面レイアウト（左手系）との差は、
                // 描いた後の左右反転コピーで吸収します。射影行列で
                // 鏡像にすると巻き方向が反転して裏面カリングが
                // 逆になり、閉じたメッシュが裏返ります（実際に
                // プローブを球の中へ置いたら球の内壁だけが焼けました）。
                const auto faceProjection =
                    DirectX::XMMatrixPerspectiveFovRH(
                        DirectX::XM_PIDIV2,
                        1.0f,
                        0.1f,
                        std::max(
                            probe->Range() * 4.0f,
                            100.0f));

                D3D11_VIEWPORT viewport{};
                viewport.Width = static_cast<float>(
                    ReflectionProbeBakeResources::
                        FaceSize);
                viewport.Height = viewport.Width;
                viewport.MaxDepth = 1.0f;

                for (std::uint32_t face = 0;
                    face < 6u;
                    ++face)
                {
                    ID3D11RenderTargetView* targets[]{
                        m_probeBakeResources
                            ->scratchTarget.Get()
                    };
                    context->OMSetRenderTargets(
                        1,
                        targets,
                        m_probeBakeResources
                            ->depthView.Get());
                    context->RSSetViewports(
                        1, &viewport);
                    constexpr float clearColor[4]{};
                    context->ClearRenderTargetView(
                        targets[0], clearColor);
                    context->ClearDepthStencilView(
                        m_probeBakeResources
                            ->depthView.Get(),
                        D3D11_CLEAR_DEPTH
                            | D3D11_CLEAR_STENCIL,
                        1.0f,
                        0);

                    const auto faceView =
                        DirectX::XMMatrixLookToRH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &FaceDirections[face]),
                            DirectX::XMLoadFloat3(
                                &FaceUps[face]));
                    // ポスト処理なしのHDRリニアで焼きます
                    // （IBLはトーンマップ前の値が正）。
                    RenderWithMatrices(
                        faceView,
                        faceProjection,
                        false,
                        false);

                    // 左右反転してキューブ面へ書き込みます。
                    m_graphics.Environment().CopyMirroredX(
                        m_probeBakeResources
                            ->scratchShaderResourceView
                            .Get(),
                        m_probeBakeResources
                            ->faceTargets[face].Get(),
                        ReflectionProbeBakeResources::
                            FaceSize,
                        ReflectionProbeBakeResources::
                            FaceSize);
                }

                // 事前フィルタはこのキューブをSRV（t1）として読む
                // ので、先に描画先から外します。最後の面のRTVが
                // 着いたままだと、同じリソースのRTV/SRV同時バインド
                // をD3D11が検出してSRVを黙ってnullにし、真っ黒を
                // 積分した結果が返ります（実際に踏みました）。
                context->OMSetRenderTargets(
                    0, nullptr, nullptr);
                auto baked = m_graphics.Environment()
                    .CreatePrefilteredEnvironment(
                        m_probeBakeResources
                            ->cubeShaderResourceView
                            .Get());
                // シーン由来のプローブは結果をディスクへ残し、
                // 次にシーンを開いたとき復元できるようにします。
                if (probe->IsLoadedFromScene())
                {
                    EnvironmentCache::Store(
                        ProbeEnvironmentCacheKey(
                            m_sceneManager != nullptr
                                ? m_sceneManager
                                    ->CurrentScenePath()
                                : std::filesystem::path{},
                            *probe,
                            ReflectionProbeBakeResources::
                                FaceSize),
                        m_graphics.Device(),
                        context,
                        baked);
                }
                probe->SetBakedEnvironment(
                    std::move(baked));
            }
        }
        catch (...)
        {
            m_bakingReflectionProbes = false;
            m_graphics.Gpu().EndSection();
            throw;
        }
        m_bakingReflectionProbes = false;
        m_graphics.Gpu().EndSection();

        // 描画先を戻します。
        context->OMSetRenderTargets(
            1,
            previousTarget.GetAddressOf(),
            previousDepth.Get());
        if (previousViewportCount > 0)
        {
            context->RSSetViewports(
                1, &previousViewport);
        }
    }


    void Scene::SetBakedGlobalIlluminationSettings(
        const BakedGlobalIlluminationSettings& settings)
        noexcept
    {
        m_bakedGiSettings = settings;
        m_bakedGiSettings.size.x =
            std::max(m_bakedGiSettings.size.x, 0.1f);
        m_bakedGiSettings.size.y =
            std::max(m_bakedGiSettings.size.y, 0.1f);
        m_bakedGiSettings.size.z =
            std::max(m_bakedGiSettings.size.z, 0.1f);
        // 各軸64まで・合計32768点まで。上限が無いと、桁を1つ
        // 打ち間違えただけでベイクが何時間も終わらなくなります。
        m_bakedGiSettings.resolutionX = std::clamp(
            m_bakedGiSettings.resolutionX, 1u, 64u);
        m_bakedGiSettings.resolutionY = std::clamp(
            m_bakedGiSettings.resolutionY, 1u, 64u);
        m_bakedGiSettings.resolutionZ = std::clamp(
            m_bakedGiSettings.resolutionZ, 1u, 64u);
        while (static_cast<std::uint64_t>(
                m_bakedGiSettings.resolutionX)
            * m_bakedGiSettings.resolutionY
            * m_bakedGiSettings.resolutionZ
            > 32768u)
        {
            // どれか一番大きい軸を半分にして上限へ収めます。
            auto* largest = &m_bakedGiSettings.resolutionX;
            if (m_bakedGiSettings.resolutionY > *largest)
            {
                largest = &m_bakedGiSettings.resolutionY;
            }
            if (m_bakedGiSettings.resolutionZ > *largest)
            {
                largest = &m_bakedGiSettings.resolutionZ;
            }
            *largest = std::max(1u, *largest / 2u);
        }
        m_bakedGiSettings.intensity = std::clamp(
            m_bakedGiSettings.intensity, 0.0f, 8.0f);
    }

    void Scene::RequestBakedGlobalIlluminationBake() noexcept
    {
        // 焼く形は要求時の設定で固定します（ベイク中に設定を
        // いじっても、進行中の焼き込みが崩れないように）。
        m_bakedGiBakedShape = m_bakedGiSettings;
        const std::size_t total =
            static_cast<std::size_t>(
                m_bakedGiBakedShape.resolutionX)
            * m_bakedGiBakedShape.resolutionY
            * m_bakedGiBakedShape.resolutionZ;
        if (total == 0)
        {
            return;
        }
        try
        {
            m_bakedGiWorking.assign(total * 12, 0.0f);
        }
        catch (...)
        {
            return;
        }
        m_bakedGiNextProbe = 0;
        m_bakedGiBaking = true;
    }

    void Scene::RestoreBakedGlobalIllumination(
        const BakedGlobalIlluminationSettings& shape,
        std::vector<std::uint16_t> payload) noexcept
    {
        // シーン読み込みからの復元。サイズが形と合わないデータは
        // 受け取りません（壊れたファイルより「GIなし」のほうが
        // ましです）。
        const std::size_t total =
            static_cast<std::size_t>(shape.resolutionX)
            * shape.resolutionY
            * shape.resolutionZ;
        if (total == 0 || payload.size() != total * 12)
        {
            return;
        }
        m_bakedGiBakedShape = shape;
        m_bakedGiData = std::move(payload);
        m_bakedGiTexturesDirty = true;
        m_bakedGiBaking = false;
        m_bakedGiNextProbe = 0;
        m_bakedGiWorking.clear();
    }

    float Scene::BakedGlobalIlluminationBakeProgress()
        const noexcept
    {
        if (!m_bakedGiBaking)
        {
            return -1.0f;
        }
        const std::size_t total =
            static_cast<std::size_t>(
                m_bakedGiBakedShape.resolutionX)
            * m_bakedGiBakedShape.resolutionY
            * m_bakedGiBakedShape.resolutionZ;
        if (total == 0)
        {
            return -1.0f;
        }
        return static_cast<float>(m_bakedGiNextProbe)
            / static_cast<float>(total);
    }

    namespace
    {
        // 16pxの照度キューブを読み戻してL1球面調和へ射影します。
        // 出力は12個のfloat（RGB各チャンネルの x,y,z,定数項）。
        //
        // 係数は「関数の当てはめ」です。E(方向)をY00とY1mへ射影し、
        // そのまま再構成すると
        //   定数項 = (1/4π)ΣE・ω、軸の項 = (3/4π)ΣE・軸・ω
        // になります（一様なEなら定数項=E、軸の項=0になるのが
        // 検算です）。
        [[nodiscard]] bool ProjectIrradianceToSh(
            ID3D11Device* const device,
            ID3D11DeviceContext* const context,
            ID3D11ShaderResourceView* const irradiance,
            float* const outCoefficients)
        {
            using Microsoft::WRL::ComPtr;
            if (irradiance == nullptr)
            {
                return false;
            }
            ComPtr<ID3D11Resource> resource;
            irradiance->GetResource(
                resource.ReleaseAndGetAddressOf());
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(resource.As(&texture)))
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Format
                    != DXGI_FORMAT_R16G16B16A16_FLOAT
                || description.ArraySize != 6)
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC staging = description;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> copy;
            if (FAILED(device->CreateTexture2D(
                &staging,
                nullptr,
                copy.ReleaseAndGetAddressOf())))
            {
                return false;
            }
            context->CopyResource(copy.Get(), texture.Get());

            const std::uint32_t edge = description.Width;
            double sums[12]{};
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                const UINT subresource = D3D11CalcSubresource(
                    0, face, description.MipLevels);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (FAILED(context->Map(
                    copy.Get(),
                    subresource,
                    D3D11_MAP_READ,
                    0,
                    &mapped)))
                {
                    return false;
                }
                for (std::uint32_t y = 0; y < edge; ++y)
                {
                    const auto* row =
                        reinterpret_cast<
                            const DirectX::PackedVector::
                                HALF*>(
                            static_cast<const std::uint8_t*>(
                                mapped.pData)
                            + y * mapped.RowPitch);
                    for (std::uint32_t x = 0;
                        x < edge;
                        ++x)
                    {
                        // テクセル中心の方向。シェーダーの
                        // CubeDirection（LamaPonEnvironment.hlsl）と
                        // 同じ式です。ここが食い違うと間接光の
                        // 向きが狂います。
                        const float uc =
                            (static_cast<float>(x) + 0.5f)
                                / edge * 2.0f
                            - 1.0f;
                        const float vc =
                            (static_cast<float>(y) + 0.5f)
                                / edge * 2.0f
                            - 1.0f;
                        DirectX::XMFLOAT3 direction{};
                        switch (face)
                        {
                        case 0:
                            direction = { 1.0f, -vc, -uc };
                            break;
                        case 1:
                            direction = { -1.0f, -vc, uc };
                            break;
                        case 2:
                            direction = { uc, 1.0f, vc };
                            break;
                        case 3:
                            direction = { uc, -1.0f, -vc };
                            break;
                        case 4:
                            direction = { uc, -vc, 1.0f };
                            break;
                        default:
                            direction = { -uc, -vc, -1.0f };
                            break;
                        }
                        const float lengthSquared =
                            direction.x * direction.x
                            + direction.y * direction.y
                            + direction.z * direction.z;
                        const float length =
                            std::sqrt(lengthSquared);
                        // キューブ1テクセルの立体角。面上の面積
                        // (2/N)^2 を距離の3乗で割ったものです。
                        const float solidAngle =
                            4.0f / (edge * edge)
                            / (lengthSquared * length);
                        const float nx =
                            direction.x / length;
                        const float ny =
                            direction.y / length;
                        const float nz =
                            direction.z / length;
                        for (int channel = 0;
                            channel < 3;
                            ++channel)
                        {
                            const float value =
                                DirectX::PackedVector::
                                    XMConvertHalfToFloat(
                                        row[x * 4
                                            + channel]);
                            const double weighted =
                                static_cast<double>(value)
                                * solidAngle;
                            double* base =
                                sums + channel * 4;
                            base[0] += weighted * nx;
                            base[1] += weighted * ny;
                            base[2] += weighted * nz;
                            base[3] += weighted;
                        }
                    }
                }
                context->Unmap(copy.Get(), subresource);
            }
            constexpr double AxisScale =
                3.0 / (4.0 * 3.14159265358979323846);
            constexpr double ConstantScale =
                1.0 / (4.0 * 3.14159265358979323846);
            for (int channel = 0; channel < 3; ++channel)
            {
                outCoefficients[channel * 4 + 0] =
                    static_cast<float>(
                        sums[channel * 4 + 0] * AxisScale);
                outCoefficients[channel * 4 + 1] =
                    static_cast<float>(
                        sums[channel * 4 + 1] * AxisScale);
                outCoefficients[channel * 4 + 2] =
                    static_cast<float>(
                        sums[channel * 4 + 2] * AxisScale);
                outCoefficients[channel * 4 + 3] =
                    static_cast<float>(
                        sums[channel * 4 + 3]
                        * ConstantScale);
            }
            return true;
        }
    }

    void Scene::ProcessBakedGlobalIlluminationBake()
    {
        if (!m_bakedGiBaking
            || m_bakingReflectionProbes
            || !m_graphics.IsInitialized())
        {
            return;
        }
        const auto& shape = m_bakedGiBakedShape;
        const std::size_t total =
            static_cast<std::size_t>(shape.resolutionX)
            * shape.resolutionY
            * shape.resolutionZ;
        if (total == 0
            || m_bakedGiWorking.size() != total * 12)
        {
            m_bakedGiBaking = false;
            return;
        }

        try
        {
            if (!m_probeBakeResources)
            {
                m_probeBakeResources = std::make_unique<
                    ReflectionProbeBakeResources>(
                    m_graphics.Device());
            }
        }
        catch (const std::exception& exception)
        {
            m_bakedGiBaking = false;
            Logger::Instance().Warning(
                std::string{
                    "GIベイクを初期化できませんでした: " }
                + exception.what());
            return;
        }

        auto* const context = m_graphics.Context();

        // 現在の描画先を退避します（フレーム途中で呼ばれるため）。
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            previousTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            previousDepth;
        context->OMGetRenderTargets(
            1,
            previousTarget.ReleaseAndGetAddressOf(),
            previousDepth.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT previousViewport{};
        UINT previousViewportCount = 1;
        context->RSGetViewports(
            &previousViewportCount,
            &previousViewport);

        // プローブベイクと同じ面の並び（D3D標準のキューブ面）。
        static constexpr DirectX::XMFLOAT3 FaceDirections[6]{
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f }
        };
        static constexpr DirectX::XMFLOAT3 FaceUps[6]{
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f }
        };

        // 1フレームに焼く点数。多いほど早く終わりますが、その
        // フレームが長く止まります（8点×7描画＝56回のシーン描画が
        // 上限の目安です）。
        constexpr std::size_t ProbesPerFrame = 8;

        m_graphics.Gpu().BeginSection("GIベイク");
        // ベイク中の描画が自分（焼きかけのGI）やプローブを読まない
        // よう、プローブベイクと同じ再入ガードを立てます。焼き込みは
        // 常に「GIなしの絵」から作られる＝1バウンスで確定します。
        m_bakingReflectionProbes = true;
        try
        {
            const float farPlane = std::max(
                std::sqrt(
                    shape.size.x * shape.size.x
                    + shape.size.y * shape.size.y
                    + shape.size.z * shape.size.z)
                    * 2.0f,
                100.0f);
            const auto faceProjection =
                DirectX::XMMatrixPerspectiveFovRH(
                    DirectX::XM_PIDIV2,
                    1.0f,
                    0.1f,
                    farPlane);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(
                ReflectionProbeBakeResources::FaceSize);
            viewport.Height = viewport.Width;
            viewport.MaxDepth = 1.0f;

            const std::size_t endProbe = std::min(
                m_bakedGiNextProbe + ProbesPerFrame,
                total);
            for (; m_bakedGiNextProbe < endProbe;
                ++m_bakedGiNextProbe)
            {
                const std::size_t index =
                    m_bakedGiNextProbe;
                const std::uint32_t gridX =
                    static_cast<std::uint32_t>(
                        index % shape.resolutionX);
                const std::uint32_t gridY =
                    static_cast<std::uint32_t>(
                        (index / shape.resolutionX)
                        % shape.resolutionY);
                const std::uint32_t gridZ =
                    static_cast<std::uint32_t>(
                        index
                        / (static_cast<std::size_t>(
                            shape.resolutionX)
                            * shape.resolutionY));
                const auto axisFraction =
                    [](const std::uint32_t position,
                       const std::uint32_t resolution)
                {
                    // プローブは箱の角から角まで等間隔。1点だけの
                    // 軸は中央です。
                    return resolution <= 1
                        ? 0.5f
                        : static_cast<float>(position)
                            / static_cast<float>(
                                resolution - 1);
                };
                DirectX::XMFLOAT3 eye{
                    shape.center.x - shape.size.x * 0.5f
                        + shape.size.x
                            * axisFraction(
                                gridX,
                                shape.resolutionX),
                    shape.center.y - shape.size.y * 0.5f
                        + shape.size.y
                            * axisFraction(
                                gridY,
                                shape.resolutionY),
                    shape.center.z - shape.size.z * 0.5f
                        + shape.size.z
                            * axisFraction(
                                gridZ,
                                shape.resolutionZ)
                };

                for (std::uint32_t face = 0;
                    face < 6u;
                    ++face)
                {
                    ID3D11RenderTargetView* targets[]{
                        m_probeBakeResources
                            ->scratchTarget.Get()
                    };
                    context->OMSetRenderTargets(
                        1,
                        targets,
                        m_probeBakeResources
                            ->depthView.Get());
                    context->RSSetViewports(1, &viewport);
                    constexpr float clearColor[4]{};
                    context->ClearRenderTargetView(
                        targets[0], clearColor);
                    context->ClearDepthStencilView(
                        m_probeBakeResources
                            ->depthView.Get(),
                        D3D11_CLEAR_DEPTH
                            | D3D11_CLEAR_STENCIL,
                        1.0f,
                        0);
                    const auto faceView =
                        DirectX::XMMatrixLookToRH(
                            DirectX::XMLoadFloat3(&eye),
                            DirectX::XMLoadFloat3(
                                &FaceDirections[face]),
                            DirectX::XMLoadFloat3(
                                &FaceUps[face]));
                    // プローブベイクと同じくポスト処理なしの
                    // HDRリニアで焼きます。
                    RenderWithMatrices(
                        faceView,
                        faceProjection,
                        false,
                        false);
                    m_graphics.Environment().CopyMirroredX(
                        m_probeBakeResources
                            ->scratchShaderResourceView
                            .Get(),
                        m_probeBakeResources
                            ->faceTargets[face].Get(),
                        ReflectionProbeBakeResources::
                            FaceSize,
                        ReflectionProbeBakeResources::
                            FaceSize);
                }
                // 畳み込みはこのキューブをSRVとして読むので、
                // 先に描画先から外します（プローブベイクと同じ罠）。
                context->OMSetRenderTargets(
                    0, nullptr, nullptr);
                // スペキュラは使わないので照度だけ畳み込みます。
                auto irradianceOnly =
                    m_graphics.Environment()
                        .CreatePrefilteredEnvironment(
                            m_probeBakeResources
                                ->cubeShaderResourceView
                                .Get(),
                            false);
                if (irradianceOnly.irradiance == nullptr
                    || !ProjectIrradianceToSh(
                        m_graphics.Device(),
                        context,
                        irradianceOnly.irradiance.Get(),
                        m_bakedGiWorking.data()
                            + index * 12))
                {
                    throw std::runtime_error(
                        "GI probe readback failed.");
                }
            }
        }
        catch (const std::exception& exception)
        {
            m_bakingReflectionProbes = false;
            m_graphics.Gpu().EndSection();
            m_bakedGiBaking = false;
            Logger::Instance().Warning(
                std::string{ "GIベイクに失敗しました: " }
                + exception.what());
            context->OMSetRenderTargets(
                1,
                previousTarget.GetAddressOf(),
                previousDepth.Get());
            if (previousViewportCount > 0)
            {
                context->RSSetViewports(
                    1, &previousViewport);
            }
            return;
        }
        m_bakingReflectionProbes = false;
        m_graphics.Gpu().EndSection();

        // 描画先を戻します。
        context->OMSetRenderTargets(
            1,
            previousTarget.GetAddressOf(),
            previousDepth.Get());
        if (previousViewportCount > 0)
        {
            context->RSSetViewports(1, &previousViewport);
        }

        if (m_bakedGiNextProbe < total)
        {
            return;
        }

        // 全点完了。fp16へ詰めてテクスチャの作り直しを予約します。
        // 並びは [R,G,B]×[z,y,x]×(x,y,z,定数項) で、そのまま
        // Texture3Dの初期データになります。
        m_bakedGiBaking = false;
        m_bakedGiData.assign(total * 12, 0);
        for (std::size_t probe = 0; probe < total; ++probe)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                for (int component = 0;
                    component < 4;
                    ++component)
                {
                    m_bakedGiData[
                        static_cast<std::size_t>(channel)
                            * total * 4
                        + probe * 4
                        + component] =
                        DirectX::PackedVector::
                            XMConvertFloatToHalf(
                                m_bakedGiWorking[
                                    probe * 12
                                    + channel * 4
                                    + component]);
                }
            }
        }
        m_bakedGiWorking.clear();
        m_bakedGiWorking.shrink_to_fit();
        m_bakedGiTexturesDirty = true;
        Logger::Instance().Info(
            "GIベイクが完了しました（"
            + std::to_string(total)
            + "点）");
    }

    void Scene::EnsureBakedGlobalIlluminationTextures()
    {
        if (!m_bakedGiTexturesDirty)
        {
            return;
        }
        m_bakedGiTexturesDirty = false;
        m_bakedGiViews = {};
        const auto& shape = m_bakedGiBakedShape;
        const std::size_t total =
            static_cast<std::size_t>(shape.resolutionX)
            * shape.resolutionY
            * shape.resolutionZ;
        if (total == 0 || m_bakedGiData.size() != total * 12)
        {
            return;
        }
        auto* const device = m_graphics.Device();
        for (int channel = 0; channel < 3; ++channel)
        {
            D3D11_TEXTURE3D_DESC description{};
            description.Width = shape.resolutionX;
            description.Height = shape.resolutionY;
            description.Depth = shape.resolutionZ;
            description.MipLevels = 1;
            description.Format =
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags =
                D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem =
                m_bakedGiData.data()
                + static_cast<std::size_t>(channel)
                    * total * 4;
            initialData.SysMemPitch =
                shape.resolutionX * 8;
            initialData.SysMemSlicePitch =
                shape.resolutionX
                * shape.resolutionY * 8;
            Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
            if (FAILED(device->CreateTexture3D(
                &description,
                &initialData,
                texture.ReleaseAndGetAddressOf())))
            {
                m_bakedGiViews = {};
                return;
            }
            if (FAILED(device->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                m_bakedGiViews[
                    static_cast<std::size_t>(channel)]
                    .ReleaseAndGetAddressOf())))
            {
                m_bakedGiViews = {};
                return;
            }
        }
    }

    LightingState Scene::BuildLightingState() const noexcept
    {
        LightingState lighting;
        lighting.ambientColor = m_ambientLightColor;
        lighting.ambientIntensity = m_ambientLightIntensity;
        // 朝昼夜モードでは環境光も太陽の高度に追従させます。
        // ここを固定のままにすると、夜になっても回り込みの光だけ
        // 昼のまま残り、影の中が妙に明るい絵になります。
        if (m_sky.sunDriven)
        {
            DirectX::XMFLOAT3 directionToSun{};
            DirectX::XMFLOAT3 sunColor{};
            float sunAngularRadius{};
            if (ResolveSkySun(
                    directionToSun,
                    sunColor,
                    sunAngularRadius))
            {
                const auto evaluated =
                    EvaluateSunDrivenSky(directionToSun);
                lighting.ambientColor =
                    evaluated.ambientColor;
                lighting.ambientIntensity =
                    evaluated.ambientIntensity;
            }
        }
        lighting.directionalShadowResolution =
            static_cast<float>(
                std::max(
                    m_graphics.Settings()
                        .shadowResolution,
                    1u));
        lighting.localShadowResolution =
            static_cast<float>(
                std::max(
                    m_graphics.Settings()
                            .shadowResolution
                        / 2u,
                    256u));
        lighting.fog = m_fog;
        lighting.fog.enabled =
            lighting.fog.enabled
            && m_graphics.Settings().fogEnabled;

        for (const auto& gameObject : m_gameObjects)
        {
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            const auto* light =
                gameObject->GetComponent<DirectionalLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            auto& destination =
                lighting.directionalLights[
                    lighting.directionalLightCount++];
            destination.direction = light->WorldDirection();
            destination.color = light->Color();
            destination.intensity = light->Intensity();
            // Inspectorは「直径・度」で見せ、シェーダーは「半径・
            // ラジアン」で使います。度のほうが太陽の0.53度という
            // 実際の値と直に結び付き、ラジアンの半径のほうが
            // 円盤との角度比較にそのまま使えるためです。
            destination.angularRadius =
                DirectX::XMConvertToRadians(
                    light->AngularDiameterDegrees() * 0.5f);

            if (lighting.directionalLightCount
                == MaximumDirectionalLights)
            {
                break;
            }
        }

        // ポイント／スポットは2つの行き先へ集めます。
        //
        // ①従来の固定配列（先着N灯）: 自作ShaderとDirectXTKへの
        //   フォールバックが読む互換経路。上限は品質設定どおり。
        // ②clusteredLights: クラスタカリング（Forward+）の全量。
        //   LamaPonLit.hlslはこちらを使うので、上限は256灯。
        //
        // ②の並び順は①と同じ（ポイントが先、次にスポット、どちらも
        // m_gameObjects順）。影のスロット対応が「①でのライト番号＝
        // ②での並び位置」で成立することに依存しています。ここの
        // 順序を変えるときはRenderWithMatricesの影割り当ても直すこと。
        const std::size_t pointLimit =
            std::min<std::size_t>(
                m_graphics.Settings().pointLightLimit,
                MaximumPointLights);
        for (const auto& gameObject : m_gameObjects)
        {
            if (lighting.clusteredLights.size()
                >= MaximumClusteredLights)
            {
                break;
            }
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            const auto* light =
                gameObject->GetComponent<PointLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            const auto position = light->WorldPosition();
            const auto color = light->Color();
            if (lighting.pointLightCount < pointLimit)
            {
                auto& destination =
                    lighting.pointLights[
                        lighting.pointLightCount];
                destination.position = position;
                destination.color = color;
                destination.intensity =
                    light->Intensity();
                destination.range = light->Range();
                ++lighting.pointLightCount;
            }

            GpuLight clustered{};
            clustered.positionRange = {
                position.x,
                position.y,
                position.z,
                light->Range()
            };
            clustered.colorIntensity = {
                color.x,
                color.y,
                color.z,
                light->Intensity()
            };
            // z=クラスタ経路で見たポイントライトの通し番号+1。
            // 固定配列の番号と同じ並びなので、影の対象かどうかは
            // シェーダー側がPointShadowParametersと照合できます
            // （上限から溢れた分は照合が成立せず影なしになるだけ）。
            clustered.extraParameters = {
                0.0f,
                0.0f,
                static_cast<float>(
                    lighting.clusteredLights.size() + 1),
                0.0f
            };
            lighting.clusteredLights.push_back(clustered);
        }
        // クラスタ配列の中で「ここまでがポイント」の境界。スポットの
        // 影スロット対応（RenderWithMatrices）が使います。
        const std::size_t clusteredPointCount =
            lighting.clusteredLights.size();
        static_cast<void>(clusteredPointCount);

        const std::size_t spotLimit =
            std::min<std::size_t>(
                m_graphics.Settings().spotLightLimit,
                MaximumSpotLights);
        for (const auto& gameObject : m_gameObjects)
        {
            if (lighting.clusteredLights.size()
                >= MaximumClusteredLights)
            {
                break;
            }
            if (!gameObject->IsActiveInHierarchy())
            {
                continue;
            }

            const auto* light =
                gameObject->GetComponent<SpotLightComponent>();
            if (light == nullptr || !light->IsEnabled())
            {
                continue;
            }

            const auto position = light->WorldPosition();
            const auto direction = light->WorldDirection();
            const auto color = light->Color();
            const float innerCosine =
                std::cos(light->InnerConeAngle());
            const float outerCosine =
                std::cos(light->OuterConeAngle());
            if (lighting.spotLightCount < spotLimit)
            {
                auto& destination =
                    lighting.spotLights[
                        lighting.spotLightCount++];
                destination.position = position;
                destination.direction = direction;
                destination.color = color;
                destination.intensity =
                    light->Intensity();
                destination.range = light->Range();
                destination.innerConeCosine = innerCosine;
                destination.outerConeCosine = outerCosine;
            }

            GpuLight clustered{};
            clustered.positionRange = {
                position.x,
                position.y,
                position.z,
                light->Range()
            };
            clustered.colorIntensity = {
                color.x,
                color.y,
                color.z,
                light->Intensity()
            };
            clustered.directionInnerCosine = {
                direction.x,
                direction.y,
                direction.z,
                innerCosine
            };
            // 影スロットはRenderWithMatricesの影割り当て後に
            // 書き込まれます（ここでは影なし）。
            clustered.extraParameters = {
                outerCosine,
                1.0f,
                0.0f,
                0.0f
            };
            lighting.clusteredLights.push_back(clustered);
        }

        return lighting;
    }
}
