#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 数値設定には有限値を指定します。
    class SpotLightComponent final : public Component
    {
    public:
        // 円錐光源を作ります(color: RGB色, intensity: 光の強度, range: 到達距離, innerConeAngle: 内側半角ラジアン, outerConeAngle: 外側半角ラジアン)。
        explicit SpotLightComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.88f, 0.68f },
            float intensity = 5.0f,
            float range = 12.0f,
            float innerConeAngle = DirectX::XMConvertToRadians(22.5f),
            float outerConeAngle = DirectX::XMConvertToRadians(35.0f)) noexcept;

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

        // 内側半角を1度〜外側半角に制限します(angle: 内側半角ラジアン)。
        void SetInnerConeAngle(float angle) noexcept;
        // 内側半角をラジアンで返します。
        [[nodiscard]] float InnerConeAngle() const noexcept
        {
            return m_innerConeAngle;
        }

        // 外側半角を内側半角以上かつ89度以下に制限します(angle: 外側半角ラジアン)。
        void SetOuterConeAngle(float angle) noexcept;
        // 外側半角をラジアンで返します。
        [[nodiscard]] float OuterConeAngle() const noexcept
        {
            return m_outerConeAngle;
        }

        // 影を作る設定を変更します(enabled: 影を作る指定)。
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
        // 法線バイアスを0〜0.5に制限して設定します(bias: 法線方向のバイアス)。
        void SetShadowNormalBias(float bias) noexcept;
        // 影判定の法線方向バイアスを返します。
        [[nodiscard]] float ShadowNormalBias() const noexcept
        {
            return m_shadowNormalBias;
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
        // ワールドの負Z方向を正規化し短すぎる軸は下向きを返します。
        [[nodiscard]] DirectX::XMFLOAT3 WorldDirection() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpotLight";
        }

    private:
        // 光源のRGB色
        DirectX::XMFLOAT3 m_color;
        // 光の強度
        float m_intensity;
        // 到達距離ワールド単位
        float m_range;
        // 内側半角ラジアン
        float m_innerConeAngle;
        // 外側半角ラジアン
        float m_outerConeAngle;
        // 影を作る指定
        bool m_castsShadows{};
        // 影判定の深度バイアス
        float m_shadowBias{ 0.002f };
        // 影判定の法線バイアス
        float m_shadowNormalBias{ 0.01f };
        // 影の強度
        float m_shadowStrength{ 0.9f };
    };
}
