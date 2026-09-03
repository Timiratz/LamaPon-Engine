#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class UICanvasComponent final : public Component
    {
    public:
        explicit UICanvasComponent(
            DirectX::XMFLOAT2 referenceResolution =
                { 1280.0f, 720.0f },
            float matchWidthOrHeight = 0.5f) noexcept;

        void SetReferenceResolution(
            const DirectX::XMFLOAT2& resolution) noexcept;
        void SetMatchWidthOrHeight(float match) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT2&
            ReferenceResolution() const noexcept
        {
            return m_referenceResolution;
        }
        [[nodiscard]] float
            MatchWidthOrHeight() const noexcept
        {
            return m_matchWidthOrHeight;
        }
        [[nodiscard]] float ScaleFactor(
            float viewportWidth,
            float viewportHeight) const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UICanvas";
        }

    private:
        DirectX::XMFLOAT2 m_referenceResolution;
        float m_matchWidthOrHeight;
    };
}
