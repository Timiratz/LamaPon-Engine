#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    struct UIRect final
    {
        DirectX::XMFLOAT2 minimum{};
        DirectX::XMFLOAT2 maximum{};

        [[nodiscard]] DirectX::XMFLOAT2 Size() const noexcept
        {
            return {
                maximum.x - minimum.x,
                maximum.y - minimum.y
            };
        }
        [[nodiscard]] bool Contains(
            const DirectX::XMFLOAT2& point) const noexcept
        {
            return point.x >= minimum.x
                && point.x <= maximum.x
                && point.y >= minimum.y
                && point.y <= maximum.y;
        }
    };

    class UIRectTransformComponent final : public Component
    {
    public:
        explicit UIRectTransformComponent(
            DirectX::XMFLOAT2 anchorMin =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchorMax =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 pivot =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchoredPosition = {},
            DirectX::XMFLOAT2 sizeDelta =
                { 220.0f, 56.0f }) noexcept;

        void SetAnchorMin(
            const DirectX::XMFLOAT2& value) noexcept;
        void SetAnchorMax(
            const DirectX::XMFLOAT2& value) noexcept;
        void SetPivot(
            const DirectX::XMFLOAT2& value) noexcept;
        void SetAnchoredPosition(
            const DirectX::XMFLOAT2& value) noexcept;
        void SetSizeDelta(
            const DirectX::XMFLOAT2& value) noexcept;

        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchorMin() const noexcept { return m_anchorMin; }
        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchorMax() const noexcept { return m_anchorMax; }
        [[nodiscard]] const DirectX::XMFLOAT2&
            Pivot() const noexcept { return m_pivot; }
        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchoredPosition() const noexcept
        {
            return m_anchoredPosition;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            SizeDelta() const noexcept { return m_sizeDelta; }

        [[nodiscard]] UIRect Resolve(
            float viewportWidth,
            float viewportHeight) const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIRectTransform";
        }

    private:
        DirectX::XMFLOAT2 m_anchorMin;
        DirectX::XMFLOAT2 m_anchorMax;
        DirectX::XMFLOAT2 m_pivot;
        DirectX::XMFLOAT2 m_anchoredPosition;
        DirectX::XMFLOAT2 m_sizeDelta;
    };
}
