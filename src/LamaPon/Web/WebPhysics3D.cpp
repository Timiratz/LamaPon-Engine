#include "LamaPon/Web/WebPhysics3D.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace LamaPon::Web
{
    namespace
    {
        // 物理更新の固定間隔（秒）
        constexpr float FixedStep = 1.0f / 60.0f;
        // Ray判定の零方向しきい値
        constexpr float Epsilon = 0.0001f;

        // 軸の成分を読む(value: ベクトル, axis: 0X／1Y／その他Z)。
        [[nodiscard]] float Component(
            const Vec3& value,
            int axis) noexcept
        {
            return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
        }

        // 軸の成分を書き替える(value: ベクトル, axis: 0X／1Y／その他Z, component: 設定値)。
        void SetComponent(Vec3& value, int axis, float component) noexcept
        {
            if (axis == 0)
            {
                value.x = component;
            }
            else if (axis == 1)
            {
                value.y = component;
            }
            else
            {
                value.z = component;
            }
        }

        // RayとAABBの交差を調べる(ray: 検査範囲, center: Box中心, halfExtents: 各軸の半幅, distance: 交差までの距離, normal: 交差面の法線)。
        [[nodiscard]] bool RayBoxIntersection(
            const PhysicsRay& ray,
            const Vec3& center,
            const Vec3& halfExtents,
            float& distance,
            Vec3& normal) noexcept
        {
            // 交差範囲の近端距離
            float nearDistance = 0.0f;
            // 交差範囲の遠端距離
            float farDistance = ray.maxDistance;
            // 最初に交差する面の軸番号
            int nearAxis = -1;
            // 最初に交差する面の向き
            float nearSign = 0.0f;
            // 判定する座標軸番号
            for (int axis = 0; axis < 3; ++axis)
            {
                // Ray始点の軸成分
                const float origin = Component(ray.origin, axis);
                // Ray方向の軸成分
                const float direction = Component(ray.direction, axis);
                // AABBの軸方向の最小値
                const float minimum = Component(center, axis)
                    - Component(halfExtents, axis);
                // AABBの軸方向の最大値
                const float maximum = Component(center, axis)
                    + Component(halfExtents, axis);
                if (std::abs(direction) <= Epsilon)
                {
                    if (origin < minimum || origin > maximum)
                    {
                        return false;
                    }
                    continue;
                }
                // Ray方向成分の逆数
                const float inverseDirection = 1.0f / direction;
                // 軸方向の近端交差距離
                float axisNear = (minimum - origin) * inverseDirection;
                // 軸方向の遠端交差距離
                float axisFar = (maximum - origin) * inverseDirection;
                // 押し戻し・面法線の向き
                float sign = -1.0f;
                if (axisNear > axisFar)
                {
                    std::swap(axisNear, axisFar);
                    sign = 1.0f;
                }
                if (axisNear > nearDistance)
                {
                    nearDistance = axisNear;
                    nearAxis = axis;
                    nearSign = sign;
                }
                farDistance = std::min(farDistance, axisFar);
                if (nearDistance > farDistance)
                {
                    return false;
                }
            }
            distance = nearDistance;
            normal = {};
            if (nearAxis >= 0)
            {
                SetComponent(normal, nearAxis, nearSign);
            }
            return distance >= 0.0f && distance <= ray.maxDistance;
        }
    }

    PhysicsBodyId Physics3D::CreateBody(const PhysicsBodyDesc& desc)
    {
        // 寸法と質量を補正したBody設定
        PhysicsBodyDesc normalized = desc;
        normalized.mass = std::max(normalized.mass, 0.001f);
        normalized.halfExtents.x = std::max(normalized.halfExtents.x, 0.001f);
        normalized.halfExtents.y = std::max(normalized.halfExtents.y, 0.001f);
        normalized.halfExtents.z = std::max(normalized.halfExtents.z, 0.001f);
        normalized.radius = std::max(normalized.radius, 0.001f);
        // 新規Bodyに発行するID
        const PhysicsBodyId id = m_nextBodyId++;
        m_bodies.push_back({ id, normalized, {}, true });
        return id;
    }

    void Physics3D::RemoveBody(PhysicsBodyId body) noexcept
    {
        // IDに対応する有効なBody
        if (Body* found = Find(body); found != nullptr)
        {
            found->active = false;
        }
    }

    void Physics3D::SetLinearVelocity(
        PhysicsBodyId body,
        Vec3 velocity) noexcept
    {
        // IDに対応する有効なBody
        if (Body* found = Find(body); found != nullptr)
        {
            found->velocity = velocity;
        }
    }

    Vec3 Physics3D::LinearVelocity(PhysicsBodyId body) const noexcept
    {
        // IDに対応する有効なBody
        const Body* found = Find(body);
        return found != nullptr ? found->velocity : Vec3{};
    }

    Vec3 Physics3D::Position(PhysicsBodyId body) const noexcept
    {
        // IDに対応する有効なBody
        const Body* found = Find(body);
        return found != nullptr ? found->desc.position : Vec3{};
    }

    void Physics3D::ApplyImpulse(
        PhysicsBodyId body,
        Vec3 impulse) noexcept
    {
        // IDに対応する有効なBody
        if (Body* found = Find(body); found != nullptr && found->desc.dynamic)
        {
            found->velocity += impulse * (1.0f / found->desc.mass);
        }
    }

    void Physics3D::Step(float deltaTime) noexcept
    {
        m_accumulator = std::min(m_accumulator + std::max(deltaTime, 0.0f), 0.25f);
        while (m_accumulator >= FixedStep)
        {
            // 更新・判定するBody
            for (Body& body : m_bodies)
            {
                if (!body.active || !body.desc.dynamic)
                {
                    continue;
                }
                body.velocity += m_gravity * FixedStep;
                body.desc.position += body.velocity * FixedStep;
                ResolveGround(body);
                ResolveStaticBoxes(body);
            }
            m_accumulator -= FixedStep;
        }
    }

    bool Physics3D::Raycast(
        const PhysicsRay& ray,
        PhysicsHit& hit) const noexcept
    {
        // 正規化したRay方向
        const Vec3 direction = Normalize(ray.direction);
        if (LengthSquared(direction) <= Epsilon)
        {
            return false;
        }
        // 単位方向に補正したRay
        PhysicsRay normalizedRay = ray;
        normalizedRay.direction = direction;
        // 現在最も近い交差距離
        float closest = std::numeric_limits<float>::max();
        // 交差を見つけたか
        bool found = false;
        // 更新・判定するBody
        for (const Body& body : m_bodies)
        {
            if (!body.active)
            {
                continue;
            }
            // 候補Bodyまでの交差距離
            float distance = 0.0f;
            // 候補Bodyの交差面法線
            Vec3 normal{};
            // 候補Bodyと交差したか
            bool intersects = false;
            if (body.desc.shape == PhysicsShape::Sphere)
            {
                // Body中心からRay始点への差
                const Vec3 offset = normalizedRay.origin - body.desc.position;
                // 中心から始点への差の方向成分
                const float projection = Dot(offset, normalizedRay.direction);
                // RayとSphereの交差判別式
                const float discriminant = projection * projection
                    - (LengthSquared(offset) - body.desc.radius * body.desc.radius);
                if (discriminant >= 0.0f)
                {
                    distance = -projection - std::sqrt(discriminant);
                    if (distance < 0.0f)
                    {
                        distance = -projection + std::sqrt(discriminant);
                    }
                    intersects = distance >= 0.0f
                        && distance <= normalizedRay.maxDistance;
                    if (intersects)
                    {
                        // Sphere表面の交差位置
                        const Vec3 point = normalizedRay.origin
                            + normalizedRay.direction * distance;
                        normal = Normalize(point - body.desc.position);
                    }
                }
            }
            else
            {
                intersects = RayBoxIntersection(
                    normalizedRay,
                    body.desc.position,
                    body.desc.halfExtents,
                    distance,
                    normal);
            }
            if (intersects && distance < closest)
            {
                closest = distance;
                hit = {
                    body.id,
                    normalizedRay.origin + normalizedRay.direction * distance,
                    normal,
                    distance,
                };
                found = true;
            }
        }
        return found;
    }

    Physics3D::Body* Physics3D::Find(PhysicsBodyId body) noexcept
    {
        // 検索中のBody
        for (Body& candidate : m_bodies)
        {
            if (candidate.active && candidate.id == body)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    const Physics3D::Body* Physics3D::Find(PhysicsBodyId body) const noexcept
    {
        // 検索中のBody
        for (const Body& candidate : m_bodies)
        {
            if (candidate.active && candidate.id == body)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    void Physics3D::ResolveGround(Body& body) noexcept
    {
        // Body下端のY位置
        const float bottom = body.desc.shape == PhysicsShape::Sphere
            ? body.desc.position.y - body.desc.radius
            : body.desc.position.y - body.desc.halfExtents.y;
        if (bottom < 0.0f)
        {
            body.desc.position.y -= bottom;
            if (body.velocity.y < 0.0f)
            {
                body.velocity.y = -body.velocity.y * 0.08f;
                if (std::abs(body.velocity.y) < 0.05f)
                {
                    body.velocity.y = 0.0f;
                }
            }
            body.velocity.x *= 0.985f;
            body.velocity.z *= 0.985f;
        }
    }

    void Physics3D::ResolveStaticBoxes(Body& body) noexcept
    {
        // 静的SphereもhalfExtentsのAABBとして扱い、動的Sphereは外接AABBで押し戻す。
        // 押し戻し判定の静的Body
        for (const Body& obstacle : m_bodies)
        {
            if (!obstacle.active || obstacle.desc.dynamic || &obstacle == &body)
            {
                continue;
            }
            // 動的Bodyの外接AABBの半幅
            const Vec3 movingExtents = body.desc.shape == PhysicsShape::Sphere
                ? Vec3{ body.desc.radius, body.desc.radius, body.desc.radius }
                : body.desc.halfExtents;
            // 静的Body中心から動的中心への差
            const Vec3 delta = body.desc.position - obstacle.desc.position;
            // 各軸のAABB重なり幅
            const Vec3 overlap{
                obstacle.desc.halfExtents.x + movingExtents.x - std::abs(delta.x),
                obstacle.desc.halfExtents.y + movingExtents.y - std::abs(delta.y),
                obstacle.desc.halfExtents.z + movingExtents.z - std::abs(delta.z),
            };
            if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f)
            {
                continue;
            }
            if (overlap.x < overlap.y && overlap.x < overlap.z)
            {
                // 押し戻し・面法線の向き
                const float sign = delta.x < 0.0f ? -1.0f : 1.0f;
                body.desc.position.x += overlap.x * sign;
                body.velocity.x = 0.0f;
            }
            else if (overlap.y < overlap.z)
            {
                // 押し戻し・面法線の向き
                const float sign = delta.y < 0.0f ? -1.0f : 1.0f;
                body.desc.position.y += overlap.y * sign;
                body.velocity.y = 0.0f;
            }
            else
            {
                // 押し戻し・面法線の向き
                const float sign = delta.z < 0.0f ? -1.0f : 1.0f;
                body.desc.position.z += overlap.z * sign;
                body.velocity.z = 0.0f;
            }
        }
    }
}
