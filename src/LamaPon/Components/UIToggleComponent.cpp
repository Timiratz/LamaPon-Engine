#include "LamaPon/Components/UIToggleComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <utility>

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
    UIToggleComponent::UIToggleComponent(
        std::string label,
        const bool isOn)
        : m_label(std::move(label))
        , m_isOn(isOn)
    {
    }

    void UIToggleComponent::SetIsOn(
        const bool isOn) noexcept
    {
        if (m_isOn == isOn)
        {
            return;
        }
        m_isOn = isOn;
        m_valueChanged = true;
    }

    void UIToggleComponent::SetLabel(std::string label)
    {
        m_label = std::move(label);
        RefreshText();
    }

    void UIToggleComponent::SetFontFamily(
        std::string family)
    {
        m_fontFamily = std::move(family);
        RefreshText();
    }

    void UIToggleComponent::SetFontSize(const float size)
    {
        m_fontSize = std::clamp(size, 1.0f, 256.0f);
        RefreshText();
    }

    void UIToggleComponent::SetInteractable(
        const bool interactable) noexcept
    {
        m_interactable = interactable;
        if (!interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
        }
    }

    void UIToggleComponent::SetTextColor(
        const DirectX::XMFLOAT4& color)
    {
        m_textColor = color;
        RefreshText();
    }

    void UIToggleComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    void UIToggleComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        RefreshText();
    }

    void UIToggleComponent::OnUpdate(float)
    {
        if (m_graphics == nullptr
            || !m_graphics->IsInitialized()
            || !m_interactable)
        {
            m_hovered = false;
            m_pressedInside = false;
            return;
        }

        const auto& pointer =
            m_graphics->Input().Pointer();
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);
        m_hovered =
            pointer.valid
            && rect.Contains(pointer.position);
        if (pointer.pressed)
        {
            m_pressedInside = m_hovered;
        }
        if (pointer.released)
        {
            if (m_pressedInside && m_hovered)
            {
                m_isOn = !m_isOn;
                m_valueChanged = true;
            }
            m_pressedInside = false;
        }
        if (!pointer.down && !pointer.released)
        {
            m_pressedInside = false;
        }
    }

    void UIToggleComponent::OnRender2D(
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

        // 左端にチェックボックス（高さに合わせた正方形）を描きます。
        const float boxSize = size.y;
        auto boxColor = m_boxColor;
        if (!m_interactable)
        {
            boxColor.w *= 0.5f;
        }
        else if (m_hovered)
        {
            boxColor.x *= 1.25f;
            boxColor.y *= 1.25f;
            boxColor.z *= 1.25f;
        }
        const auto premultipliedBox =
            Premultiply(boxColor);
        spriteBatch.Draw(
            whiteTexture,
            rect.minimum,
            nullptr,
            XMLoadFloat4(&premultipliedBox),
            0.0f,
            {},
            XMFLOAT2{ boxSize, boxSize });

        if (m_isOn)
        {
            const float inset = boxSize * 0.25f;
            auto checkColor = m_checkColor;
            if (!m_interactable)
            {
                checkColor.w *= 0.5f;
            }
            const auto premultipliedCheck =
                Premultiply(checkColor);
            spriteBatch.Draw(
                whiteTexture,
                XMFLOAT2{
                    rect.minimum.x + inset,
                    rect.minimum.y + inset },
                nullptr,
                XMLoadFloat4(&premultipliedCheck),
                0.0f,
                {},
                XMFLOAT2{
                    boxSize - inset * 2.0f,
                    boxSize - inset * 2.0f });
        }

        if (m_textTexture)
        {
            const float labelLeft =
                rect.minimum.x + boxSize
                + boxSize * 0.25f;
            const float labelWidth =
                rect.maximum.x - labelLeft;
            if (labelWidth > 0.0f)
            {
                spriteBatch.Draw(
                    m_textTexture->view.Get(),
                    XMFLOAT2{
                        labelLeft,
                        rect.minimum.y },
                    nullptr,
                    // 白で焼いた文字へ色を掛けます。
                    PremultipliedTextColor(m_textColor),
                    0.0f,
                    {},
                    XMFLOAT2{
                        labelWidth
                            / static_cast<float>(
                                m_textTexture->width),
                        size.y
                            / static_cast<float>(
                                m_textTexture->height)
                    });
            }
        }
    }

    void UIToggleComponent::RefreshText()
    {
        if (m_assets == nullptr || m_label.empty())
        {
            m_textTexture.reset();
            return;
        }
        m_textTexture = m_assets->LoadTextTexture(
            m_label,
            m_fontFamily,
            m_fontSize,
            TextLayoutOptions{
                { 512.0f, 128.0f },
                TextHorizontalAlignment::Left,
                TextVerticalAlignment::Center,
                false });
    }
}
