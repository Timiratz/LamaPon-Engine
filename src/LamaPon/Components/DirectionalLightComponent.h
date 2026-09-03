#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    class DirectionalLightComponent final : public Component
    {
    public:
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

        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        void SetIntensity(float intensity) noexcept;
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        void SetCastsShadows(const bool enabled) noexcept
        {
            m_castsShadows = enabled;
        }
        [[nodiscard]] bool CastsShadows() const noexcept
        {
            return m_castsShadows;
        }

        void SetShadowDistance(float distance) noexcept;
        [[nodiscard]] float ShadowDistance() const noexcept
        {
            return m_shadowDistance;
        }

        void SetShadowBias(float bias) noexcept;
        [[nodiscard]] float ShadowBias() const noexcept
        {
            return m_shadowBias;
        }

        void SetShadowNormalBias(float bias) noexcept;
        [[nodiscard]] float ShadowNormalBias() const noexcept
        {
            return m_shadowNormalBias;
        }

        void SetShadowStrength(float strength) noexcept;
        [[nodiscard]] float ShadowStrength() const noexcept
        {
            return m_shadowStrength;
        }

        void SetShadowCascadeCount(
            std::uint32_t count) noexcept;
        [[nodiscard]] std::uint32_t
            ShadowCascadeCount() const noexcept
        {
            return m_shadowCascadeCount;
        }

        void SetShadowSplitLambda(float value) noexcept;
        [[nodiscard]] float ShadowSplitLambda() const noexcept
        {
            return m_shadowSplitLambda;
        }

        // 光源の見かけの大きさ（直径・度）。空に見えている太陽は点では
        // なく0.53度の円盤で、これがハイライトの大きさを決めています。
        // 0にすると従来どおりの「数学的な点」になり、つるつるの物体では
        // ハイライトが1画素まで縮んで消えたように見えます。
        // 大きくするほど曇り空のように、光沢がぼんやり広がります。
        void SetAngularDiameterDegrees(float degrees) noexcept;
        [[nodiscard]] float AngularDiameterDegrees() const noexcept
        {
            return m_angularDiameterDegrees;
        }

        [[nodiscard]] DirectX::XMFLOAT3 WorldDirection() const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "DirectionalLight";
        }

    private:
        DirectX::XMFLOAT3 m_color;
        float m_intensity;
        float m_shadowDistance;
        float m_shadowBias;
        float m_shadowNormalBias;
        float m_shadowStrength;
        float m_shadowSplitLambda;
        std::uint32_t m_shadowCascadeCount;
        bool m_castsShadows;
        // 末尾に足しています。既存メンバーの位置がずれないので、
        // 作り直していないGame Module DLLでも読み書きが壊れません。
        float m_angularDiameterDegrees{ 0.53f };
    };
}
