#include "LamaPon/Components/UISliderComponent.h"

#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <cmath>

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

    LamaPon::UIRect ResolveWidgetRect(
        const LamaPon::GameObject& owner,
        const LamaPon::GraphicsDevice* graphics,
        const DirectX::XMFLOAT2& fallbackSize) noexcept
    {
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
        const float range =
            m_maximumValue - m_minimumValue;
        return range > 0.0f
            ? (m_value - m_minimumValue) / range
            : 0.0f;
    }

    float UISliderComponent::ApplyConstraints(
        const float value) const noexcept
    {
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

        const auto& pointer =
            m_graphics->Input().Pointer();
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
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
        DirectX::SpriteBatch& spriteBatch,
        ID3D11ShaderResourceView* whiteTexture)
    {
        using namespace DirectX;

        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        const auto size = rect.Size();
        if (size.x <= 0.0f || size.y <= 0.0f)
        {
            return;
        }

        const float disabledAlpha =
            m_interactable ? 1.0f : 0.5f;

        auto backgroundColor = m_backgroundColor;
        backgroundColor.w *= disabledAlpha;
        const auto premultipliedBackground =
            Premultiply(backgroundColor);
        spriteBatch.Draw(
            whiteTexture,
            rect.minimum,
            nullptr,
            XMLoadFloat4(&premultipliedBackground),
            0.0f,
            {},
            XMFLOAT2{ size.x, size.y });

        const float fillWidth =
            size.x * NormalizedValue();
        if (fillWidth > 0.0f)
        {
            auto fillColor = m_fillColor;
            fillColor.w *= disabledAlpha;
            const auto premultipliedFill =
                Premultiply(fillColor);
            spriteBatch.Draw(
                whiteTexture,
                rect.minimum,
                nullptr,
                XMLoadFloat4(&premultipliedFill),
                0.0f,
                {},
                XMFLOAT2{ fillWidth, size.y });
        }

        // ハンドルは高さ基準の縦長四角形で描きます。
        const float handleWidth =
            std::min(size.y * 0.6f, size.x);
        const float handleCenter =
            rect.minimum.x
            + size.x * NormalizedValue();
        const float handleLeft = std::clamp(
            handleCenter - handleWidth * 0.5f,
            rect.minimum.x,
            rect.maximum.x - handleWidth);
        auto handleColor = m_handleColor;
        handleColor.w *= disabledAlpha;
        const auto premultipliedHandle =
            Premultiply(handleColor);
        spriteBatch.Draw(
            whiteTexture,
            XMFLOAT2{ handleLeft, rect.minimum.y },
            nullptr,
            XMLoadFloat4(&premultipliedHandle),
            0.0f,
            {},
            XMFLOAT2{ handleWidth, size.y });
    }
}
