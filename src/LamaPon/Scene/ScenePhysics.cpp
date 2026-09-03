#include "LamaPon/Scene/Scene.h"

#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Physics/PhysicsQuery.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
    template<typename Collider>
    bool AcceptCollider(
        const LamaPon::GameObject& object,
        const Collider& collider,
        const LamaPon::PhysicsQueryFilter&
            filter) noexcept
    {
        return object.IsEnabled()
            && collider.IsEnabled()
            && object.Id()
                != filter.ignoredGameObjectId
            && (filter.layerMask
                    & (1u << collider.Layer()))
                != 0
            && (filter.includeTriggers
                || !collider.IsTrigger());
    }

    bool Overlaps(
        const LamaPon::Bounds3D& left,
        const LamaPon::Bounds3D& right) noexcept
    {
        return left.minimum.x
                < right.maximum.x
            && left.maximum.x
                > right.minimum.x
            && left.minimum.y
                < right.maximum.y
            && left.maximum.y
                > right.minimum.y
            && left.minimum.z
                < right.maximum.z
            && left.maximum.z
                > right.minimum.z;
    }

    bool RaycastBounds(
        const LamaPon::Ray& ray,
        const LamaPon::Bounds3D& bounds,
        const float maximumDistance,
        float& distance,
        DirectX::XMFLOAT3& normal) noexcept
    {
        constexpr float epsilon =
            0.000001f;
        float nearest = 0.0f;
        float farthest = maximumDistance;
        int nearestAxis = -1;
        float nearestSign{};

        const std::array origins{
            ray.origin.x,
            ray.origin.y,
            ray.origin.z
        };
        const std::array directions{
            ray.direction.x,
            ray.direction.y,
            ray.direction.z
        };
        const std::array minimums{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z
        };
        const std::array maximums{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z
        };

        for (std::size_t axis{};
            axis < origins.size();
            ++axis)
        {
            if (std::abs(directions[axis])
                <= epsilon)
            {
                if (origins[axis]
                        < minimums[axis]
                    || origins[axis]
                        > maximums[axis])
                {
                    return false;
                }
                continue;
            }

            const float inverse =
                1.0f / directions[axis];
            float entry =
                (minimums[axis]
                    - origins[axis])
                * inverse;
            float exit =
                (maximums[axis]
                    - origins[axis])
                * inverse;
            float sign =
                directions[axis] > 0.0f
                    ? -1.0f
                    : 1.0f;
            if (entry > exit)
            {
                std::swap(entry, exit);
            }
            if (entry > nearest)
            {
                nearest = entry;
                nearestAxis =
                    static_cast<int>(axis);
                nearestSign = sign;
            }
            farthest =
                std::min(farthest, exit);
            if (nearest > farthest)
            {
                return false;
            }
        }

        if (nearest < 0.0f
            || nearest > maximumDistance)
        {
            return false;
        }
        distance = nearest;
        normal = {};
        if (nearestAxis == 0)
        {
            normal.x = nearestSign;
        }
        else if (nearestAxis == 1)
        {
            normal.y = nearestSign;
        }
        else if (nearestAxis == 2)
        {
            normal.z = nearestSign;
        }
        return true;
    }

    LamaPon::Ray NormalizedRay(
        const LamaPon::Ray& ray) noexcept
    {
        const float length =
            std::sqrt(
                ray.direction.x
                    * ray.direction.x
                + ray.direction.y
                    * ray.direction.y
                + ray.direction.z
                    * ray.direction.z);
        if (length <= 0.000001f)
        {
            return {
                ray.origin,
                {}
            };
        }
        return {
            ray.origin,
            {
                ray.direction.x / length,
                ray.direction.y / length,
                ray.direction.z / length
            }
        };
    }
}

namespace LamaPon
{
    bool Scene::Raycast(
        const Ray& sourceRay,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        const Ray ray =
            NormalizedRay(sourceRay);
        if (maximumDistance < 0.0f
            || (ray.direction.x == 0.0f
                && ray.direction.y == 0.0f
                && ray.direction.z == 0.0f))
        {
            return false;
        }

        bool found{};
        float nearest =
            std::numeric_limits<float>::max();
        for (const auto& object :
            m_gameObjects)
        {
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                float distance{};
                DirectX::XMFLOAT3 normal{};
                if (!RaycastBounds(
                    ray,
                    bounds,
                    maximumDistance,
                    distance,
                    normal)
                    || distance >= nearest)
                {
                    return;
                }
                nearest = distance;
                found = true;
                hit = {
                    object.get(),
                    box,
                    {
                        ray.origin.x
                            + ray.direction.x
                                * distance,
                        ray.origin.y
                            + ray.direction.y
                                * distance,
                        ray.origin.z
                            + ray.direction.z
                                * distance
                    },
                    normal,
                    distance,
                    capsule,
                    sphere,
                    hull
                };
            };
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter))
            {
                testBounds(
                    box->WorldBounds(),
                    box,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(*object, *capsule, filter))
            {
                testBounds(
                    capsule->WorldBounds(),
                    nullptr,
                    capsule,
                    nullptr,
                    nullptr);
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(*object, *sphere, filter))
            {
                testBounds(
                    sphere->WorldBounds(),
                    nullptr,
                    nullptr,
                    sphere,
                    nullptr);
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter))
            {
                testBounds(
                    hull->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull);
            }
            // メッシュはAABBではなく三角形と正確に交差判定します。
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                MeshColliderRaycastHit meshHit{};
                if (mesh->Raycast(
                        ray,
                        maximumDistance,
                        meshHit)
                    && meshHit.distance < nearest)
                {
                    nearest = meshHit.distance;
                    found = true;
                    hit = {
                        object.get(),
                        nullptr,
                        meshHit.point,
                        meshHit.normal,
                        meshHit.distance,
                        nullptr,
                        nullptr,
                        nullptr,
                        mesh
                    };
                }
            }
        }
        return found;
    }

    std::vector<PhysicsHit>
        Scene::RaycastAll(
            const Ray& sourceRay,
            const float maximumDistance,
            const PhysicsQueryFilter&
                filter) const
    {
        std::vector<PhysicsHit> hits;
        const Ray ray =
            NormalizedRay(sourceRay);
        if (maximumDistance < 0.0f
            || (ray.direction.x == 0.0f
                && ray.direction.y == 0.0f
                && ray.direction.z == 0.0f))
        {
            return hits;
        }
        for (const auto& object :
            m_gameObjects)
        {
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                float distance{};
                DirectX::XMFLOAT3 normal{};
                if (!RaycastBounds(
                    ray,
                    bounds,
                    maximumDistance,
                    distance,
                    normal))
                {
                    return;
                }
                hits.push_back({
                    object.get(),
                    box,
                    {
                        ray.origin.x
                            + ray.direction.x
                                * distance,
                        ray.origin.y
                            + ray.direction.y
                                * distance,
                        ray.origin.z
                            + ray.direction.z
                                * distance
                    },
                    normal,
                    distance,
                    capsule,
                    sphere,
                    hull
                });
            };
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter))
            {
                testBounds(
                    box->WorldBounds(),
                    box,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(*object, *capsule, filter))
            {
                testBounds(
                    capsule->WorldBounds(),
                    nullptr,
                    capsule,
                    nullptr,
                    nullptr);
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(*object, *sphere, filter))
            {
                testBounds(
                    sphere->WorldBounds(),
                    nullptr,
                    nullptr,
                    sphere,
                    nullptr);
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter))
            {
                testBounds(
                    hull->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull);
            }
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                MeshColliderRaycastHit meshHit{};
                if (mesh->Raycast(
                        ray,
                        maximumDistance,
                        meshHit))
                {
                    hits.push_back({
                        object.get(),
                        nullptr,
                        meshHit.point,
                        meshHit.normal,
                        meshHit.distance,
                        nullptr,
                        nullptr,
                        nullptr,
                        mesh
                    });
                }
            }
        }
        std::ranges::sort(
            hits,
            {},
            &PhysicsHit::distance);
        return hits;
    }

    bool Scene::SphereCast(
        const Ray& sourceRay,
        const float radius,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        const Ray ray =
            NormalizedRay(sourceRay);
        const float clampedRadius =
            std::max(radius, 0.0f);
        bool found{};
        float nearest =
            std::numeric_limits<float>::max();
        for (const auto& object :
            m_gameObjects)
        {
            const auto testBounds = [&](
                Bounds3D bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                bounds.minimum.x -= clampedRadius;
                bounds.minimum.y -= clampedRadius;
                bounds.minimum.z -= clampedRadius;
                bounds.maximum.x += clampedRadius;
                bounds.maximum.y += clampedRadius;
                bounds.maximum.z += clampedRadius;
                float distance{};
                DirectX::XMFLOAT3 normal{};
                if (!RaycastBounds(
                    ray,
                    bounds,
                    maximumDistance,
                    distance,
                    normal)
                    || distance >= nearest)
                {
                    return;
                }
                nearest = distance;
                const DirectX::XMFLOAT3
                    center{
                        ray.origin.x
                            + ray.direction.x
                                * distance,
                        ray.origin.y
                            + ray.direction.y
                                * distance,
                        ray.origin.z
                            + ray.direction.z
                                * distance
                    };
                hit = {
                    object.get(),
                    box,
                    {
                        center.x
                            - normal.x
                                * clampedRadius,
                        center.y
                            - normal.y
                                * clampedRadius,
                        center.z
                            - normal.z
                                * clampedRadius
                    },
                    normal,
                    distance,
                    capsule,
                    sphere,
                    hull
                };
                found = true;
            };
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter))
            {
                testBounds(
                    box->WorldBounds(),
                    box,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(*object, *capsule, filter))
            {
                testBounds(
                    capsule->WorldBounds(),
                    nullptr,
                    capsule,
                    nullptr,
                    nullptr);
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(*object, *sphere, filter))
            {
                testBounds(
                    sphere->WorldBounds(),
                    nullptr,
                    nullptr,
                    sphere,
                    nullptr);
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter))
            {
                testBounds(
                    hull->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull);
            }
            // メッシュは中心レイの三角形ヒットへ半径分の余裕を
            // 持たせた近似で判定します。
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                MeshColliderRaycastHit meshHit{};
                if (mesh->Raycast(
                        ray,
                        maximumDistance
                            + clampedRadius,
                        meshHit))
                {
                    const float adjusted = std::max(
                        meshHit.distance
                            - clampedRadius,
                        0.0f);
                    if (adjusted <= maximumDistance
                        && adjusted < nearest)
                    {
                        nearest = adjusted;
                        hit = {
                            object.get(),
                            nullptr,
                            meshHit.point,
                            meshHit.normal,
                            adjusted,
                            nullptr,
                            nullptr,
                            nullptr,
                            mesh
                        };
                        found = true;
                    }
                }
            }
        }
        return found;
    }

    bool Scene::BoxCast(
        const Ray& sourceRay,
        const DirectX::XMFLOAT3& halfExtents,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        const Ray ray = NormalizedRay(sourceRay);
        const DirectX::XMFLOAT3 extents{
            std::max(halfExtents.x, 0.0f),
            std::max(halfExtents.y, 0.0f),
            std::max(halfExtents.z, 0.0f) };
        bool found{};
        float nearest =
            std::numeric_limits<float>::max();
        for (const auto& object : m_gameObjects)
        {
            // 対象AABBを半エクステント分膨らませてレイ判定する近似。
            const auto testBounds = [&](
                Bounds3D bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull,
                MeshCollider3DComponent* mesh)
            {
                bounds.minimum.x -= extents.x;
                bounds.minimum.y -= extents.y;
                bounds.minimum.z -= extents.z;
                bounds.maximum.x += extents.x;
                bounds.maximum.y += extents.y;
                bounds.maximum.z += extents.z;
                float distance{};
                DirectX::XMFLOAT3 normal{};
                if (!RaycastBounds(
                        ray,
                        bounds,
                        maximumDistance,
                        distance,
                        normal)
                    || distance >= nearest)
                {
                    return;
                }
                nearest = distance;
                found = true;
                hit = {
                    object.get(),
                    box,
                    {
                        ray.origin.x
                            + ray.direction.x * distance,
                        ray.origin.y
                            + ray.direction.y * distance,
                        ray.origin.z
                            + ray.direction.z * distance
                    },
                    normal,
                    distance,
                    capsule,
                    sphere,
                    hull,
                    mesh
                };
            };
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter))
            {
                testBounds(
                    box->WorldBounds(),
                    box,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(
                    *object,
                    *capsule,
                    filter))
            {
                testBounds(
                    capsule->WorldBounds(),
                    nullptr,
                    capsule,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(
                    *object,
                    *sphere,
                    filter))
            {
                testBounds(
                    sphere->WorldBounds(),
                    nullptr,
                    nullptr,
                    sphere,
                    nullptr,
                    nullptr);
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter))
            {
                testBounds(
                    hull->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull,
                    nullptr);
            }
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                testBounds(
                    mesh->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    mesh);
            }
        }
        return found;
    }

    bool Scene::CapsuleCast(
        const Ray& sourceRay,
        const float radius,
        const float height,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // Y軸カプセルを、半径と半高さで膨らませたBoxCastで近似。
        const float clampedRadius = std::max(radius, 0.0f);
        const float halfHeight = std::max(
            height * 0.5f,
            clampedRadius);
        return BoxCast(
            sourceRay,
            {
                clampedRadius,
                halfHeight,
                clampedRadius
            },
            maximumDistance,
            hit,
            filter);
    }

    std::vector<PhysicsOverlapHit>
        Scene::OverlapCapsule(
            const DirectX::XMFLOAT3& start,
            const DirectX::XMFLOAT3& end,
            const float radius,
            const PhysicsQueryFilter& filter) const
    {
        const float clampedRadius = std::max(radius, 0.0f);
        // カプセル全体のAABBで一次判定し、線分上のサンプル点との
        // 距離で二次判定します（近似）。
        const Bounds3D capsuleBounds{
            {
                std::min(start.x, end.x) - clampedRadius,
                std::min(start.y, end.y) - clampedRadius,
                std::min(start.z, end.z) - clampedRadius
            },
            {
                std::max(start.x, end.x) + clampedRadius,
                std::max(start.y, end.y) + clampedRadius,
                std::max(start.z, end.z) + clampedRadius
            } };
        const float squaredRadius =
            clampedRadius * clampedRadius;
        const auto segmentTouchesBounds =
            [&](const Bounds3D& bounds)
        {
            if (!Overlaps(capsuleBounds, bounds))
            {
                return false;
            }
            constexpr int SampleCount = 8;
            for (int sample = 0;
                sample <= SampleCount;
                ++sample)
            {
                const float t =
                    static_cast<float>(sample)
                    / static_cast<float>(SampleCount);
                const DirectX::XMFLOAT3 point{
                    start.x + (end.x - start.x) * t,
                    start.y + (end.y - start.y) * t,
                    start.z + (end.z - start.z) * t };
                const DirectX::XMFLOAT3 closest{
                    std::clamp(
                        point.x,
                        bounds.minimum.x,
                        bounds.maximum.x),
                    std::clamp(
                        point.y,
                        bounds.minimum.y,
                        bounds.maximum.y),
                    std::clamp(
                        point.z,
                        bounds.minimum.z,
                        bounds.maximum.z) };
                const float deltaX = point.x - closest.x;
                const float deltaY = point.y - closest.y;
                const float deltaZ = point.z - closest.z;
                if (deltaX * deltaX
                    + deltaY * deltaY
                    + deltaZ * deltaZ
                    <= squaredRadius)
                {
                    return true;
                }
            }
            return false;
        };

        std::vector<PhysicsOverlapHit> hits;
        for (const auto& object : m_gameObjects)
        {
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter)
                && segmentTouchesBounds(
                    box->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    box
                });
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(
                    *object,
                    *capsule,
                    filter)
                && segmentTouchesBounds(
                    capsule->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    capsule
                });
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(
                    *object,
                    *sphere,
                    filter)
                && segmentTouchesBounds(
                    sphere->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    sphere
                });
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter)
                && segmentTouchesBounds(
                    hull->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull
                });
            }
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter)
                && Overlaps(
                    capsuleBounds,
                    mesh->WorldBounds())
                && mesh->OverlapsBounds(capsuleBounds))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    mesh
                });
            }
        }
        return hits;
    }

    std::vector<PhysicsOverlapHit>
        Scene::OverlapBox(
            const Bounds3D& bounds,
            const PhysicsQueryFilter&
                filter) const
    {
        std::vector<PhysicsOverlapHit>
            hits;
        for (const auto& object :
            m_gameObjects)
        {
            auto* collider =
                object->GetComponent<
                    BoxCollider3DComponent>();
            if (collider != nullptr
                && AcceptCollider(
                    *object,
                    *collider,
                    filter)
                && Overlaps(
                    bounds,
                    collider->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    collider,
                    nullptr
                });
            }
            auto* capsule =
                object->GetComponent<
                    CapsuleCollider3DComponent>();
            if (capsule != nullptr
                && AcceptCollider(
                    *object,
                    *capsule,
                    filter)
                && Overlaps(
                    bounds,
                    capsule->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    capsule
                });
            }
            auto* sphere =
                object->GetComponent<
                    SphereCollider3DComponent>();
            if (sphere != nullptr
                && AcceptCollider(
                    *object,
                    *sphere,
                    filter)
                && Overlaps(
                    bounds,
                    sphere->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    sphere
                });
            }
            auto* hull =
                object->GetComponent<
                    ConvexHullCollider3DComponent>();
            if (hull != nullptr
                && AcceptCollider(
                    *object,
                    *hull,
                    filter)
                && Overlaps(
                    bounds,
                    hull->WorldBounds()))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull
                });
            }
            auto* mesh =
                object->GetComponent<
                    MeshCollider3DComponent>();
            if (mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(
                    *object,
                    *mesh,
                    filter)
                && Overlaps(
                    bounds,
                    mesh->WorldBounds())
                // AABB通過後に三角形と正確に判定します。
                && mesh->OverlapsBounds(bounds))
            {
                hits.push_back({
                    object.get(),
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    mesh
                });
            }
        }
        return hits;
    }

    std::vector<PhysicsOverlapHit>
        Scene::OverlapSphere(
            const DirectX::XMFLOAT3& center,
            const float radius,
            const PhysicsQueryFilter&
                filter) const
    {
        std::vector<PhysicsOverlapHit>
            hits;
        const float squaredRadius =
            std::max(radius, 0.0f)
            * std::max(radius, 0.0f);
        for (const auto& object :
            m_gameObjects)
        {
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                const DirectX::XMFLOAT3 closest{
                    std::clamp(
                        center.x,
                        bounds.minimum.x,
                        bounds.maximum.x),
                    std::clamp(
                        center.y,
                        bounds.minimum.y,
                        bounds.maximum.y),
                    std::clamp(
                        center.z,
                        bounds.minimum.z,
                        bounds.maximum.z)
                };
                const float deltaX =
                    center.x - closest.x;
                const float deltaY =
                    center.y - closest.y;
                const float deltaZ =
                    center.z - closest.z;
                if (deltaX * deltaX
                    + deltaY * deltaY
                    + deltaZ * deltaZ
                    > squaredRadius)
                {
                    return;
                }
                hits.push_back({
                    object.get(),
                    box,
                    capsule,
                    sphere,
                    hull
                });
            };
            if (auto* box = object->GetComponent<
                    BoxCollider3DComponent>();
                box != nullptr
                && AcceptCollider(*object, *box, filter))
            {
                testBounds(
                    box->WorldBounds(),
                    box,
                    nullptr,
                    nullptr,
                    nullptr);
            }
            if (auto* capsule = object->GetComponent<
                    CapsuleCollider3DComponent>();
                capsule != nullptr
                && AcceptCollider(*object, *capsule, filter))
            {
                testBounds(
                    capsule->WorldBounds(),
                    nullptr,
                    capsule,
                    nullptr,
                    nullptr);
            }
            if (auto* sphere = object->GetComponent<
                    SphereCollider3DComponent>();
                sphere != nullptr
                && AcceptCollider(*object, *sphere, filter))
            {
                testBounds(
                    sphere->WorldBounds(),
                    nullptr,
                    nullptr,
                    sphere,
                    nullptr);
            }
            if (auto* hull = object->GetComponent<
                    ConvexHullCollider3DComponent>();
                hull != nullptr
                && AcceptCollider(*object, *hull, filter))
            {
                testBounds(
                    hull->WorldBounds(),
                    nullptr,
                    nullptr,
                    nullptr,
                    hull);
            }
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                // 球のAABBで三角形の有無を判定します（近似）。
                const Bounds3D sphereBounds{
                    {
                        center.x - radius,
                        center.y - radius,
                        center.z - radius
                    },
                    {
                        center.x + radius,
                        center.y + radius,
                        center.z + radius
                    } };
                if (Overlaps(
                        sphereBounds,
                        mesh->WorldBounds())
                    && mesh->OverlapsBounds(sphereBounds))
                {
                    hits.push_back({
                        object.get(),
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        mesh
                    });
                }
            }
        }
        return hits;
    }
}
