#include "LamaPon/Components/Sway2DComponent.h"

#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 1回の積分で進める最大秒数
        constexpr float MaximumSubstepSeconds = 1.0f / 120.0f;
        // 1フレームで計算する最大秒数
        constexpr float MaximumFrameSeconds = 0.1f;
        // 1フレームの最大積分回数
        constexpr int MaximumSubsteps = 16;
        // 瞬間移動とみなす回転中心の移動量(先端までの長さに対する倍率)
        constexpr float TeleportLengthRatio = 10.0f;
        // 方向を求められない長さの上限
        constexpr float MinimumLength = 1.0e-4f;
        // 位置・加速度の絶対値の上限
        constexpr float MaximumMagnitude = 1.0e6f;

        // 有限値ならそのまま、非有限なら代替値を返します(value: 確認する値, fallback: 代替値)。
        [[nodiscard]] float FiniteOr(
            const float value,
            const float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        // 度をラジアンへ変換します(degrees: 角度の度)。
        [[nodiscard]] float ToRadians(const float degrees) noexcept
        {
            return degrees * (std::numbers::pi_v<float> / 180.0f);
        }

        // 2つのXYの差を返します(left: 引かれる値, right: 引く値)。
        [[nodiscard]] DirectX::XMFLOAT2 Subtract(
            const DirectX::XMFLOAT2& left,
            const DirectX::XMFLOAT2& right) noexcept
        {
            return { left.x - right.x, left.y - right.y };
        }

        // 2つのXYの和を返します(left: 足される値, right: 足す値)。
        [[nodiscard]] DirectX::XMFLOAT2 Add(
            const DirectX::XMFLOAT2& left,
            const DirectX::XMFLOAT2& right) noexcept
        {
            return { left.x + right.x, left.y + right.y };
        }

        // XYを倍率で拡縮します(value: 元の値, factor: 倍率)。
        [[nodiscard]] DirectX::XMFLOAT2 Scale(
            const DirectX::XMFLOAT2& value,
            const float factor) noexcept
        {
            return { value.x * factor, value.y * factor };
        }

        // 2点を線形補間します(from: 始点, to: 終点, amount: 0〜1の補間率)。
        [[nodiscard]] DirectX::XMFLOAT2 Lerp(
            const DirectX::XMFLOAT2& from,
            const DirectX::XMFLOAT2& to,
            const float amount) noexcept
        {
            return Add(from, Scale(Subtract(to, from), amount));
        }

        // XYの長さを返します(value: 長さを求める値)。
        [[nodiscard]] float Length(
            const DirectX::XMFLOAT2& value) noexcept
        {
            return std::sqrt(value.x * value.x + value.y * value.y);
        }

        // fromからtoへの符号付き角度を-π〜πで返します(from: 基準方向, to: 対象方向)。
        // 行ベクトルのZ回転と同じく、正の角度は+Xを+Yへ回す向きです。
        [[nodiscard]] float SignedAngle(
            const DirectX::XMFLOAT2& from,
            const DirectX::XMFLOAT2& to) noexcept
        {
            return std::atan2(
                from.x * to.y - from.y * to.x,
                from.x * to.x + from.y * to.y);
        }

        // XYを行ベクトルのZ回転で回します(value: 回す値, radians: 回転角)。
        [[nodiscard]] DirectX::XMFLOAT2 Rotate(
            const DirectX::XMFLOAT2& value,
            const float radians) noexcept
        {
            // 回転角の余弦
            const float cosine = std::cos(radians);
            // 回転角の正弦
            const float sine = std::sin(radians);
            return {
                value.x * cosine - value.y * sine,
                value.x * sine + value.y * cosine
            };
        }

        // 2つの回転クォータニオンが全成分で一致するか返します(left: 比較元, right: 比較先)。
        [[nodiscard]] bool SameRotation(
            const DirectX::XMFLOAT4& left,
            const DirectX::XMFLOAT4& right) noexcept
        {
            return left.x == right.x
                && left.y == right.y
                && left.z == right.z
                && left.w == right.w;
        }

        // XYの各成分を有限かつ上限内へ収めます(value: 補正する値, fallback: 非有限時の値)。
        [[nodiscard]] DirectX::XMFLOAT2 SanitizeFloat2(
            const DirectX::XMFLOAT2& value,
            const DirectX::XMFLOAT2& fallback) noexcept
        {
            return {
                std::clamp(
                    FiniteOr(value.x, fallback.x),
                    -MaximumMagnitude,
                    MaximumMagnitude),
                std::clamp(
                    FiniteOr(value.y, fallback.y),
                    -MaximumMagnitude,
                    MaximumMagnitude)
            };
        }
    }

    Sway2DComponent::Sway2DComponent(
        const Sway2DSettings& settings) noexcept
        : m_settings(Sanitize(settings))
    {
    }

    void Sway2DComponent::SetSettings(
        const Sway2DSettings& settings) noexcept
    {
        m_settings = Sanitize(settings);
    }

    Sway2DSettings Sway2DComponent::Sanitize(
        Sway2DSettings settings) noexcept
    {
        // 非有限値の置き換えに使う既定設定
        const Sway2DSettings defaults{};
        settings.tipOffset = SanitizeFloat2(
            settings.tipOffset,
            defaults.tipOffset);
        settings.stiffness = std::clamp(
            FiniteOr(settings.stiffness, defaults.stiffness),
            0.0f,
            10000.0f);
        settings.damping = std::clamp(
            FiniteOr(settings.damping, defaults.damping),
            0.0f,
            1000.0f);
        settings.inertia = std::clamp(
            FiniteOr(settings.inertia, defaults.inertia),
            0.0f,
            1.0f);
        settings.gravity = SanitizeFloat2(
            settings.gravity,
            defaults.gravity);
        settings.maxAngleDegrees = std::clamp(
            FiniteOr(
                settings.maxAngleDegrees,
                defaults.maxAngleDegrees),
            0.0f,
            180.0f);
        settings.windAmplitudeDegrees = std::clamp(
            FiniteOr(
                settings.windAmplitudeDegrees,
                defaults.windAmplitudeDegrees),
            0.0f,
            180.0f);
        settings.windFrequency = std::clamp(
            FiniteOr(
                settings.windFrequency,
                defaults.windFrequency),
            0.0f,
            60.0f);
        settings.windPhaseDegrees = std::fmod(
            std::clamp(
                FiniteOr(
                    settings.windPhaseDegrees,
                    defaults.windPhaseDegrees),
                -MaximumMagnitude,
                MaximumMagnitude),
            360.0f);
        return settings;
    }

    void Sway2DComponent::ResetSimulation() noexcept
    {
        m_simulationReady = false;
        m_velocity = {};
        m_angle = 0.0f;
    }

    void Sway2DComponent::OnUpdate(const float deltaTime)
    {
        RemoveAppliedRotation();
        m_pendingDeltaTime =
            std::isfinite(deltaTime)
                ? std::clamp(
                    deltaTime,
                    0.0f,
                    MaximumFrameSeconds)
                : 0.0f;
        m_solvedThisFrame = false;
    }

    void Sway2DComponent::OnLateUpdate(float)
    {
        SolveFrame();
    }

    void Sway2DComponent::OnActiveStateChanged(
        const bool active)
    {
        if (!active)
        {
            RemoveAppliedRotation();
            m_solvedThisFrame = true;
        }
        ResetSimulation();
    }

    void Sway2DComponent::RemoveAppliedRotation() noexcept
    {
        if (!m_rotationApplied)
        {
            return;
        }
        m_rotationApplied = false;
        // 現在のローカル回転
        auto& rotation =
            GetTransform().rotationQuaternion;
        // 他の処理が回転を書き換えた場合は、その値を次の静止姿勢として残します。
        if (SameRotation(rotation, m_appliedRotation))
        {
            rotation = m_restRotation;
        }
    }

    void Sway2DComponent::SolveFrame() noexcept
    {
        if (m_solvedThisFrame)
        {
            return;
        }
        m_solvedThisFrame = true;

        // 祖先の揺れを先に確定させ、自身の静止姿勢へ反映します。
        // 祖先をたどる途中の物体
        for (GameObject* ancestor = Owner().Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            // 祖先が持つ揺れ物
            auto* ancestorSway =
                ancestor->GetComponent<Sway2DComponent>();
            if (ancestorSway != nullptr
                && ancestorSway->IsActiveAndEnabled())
            {
                ancestorSway->SolveFrame();
                break;
            }
        }

        using namespace DirectX;

        // 揺れを足す前のローカル変換
        auto& transform = GetTransform();
        m_restRotation = transform.rotationQuaternion;
        // 今フレームで進める秒数
        const float deltaTime =
            std::exchange(m_pendingDeltaTime, 0.0f);

        // 静止姿勢のワールド変換
        const XMMATRIX world = Owner().WorldMatrix();
        // 回転中心のワールド位置
        XMFLOAT3 pivot3{};
        XMStoreFloat3(&pivot3, world.r[3]);
        // 先端の静止位置のローカル座標
        const XMVECTOR tipOffset = XMVectorSet(
            m_settings.tipOffset.x,
            m_settings.tipOffset.y,
            0.0f,
            1.0f);
        // 先端の静止位置のワールド座標
        XMFLOAT3 restTip3{};
        XMStoreFloat3(
            &restTip3,
            XMVector3TransformCoord(tipOffset, world));
        // 回転中心のワールドXY
        const XMFLOAT2 pivot{ pivot3.x, pivot3.y };
        // 先端の静止位置のワールドXY
        const XMFLOAT2 restTip{ restTip3.x, restTip3.y };
        // 回転中心から静止先端までのワールド長
        const float length = Length(Subtract(restTip, pivot));
        if (!std::isfinite(length)
            || length < MinimumLength)
        {
            ResetSimulation();
            return;
        }

        if (!m_simulationReady
            || Length(Subtract(pivot, m_previousPivot))
                > length * TeleportLengthRatio)
        {
            m_tip = restTip;
            m_velocity = {};
            m_previousPivot = pivot;
            m_previousRestTip = restTip;
            m_simulationReady = true;
        }

        if (deltaTime > 0.0f)
        {
            Simulate(pivot, restTip, deltaTime);
            m_windPhase = std::fmod(
                m_windPhase
                    + 2.0f
                        * std::numbers::pi_v<float>
                        * m_settings.windFrequency
                        * deltaTime,
                2.0f * std::numbers::pi_v<float>);
        }
        m_previousPivot = pivot;
        m_previousRestTip = restTip;

        // 親を基準にした回転中心
        const XMFLOAT2 localPivot{
            transform.position.x,
            transform.position.y };
        // 親を基準にした静止先端
        XMFLOAT3 localRestTip{};
        XMStoreFloat3(
            &localRestTip,
            XMVector3TransformCoord(
                tipOffset,
                transform.LocalMatrix()));
        // ワールドの先端を親基準へ戻す行列
        XMMATRIX worldToParent = XMMatrixIdentity();
        if (const auto* parent = Owner().Parent())
        {
            // 親のワールド変換
            const XMMATRIX parentWorld =
                parent->WorldMatrix();
            // 親変換の行列式
            const float determinant =
                XMVectorGetX(
                    XMMatrixDeterminant(parentWorld));
            if (!std::isfinite(determinant)
                || std::abs(determinant) < 1.0e-12f)
            {
                ResetSimulation();
                return;
            }
            worldToParent =
                XMMatrixInverse(nullptr, parentWorld);
        }
        // 親を基準にした先端の点
        XMFLOAT3 localTip{};
        XMStoreFloat3(
            &localTip,
            XMVector3TransformCoord(
                XMVectorSet(
                    m_tip.x,
                    m_tip.y,
                    restTip3.z,
                    1.0f),
                worldToParent));

        // 親基準で測るため、親の反転や非一様拡縮があっても回転の向きが一致します。
        // 物理と風を合わせた回転角
        float angle = SignedAngle(
            Subtract(
                { localRestTip.x, localRestTip.y },
                localPivot),
            Subtract(
                { localTip.x, localTip.y },
                localPivot));
        angle += ToRadians(m_settings.windAmplitudeDegrees)
            * std::sin(
                m_windPhase
                + ToRadians(m_settings.windPhaseDegrees));
        // 振れ角の上限ラジアン
        const float maximumAngle =
            ToRadians(m_settings.maxAngleDegrees);
        m_angle = std::isfinite(angle)
            ? std::clamp(angle, -maximumAngle, maximumAngle)
            : 0.0f;

        transform.SetRotationVector(
            XMQuaternionMultiply(
                XMLoadFloat4(&m_restRotation),
                XMQuaternionRotationAxis(
                    XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
                    m_angle)));
        m_appliedRotation = transform.rotationQuaternion;
        m_rotationApplied = true;
    }

    void Sway2DComponent::Simulate(
        const DirectX::XMFLOAT2 pivot,
        const DirectX::XMFLOAT2 restTip,
        const float deltaTime) noexcept
    {
        using namespace DirectX;

        // 回転中心から静止先端までの長さ
        const float length = Length(Subtract(restTip, pivot));
        // 振れ角の上限ラジアン
        const float maximumAngle =
            ToRadians(m_settings.maxAngleDegrees);
        // フレーム内の積分回数
        const int substeps = std::clamp(
            static_cast<int>(
                std::ceil(deltaTime / MaximumSubstepSeconds)),
            1,
            MaximumSubsteps);
        // 1回の積分秒数
        const float step =
            deltaTime / static_cast<float>(substeps);

        // 直前の積分時点の回転中心
        XMFLOAT2 stepPivot = m_previousPivot;
        // 積分回数の添字
        for (int index = 1; index <= substeps; ++index)
        {
            // 前フレームから今フレームへの補間率
            const float amount =
                static_cast<float>(index)
                / static_cast<float>(substeps);
            // 今回の回転中心
            const XMFLOAT2 currentPivot =
                Lerp(m_previousPivot, pivot, amount);
            // 今回の静止先端
            const XMFLOAT2 currentRestTip =
                Lerp(m_previousRestTip, restTip, amount);
            // 今回の回転中心の移動量
            const XMFLOAT2 pivotDelta =
                Subtract(currentPivot, stepPivot);
            stepPivot = currentPivot;

            // 取り残されない分だけ先端を回転中心と一緒に運びます。
            m_tip = Add(
                m_tip,
                Scale(pivotDelta, 1.0f - m_settings.inertia));
            // 積分前の先端位置
            const XMFLOAT2 previousTip = m_tip;
            // 先端が物理で追う回転中心の速度
            const XMFLOAT2 followedPivotVelocity =
                Scale(
                    pivotDelta,
                    m_settings.inertia / step);
            // 回転中心に対する先端の相対速度
            const XMFLOAT2 relativeVelocity =
                Subtract(m_velocity, followedPivotVelocity);
            // ばね・重力・減衰による加速度
            const XMFLOAT2 acceleration{
                m_settings.stiffness
                        * (currentRestTip.x - m_tip.x)
                    + m_settings.gravity.x
                    - m_settings.damping
                        * relativeVelocity.x,
                m_settings.stiffness
                        * (currentRestTip.y - m_tip.y)
                    + m_settings.gravity.y
                    - m_settings.damping
                        * relativeVelocity.y
            };
            m_velocity = Add(
                m_velocity,
                Scale(acceleration, step));
            m_tip = Add(m_tip, Scale(m_velocity, step));

            // 先端を回転中心から一定距離・上限角度の円弧上へ戻します。
            // 回転中心から静止先端への方向
            XMFLOAT2 restDirection =
                Subtract(currentRestTip, currentPivot);
            // 補間中の静止方向の長さ
            const float restLength = Length(restDirection);
            if (restLength < MinimumLength)
            {
                continue;
            }
            restDirection = Scale(
                restDirection,
                length / restLength);
            // 回転中心から先端への方向
            const XMFLOAT2 tipDirection =
                Subtract(m_tip, currentPivot);
            // 静止方向から先端方向への角度
            float angle =
                Length(tipDirection) < MinimumLength
                    ? 0.0f
                    : SignedAngle(
                        restDirection,
                        tipDirection);
            angle = std::clamp(
                angle,
                -maximumAngle,
                maximumAngle);
            m_tip = Add(
                currentPivot,
                Rotate(restDirection, angle));
            m_velocity = Scale(
                Subtract(m_tip, previousTip),
                1.0f / step);
        }

        if (!std::isfinite(m_tip.x)
            || !std::isfinite(m_tip.y)
            || !std::isfinite(m_velocity.x)
            || !std::isfinite(m_velocity.y))
        {
            m_tip = restTip;
            m_velocity = {};
        }
    }
}
