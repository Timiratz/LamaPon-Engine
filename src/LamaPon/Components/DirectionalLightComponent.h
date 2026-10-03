#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    // 数値設定には有限値を指定します。
    class DirectionalLightComponent final : public Component
    {
    public:
        // 方向光源を作ります(color: RGB色, intensity: 光の強度, castsShadows: 影を作る指定, shadowDistance: 影の到達距離, shadowBias: 深度バイアス, shadowNormalBias: 法線バイアス, shadowStrength: 影の強度, shadowCascadeCount: 影の分割数, shadowSplitLambda: 対数分割の比率)。
        explicit DirectionalLightComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.96f, 0.88f },
            float intensity = 1.0f,
            bool castsShadows = true,
            float shadowDistance = 24.0f,
            float shadowBias = 0.0015f,
            float shadowNormalBias = 0.0025f,
            float shadowStrength = 0.85f,
            std::uint32_t shadowCascadeCount = 4,
            float shadowSplitLambda = 0.65f) noexcept;

        // RGBを0〜1に制限して設定します(color: RGB色)。
        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        // RGB色を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        // 強度を0〜16に制限して設定します(intensity: 光の強度)。
        void SetIntensity(float intensity) noexcept;
        // 光の強度を返します。
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
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

        // 影の距離を2〜100に制限して設定します(distance: ワールド距離)。
        void SetShadowDistance(float distance) noexcept;
        // 影の到達距離をワールド単位で返します。
        [[nodiscard]] float ShadowDistance() const noexcept
        {
            return m_shadowDistance;
        }

        // 深度バイアスを0〜0.02に制限して設定します(bias: 深度バイアス)。
        void SetShadowBias(float bias) noexcept;
        // 影判定の深度バイアスを返します。
        [[nodiscard]] float ShadowBias() const noexcept
        {
            return m_shadowBias;
        }

        // 法線バイアスを0〜0.05に制限して設定します(bias: 法線方向のバイアス)。
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

        // 影の分割数を1〜4に制限して設定します(count: 分割数)。
        void SetShadowCascadeCount(
            std::uint32_t count) noexcept;
        // 影の分割数を返します。
        [[nodiscard]] std::uint32_t
            ShadowCascadeCount() const noexcept
        {
            return m_shadowCascadeCount;
        }

        // 対数分割比率を0〜1に制限して設定します(value: 均等分割から対数分割への比率)。
        void SetShadowSplitLambda(float value) noexcept;
        // 影の対数分割比率を返します。
        [[nodiscard]] float ShadowSplitLambda() const noexcept
        {
            return m_shadowSplitLambda;
        }

        // 見かけの直径を0〜20度に制限して設定します(degrees: 角直径度)。
        void SetAngularDiameterDegrees(float degrees) noexcept;
        // 光源の見かけの角直径を度で返します。
        [[nodiscard]] float AngularDiameterDegrees() const noexcept
        {
            return m_angularDiameterDegrees;
        }

        // ワールドの負Z方向を正規化し短すぎる軸は下向きを返します。
        [[nodiscard]] DirectX::XMFLOAT3 WorldDirection() const noexcept;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "DirectionalLight";
        }

    private:
        // 光源のRGB色
        DirectX::XMFLOAT3 m_color;
        // 光の強度
        float m_intensity;
        // 影の到達距離ワールド単位
        float m_shadowDistance;
        // 影判定の深度バイアス
        float m_shadowBias;
        // 影判定の法線バイアス
        float m_shadowNormalBias;
        // 影の強度
        float m_shadowStrength;
        // 対数分割の比率
        float m_shadowSplitLambda;
        // 影の分割数
        std::uint32_t m_shadowCascadeCount;
        // 影を作る指定
        bool m_castsShadows;
        // DLLのメンバー位置の互換性のため末尾配置を維持します。
        // 光源の角直径度
        float m_angularDiameterDegrees{ 0.53f };
    };
}
