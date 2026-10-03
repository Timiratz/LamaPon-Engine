#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    enum class CollisionDetectionMode
    {
        // 各ステップの接触だけで判定します。
        Discrete,
        // 高速時に分割と近似スイープを使います。
        Continuous
    };

    enum class ForceMode
    {
        // 力・トルクを蓄積して質量・慣性と経過時間を使います。
        Force,
        // 加速度・角加速度を蓄積して経過時間を使います。
        Acceleration,
        // 力積・角力積を質量・慣性で換算して即時適用します。
        Impulse,
        // 速度・角速度の差を即時適用します。
        VelocityChange
    };

    struct RigidbodyConstraints final
    {
        // X回転を凍結する指定
        bool freezeRotationX{};
        // Y回転を凍結する指定
        bool freezeRotationY{};
        // Z回転を凍結する指定
        bool freezeRotationZ{};
        // ワールドX移動の凍結指定
        bool freezePositionX{};
        // ワールドY移動の凍結指定
        bool freezePositionY{};
        // ワールドZ移動の凍結指定
        bool freezePositionZ{};
    };

    // 状態・作用量・経過時間の数値には有限値を指定します。
    class RigidbodyComponent final : public Component
    {
    public:
        // 剛体の初期状態を設定します(velocity: ワールド速度毎秒, useGravity: 重力を使う指定, isKinematic: 外力を無視する指定, collisionDetection: 衝突判定方式, mass: 質量, angularVelocity: ワールド角速度ラジアン毎秒, centerOfMass: ローカル重心, linearDrag: 線形減衰係数, angularDrag: 角減衰係数, constraints: 軸の凍結指定, interpolate: 描画補間の指定)。
        explicit RigidbodyComponent(
            DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f },
            bool useGravity = true,
            bool isKinematic = false,
            CollisionDetectionMode collisionDetection =
                CollisionDetectionMode::Discrete,
            float mass = 1.0f,
            DirectX::XMFLOAT3 angularVelocity = {},
            DirectX::XMFLOAT3 centerOfMass = {},
            float linearDrag = 0.0f,
            float angularDrag = 0.05f,
            RigidbodyConstraints constraints = {},
            bool interpolate = true) noexcept;

        // ワールド速度を毎秒単位で返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Velocity() const noexcept
        {
            return m_velocity;
        }
        // 速度を設定し十分な非ゼロ値なら休止を解除します(velocity: ワールド速度毎秒)。
        void SetVelocity(
            const DirectX::XMFLOAT3& velocity) noexcept;
        // ワールド角速度をラジアン毎秒で返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        // 凍結軸を除いて角速度を設定し必要なら休止を解除します(value: ワールド角速度ラジアン毎秒)。
        void SetAngularVelocity(
            DirectX::XMFLOAT3 value) noexcept;
        // 剛体の質量を返します。
        [[nodiscard]] float Mass() const noexcept { return m_mass; }
        // 質量を0.0001以上に制限して設定します(value: 質量)。
        void SetMass(float value) noexcept;
        // キネマティックなら0、それ以外は質量の逆数を返します。
        [[nodiscard]] float InverseMass() const noexcept
        {
            return !m_isKinematic
                ? 1.0f / m_mass
                : 0.0f;
        }
        // ローカル重心を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            CenterOfMass() const noexcept
        {
            return m_centerOfMass;
        }
        // ローカル重心を設定します(value: ローカルXYZ位置)。
        void SetCenterOfMass(
            const DirectX::XMFLOAT3& value) noexcept
        {
            m_centerOfMass = value;
        }
        // 線形速度の減衰係数を返します。
        [[nodiscard]] float LinearDrag() const noexcept
        {
            return m_linearDrag;
        }
        // 線形減衰係数を非負に制限して設定します(value: 線形減衰係数)。
        void SetLinearDrag(float value) noexcept;
        // 角速度の減衰係数を返します。
        [[nodiscard]] float AngularDrag() const noexcept
        {
            return m_angularDrag;
        }
        // 角減衰係数を非負に制限して設定します(value: 角減衰係数)。
        void SetAngularDrag(float value) noexcept;
        // 移動・回転の凍結指定を返します。
        [[nodiscard]] const RigidbodyConstraints&
            Constraints() const noexcept
        {
            return m_constraints;
        }
        // 軸の凍結指定を設定して角速度に反映します(value: 軸の凍結指定)。
        void SetConstraints(
            RigidbodyConstraints value) noexcept;

        // 重力を使う指定を返します。
        [[nodiscard]] bool UsesGravity() const noexcept { return m_useGravity; }
        // 重力の使用を設定します(useGravity: 重力を使う指定)。
        void SetUseGravity(const bool useGravity) noexcept { m_useGravity = useGravity; }

        // 力や重力を無視する指定を返します。
        [[nodiscard]] bool IsKinematic() const noexcept { return m_isKinematic; }
        // キネマティック指定を設定して休止を解除します(kinematic: 外力を無視する指定)。
        void SetKinematic(bool kinematic) noexcept;

        // 衝突判定方式を返します。
        [[nodiscard]] CollisionDetectionMode CollisionDetection() const noexcept
        {
            return m_collisionDetection;
        }
        // 判定方式を設定し高速離散判定の通知済み状態を解除します(value: 衝突判定方式)。
        void SetCollisionDetection(
            const CollisionDetectionMode value) noexcept
        {
            m_collisionDetection = value;
            m_discreteSpeedWarned = false;
        }
        // 描画補間を使う指定を返します。
        [[nodiscard]] bool Interpolates() const noexcept
        {
            return m_interpolate;
        }
        // 描画補間の使用を設定します(value: 補間する指定)。
        void SetInterpolate(
            const bool value) noexcept
        {
            m_interpolate = value;
        }
        // 連続衝突判定を使うか返します。
        [[nodiscard]] bool UsesContinuousCollisionDetection() const noexcept
        {
            return m_collisionDetection == CollisionDetectionMode::Continuous;
        }

        // 外力と減衰を速度に反映して移動・回転します(gameObject: この剛体で動かす物体, deltaTime: 積分する経過秒数)。
        // 休止中は更新せず、蓄積力は消去しないため全サブステップ後にClearAccumulatorsを呼びます。
        void Integrate(GameObject& gameObject, float deltaTime) noexcept;
        // 蓄積した力・トルク・加速度をすべて消去します。
        void ClearAccumulators() noexcept;
        // 積分を停止する休止状態か返します。
        [[nodiscard]] bool IsSleeping() const noexcept
        {
            return m_isSleeping;
        }
        // 非キネマティックなら休止し速度と蓄積力を消去します。
        void Sleep() noexcept;
        // 休止状態と休止待ち時間を解除します。
        void WakeUp() noexcept;
        // 低速で支持された時間を積算して休止を判定します(deltaTime: 絶対値を使う経過秒数, supported: 支持されている指定)。
        void UpdateSleepState(
            float deltaTime,
            bool supported) noexcept;
        // 剛体を起こして外力を適用しキネマティックなら無視します(force: ワールド作用量, mode: 力・加速度・力積・速度差の方式)。
        // 力・加速度は蓄積し、力積・速度差は即時適用します。
        void AddForce(
            const DirectX::XMFLOAT3& force,
            ForceMode mode = ForceMode::Force) noexcept;
        // 剛体を起こして回転作用を適用しキネマティックなら無視します(torque: ワールド回転作用量, mode: 回転作用量の適用方式)。
        // トルク・角加速度は蓄積し、角力積・角速度差は即時適用します。
        void AddTorque(
            const DirectX::XMFLOAT3& torque,
            ForceMode mode = ForceMode::Force) noexcept;
        // 指定点の外力と重心回りの回転作用を適用します(force: 方式に応じたワールド作用量, worldPosition: 作用点, mode: 作用量の適用方式)。
        void AddForceAtPosition(
            const DirectX::XMFLOAT3& force,
            const DirectX::XMFLOAT3& worldPosition,
            ForceMode mode = ForceMode::Force) noexcept;
        // ローカル重心をワールド変換した位置を返します。
        [[nodiscard]] DirectX::XMFLOAT3
            WorldCenterOfMass() const noexcept;
        // 並進と重心回りの回転を合成した点の速度を返します(worldPosition: 評価するワールド位置)。
        [[nodiscard]] DirectX::XMFLOAT3
            VelocityAtPoint(
                const DirectX::XMFLOAT3& worldPosition) const noexcept;
        // 箱で近似した慣性の逆数を作用量に適用しキネマティックなら0を返します(worldTorque: ワールド回転作用量)。
        // 自身の最初の対応形状または子孫の境界を使い、無効形状・子孫の別剛体も区別しません。
        // noexcept内の領域確保に失敗すると終了します。
        [[nodiscard]] DirectX::XMFLOAT3
            ApplyInverseInertia(
                const DirectX::XMFLOAT3& worldTorque) const noexcept;
        // 指定点の力積を並進・角速度に適用します(impulse: ワールド力積, worldPosition: 作用点)。
        void ApplyImpulseAtPoint(
            const DirectX::XMFLOAT3& impulse,
            const DirectX::XMFLOAT3& worldPosition) noexcept;
        // 面へ入り込む速度成分を除去します(surfaceNormal: 外向きのワールド単位法線)。
        void RemoveInwardVelocity(const DirectX::XMFLOAT3& surfaceNormal) noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Rigidbody";
        }

    private:
        // 凍結軸を除いて角速度を設定します(value: ワールド角速度ラジアン毎秒, wake: 必要なら休止を解除する指定)。
        void SetAngularVelocityInternal(
            DirectX::XMFLOAT3 value,
            bool wake) noexcept;

        // ワールド速度毎秒
        DirectX::XMFLOAT3 m_velocity;
        // ワールド角速度ラジアン毎秒
        DirectX::XMFLOAT3 m_angularVelocity{};
        // ローカル重心位置
        DirectX::XMFLOAT3 m_centerOfMass{};
        // 蓄積したワールド力
        DirectX::XMFLOAT3 m_accumulatedForce{};
        // 蓄積したワールドトルク
        DirectX::XMFLOAT3 m_accumulatedTorque{};
        // 蓄積したワールド加速度
        DirectX::XMFLOAT3 m_accumulatedAcceleration{};
        // 蓄積したワールド角加速度
        DirectX::XMFLOAT3 m_accumulatedAngularAcceleration{};
        // 剛体の質量
        float m_mass{ 1.0f };
        // 線形速度の減衰係数
        float m_linearDrag{};
        // 角速度の減衰係数
        float m_angularDrag{ 0.05f };
        // 移動・回転の凍結指定
        RigidbodyConstraints m_constraints;
        // 重力を使う指定
        bool m_useGravity{ true };
        // 外力を無視する指定
        bool m_isKinematic{};
        // 積分を停止する休止状態
        bool m_isSleeping{};
        // 高速離散判定の通知済み状態
        bool m_discreteSpeedWarned{};
        // 描画補間を使う指定
        bool m_interpolate{ true };
        // 休止を待つ時間秒
        float m_sleepTimer{};
        // 衝突判定方式
        CollisionDetectionMode m_collisionDetection{
            CollisionDetectionMode::Discrete
        };
    };
}
