#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class RotatorComponent final : public Component
    {
    public:
        // 毎秒の回転量を設定します(angularVelocity: XYZ角速度ラジアン毎秒)。
        explicit RotatorComponent(
            DirectX::XMFLOAT3 angularVelocity = { 0.0f, 1.0f, 0.0f }) noexcept;

        // XYZ角速度をラジアン毎秒で返します。
        [[nodiscard]] const DirectX::XMFLOAT3& AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        // XYZ角速度を設定します(velocity: 角速度ラジアン毎秒)。
        void SetAngularVelocity(const DirectX::XMFLOAT3& velocity) noexcept
        {
            m_angularVelocity = velocity;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "Rotator"; }

    protected:
        // 角速度に経過秒数を掛けて回転を合成します(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // XYZ角速度ラジアン毎秒
        DirectX::XMFLOAT3 m_angularVelocity;
    };
}
