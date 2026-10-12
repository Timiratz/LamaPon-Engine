#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 髪・服・飾りなど、回転中心から垂れ下がる2Dパーツの揺れ設定です。
    // 長さはワールド単位(2Dでは1ピクセル)、Yは下向きです。
    struct Sway2DSettings final
    {
        // 回転中心から揺れの先端までのローカルXY(拡縮前の単位)
        DirectX::XMFLOAT2 tipOffset{ 0.0f, 100.0f };
        // 先端を静止姿勢へ戻すばねの強さ(毎秒毎秒)
        float stiffness{ 60.0f };
        // 揺れを弱める減衰の強さ(毎秒)
        float damping{ 8.0f };
        // 回転中心の移動に先端が取り残される割合(0で追従・1で完全に残る)
        float inertia{ 1.0f };
        // 先端へ掛かるワールド加速度(毎秒毎秒、Yは下向き)
        DirectX::XMFLOAT2 gravity{ 0.0f, 0.0f };
        // 静止姿勢から振れる最大角度(度、0〜180)
        float maxAngleDegrees{ 45.0f };
        // 風で加える周期的な揺れ幅(度、0〜180)
        float windAmplitudeDegrees{ 0.0f };
        // 風の揺れの周波数(Hz、0〜60)
        float windFrequency{ 0.5f };
        // 風の揺れの位相(度)
        float windPhaseDegrees{ 0.0f };
    };

    // 先端を仮想の点としてばね・減衰・重力で追わせ、その角度だけ親基準のZ回転を足します。
    // 通常更新後の姿勢を静止姿勢として扱うため、TransformAnimatorなどの回転にも上乗せできます。
    // 祖先のSway2Dを先に解くため、髪を数節に分けて親子でつなぐとしなるように揺れます。
    class Sway2DComponent final : public Component
    {
    public:
        // 揺れ物を作ります(settings: 揺れ設定で範囲外の値は補正)。
        explicit Sway2DComponent(
            const Sway2DSettings& settings = {}) noexcept;

        // 範囲外の値を補正して揺れ設定を置き換えます(settings: 新しい揺れ設定)。
        void SetSettings(
            const Sway2DSettings& settings) noexcept;
        // 補正済みの揺れ設定を返します。
        [[nodiscard]] const Sway2DSettings&
            Settings() const noexcept
        {
            return m_settings;
        }
        // 非有限値を既定値へ戻し、各値を許容範囲へ収めた設定を返します(settings: 補正する設定)。
        [[nodiscard]] static Sway2DSettings Sanitize(
            Sway2DSettings settings) noexcept;

        // 揺れを止め、次の更新で現在の姿勢から計算をやり直します。
        void ResetSimulation() noexcept;
        // 直近に静止姿勢へ足したZ回転をラジアンで返します。
        [[nodiscard]] float CurrentAngle() const noexcept
        {
            return m_angle;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Sway2D";
        }

    protected:
        // 前フレームに足した回転が残っていれば外し、今フレームの経過秒数を記録します(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 祖先の揺れを先に解いてから自身の揺れを計算し、回転を上乗せします(deltaTime: 経過秒数)。
        void OnLateUpdate(float deltaTime) override;
        // 無効化時は足した回転を外し、再開時は計算をやり直します(active: 新しい稼働状態)。
        void OnActiveStateChanged(bool active) override;

    private:
        // このフレームの揺れを一度だけ計算して適用します。
        void SolveFrame() noexcept;
        // 足した回転が上書きされていなければ静止姿勢の回転へ戻します。
        void RemoveAppliedRotation() noexcept;
        // 先端の点を経過秒数だけ進めます(pivot: ワールド回転中心, restTip: ワールド静止先端, deltaTime: 経過秒数)。
        void Simulate(
            DirectX::XMFLOAT2 pivot,
            DirectX::XMFLOAT2 restTip,
            float deltaTime) noexcept;

        // 補正済みの揺れ設定
        Sway2DSettings m_settings;
        // 先端の点のワールドXY
        DirectX::XMFLOAT2 m_tip{};
        // 先端の点のワールド速度
        DirectX::XMFLOAT2 m_velocity{};
        // 前フレームの回転中心のワールドXY
        DirectX::XMFLOAT2 m_previousPivot{};
        // 前フレームの静止先端のワールドXY
        DirectX::XMFLOAT2 m_previousRestTip{};
        // 静止姿勢の回転クォータニオン
        DirectX::XMFLOAT4 m_restRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 直近に書き込んだ回転クォータニオン
        DirectX::XMFLOAT4 m_appliedRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 静止姿勢へ足したZ回転ラジアン
        float m_angle{};
        // 風の揺れの累積位相ラジアン(0〜2π)
        float m_windPhase{};
        // 今フレームでまだ計算していない経過秒数
        float m_pendingDeltaTime{};
        // 先端の点を初期化済みか
        bool m_simulationReady{};
        // 書き込んだ回転が残っている可能性があるか
        bool m_rotationApplied{};
        // 今フレームの計算を済ませたか
        bool m_solvedThisFrame{ true };
    };
}
