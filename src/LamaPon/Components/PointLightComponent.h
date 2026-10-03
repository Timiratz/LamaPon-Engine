#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 数値設定には有限値を指定します。
    class PointLightComponent final : public Component
    {
    public:
        // 色・強度・範囲を制限して点光源を作ります(color: RGB色, intensity: 光の強度, range: 到達距離)。
        explicit PointLightComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.72f, 0.42f },
            float intensity = 3.0f,
            float range = 8.0f) noexcept;

        // RGBを0〜1に制限して設定します(color: RGB色)。
        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        // RGB色を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        // 強度を0〜64に制限して設定します(intensity: 光の強度)。
        void SetIntensity(float intensity) noexcept;
        // 光の強度を返します。
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        // 到達距離を0.1〜1000に制限して設定します(range: ワールド距離)。
        void SetRange(float range) noexcept;
        // 光の到達距離をワールド単位で返します。
        [[nodiscard]] float Range() const noexcept
        {
            return m_range;
        }

        // 点光源の影を要求します(enabled: 影を作る指定)。
        // 同時に点光源の影を作れるのは1灯です。
        void SetCastsShadows(const bool enabled) noexcept
        {
            m_castsShadows = enabled;
        }
        // 影を作る指定を返します。
        [[nodiscard]] bool CastsShadows() const noexcept
        {
            return m_castsShadows;
        }
        // 深度バイアスを0〜0.05に制限して設定します(bias: 深度バイアス)。
        void SetShadowBias(float bias) noexcept;
        // 影判定の深度バイアスを返します。
        [[nodiscard]] float ShadowBias() const noexcept
        {
            return m_shadowBias;
        }
        // 影の強度を0〜1に制限して設定します(strength: 影の強度)。
        void SetShadowStrength(float strength) noexcept;
        // 影の強度を返します。
        [[nodiscard]] float ShadowStrength() const noexcept
        {
            return m_shadowStrength;
        }

        // 光源のワールド位置を返します。
        [[nodiscard]] DirectX::XMFLOAT3 WorldPosition() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "PointLight";
        }

    private:
        // 光源のRGB色
        DirectX::XMFLOAT3 m_color;
        // 光の強度
        float m_intensity;
        // 到達距離ワールド単位
        float m_range;
        // 影を作る指定
        bool m_castsShadows{};
        // 影判定の深度バイアス
        float m_shadowBias{ 0.002f };
        // 影の強度
        float m_shadowStrength{ 0.9f };
    };
}
