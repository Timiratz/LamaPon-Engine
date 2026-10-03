#include "LamaPon/Components/UIRectTransformComponent.h"

#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace
{
    // 各軸を0〜1に制限します(value: 有限のXY比率)。
    DirectX::XMFLOAT2 ClampUnit(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return {
            std::clamp(value.x, 0.0f, 1.0f),
            std::clamp(value.y, 0.0f, 1.0f)
        };
    }
}

namespace LamaPon
{
    UIRectTransformComponent::
        UIRectTransformComponent(
            const DirectX::XMFLOAT2 anchorMin,
            const DirectX::XMFLOAT2 anchorMax,
            const DirectX::XMFLOAT2 pivot,
            const DirectX::XMFLOAT2 anchoredPosition,
            const DirectX::XMFLOAT2 sizeDelta) noexcept
        : m_anchorMin(ClampUnit(anchorMin))
        , m_anchorMax(ClampUnit(anchorMax))
        , m_pivot(ClampUnit(pivot))
        , m_anchoredPosition(anchoredPosition)
        , m_sizeDelta(sizeDelta)
    {
        m_anchorMax.x =
            std::max(m_anchorMax.x, m_anchorMin.x);
        m_anchorMax.y =
            std::max(m_anchorMax.y, m_anchorMin.y);
    }

    void UIRectTransformComponent::SetAnchorMin(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_anchorMin = ClampUnit(value);
        m_anchorMax.x =
            std::max(m_anchorMax.x, m_anchorMin.x);
        m_anchorMax.y =
            std::max(m_anchorMax.y, m_anchorMin.y);
    }

    void UIRectTransformComponent::SetAnchorMax(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_anchorMax = ClampUnit(value);
        m_anchorMin.x =
            std::min(m_anchorMin.x, m_anchorMax.x);
        m_anchorMin.y =
            std::min(m_anchorMin.y, m_anchorMax.y);
    }

    void UIRectTransformComponent::SetPivot(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_pivot = ClampUnit(value);
    }

    void UIRectTransformComponent::SetAnchoredPosition(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_anchoredPosition = value;
    }

    void UIRectTransformComponent::SetSizeDelta(
        const DirectX::XMFLOAT2& value) noexcept
    {
        m_sizeDelta = value;
    }

    UIRect UIRectTransformComponent::Resolve(
        const float viewportWidth,
        const float viewportHeight) const noexcept
    {
        // 基準となる親の表示矩形
        UIRect parentRect{
            {},
            {
                std::max(viewportWidth, 1.0f),
                std::max(viewportHeight, 1.0f)
            }
        };
        // 配置に掛けるキャンバス倍率
        float scale = 1.0f;
        // 最寄りの親矩形のスクロール
        const UIScrollViewComponent* scrollView{};

        // 矩形・倍率を探す物体
        for (const GameObject* ancestor =
                Owner().Parent();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            // 最寄りの親のUI矩形
            if (const auto* parentTransform =
                ancestor->GetComponent<
                    UIRectTransformComponent>())
            {
                parentRect = parentTransform->Resolve(
                    viewportWidth,
                    viewportHeight);
                // 親がScrollViewならコンテンツをスクロール量だけずらします。
                scrollView = ancestor->GetComponent<
                    UIScrollViewComponent>();
                break;
            }
        }

        // 矩形・倍率を探す物体
        for (const GameObject* ancestor =
                &Owner();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            // 最寄りのキャンバス設定
            if (const auto* canvas =
                ancestor->GetComponent<
                    UICanvasComponent>())
            {
                scale = canvas->ScaleFactor(
                    viewportWidth,
                    viewportHeight);
                break;
            }
        }

        if (scrollView != nullptr
            && scrollView->IsEnabled())
        {
            // ピクセル換算の上向き移動量
            const float shift =
                scrollView->ScrollOffset() * scale;
            parentRect.minimum.y -= shift;
            parentRect.maximum.y -= shift;
        }

        // 親の表示幅・高さピクセル
        const auto parentSize =
            parentRect.Size();
        // アンカー始点のピクセル位置
        const DirectX::XMFLOAT2 anchorPixelsMin{
            parentRect.minimum.x
                + parentSize.x * m_anchorMin.x,
            parentRect.minimum.y
                + parentSize.y * m_anchorMin.y
        };
        // アンカー終点のピクセル位置
        const DirectX::XMFLOAT2 anchorPixelsMax{
            parentRect.minimum.x
                + parentSize.x * m_anchorMax.x,
            parentRect.minimum.y
                + parentSize.y * m_anchorMax.y
        };
        // 非負の表示幅・高さピクセル
        const DirectX::XMFLOAT2 size{
            std::max(
                anchorPixelsMax.x
                    - anchorPixelsMin.x
                    + m_sizeDelta.x * scale,
                0.0f),
            std::max(
                anchorPixelsMax.y
                    - anchorPixelsMin.y
                    + m_sizeDelta.y * scale,
                0.0f)
        };
        // 基準点の表示ピクセル位置
        const DirectX::XMFLOAT2 pivotPosition{
            anchorPixelsMin.x
                + (anchorPixelsMax.x
                    - anchorPixelsMin.x)
                    * m_pivot.x
                + m_anchoredPosition.x * scale,
            anchorPixelsMin.y
                + (anchorPixelsMax.y
                    - anchorPixelsMin.y)
                    * m_pivot.y
                + m_anchoredPosition.y * scale
        };
        return UIRect{
            {
                pivotPosition.x
                    - size.x * m_pivot.x,
                pivotPosition.y
                    - size.y * m_pivot.y
            },
            {
                pivotPosition.x
                    + size.x * (1.0f - m_pivot.x),
                pivotPosition.y
                    + size.y * (1.0f - m_pivot.y)
            }
        };
    }
}
