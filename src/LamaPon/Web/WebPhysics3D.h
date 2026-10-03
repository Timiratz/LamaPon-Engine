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
        // Ray判定と地面判定の形状
        PhysicsShape shape{ PhysicsShape::Box };
        // Bodyの中心位置
        Vec3 position{};
        // AABBの各軸の半幅
        Vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
        // Sphereの半径
        float radius{ 0.5f };
        // Bodyの質量
        float mass{ 1.0f };
        // 重力と力積で動くか
        bool dynamic{ true };
    };

    struct PhysicsRay final
    {
        // Rayの始点
        Vec3 origin{};
        // 正規化前のRay方向
        Vec3 direction{ 0.0f, -1.0f, 0.0f };
        // 検査する最大距離
        float maxDistance{ 1000.0f };
    };

    struct PhysicsHit final
    {
        // 交差したBodyのID
        PhysicsBodyId body{};
        // 最も近い交差位置
        Vec3 point{};
        // 交差面の法線
        Vec3 normal{};
        // 始点から交差までの距離
        float distance{};
    };

    // 動的Body同士の衝突は扱わず、地面と静的AABBだけで衝突を解決する。
    class Physics3D final
    {
    public:
        // 既定重力と空のBody一覧を用意する。
        Physics3D() = default;

        // 重力加速度を設定する(gravity: 各軸の加速度)。
        void SetGravity(Vec3 gravity) noexcept { m_gravity = gravity; }
        // 寸法と質量を最小0.001へ補正してBodyを登録する(desc: 形状と初期状態)。
        [[nodiscard]] PhysicsBodyId CreateBody(const PhysicsBodyDesc& desc);
        // Bodyを無効化し、未知のIDは無視する(body: Body ID)。
        void RemoveBody(PhysicsBodyId body) noexcept;
        // Bodyの速度を設定し、未知のIDは無視する(body: Body ID, velocity: 各軸の速度)。
        void SetLinearVelocity(PhysicsBodyId body, Vec3 velocity) noexcept;
        // Bodyの速度を返し、未知のIDは零とする(body: Body ID)。
        [[nodiscard]] Vec3 LinearVelocity(PhysicsBodyId body) const noexcept;
        // Bodyの位置を返し、未知のIDは零とする(body: Body ID)。
        [[nodiscard]] Vec3 Position(PhysicsBodyId body) const noexcept;
        // 動的Bodyの速度へ力積を反映する(body: Body ID, impulse: 各軸の力積)。
        void ApplyImpulse(PhysicsBodyId body, Vec3 impulse) noexcept;

        // 最大0.25秒を蓄積して1/60秒ずつ物理更新する(deltaTime: 経過秒数)。
        void Step(float deltaTime) noexcept;
        // AABB内部からのRayは距離0・零法線で命中する。
        // 正規化したRayに最も近いBodyを返す(ray: 検査範囲, hit: 成功時のみ更新する結果)。
        [[nodiscard]] bool Raycast(
            const PhysicsRay& ray,
            PhysicsHit& hit) const noexcept;

    private:
        struct Body final
        {
            // 登録時に発行するBody ID
            PhysicsBodyId id{};
            // 形状と現在の位置
            PhysicsBodyDesc desc{};
            // 各軸の速度
            Vec3 velocity{};
            // 検索・更新・判定の対象か
            bool active{};
        };

        // 有効なBodyを検索し、未知のIDはnullを返す(body: Body ID)。
        [[nodiscard]] Body* Find(PhysicsBodyId body) noexcept;
        // 有効なBodyを参照し、未知のIDはnullを返す(body: Body ID)。
        [[nodiscard]] const Body* Find(PhysicsBodyId body) const noexcept;
        // 動的BodyをY=0の地面から押し戻す(body: 対象Body)。
        void ResolveGround(Body& body) noexcept;
        // 静的BodyをAABBとみなして動的Bodyを押し戻す(body: 対象Body)。
        void ResolveStaticBoxes(Body& body) noexcept;

        // 全動的Bodyの重力加速度
        Vec3 m_gravity{ 0.0f, -18.0f, 0.0f };
        // 無効化したBodyも保持する一覧
        std::vector<Body> m_bodies;
        // 次に発行するBody ID
        std::uint32_t m_nextBodyId{ 1 };
        // 固定更新に未消費の秒数
        float m_accumulator{};
    };
}
