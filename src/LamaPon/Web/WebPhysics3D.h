#pragma once

#include "LamaPon/Web/WebMath.h"

#include <cstdint>
#include <vector>

namespace LamaPon::Web
{
    using PhysicsBodyId = std::uint32_t;

    enum class PhysicsShape : std::uint8_t
    {
        Box,
        Sphere,
    };

    struct PhysicsBodyDesc final
    {
        PhysicsShape shape{ PhysicsShape::Box };
        Vec3 position{};
        Vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
        float radius{ 0.5f };
        float mass{ 1.0f };
        bool dynamic{ true };
    };

    struct PhysicsRay final
    {
        Vec3 origin{};
        Vec3 direction{ 0.0f, -1.0f, 0.0f };
        float maxDistance{ 1000.0f };
    };

    struct PhysicsHit final
    {
        PhysicsBodyId body{};
        Vec3 point{};
        Vec3 normal{};
        float distance{};
    };

    // Web向けの決定論的で小規模な3D物理機能を提供します。
    // ネイティブの物理DLLには依存しません。高度な物理機能が必要な
    // プロジェクトでは、別のWebモジュールへ置き換えられます。
    class Physics3D final
    {
    public:
        Physics3D() = default;

        void SetGravity(Vec3 gravity) noexcept { m_gravity = gravity; }
        [[nodiscard]] PhysicsBodyId CreateBody(const PhysicsBodyDesc& desc);
        void RemoveBody(PhysicsBodyId body) noexcept;
        void SetLinearVelocity(PhysicsBodyId body, Vec3 velocity) noexcept;
        [[nodiscard]] Vec3 LinearVelocity(PhysicsBodyId body) const noexcept;
        [[nodiscard]] Vec3 Position(PhysicsBodyId body) const noexcept;
        void ApplyImpulse(PhysicsBodyId body, Vec3 impulse) noexcept;

        void Step(float deltaTime) noexcept;
        [[nodiscard]] bool Raycast(
            const PhysicsRay& ray,
            PhysicsHit& hit) const noexcept;

    private:
        struct Body final
        {
            PhysicsBodyId id{};
            PhysicsBodyDesc desc{};
            Vec3 velocity{};
            bool active{};
        };

        [[nodiscard]] Body* Find(PhysicsBodyId body) noexcept;
        [[nodiscard]] const Body* Find(PhysicsBodyId body) const noexcept;
        void ResolveGround(Body& body) noexcept;
        void ResolveStaticBoxes(Body& body) noexcept;

        Vec3 m_gravity{ 0.0f, -18.0f, 0.0f };
        std::vector<Body> m_bodies;
        std::uint32_t m_nextBodyId{ 1 };
        float m_accumulator{};
    };
}
