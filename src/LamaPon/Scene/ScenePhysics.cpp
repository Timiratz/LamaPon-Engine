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
    // 自身と形状の有効状態・除外番号・対象層・トリガー指定で問い合わせ対象を選びます(object: 形状の物体, collider: 問い合わせる形状, filter: 対象条件)。
    // 親の有効状態、形状のcollisionMaskとプロジェクトの衝突マトリクスは参照しません。
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

    // 境界の各軸が正の幅で重なるか返し、面が接するだけの組は除外します(left: 一方のAABB, right: もう一方のAABB)。
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

    // スラブ法でAABBまでの距離と法線を求め、内部開始では距離0と零法線を返します(ray: 探索線, bounds: 対象境界, maximumDistance: 距離上限, distance: 接触距離の出力, normal: 接触法線の出力)。
    bool RaycastBounds(
        const LamaPon::Ray& ray,
        const LamaPon::Bounds3D& bounds,
        const float maximumDistance,
        float& distance,
        DirectX::XMFLOAT3& normal) noexcept
    {
        // 平行と判定する方向成分の閾値
        constexpr float epsilon =
            0.000001f;
        // 全軸のスラブへ入る最短距離
        float nearest = 0.0f;
        // スラブを出る距離の上限
        float farthest = maximumDistance;
        // 最も遅い進入面の軸番号
        int nearestAxis = -1;
        // 進入面の外向き法線の符号
        float nearestSign{};

        // 探索線原点のXYZ成分
        const std::array origins{
            ray.origin.x,
            ray.origin.y,
            ray.origin.z
        };
        // 探索方向のXYZ成分
        const std::array directions{
            ray.direction.x,
            ray.direction.y,
            ray.direction.z
        };
        // 境界の最小XYZ成分
        const std::array minimums{
            bounds.minimum.x,
            bounds.minimum.y,
            bounds.minimum.z
        };
        // 境界の最大XYZ成分
        const std::array maximums{
            bounds.maximum.x,
            bounds.maximum.y,
            bounds.maximum.z
        };

        // スラブ判定する座標軸の添字
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

            // 探索方向成分の逆数
            const float inverse =
                1.0f / directions[axis];
            // 軸のスラブへ入る距離
            float entry =
                (minimums[axis]
                    - origins[axis])
                * inverse;
            // 軸のスラブから出る距離
            float exit =
                (maximums[axis]
                    - origins[axis])
                * inverse;
            // 軸の進入面の法線符号
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

    // 方向を正規化し、長さが1e-6以下なら零方向を返します(ray: 有限値の探索線)。
    LamaPon::Ray NormalizedRay(
        const LamaPon::Ray& ray) noexcept
    {
        // 正規化前の探索方向の長さ
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
    // 最も近い問い合わせ対象を返します(sourceRay: 原点と未正規化方向, maximumDistance: 探索距離上限, hit: 成功時の接触情報, filter: 対象条件)。
    bool Scene::Raycast(
        const Ray& sourceRay,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // 方向を正規化した探索線
        const Ray ray =
            NormalizedRay(sourceRay);
        if (maximumDistance < 0.0f
            || (ray.direction.x == 0.0f
                && ray.direction.y == 0.0f
                && ray.direction.z == 0.0f))
        {
            return false;
        }

        // 問い合わせ対象へ接触したか
        bool found{};
        // 見つかった最短接触距離
        float nearest =
            std::numeric_limits<float>::max();
        // 問い合わせ対象の形状所有物体
        for (const auto& object :
            m_gameObjects)
        {
            // AABBへのより近い接触だけを採用します(bounds: ワールド境界, box: 箱形状の参照, capsule: カプセルの参照, sphere: 球形状の参照, hull: 凸形状の参照)。
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                // 境界へ接触する移動距離
                float distance{};
                // 接触面の外向き法線
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
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                // 三角形への探索線の接触情報
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

    // 問い合わせ対象の接触を距離順に返します(sourceRay: 原点と未正規化方向, maximumDistance: 探索距離上限, filter: 対象条件)。
    std::vector<PhysicsHit>
        Scene::RaycastAll(
            const Ray& sourceRay,
            const float maximumDistance,
            const PhysicsQueryFilter&
                filter) const
    {
        // 問い合わせ条件に合う接触一覧
        std::vector<PhysicsHit> hits;
        // 方向を正規化した探索線
        const Ray ray =
            NormalizedRay(sourceRay);
        if (maximumDistance < 0.0f
            || (ray.direction.x == 0.0f
                && ray.direction.y == 0.0f
                && ray.direction.z == 0.0f))
        {
            return hits;
        }
        // 問い合わせ対象の形状所有物体
        for (const auto& object :
            m_gameObjects)
        {
            // AABBへの接触を結果へ追加します(bounds: ワールド境界, box: 箱形状の参照, capsule: カプセルの参照, sphere: 球形状の参照, hull: 凸形状の参照)。
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                // 境界へ接触する移動距離
                float distance{};
                // 接触面の外向き法線
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
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                // 三角形への探索線の接触情報
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

    // 球の半径で拡張した境界への最短接触を近似します(sourceRay: 原点と未正規化方向, radius: 球の半径, maximumDistance: 移動距離上限, hit: 成功時の接触情報, filter: 対象条件)。
    bool Scene::SphereCast(
        const Ray& sourceRay,
        const float radius,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // 方向を正規化した探索線
        const Ray ray =
            NormalizedRay(sourceRay);
        // 非負に制限した半径
        const float clampedRadius =
            std::max(radius, 0.0f);
        // 問い合わせ対象へ接触したか
        bool found{};
        // 見つかった最短接触距離
        float nearest =
            std::numeric_limits<float>::max();
        // 問い合わせ対象の形状所有物体
        for (const auto& object :
            m_gameObjects)
        {
            // 半径で拡張したAABBへの最短接触を採用します(bounds: 拡張する境界の写し, box: 箱形状の参照, capsule: カプセルの参照, sphere: 球形状の参照, hull: 凸形状の参照)。
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
                // 境界へ接触する移動距離
                float distance{};
                // 接触面の外向き法線
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
                // 境界へ接触した時点の球中心
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
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // メッシュは中心レイの三角形ヒットへ半径分の余裕を持たせた近似で判定します。
            // 問い合わせるメッシュ形状
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                // 三角形への探索線の接触情報
                MeshColliderRaycastHit meshHit{};
                if (mesh->Raycast(
                        ray,
                        maximumDistance
                            + clampedRadius,
                        meshHit))
                {
                    // 半径を差し引いた接触距離
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

    // 箱の半寸法で拡張した境界への最短接触を近似します(sourceRay: 原点と未正規化方向, halfExtents: 軸平行箱の半寸法, maximumDistance: 移動距離上限, hit: 成功時の接触情報, filter: 対象条件)。
    bool Scene::BoxCast(
        const Ray& sourceRay,
        const DirectX::XMFLOAT3& halfExtents,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // 方向を正規化した探索線
        const Ray ray = NormalizedRay(sourceRay);
        // 非負に制限した軸平行箱の半寸法
        const DirectX::XMFLOAT3 extents{
            std::max(halfExtents.x, 0.0f),
            std::max(halfExtents.y, 0.0f),
            std::max(halfExtents.z, 0.0f) };
        // 問い合わせ対象へ接触したか
        bool found{};
        // 見つかった最短接触距離
        float nearest =
            std::numeric_limits<float>::max();
        // 問い合わせ対象の形状所有物体
        for (const auto& object : m_gameObjects)
        {
            // 箱の半寸法で拡張したAABBへの最短接触を採用します(bounds: 拡張する境界の写し, box: 箱形状の参照, capsule: カプセルの参照, sphere: 球形状の参照, hull: 凸形状の参照, mesh: メッシュ形状の参照)。
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
                // 境界へ接触する移動距離
                float distance{};
                // 接触面の外向き法線
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
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
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

    // Y軸カプセルを包む箱によるスイープを返します(sourceRay: 原点と未正規化方向, radius: カプセル半径, height: 両端を含む全高, maximumDistance: 移動距離上限, hit: 成功時の接触情報, filter: 対象条件)。
    bool Scene::CapsuleCast(
        const Ray& sourceRay,
        const float radius,
        const float height,
        const float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // 非負に制限した半径
        const float clampedRadius = std::max(radius, 0.0f);
        // 半径以上に制限した全高の半分
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
        // 非負に制限した半径
        const float clampedRadius = std::max(radius, 0.0f);
        // カプセル全体のAABBで一次判定し、線分上のサンプル点との距離で二次判定します（近似）。
        // カプセル全体を包む軸平行境界
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
        // 判定半径の二乗
        const float squaredRadius =
            clampedRadius * clampedRadius;
        // 線分の端点を含む9サンプルから境界までの距離を調べます(bounds: 形状のワールド境界)。
        const auto segmentTouchesBounds =
            [&](const Bounds3D& bounds)
        {
            if (!Overlaps(capsuleBounds, bounds))
            {
                return false;
            }
            // 線分を分割する区間数
            constexpr int SampleCount = 8;
            // 端点を含む線分サンプル番号
            for (int sample = 0;
                sample <= SampleCount;
                ++sample)
            {
                // 線分上のサンプル位置の比率
                const float t =
                    static_cast<float>(sample)
                    / static_cast<float>(SampleCount);
                // 線分上のワールドサンプル位置
                const DirectX::XMFLOAT3 point{
                    start.x + (end.x - start.x) * t,
                    start.y + (end.y - start.y) * t,
                    start.z + (end.z - start.z) * t };
                // 問い合わせ点に最も近い境界点
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
                // 境界の最近接点からのX差
                const float deltaX = point.x - closest.x;
                // 境界の最近接点からのY差
                const float deltaY = point.y - closest.y;
                // 境界の最近接点からのZ差
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

        // 問い合わせ条件に合う接触一覧
        std::vector<PhysicsOverlapHit> hits;
        // 問い合わせ対象の形状所有物体
        for (const auto& object : m_gameObjects)
        {
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
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
        // 問い合わせ条件に合う重なり一覧
        std::vector<PhysicsOverlapHit>
            hits;
        // 問い合わせ対象の形状所有物体
        for (const auto& object :
            m_gameObjects)
        {
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
            auto* mesh =
                object->GetComponent<
                    MeshCollider3DComponent>();
            // AABB通過後に三角形と正確に判定します。
            if (mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(
                    *object,
                    *mesh,
                    filter)
                && Overlaps(
                    bounds,
                    mesh->WorldBounds())
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
        // 問い合わせ条件に合う重なり一覧
        std::vector<PhysicsOverlapHit>
            hits;
        // 判定半径の二乗
        const float squaredRadius =
            std::max(radius, 0.0f)
            * std::max(radius, 0.0f);
        // 問い合わせ対象の形状所有物体
        for (const auto& object :
            m_gameObjects)
        {
            // 球中心からAABBまでの距離が半径以内なら結果へ追加します(bounds: ワールド境界, box: 箱形状の参照, capsule: カプセルの参照, sphere: 球形状の参照, hull: 凸形状の参照)。
            const auto testBounds = [&](
                const Bounds3D& bounds,
                BoxCollider3DComponent* box,
                CapsuleCollider3DComponent* capsule,
                SphereCollider3DComponent* sphere,
                ConvexHullCollider3DComponent* hull)
            {
                // 問い合わせ点に最も近い境界点
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
                // 境界の最近接点からのX差
                const float deltaX =
                    center.x - closest.x;
                // 境界の最近接点からのY差
                const float deltaY =
                    center.y - closest.y;
                // 境界の最近接点からのZ差
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
            // 問い合わせる箱形状
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
            // 問い合わせるカプセル形状
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
            // 問い合わせる球形状
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
            // 問い合わせる凸形状
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
            // 問い合わせるメッシュ形状
            if (auto* mesh = object->GetComponent<
                    MeshCollider3DComponent>();
                mesh != nullptr
                && mesh->HasMesh()
                && AcceptCollider(*object, *mesh, filter))
            {
                // 球のAABBで三角形の有無を判定します（近似）。
                // 問い合わせ球を包む軸平行境界
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
