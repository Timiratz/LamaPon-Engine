#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    enum class JointType
    {
        // 接続点と姿勢を拘束します。
        Fixed,
        // 接続点と軸を合わせて軸回りの回転を許可します。
        Hinge,
        // 接続点間の距離をバネと減衰で拘束します。
        Spring
    };

    struct HingeLimits final
    {
        // ヒンジの最小角度度
        float minimumAngleDegrees{ -90.0f };
        // ヒンジの最大角度度
        float maximumAngleDegrees{ 90.0f };
    };

    struct HingeMotor final
    {
        // ヒンジの目標角速度度毎秒
        float targetVelocityDegrees{ 90.0f };
        // 駆動の最大トルク
        float maximumTorque{ 10.0f };
    };

    // 数値設定には有限値を指定し、ヒンジの姿勢には可逆な変換を使います。
    class JointComponent final : public Component
    {
    public:
        // 物体間の拘束を作ります(type: 拘束方式, connectedBodyId: 接続物体IDで0は未接続, anchor: 自身のローカル接続点, connectedAnchor: 相手のローカル接続点, axis: 自身のローカル回転軸, restLength: バネ自然長ワールド単位, stiffness: バネ定数, damping: 減衰係数, collideConnected: 接続物体との衝突を許可, useLimits: ヒンジ角度制限を使う指定, limits: ヒンジ角度の範囲度, useMotor: ヒンジ駆動を使う指定, motor: ヒンジ駆動の速度とトルク)。
        explicit JointComponent(
            JointType type = JointType::Fixed,
            std::uint64_t connectedBodyId = 0,
            DirectX::XMFLOAT3 anchor = {},
            DirectX::XMFLOAT3 connectedAnchor = {},
            DirectX::XMFLOAT3 axis = { 0.0f, 1.0f, 0.0f },
            float restLength = 1.0f,
            float stiffness = 20.0f,
            float damping = 2.0f,
            bool collideConnected = false,
            bool useLimits = false,
            HingeLimits limits = {},
            bool useMotor = false,
            HingeMotor motor = {}) noexcept;

        // 拘束方式を返します。
        [[nodiscard]] JointType Type() const noexcept { return m_type; }
        // 拘束方式を変更してヒンジ基準を破棄します(value: 拘束方式)。
        void SetType(JointType value) noexcept;

        // 接続物体のIDを返し0なら未接続を示します。
        [[nodiscard]] std::uint64_t ConnectedBodyId() const noexcept
        {
            return m_connectedBodyId;
        }
        // 接続物体を変更してヒンジ基準を破棄します(value: 接続物体IDで0は未接続)。
        void SetConnectedBodyId(std::uint64_t value) noexcept;

        // 自身のローカル接続点を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Anchor() const noexcept
        {
            return m_anchor;
        }
        // 自身のローカル接続点を設定します(value: ローカルXYZ位置)。
        void SetAnchor(const DirectX::XMFLOAT3& value) noexcept
        {
            m_anchor = value;
        }

        // 相手のローカル接続点を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& ConnectedAnchor() const noexcept
        {
            return m_connectedAnchor;
        }
        // 相手のローカル接続点を設定します(value: ローカルXYZ位置)。
        void SetConnectedAnchor(const DirectX::XMFLOAT3& value) noexcept
        {
            m_connectedAnchor = value;
        }

        // 自身のローカルヒンジ軸を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Axis() const noexcept
        {
            return m_axis;
        }
        // 軸を正規化してヒンジ基準を破棄し短すぎる軸はYにします(value: ローカル回転軸)。
        void SetAxis(const DirectX::XMFLOAT3& value) noexcept;

        // バネの自然長をワールド単位で返します。
        [[nodiscard]] float RestLength() const noexcept { return m_restLength; }
        // 自然長を非負に制限して設定します(value: ワールド距離)。
        void SetRestLength(float value) noexcept;
        // バネ定数を返します。
        [[nodiscard]] float Stiffness() const noexcept { return m_stiffness; }
        // バネ定数を非負に制限して設定します(value: バネ定数)。
        void SetStiffness(float value) noexcept;
        // 減衰係数を返します。
        [[nodiscard]] float Damping() const noexcept { return m_damping; }
        // 減衰係数を非負に制限して設定します(value: 減衰係数)。
        void SetDamping(float value) noexcept;

        // ヒンジ角度制限を使う指定を返します。
        [[nodiscard]] bool UseLimits() const noexcept
        {
            return m_useLimits;
        }
        // ヒンジ角度制限の使用を設定します(value: 制限する指定)。
        void SetUseLimits(bool value) noexcept
        {
            m_useLimits = value;
        }
        // ヒンジの角度制限を度単位で返します。
        [[nodiscard]] const HingeLimits& Limits() const noexcept
        {
            return m_limits;
        }
        // 角度を±360度に制限し逆順なら交換して設定します(value: 最小・最大角度度)。
        void SetLimits(HingeLimits value) noexcept;

        // ヒンジを駆動する指定を返します。
        [[nodiscard]] bool UseMotor() const noexcept
        {
            return m_useMotor;
        }
        // ヒンジ駆動の使用を設定します(value: 駆動する指定)。
        void SetUseMotor(bool value) noexcept
        {
            m_useMotor = value;
        }
        // ヒンジ駆動の速度と最大トルクを返します。
        [[nodiscard]] const HingeMotor& Motor() const noexcept
        {
            return m_motor;
        }
        // 速度を±100000度毎秒、最大トルクを非負に制限します(value: 駆動速度と最大トルク)。
        void SetMotor(HingeMotor value) noexcept;

        // 未取得のヒンジ基準を現在の姿勢から記録します(connected: 接続物体)。
        // 接続物体には有限かつ可逆なワールド変換が必要です。
        void EnsureHingeReference(
            const GameObject& connected) noexcept;
        // ヒンジ基準を破棄して次の取得を許可します。
        void ResetHingeReference() noexcept;
        // 取得済みなら接続側、未取得なら自身の姿勢から軸を返します(connected: 接続物体)。
        [[nodiscard]] DirectX::XMFLOAT3 HingeWorldAxis(
            const GameObject& connected) const noexcept;
        // 基準からのヒンジ角を返し未取得なら0を返します(connected: 接続物体)。
        // 角度は−π〜πで、複数回転の累積角度は保持しません。
        [[nodiscard]] float HingeAngleRadians(
            const GameObject& connected) const noexcept;

        // 接続物体との衝突を許可する指定を返します。
        [[nodiscard]] bool CollideConnected() const noexcept
        {
            return m_collideConnected;
        }
        // 接続物体との衝突許可を設定します(value: 衝突を許可する指定)。
        void SetCollideConnected(bool value) noexcept
        {
            m_collideConnected = value;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Joint";
        }

    private:
        // 物体間の拘束方式
        JointType m_type{ JointType::Fixed };
        // 接続物体IDで0は未接続
        std::uint64_t m_connectedBodyId{};
        // 自身のローカル接続点
        DirectX::XMFLOAT3 m_anchor{};
        // 相手のローカル接続点
        DirectX::XMFLOAT3 m_connectedAnchor{};
        // 自身のローカルヒンジ軸
        DirectX::XMFLOAT3 m_axis{ 0.0f, 1.0f, 0.0f };
        // バネ自然長ワールド単位
        float m_restLength{ 1.0f };
        // バネ定数
        float m_stiffness{ 20.0f };
        // 減衰係数
        float m_damping{ 2.0f };
        // ヒンジ角度制限を使う指定
        bool m_useLimits{};
        // ヒンジの角度制限度
        HingeLimits m_limits;
        // ヒンジを駆動する指定
        bool m_useMotor{};
        // ヒンジ駆動の速度とトルク
        HingeMotor m_motor;
        // 接続物体との衝突許可
        bool m_collideConnected{};
        // ヒンジ基準を取得済み
        bool m_hingeReferenceInitialized{};
        // 自身のローカル基準方向
        DirectX::XMFLOAT3 m_hingeOwnerReference{
            1.0f,
            0.0f,
            0.0f
        };
        // 相手のローカルヒンジ軸
        DirectX::XMFLOAT3 m_hingeConnectedAxis{
            0.0f,
            1.0f,
            0.0f
        };
        // 相手のローカル基準方向
        DirectX::XMFLOAT3 m_hingeConnectedReference{
            1.0f,
            0.0f,
            0.0f
        };
    };
}
