#include "LamaPon/Components/UICanvasComponent.h"

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    UICanvasComponent::UICanvasComponent(
        const DirectX::XMFLOAT2 referenceResolution,
        const float matchWidthOrHeight) noexcept
        : m_referenceResolution{
            std::max(referenceResolution.x, 1.0f),
            std::max(referenceResolution.y, 1.0f) }
        , m_matchWidthOrHeight(
            std::clamp(matchWidthOrHeight, 0.0f, 1.0f))
    {
    }

    void UICanvasComponent::SetReferenceResolution(
        const DirectX::XMFLOAT2& resolution) noexcept
    {
        m_referenceResolution = {
            std::max(resolution.x, 1.0f),
            std::max(resolution.y, 1.0f)
        };
    }

    void UICanvasComponent::SetMatchWidthOrHeight(
        const float match) noexcept
    {
        m_matchWidthOrHeight =
            std::clamp(match, 0.0f, 1.0f);
    }

    float UICanvasComponent::ScaleFactor(
        const float viewportWidth,
        const float viewportHeight) const noexcept
    {
        const float widthScale =
            std::max(viewportWidth, 1.0f)
            / m_referenceResolution.x;
        const float heightScale =
            std::max(viewportHeight, 1.0f)
            / m_referenceResolution.y;
        return std::exp2(
            std::lerp(
                std::log2(widthScale),
                std::log2(heightScale),
                m_matchWidthOrHeight));
    }
}
