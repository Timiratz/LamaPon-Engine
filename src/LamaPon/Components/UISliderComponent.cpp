#include "LamaPon/Components/UISliderComponent.h"

#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <algorithm>
#include <cmath>

namespace
{
    // 描画用にRGBへアルファを掛ける(color: アルファ乗算前のRGBA)。
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

    // UI矩形またはワールドXYと代替サイズから表示範囲を求める(owner: 所有オブジェクト, graphics: 描画機器かnullptr, fallbackSize: 代替の幅と高さ)。
    LamaPon::UIRect ResolveWidgetRect(
        const LamaPon::GameObject& owner,
        const LamaPon::GraphicsDevice* graphics,
        const DirectX::XMFLOAT2& fallbackSize) noexcept
    {
        // UI矩形の配置情報
        if (const auto* transform =
            owner.GetComponent<
                LamaPon::UIRectTransformComponent>();
            transform != nullptr && graphics != nullptr)
        {
            return transform->Resolve(
                static_cast<float>(graphics->UIWidth()),
                static_cast<float>(
                    graphics->UIHeight()));
        }
        // 所有者のワールド行列
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            owner.WorldMatrix());
        return {
            { world._41, world._42 },
            {
                world._41 + fallbackSize.x,
                world._42 + fallbackSize.y
            }
        };
    }
}

namespace LamaPon
{
    UISliderComponent::UISliderComponent(
        const float minimumValue,
        const float maximumValue,
        const float value) noexcept
        : m_minimumValue(minimumValue)
        , m_maximumValue(
            std::max(maximumValue, minimumValue))
        , m_value(value)
    {
        m_value = ApplyConstraints(value);
    }

    void UISliderComponent::SetRange(
        const float minimumValue,
        const float maximumValue) noexcept
    {
        m_minimumValue = minimumValue;
        m_maximumValue =
            std::max(maximumValue, minimumValue);
        SetValue(m_value);
    }

    void UISliderComponent::SetValue(
        const float value) noexcept
    {
        // 制約適用後の設定値
        const float constrained =
            ApplyConstraints(value);
        if (constrained != m_value)
        {
            m_value = constrained;
            m_valueChanged = true;
        }
    }

    void UISliderComponent::SetNormalizedValue(
        const float normalized) noexcept
    {
        SetValue(
            m_minimumValue
            + (m_maximumValue - m_minimumValue)
                * std::clamp(normalized, 0.0f, 1.0f));
    }

    void UISliderComponent::SetWholeNumbers(
        const bool wholeNumbers) noexcept
    {
        m_wholeNumbers = wholeNumbers;
        SetValue(m_value);
    }

    void UISliderComponent::SetInteractable(
        const bool interactable) noexcept
    {
        m_interactable = interactable;
        if (!interactable)
        {
            m_dragging = false;
        }
    }

    void UISliderComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    float UISliderComponent::NormalizedValue()
        const noexcept
    {
        // 上限と下限の差
        const float range =
            m_maximumValue - m_minimumValue;
        return range > 0.0f
            ? (m_value - m_minimumValue) / range
            : 0.0f;
    }

    float UISliderComponent::ApplyConstraints(
        const float value) const noexcept
    {
        // 範囲制限と丸め後の値
        float result = std::clamp(
            value,
            m_minimumValue,
            m_maximumValue);
        if (m_wholeNumbers)
        {
            result = std::round(result);
        }
        return result;
    }

    void UISliderComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
    }

    void UISliderComponent::OnUpdate(float)
    {
        if (m_graphics == nullptr
            || !m_graphics->IsInitialized()
            || !m_interactable)
        {
            m_dragging = false;
            return;
        }

        // ポインターの状態
        const auto& pointer =
            m_graphics->Input().Pointer();
        // スライダーの表示矩形
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        // ポインターが矩形内か
        const bool hovered =
            pointer.valid
            && rect.Contains(pointer.position);
        if (pointer.pressed && hovered)
        {
            m_dragging = true;
        }
        if (!pointer.down)
        {
            m_dragging = false;
        }
        if (m_dragging)
        {
            // ドラッグ範囲の表示幅
            const float width = rect.Size().x;
            if (width > 0.0f)
            {
                SetNormalizedValue(
                    (pointer.position.x
                        - rect.minimum.x)
                    / width);
            }
        }
    }

    void UISliderComponent::OnRender2D(
        const SpriteDrawContext& sprites)
    {
        // スライダーの表示矩形
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        // スライダーの表示幅と高さ
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        // 操作禁止時のアルファ倍率
        const float disabledAlpha =
            m_interactable ? 1.0f : 0.5f;

        // 状態に応じた背景RGBA
        auto backgroundColor = m_backgroundColor;
        backgroundColor.w *= disabledAlpha;
        // アルファ乗算済み背景色
        const auto premultipliedBackground =
            Premultiply(backgroundColor);
        // 各部分のスプライト描画指定
        SpriteDrawRequest request;
        request.position = rect.minimum;
        request.tint = premultipliedBackground;
        request.scale = { size.x, size.y };
        static_cast<void>(sprites.Draw(request));

        // 値まで塗りつぶす表示幅
        const float fillWidth =
            size.x * NormalizedValue();
        if (fillWidth > 0.0f)
        {
            // 状態に応じた塗りつぶし色
            auto fillColor = m_fillColor;
            fillColor.w *= disabledAlpha;
            // アルファ乗算済み塗りつぶし色
            const auto premultipliedFill =
                Premultiply(fillColor);
            request.tint = premultipliedFill;
            request.scale = { fillWidth, size.y };
            static_cast<void>(sprites.Draw(request));
        }


        // ハンドルの表示幅
        const float handleWidth =
            std::min(size.y * 0.6f, size.x);
        // 値が示すハンドル中心のX座標
        const float handleCenter =
            rect.minimum.x
            + size.x * NormalizedValue();
        // 表示範囲に収めたハンドル左端
        const float handleLeft = std::clamp(
            handleCenter - handleWidth * 0.5f,
            rect.minimum.x,
            rect.maximum.x - handleWidth);
        // 状態に応じたハンドル色
        auto handleColor = m_handleColor;
        handleColor.w *= disabledAlpha;
        // アルファ乗算済みハンドル色
        const auto premultipliedHandle =
            Premultiply(handleColor);
        request.position = { handleLeft, rect.minimum.y };
        request.tint = premultipliedHandle;
        request.scale = { handleWidth, size.y };
        static_cast<void>(sprites.Draw(request));
    }

    bool UISliderComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        description.geometry = "UIスライダー";
        description.material = "値 " + Detail::FrameDebugNumber(Value());
        description.state = "並び順 " + std::to_string(RenderSortOrder());
        return true;
    }
}
