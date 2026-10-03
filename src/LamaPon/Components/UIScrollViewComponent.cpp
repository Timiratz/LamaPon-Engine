#include "LamaPon/Components/UIScrollViewComponent.h"

#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>

namespace
{
    // RGBへアルファを乗算した描画色を返します(color: アルファ乗算前のRGBA色)。
    DirectX::XMFLOAT4 Premultiply(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
    }
}

namespace LamaPon
{
    void UIScrollViewComponent::SetScrollOffset(
        const float offset) noexcept
    {
        m_scrollOffset = std::clamp(
            offset,
            0.0f,
            MaximumScrollOffset());
    }

    void UIScrollViewComponent::SetScrollSpeed(
        const float speed) noexcept
    {
        m_scrollSpeed = std::max(speed, 1.0f);
    }

    float UIScrollViewComponent::MaximumScrollOffset()
        const noexcept
    {
        // 表示領域のUI矩形
        const auto* transform =
            Owner().GetComponent<
                UIRectTransformComponent>();
        // 表示領域の高さ
        const float viewHeight = transform != nullptr
            ? transform->SizeDelta().y
            : 0.0f;
        return std::max(
            m_contentHeight - viewHeight,
            0.0f);
    }

    UIRect UIScrollViewComponent::ViewRect(
        const GraphicsDevice& graphics) const noexcept
    {
        // 表示領域のUI矩形
        if (const auto* transform =
            Owner().GetComponent<
                UIRectTransformComponent>())
        {
            return transform->Resolve(
                static_cast<float>(graphics.UIWidth()),
                static_cast<float>(
                    graphics.UIHeight()));
        }
        return {};
    }

    float UIScrollViewComponent::CanvasScale()
        const noexcept
    {
        if (m_graphics == nullptr)
        {
            return 1.0f;
        }
        // キャンバスを探す物体
        for (const GameObject* ancestor = &Owner();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
            // 最寄りのキャンバス設定
            if (const auto* canvas =
                ancestor->GetComponent<
                    UICanvasComponent>())
            {
                return canvas->ScaleFactor(
                    static_cast<float>(
                        m_graphics->UIWidth()),
                    static_cast<float>(
                        m_graphics->UIHeight()));
            }
        }
        return 1.0f;
    }

    void UIScrollViewComponent::RefreshContentHeight()
        noexcept
    {
        // 子の左上基準の下端最大値を内容高に使います。
        // 拡縮前の子の下端の最大値
        float bottom = 0.0f;
        // 内容高を計算する直下の子
        for (const auto* child : Owner().Children())
        {
            if (child == nullptr || !child->IsEnabled())
            {
                continue;
            }
            // 内容高を計算する子の矩形
            if (const auto* childTransform =
                child->GetComponent<
                    UIRectTransformComponent>())
            {
                bottom = std::max(
                    bottom,
                    childTransform->AnchoredPosition().y
                        + childTransform->SizeDelta().y);
            }
        }
        m_contentHeight = bottom;
    }

    void UIScrollViewComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
    }

    void UIScrollViewComponent::OnUpdate(float)
    {
        RefreshContentHeight();
        if (m_graphics == nullptr
            || !m_graphics->IsInitialized()
            || !m_interactable)
        {
            m_dragging = false;
            SetScrollOffset(m_scrollOffset);
            return;
        }

        // 今回のポインター入力状態
        const auto& pointer =
            m_graphics->Input().Pointer();
        // 表示領域の矩形ピクセル
        const auto rect = ViewRect(*m_graphics);
        // 有効なポインターが矩形内
        const bool hovered =
            pointer.valid
            && rect.Contains(pointer.position);
        // ピクセル換算のキャンバス倍率
        const float scale = std::max(
            CanvasScale(),
            0.0001f);

        // ホイールスクロール（上回転で先頭方向へ）。
        if (hovered && pointer.wheel != 0.0f)
        {
            SetScrollOffset(
                m_scrollOffset
                - pointer.wheel * m_scrollSpeed);
        }

        // ドラッグスクロール。
        if (pointer.pressed && hovered)
        {
            m_dragging = true;
        }
        if (!pointer.down)
        {
            m_dragging = false;
        }
        if (m_dragging
            && (pointer.delta.x != 0.0f
                || pointer.delta.y != 0.0f))
        {
            SetScrollOffset(
                m_scrollOffset
                - pointer.delta.y / scale);
        }

        // コンテンツが縮んだ場合のはみ出しを詰めます。
        SetScrollOffset(m_scrollOffset);
    }

    void UIScrollViewComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        if (m_graphics == nullptr)
        {
            return;
        }
        // 表示領域の矩形ピクセル
        const auto rect = ViewRect(*m_graphics);
        // 表示幅・高さピクセル
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        // アルファ乗算済みの背景色
        const auto premultipliedBackground =
            Premultiply(m_backgroundColor);
        // 背景またはバーの描画要求
        SpriteDrawRequest request;
        request.position = rect.minimum;
        request.tint = premultipliedBackground;
        request.scale = { size.x, size.y };
        static_cast<void>(sprites.Draw(request));

        // 右端の縦スクロールバー。
        // 拡縮前のスクロール量上限
        const float maximumOffset =
            MaximumScrollOffset();
        if (maximumOffset <= 0.0f
            || m_contentHeight <= 0.0f)
        {
            return;
        }
        // ピクセル換算のキャンバス倍率
        const float scale = std::max(
            CanvasScale(),
            0.0001f);
        // 表示領域の高さ
        const float viewHeight = size.y;
        // contentPixels: canvas scaleを反映したcontent高さ。
        const float contentPixels =
            m_contentHeight * scale;
        // つまみの高さピクセル
        const float thumbHeight = std::max(
            viewHeight * viewHeight
                / std::max(contentPixels, 1.0f),
            8.0f);
        // つまみが移動する高さ
        const float trackRange =
            viewHeight - thumbHeight;
        // つまみの上端の移動量
        const float thumbOffset =
            trackRange
            * (m_scrollOffset / maximumOffset);
        // スクロールバーの幅ピクセル
        const float barWidth = 6.0f;
        // アルファ乗算済みのバー色
        const auto premultipliedBar =
            Premultiply(m_scrollbarColor);
        request.position = {
            rect.maximum.x - barWidth,
            rect.minimum.y + thumbOffset };
        request.tint = premultipliedBar;
        request.scale = { barWidth, thumbHeight };
        static_cast<void>(sprites.Draw(request));
    }

    bool UIScrollViewComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        description.geometry = "UIスクロールビュー";
        description.state = "並び順 " + std::to_string(RenderSortOrder());
        return true;
    }
}
