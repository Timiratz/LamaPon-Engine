#include "LamaPon/Components/UIScrollViewComponent.h"

#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>

namespace
{
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
        const auto* transform =
            Owner().GetComponent<
                UIRectTransformComponent>();
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
        for (const GameObject* ancestor = &Owner();
            ancestor != nullptr;
            ancestor = ancestor->Parent())
        {
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
        // 子Rect（左上アンカー前提。LayoutGroupの出力と同じ座標系）
        // の下端の最大値をコンテンツ高さとします。
        float bottom = 0.0f;
        for (const auto* child : Owner().Children())
        {
            if (child == nullptr || !child->IsEnabled())
            {
                continue;
            }
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

        const auto& pointer =
            m_graphics->Input().Pointer();
        const auto rect = ViewRect(*m_graphics);
        const bool hovered =
            pointer.valid
            && rect.Contains(pointer.position);
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
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        if (m_graphics == nullptr)
        {
            return;
        }
        const auto rect = ViewRect(*m_graphics);
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        const auto premultipliedBackground =
            Premultiply(m_backgroundColor);
        spriteBatch.Draw(
            whiteTexture,
            rect.minimum,
            nullptr,
            XMLoadFloat4(&premultipliedBackground),
            0.0f,
            {},
            XMFLOAT2{ size.x, size.y });

        // 右端の縦スクロールバー。
        const float maximumOffset =
            MaximumScrollOffset();
        if (maximumOffset <= 0.0f
            || m_contentHeight <= 0.0f)
        {
            return;
        }
        const float scale = std::max(
            CanvasScale(),
            0.0001f);
        const float viewHeight = size.y;
        const float contentPixels =
            m_contentHeight * scale;
        const float thumbHeight = std::max(
            viewHeight * viewHeight
                / std::max(contentPixels, 1.0f),
            8.0f);
        const float trackRange =
            viewHeight - thumbHeight;
        const float thumbOffset =
            trackRange
            * (m_scrollOffset / maximumOffset);
        const float barWidth = 6.0f;
        const auto premultipliedBar =
            Premultiply(m_scrollbarColor);
        spriteBatch.Draw(
            whiteTexture,
            XMFLOAT2{
                rect.maximum.x - barWidth,
                rect.minimum.y + thumbOffset },
            nullptr,
            XMLoadFloat4(&premultipliedBar),
            0.0f,
            {},
            XMFLOAT2{ barWidth, thumbHeight });
    }
}
