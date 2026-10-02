#include "LamaPon/Components/RigidbodyComponent.h"

#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace
{
    // 三成分の和を返します(left: 一方のベクトル, right: もう一方のベクトル)。
    DirectX::XMFLOAT3 Add(
        const DirectX::XMFLOAT3& left,
        const DirectX::XMFLOAT3& right) noexcept
    {
        return {
            left.x + right.x,
            left.y + right.y,
            left.z + right.z
        };
    }

    // 三成分を同じ倍率で拡縮します(value: ベクトル, scale: 倍率)。
    DirectX::XMFLOAT3 Multiply(
        const DirectX::XMFLOAT3& value,
        const float scale) noexcept
    {
        return {
            value.x * scale,
            value.y * scale,
            value.z * scale
        };
    }

    // 三成分の外積を返します(left: 左ベクトル, right: 右ベクトル)。
    DirectX::XMFLOAT3 Cross(
        const DirectX::XMFLOAT3& left,
        const DirectX::XMFLOAT3& right) noexcept
    {
        return {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x
        };
    }

    // 三成分の内積を返します(left: 一方のベクトル, right: もう一方のベクトル)。
    float Dot(
        const DirectX::XMFLOAT3& left,
        const DirectX::XMFLOAT3& right) noexcept
    {
        return left.x * right.x
            + left.y * right.y
            + left.z * right.z;
    }
}

namespace LamaPon
{
    RigidbodyComponent::RigidbodyComponent(
        const DirectX::XMFLOAT3 velocity,
        const bool useGravity,
        const bool isKinematic,
        const CollisionDetectionMode collisionDetection,
        const float mass,
        const DirectX::XMFLOAT3 angularVelocity,
        const DirectX::XMFLOAT3 centerOfMass,
        const float linearDrag,
        const float angularDrag,
        const RigidbodyConstraints constraints,
        const bool interpolate) noexcept
        : m_velocity(velocity)
        , m_angularVelocity(angularVelocity)
        , m_centerOfMass(centerOfMass)
        , m_mass(std::max(mass, 0.0001f))
        , m_linearDrag(std::max(linearDrag, 0.0f))
        , m_angularDrag(std::max(angularDrag, 0.0f))
        , m_constraints(constraints)
        , m_useGravity(useGravity)
        , m_isKinematic(isKinematic)
        , m_interpolate(interpolate)
        , m_collisionDetection(collisionDetection)
    {
        SetAngularVelocity(m_angularVelocity);
    }

    void RigidbodyComponent::SetAngularVelocity(
        DirectX::XMFLOAT3 value) noexcept
    {
        SetAngularVelocityInternal(value, true);
    }

    void RigidbodyComponent::SetAngularVelocityInternal(
        DirectX::XMFLOAT3 value,
        const bool wake) noexcept
    {
        if (m_constraints.freezeRotationX) value.x = 0.0f;
        if (m_constraints.freezeRotationY) value.y = 0.0f;
        if (m_constraints.freezeRotationZ) value.z = 0.0f;
        m_angularVelocity = value;
        if (wake
            && Dot(value, value) > 0.000001f)
        {
            WakeUp();
        }
    }

    void RigidbodyComponent::SetVelocity(
        const DirectX::XMFLOAT3& velocity) noexcept
    {
        m_velocity = velocity;
        if (Dot(velocity, velocity) > 0.000001f)
        {
            WakeUp();
        }
    }

    void RigidbodyComponent::SetKinematic(
        const bool kinematic) noexcept
    {
        m_isKinematic = kinematic;
        WakeUp();
    }

    void RigidbodyComponent::SetMass(const float value) noexcept
    {
        m_mass = std::max(value, 0.0001f);
    }

    void RigidbodyComponent::SetLinearDrag(const float value) noexcept
    {
        m_linearDrag = std::max(value, 0.0f);
    }

    void RigidbodyComponent::SetAngularDrag(const float value) noexcept
    {
        m_angularDrag = std::max(value, 0.0f);
    }

    void RigidbodyComponent::SetConstraints(
        const RigidbodyConstraints value) noexcept
    {
        m_constraints = value;
        SetAngularVelocity(m_angularVelocity);
    }

    DirectX::XMFLOAT3
        RigidbodyComponent::WorldCenterOfMass() const noexcept
    {
        // 変換したワールド重心
        DirectX::XMFLOAT3 result{};
        DirectX::XMStoreFloat3(
            &result,
            DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(
                    &m_centerOfMass),
                Owner().WorldMatrix()));
        return result;
    }

    DirectX::XMFLOAT3
        RigidbodyComponent::VelocityAtPoint(
            const DirectX::XMFLOAT3&
                worldPosition) const noexcept
    {
        // 剛体のワールド重心
        const auto center =
            WorldCenterOfMass();
        // 重心から作用点への変位
        const DirectX::XMFLOAT3 radius{
            worldPosition.x - center.x,
            worldPosition.y - center.y,
            worldPosition.z - center.z
        };
        return Add(
            m_velocity,
            Cross(m_angularVelocity, radius));
    }

    DirectX::XMFLOAT3
        RigidbodyComponent::ApplyInverseInertia(
            const DirectX::XMFLOAT3&
                worldTorque) const noexcept
    {
        if (m_isKinematic)
        {
            return {};
        }

        // 慣性近似箱のXYZ全幅
        DirectX::XMFLOAT3 dimensions{
            1.0f,
            1.0f,
            1.0f
        };
        // 自身の3D箱コライダー
        if (const auto* box =
                Owner().GetComponent<
                    BoxCollider3DComponent>())
        {
            // ワールド向き付き箱の形状
            const auto worldBox =
                box->WorldBox();
            dimensions = {
                std::max(
                    worldBox.halfExtents.x * 2.0f,
                    0.01f),
                std::max(
                    worldBox.halfExtents.y * 2.0f,
                    0.01f),
                std::max(
                    worldBox.halfExtents.z * 2.0f,
                    0.01f)
            };
        }
        // 自身のカプセルコライダー
        else if (const auto* capsule =
                Owner().GetComponent<
                    CapsuleCollider3DComponent>())
        {
            // ワールドカプセルの形状
            const auto worldCapsule =
                capsule->WorldCapsule();
            // カプセルの中心線の変位
            const DirectX::XMFLOAT3 segment{
                worldCapsule.end.x
                    - worldCapsule.start.x,
                worldCapsule.end.y
                    - worldCapsule.start.y,
                worldCapsule.end.z
                    - worldCapsule.start.z
            };
            // 形状のワールド直径
            const float diameter =
                worldCapsule.radius * 2.0f;
            // カプセルのワールド全高
            const float totalHeight =
                std::sqrt(Dot(segment, segment))
                    + diameter;
            dimensions = {
                std::max(diameter, 0.01f),
                std::max(totalHeight, 0.01f),
                std::max(diameter, 0.01f)
            };
        }
        // 自身の球コライダー
        else if (const auto* sphere =
                Owner().GetComponent<
                    SphereCollider3DComponent>())
        {
            // 形状のワールド直径
            const float diameter =
                sphere->WorldSphere().radius
                * 2.0f;
            dimensions = {
                std::max(diameter, 0.01f),
                std::max(diameter, 0.01f),
                std::max(diameter, 0.01f)
            };
        }
        // 自身の凸包コライダー
        else if (const auto* hull =
                Owner().GetComponent<
                    ConvexHullCollider3DComponent>())
        {
            // 形状のワールド境界
            const auto bounds = hull->WorldBounds();
            dimensions = {
                std::max(
                    bounds.maximum.x - bounds.minimum.x,
                    0.01f),
                std::max(
                    bounds.maximum.y - bounds.minimum.y,
                    0.01f),
                std::max(
                    bounds.maximum.z - bounds.minimum.z,
                    0.01f)
            };
        }
        // 自身の2D箱コライダー
        else if (const auto* box2D =
                Owner().GetComponent<
                    BoxCollider2DComponent>())
        {
            // 形状のワールド境界
            const auto bounds =
                box2D->WorldBounds();
            dimensions = {
                std::max(
                    bounds.maximum.x
                        - bounds.minimum.x,
                    0.01f),
                std::max(
                    bounds.maximum.y
                        - bounds.minimum.y,
                    0.01f),
                0.1f
            };
        }
        // 自身の2D凸多角形コライダー
        else if (const auto* polygon2D =
                Owner().GetComponent<
                    PolygonCollider2DComponent>())
        {
            // 形状のワールド境界
            const auto bounds =
                polygon2D->WorldBounds();
            dimensions = {
                std::max(
                    bounds.maximum.x
                        - bounds.minimum.x,
                    0.01f),
                std::max(
                    bounds.maximum.y
                        - bounds.minimum.y,
                    0.01f),
                0.1f
            };
        }
        else
        {
            // 子孫の形状境界を取得済み
            bool foundBounds{};
            // 子孫形状を囲む合成境界
            Bounds3D compoundBounds{};
            // 形状を調べる子孫の探索スタック
            std::vector<const GameObject*> pending;
            // 形状を探索する子物体
            for (const auto* child :
                Owner().Children())
            {
                pending.push_back(child);
            }
            while (!pending.empty())
            {
                // 形状を探索する子孫物体
                const auto* object = pending.back();
                pending.pop_back();
                // 形状のワールド境界
                std::optional<Bounds3D> bounds;
                // 子孫の3D箱コライダー
                if (const auto* childBox =
                    object->GetComponent<
                        BoxCollider3DComponent>())
                {
                    bounds = childBox->WorldBounds();
                }
                // 子孫のカプセルコライダー
                else if (const auto* childCapsule =
                    object->GetComponent<
                        CapsuleCollider3DComponent>())
                {
                    bounds = childCapsule->WorldBounds();
                }
                // 子孫の球コライダー
                else if (const auto* childSphere =
                    object->GetComponent<
                        SphereCollider3DComponent>())
                {
                    bounds = childSphere->WorldBounds();
                }
                // 子孫の凸包コライダー
                else if (const auto* childHull =
                    object->GetComponent<
                        ConvexHullCollider3DComponent>())
                {
                    bounds = childHull->WorldBounds();
                }
                if (bounds)
                {
                    if (!foundBounds)
                    {
                        compoundBounds = *bounds;
                        foundBounds = true;
                    }
                    else
                    {
                        compoundBounds.minimum.x =
                            std::min(
                                compoundBounds.minimum.x,
                                bounds->minimum.x);
                        compoundBounds.minimum.y =
                            std::min(
                                compoundBounds.minimum.y,
                                bounds->minimum.y);
                        compoundBounds.minimum.z =
                            std::min(
                                compoundBounds.minimum.z,
                                bounds->minimum.z);
                        compoundBounds.maximum.x =
                            std::max(
                                compoundBounds.maximum.x,
                                bounds->maximum.x);
                        compoundBounds.maximum.y =
                            std::max(
                                compoundBounds.maximum.y,
                                bounds->maximum.y);
                        compoundBounds.maximum.z =
                            std::max(
                                compoundBounds.maximum.z,
                                bounds->maximum.z);
                    }
                }
                // 形状を探索する子物体
                for (const auto* child :
                    object->Children())
                {
                    pending.push_back(child);
                }
            }
            if (foundBounds)
            {
                dimensions = {
                    std::max(
                        compoundBounds.maximum.x
                            - compoundBounds.minimum.x,
                        0.01f),
                    std::max(
                        compoundBounds.maximum.y
                            - compoundBounds.minimum.y,
                        0.01f),
                    std::max(
                        compoundBounds.maximum.z
                            - compoundBounds.minimum.z,
                        0.01f)
                };
            }
        }

        // 近似箱のXYZ慣性モーメント
        const DirectX::XMFLOAT3 inertia{
            m_mass / 12.0f
                * (dimensions.y * dimensions.y
                    + dimensions.z * dimensions.z),
            m_mass / 12.0f
                * (dimensions.x * dimensions.x
                    + dimensions.z * dimensions.z),
            m_mass / 12.0f
                * (dimensions.x * dimensions.x
                    + dimensions.y * dimensions.y)
        };

        // 軸方向を取得する変換行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            Owner().WorldMatrix());
        // ベクトルを正規化し短すぎればゼロを返します(value: 正規化する軸)。
        auto normalize = [](
            DirectX::XMFLOAT3 value) noexcept
        {
            // 正規化する軸の長さ
            const float length = std::sqrt(
                Dot(value, value));
            return length > 0.000001f
                ? Multiply(value, 1.0f / length)
                : DirectX::XMFLOAT3{};
        };
        // 正規化したワールドXYZ軸
        const std::array axes{
            normalize({
                world._11,
                world._12,
                world._13 }),
            normalize({
                world._21,
                world._22,
                world._23 }),
            normalize({
                world._31,
                world._32,
                world._33 })
        };
        // 作用量の各軸方向成分
        const DirectX::XMFLOAT3 local{
            Dot(worldTorque, axes[0]),
            Dot(worldTorque, axes[1]),
            Dot(worldTorque, axes[2])
        };
        // 拘束済みの逆慣性適用結果
        DirectX::XMFLOAT3 localResult{
            local.x / std::max(inertia.x, 0.0001f),
            local.y / std::max(inertia.y, 0.0001f),
            local.z / std::max(inertia.z, 0.0001f)
        };
        if (m_constraints.freezeRotationX)
            localResult.x = 0.0f;
        if (m_constraints.freezeRotationY)
            localResult.y = 0.0f;
        if (m_constraints.freezeRotationZ)
            localResult.z = 0.0f;
        return Add(
            Add(
                Multiply(axes[0], localResult.x),
                Multiply(axes[1], localResult.y)),
            Multiply(axes[2], localResult.z));
    }

    void RigidbodyComponent::AddForce(
        const DirectX::XMFLOAT3& force,
        const ForceMode mode) noexcept
    {
        if (m_isKinematic) return;
        WakeUp();
        if (mode == ForceMode::Force)
        {
            m_accumulatedForce =
                Add(m_accumulatedForce, force);
        }
        else if (mode == ForceMode::Acceleration)
        {
            m_accumulatedAcceleration =
                Add(
                    m_accumulatedAcceleration,
                    force);
        }
        else
        {
            // 力積を速度に換算する倍率
            const float scale =
                mode == ForceMode::Impulse
                ? InverseMass()
                : 1.0f;
            m_velocity =
                Add(
                    m_velocity,
                    Multiply(force, scale));
        }
    }

    void RigidbodyComponent::AddTorque(
        const DirectX::XMFLOAT3& torque,
        const ForceMode mode) noexcept
    {
        if (m_isKinematic) return;
        WakeUp();
        if (mode == ForceMode::Force)
        {
            m_accumulatedTorque =
                Add(m_accumulatedTorque, torque);
        }
        else if (mode == ForceMode::Acceleration)
        {
            m_accumulatedAngularAcceleration =
                Add(
                    m_accumulatedAngularAcceleration,
                    torque);
        }
        else
        {
            // 角速度へ加算する作用量
            const auto change =
                mode == ForceMode::Impulse
                ? ApplyInverseInertia(torque)
                : torque;
            SetAngularVelocity(
                Add(m_angularVelocity, change));
        }
    }

    void RigidbodyComponent::AddForceAtPosition(
        const DirectX::XMFLOAT3& force,
        const DirectX::XMFLOAT3& worldPosition,
        const ForceMode mode) noexcept
    {
        // 剛体のワールド重心
        const auto center =
            WorldCenterOfMass();
        // 重心から作用点への変位
        const DirectX::XMFLOAT3 radius{
            worldPosition.x - center.x,
            worldPosition.y - center.y,
            worldPosition.z - center.z
        };
        AddForce(force, mode);
        AddTorque(Cross(radius, force), mode);
    }

    void RigidbodyComponent::ApplyImpulseAtPoint(
        const DirectX::XMFLOAT3& impulse,
        const DirectX::XMFLOAT3& worldPosition) noexcept
    {
        if (m_isKinematic) return;
        if (m_isSleeping
            && Dot(impulse, impulse)
                > 0.0000001f)
        {
            WakeUp();
        }
        m_velocity = Add(
            m_velocity,
            Multiply(impulse, InverseMass()));
        // 剛体のワールド重心
        const auto center =
            WorldCenterOfMass();
        // 重心から作用点への変位
        const DirectX::XMFLOAT3 radius{
            worldPosition.x - center.x,
            worldPosition.y - center.y,
            worldPosition.z - center.z
        };
        SetAngularVelocityInternal(
            Add(
                m_angularVelocity,
                ApplyInverseInertia(
                    Cross(radius, impulse))),
            false);
    }

    void RigidbodyComponent::Integrate(
        GameObject& gameObject,
        const float deltaTime) noexcept
    {
        if (m_isSleeping)
        {
            return;
        }
        if (!m_isKinematic)
        {
            // 外力と重力の合計加速度
            DirectX::XMFLOAT3 acceleration =
                m_accumulatedAcceleration;
            acceleration = Add(
                acceleration,
                Multiply(
                    m_accumulatedForce,
                    InverseMass()));
            if (m_useGravity)
            {

                // プロジェクトの重力加速度
                const auto& gravity =
                    ActivePhysicsSettings().gravity;
                acceleration.x += gravity.x;
                acceleration.y += gravity.y;
                acceleration.z += gravity.z;
            }
            m_velocity = Add(
                m_velocity,
                Multiply(acceleration, deltaTime));
            // 線形速度に掛ける減衰倍率
            const float linearDamping =
                1.0f / (1.0f
                    + m_linearDrag
                        * std::abs(deltaTime));
            m_velocity =
                Multiply(
                    m_velocity,
                    linearDamping);

            // 合計ワールド角加速度
            auto angularAcceleration =
                Add(
                    ApplyInverseInertia(
                        m_accumulatedTorque),
                    m_accumulatedAngularAcceleration);
            SetAngularVelocityInternal(
                Add(
                    m_angularVelocity,
                    Multiply(
                        angularAcceleration,
                        deltaTime)),
                false);
            // 角速度に掛ける減衰倍率
            const float angularDamping =
                1.0f / (1.0f
                    + m_angularDrag
                        * std::abs(deltaTime));
            SetAngularVelocityInternal(
                Multiply(
                    m_angularVelocity,
                    angularDamping),
                false);

            // 離散判定の高速移動を初回通知し、指定があれば安全速度まで制限します。
            if (!UsesContinuousCollisionDetection())
            {
                // 適用するプロジェクト物理設定
                const auto& physics = ActivePhysicsSettings();
                // ワールド速度の長さ毎秒
                const float speed = std::sqrt(
                    Dot(m_velocity, m_velocity));
                if (speed > physics.discreteSafeSpeed)
                {
                    if (!m_discreteSpeedWarned)
                    {
                        m_discreteSpeedWarned = true;
                        Logger::Instance().Warning(
                            "離散判定（DCD）には速すぎます（"
                            + std::to_string(speed)
                            + " m/s、上限 "
                            + std::to_string(
                                physics.discreteSafeSpeed)
                            + " m/s）。薄い壁をすり抜ける"
                            "ことがあります。この物体の"
                            "Rigidbodyを Continuous (CCD) へ"
                            "変えてください: "
                            + std::string(gameObject.Name()),
                            gameObject.Id());
                    }
                    if (physics.clampDiscreteSpeed
                        && speed > 0.0f)
                    {
                        m_velocity = Multiply(
                            m_velocity,
                            physics.discreteSafeSpeed
                                / speed);
                    }
                }
            }
        }

        // 位置の凍結軸は速度をゼロ化して動かしません。
        if (m_constraints.freezePositionX)
        {
            m_velocity.x = 0.0f;
        }
        if (m_constraints.freezePositionY)
        {
            m_velocity.y = 0.0f;
        }
        if (m_constraints.freezePositionZ)
        {
            m_velocity.z = 0.0f;
        }

        gameObject.TranslateWorld({
            m_velocity.x * deltaTime,
            m_velocity.y * deltaTime,
            m_velocity.z * deltaTime
        });
        gameObject.RotateWorld({
            m_angularVelocity.x * deltaTime,
            m_angularVelocity.y * deltaTime,
            m_angularVelocity.z * deltaTime
        });
    }

    void RigidbodyComponent::ClearAccumulators() noexcept
    {
        m_accumulatedForce = {};
        m_accumulatedTorque = {};
        m_accumulatedAcceleration = {};
        m_accumulatedAngularAcceleration = {};
    }

    void RigidbodyComponent::Sleep() noexcept
    {
        if (m_isKinematic)
        {
            return;
        }
        m_isSleeping = true;
        // 休止待ち時間を現在の設定値に合わせ、手動休止が次の判定で解除されるのを防ぎます。
        m_sleepTimer =
            ActivePhysicsSettings().sleepDelay;
        m_velocity = {};
        m_angularVelocity = {};
        ClearAccumulators();
    }

    void RigidbodyComponent::WakeUp() noexcept
    {
        m_isSleeping = false;
        m_sleepTimer = 0.0f;
    }

    void RigidbodyComponent::UpdateSleepState(
        const float deltaTime,
        const bool supported) noexcept
    {
        if (m_isKinematic || m_isSleeping)
        {
            return;
        }

        // 適用するプロジェクト物理設定
        const auto& physics = ActivePhysicsSettings();
        // 休止判定の線形速度しきい値
        const float linearThreshold =
            physics.sleepLinearVelocity;
        // 休止判定の角速度しきい値
        const float angularThreshold =
            physics.sleepAngularVelocity;
        // 線形速度の長さの二乗
        const float linearSpeedSquared =
            Dot(m_velocity, m_velocity);
        // 角速度の長さの二乗
        const float angularSpeedSquared =
            Dot(
                m_angularVelocity,
                m_angularVelocity);
        // 両速度が休止しきい値以内
        const bool slow =
            linearSpeedSquared
                <= linearThreshold * linearThreshold
            && angularSpeedSquared
                <= angularThreshold * angularThreshold;
        if (!slow)
        {
            m_sleepTimer = 0.0f;
            return;
        }
        // 絶対値を取った経過秒数
        const float time = std::abs(deltaTime);
        if (supported)
        {
            m_sleepTimer += time;
        }
        else
        {
            m_sleepTimer = std::max(
                0.0f,
                m_sleepTimer - time * 0.25f);
        }
        if (m_sleepTimer >= physics.sleepDelay)
        {
            Sleep();
        }
    }

    void RigidbodyComponent::RemoveInwardVelocity(
        const DirectX::XMFLOAT3& surfaceNormal) noexcept
    {
        // 面の外向き法線方向の速度
        const float inwardSpeed =
            m_velocity.x * surfaceNormal.x
            + m_velocity.y * surfaceNormal.y
            + m_velocity.z * surfaceNormal.z;

        if (inwardSpeed < 0.0f)
        {
            m_velocity.x -= surfaceNormal.x * inwardSpeed;
            m_velocity.y -= surfaceNormal.y * inwardSpeed;
            m_velocity.z -= surfaceNormal.z * inwardSpeed;
        }
    }
}
