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
        // 整列領域のUI矩形
        const auto* groupTransform =
            Owner().GetComponent<
                UIRectTransformComponent>();
        if (groupTransform == nullptr)
        {
            return;
        }

        // サイズと基準点位置はともにキャンバス拡縮前の単位で計算します。
        // 拡縮前の整列領域サイズ
        const auto groupSize =
            groupTransform->SizeDelta();
        // 横方向に子を並べる指定
        const bool horizontal =
            m_axis == UILayoutAxis::Horizontal;
        // 余白を除く副軸の利用幅
        const float crossAvailable = horizontal
            ? groupSize.y - m_padding.y - m_padding.w
            : groupSize.x - m_padding.x - m_padding.z;

        // 次の子の主軸の配置位置
        float mainOffset = horizontal
            ? m_padding.x
            : m_padding.y;
        // 配置する直下の子物体
        for (auto* child : Owner().Children())
        {
            if (child == nullptr
                || !child->IsEnabled())
            {
                continue;
            }
            // 配置する子のUI矩形
            auto* childTransform =
                child->GetComponent<
                    UIRectTransformComponent>();
            if (childTransform == nullptr)
            {
                continue;
            }

            // 子の拡縮前の矩形サイズ
            const auto childSize =
                childTransform->SizeDelta();
            // 子の拡縮前の副軸サイズ
            const float childCross = horizontal
                ? childSize.y
                : childSize.x;
            // 子の副軸の配置位置
            float crossOffset = horizontal
                ? m_padding.y
                : m_padding.x;
            // 副軸の余白を除く空き幅
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
