#include "LamaPon/Web/WebPhysics3D.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace LamaPon::Web
{
    namespace
    {
        constexpr float FixedStep = 1.0f / 60.0f;
        constexpr float Epsilon = 0.0001f;

        [[nodiscard]] float Component(
            const Vec3& value,
            int axis) noexcept
        {
            return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
        }

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

        [[nodiscard]] bool RayBoxIntersection(
            const PhysicsRay& ray,
            const Vec3& center,
            const Vec3& halfExtents,
            float& distance,
            Vec3& normal) noexcept
        {
            float nearDistance = 0.0f;
            float farDistance = ray.maxDistance;
            int nearAxis = -1;
            float nearSign = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float origin = Component(ray.origin, axis);
                const float direction = Component(ray.direction, axis);
                const float minimum = Component(center, axis)
                    - Component(halfExtents, axis);
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
                const float inverseDirection = 1.0f / direction;
                float axisNear = (minimum - origin) * inverseDirection;
                float axisFar = (maximum - origin) * inverseDirection;
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
        PhysicsBodyDesc normalized = desc;
        normalized.mass = std::max(normalized.mass, 0.001f);
        normalized.halfExtents.x = std::max(normalized.halfExtents.x, 0.001f);
        normalized.halfExtents.y = std::max(normalized.halfExtents.y, 0.001f);
        normalized.halfExtents.z = std::max(normalized.halfExtents.z, 0.001f);
        normalized.radius = std::max(normalized.radius, 0.001f);
        const PhysicsBodyId id = m_nextBodyId++;
        m_bodies.push_back({ id, normalized, {}, true });
        return id;
    }

    void Physics3D::RemoveBody(PhysicsBodyId body) noexcept
    {
        if (Body* found = Find(body); found != nullptr)
        {
            found->active = false;
        }
    }

    void Physics3D::SetLinearVelocity(
        PhysicsBodyId body,
        Vec3 velocity) noexcept
    {
        if (Body* found = Find(body); found != nullptr)
        {
            found->velocity = velocity;
        }
    }

    Vec3 Physics3D::LinearVelocity(PhysicsBodyId body) const noexcept
    {
        const Body* found = Find(body);
        return found != nullptr ? found->velocity : Vec3{};
    }

    Vec3 Physics3D::Position(PhysicsBodyId body) const noexcept
    {
        const Body* found = Find(body);
        return found != nullptr ? found->desc.position : Vec3{};
    }

    void Physics3D::ApplyImpulse(
        PhysicsBodyId body,
        Vec3 impulse) noexcept
    {
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
        const Vec3 direction = Normalize(ray.direction);
        if (LengthSquared(direction) <= Epsilon)
        {
            return false;
        }
        PhysicsRay normalizedRay = ray;
        normalizedRay.direction = direction;
        float closest = std::numeric_limits<float>::max();
        bool found = false;
        for (const Body& body : m_bodies)
        {
            if (!body.active)
            {
                continue;
            }
            float distance = 0.0f;
            Vec3 normal{};
            bool intersects = false;
            if (body.desc.shape == PhysicsShape::Sphere)
            {
                const Vec3 offset = normalizedRay.origin - body.desc.position;
                const float projection = Dot(offset, normalizedRay.direction);
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
        // dynamic=falseの物体は静的なAABBとして衝突を解決します。
        for (const Body& obstacle : m_bodies)
        {
            if (!obstacle.active || obstacle.desc.dynamic || &obstacle == &body)
            {
                continue;
            }
            const Vec3 movingExtents = body.desc.shape == PhysicsShape::Sphere
                ? Vec3{ body.desc.radius, body.desc.radius, body.desc.radius }
                : body.desc.halfExtents;
            const Vec3 delta = body.desc.position - obstacle.desc.position;
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
                const float sign = delta.x < 0.0f ? -1.0f : 1.0f;
                body.desc.position.x += overlap.x * sign;
                body.velocity.x = 0.0f;
            }
            else if (overlap.y < overlap.z)
            {
                const float sign = delta.y < 0.0f ? -1.0f : 1.0f;
                body.desc.position.y += overlap.y * sign;
                body.velocity.y = 0.0f;
            }
            else
            {
                const float sign = delta.z < 0.0f ? -1.0f : 1.0f;
                body.desc.position.z += overlap.z * sign;
                body.velocity.z = 0.0f;
            }
        }
    }
}
