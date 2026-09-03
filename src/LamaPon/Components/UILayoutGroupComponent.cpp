#include "LamaPon/Components/UILayoutGroupComponent.h"

#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace LamaPon
{
    UILayoutGroupComponent::UILayoutGroupComponent(
        const UILayoutAxis axis,
        const float spacing) noexcept
        : m_axis(axis)
        , m_spacing(std::max(spacing, 0.0f))
    {
    }

    void UILayoutGroupComponent::SetSpacing(
        const float spacing) noexcept
    {
        m_spacing = std::max(spacing, 0.0f);
    }

    void UILayoutGroupComponent::SetPadding(
        const DirectX::XMFLOAT4& padding) noexcept
    {
        m_padding = {
            std::max(padding.x, 0.0f),
            std::max(padding.y, 0.0f),
            std::max(padding.z, 0.0f),
            std::max(padding.w, 0.0f)
        };
    }

    void UILayoutGroupComponent::OnUpdate(float)
    {
        ApplyLayout();
    }

    void UILayoutGroupComponent::ApplyLayout()
    {
        const auto* groupTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
        if (groupTransform == nullptr)
        {
            return;
        }

        // sizeDeltaはキャンバススケール前の単位。子の配置も同じ
        // 単位系（anchoredPosition）で行うため一貫します。
        const auto groupSize =
            groupTransform->SizeDelta();
        const bool horizontal =
            m_axis == UILayoutAxis::Horizontal;
        const float crossAvailable = horizontal
            ? groupSize.y - m_padding.y - m_padding.w
            : groupSize.x - m_padding.x - m_padding.z;

        float mainOffset = horizontal
            ? m_padding.x
            : m_padding.y;
        for (auto* child : Owner().Children())
        {
            if (child == nullptr
                || !child->IsEnabled())
            {
                continue;
            }
            auto* childTransform =
                child->GetComponent<
                    UIRectTransformComponent>();
            if (childTransform == nullptr)
            {
                continue;
            }

            const auto childSize =
                childTransform->SizeDelta();
            const float childCross = horizontal
                ? childSize.y
                : childSize.x;
            float crossOffset = horizontal
                ? m_padding.y
                : m_padding.x;
            const float slack = std::max(
                crossAvailable - childCross,
                0.0f);
            if (m_childAlignment
                == UILayoutAlignment::Center)
            {
                crossOffset += slack * 0.5f;
            }
            else if (m_childAlignment
                == UILayoutAlignment::End)
            {
                crossOffset += slack;
            }

            // 親Rectの左上を基準に子を並べます。
            childTransform->SetAnchorMin({ 0.0f, 0.0f });
            childTransform->SetAnchorMax({ 0.0f, 0.0f });
            childTransform->SetPivot({ 0.0f, 0.0f });
            childTransform->SetAnchoredPosition(
                horizontal
                    ? DirectX::XMFLOAT2{
                        mainOffset,
                        crossOffset }
                    : DirectX::XMFLOAT2{
                        crossOffset,
                        mainOffset });

            mainOffset += (horizontal
                    ? childSize.x
                    : childSize.y)
                + m_spacing;
        }
    }
}
