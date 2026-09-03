#include "LamaPon/Components/UIInputFieldComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/GameObject.h"

#include <SpriteBatch.h>

#include <algorithm>
#include <cmath>
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

    // UTF-32のコードポイント1つをUTF-8へ変換して追記します。
    void AppendUtf8(
        std::string& target,
        const char32_t codePoint)
    {
        if (codePoint < 0x80)
        {
            target.push_back(
                static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800)
        {
            target.push_back(static_cast<char>(
                0xC0 | (codePoint >> 6)));
            target.push_back(static_cast<char>(
                0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint < 0x10000)
        {
            target.push_back(static_cast<char>(
                0xE0 | (codePoint >> 12)));
            target.push_back(static_cast<char>(
                0x80 | ((codePoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(
                0x80 | (codePoint & 0x3F)));
        }
        else
        {
            target.push_back(static_cast<char>(
                0xF0 | (codePoint >> 18)));
            target.push_back(static_cast<char>(
                0x80 | ((codePoint >> 12) & 0x3F)));
            target.push_back(static_cast<char>(
                0x80 | ((codePoint >> 6) & 0x3F)));
            target.push_back(static_cast<char>(
                0x80 | (codePoint & 0x3F)));
        }
    }

    // UTF-8文字列のコードポイント数を数えます。
    std::size_t CountUtf8CodePoints(
        const std::string& text) noexcept
    {
        std::size_t count{};
        for (const char byte : text)
        {
            if ((static_cast<unsigned char>(byte)
                    & 0xC0) != 0x80)
            {
                ++count;
            }
        }
        return count;
    }
}

namespace LamaPon
{
    UIInputFieldComponent::UIInputFieldComponent(
        std::string text,
        std::string placeholder)
        : m_text(std::move(text))
        , m_placeholder(std::move(placeholder))
    {
    }

    void UIInputFieldComponent::SetText(std::string text)
    {
        if (m_text == text)
        {
            return;
        }
        m_text = std::move(text);
        m_valueChanged = true;
        RefreshText();
    }

    void UIInputFieldComponent::SetPlaceholder(
        std::string placeholder)
    {
        m_placeholder = std::move(placeholder);
        RefreshText();
    }

    void UIInputFieldComponent::SetFontFamily(
        std::string family)
    {
        m_fontFamily = std::move(family);
        RefreshText();
    }

    void UIInputFieldComponent::SetFontSize(
        const float size)
    {
        m_fontSize = std::clamp(size, 1.0f, 256.0f);
        RefreshText();
    }

    void UIInputFieldComponent::SetMaxLength(
        const std::size_t maxLength) noexcept
    {
        m_maxLength = std::clamp<std::size_t>(
            maxLength,
            1,
            4096);
    }

    void UIInputFieldComponent::SetInteractable(
        const bool interactable) noexcept
    {
        m_interactable = interactable;
        if (!interactable)
        {
            m_focused = false;
        }
    }

    void UIInputFieldComponent::SetTextColor(
        const DirectX::XMFLOAT4& color)
    {
        m_textColor = color;
        RefreshText();
    }

    void UIInputFieldComponent::SetPlaceholderColor(
        const DirectX::XMFLOAT4& color)
    {
        m_placeholderColor = color;
        RefreshText();
    }

    void UIInputFieldComponent::SetFallbackSize(
        const DirectX::XMFLOAT2& size) noexcept
    {
        m_fallbackSize = {
            std::max(size.x, 1.0f),
            std::max(size.y, 1.0f)
        };
    }

    void UIInputFieldComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();
        RefreshText();
    }

    void UIInputFieldComponent::OnUpdate(
        const float deltaTime)
    {
        if (m_graphics == nullptr
            || !m_graphics->IsInitialized()
            || !m_interactable)
        {
            m_focused = false;
            return;
        }

        const auto& input = m_graphics->Input();
        const auto& pointer = input.Pointer();
        const auto rect = ResolveWidgetRect(
            Owner(),
            m_graphics,
            m_fallbackSize);

        // クリックでフォーカスを取得し、外側クリックで手放します。
        if (pointer.pressed)
        {
            m_focused =
                pointer.valid
                && rect.Contains(pointer.position);
        }

        if (!m_focused)
        {
            m_caretTimer = 0.0f;
            return;
        }
        m_caretTimer += deltaTime;

        bool textChanged = false;
        for (const wchar_t character :
            input.TextInput())
        {
            if (character == L'\r')
            {
                m_submitted = true;
                m_focused = false;
                break;
            }
            if (character == L'\b')
            {
                if (!m_text.empty())
                {
                    RemoveLastCodePoint();
                    textChanged = true;
                }
                continue;
            }
            if (character == L'\t')
            {
                continue;
            }
            if (CountUtf8CodePoints(m_text)
                < m_maxLength)
            {
                AppendCharacter(character);
                textChanged = true;
            }
        }
        if (textChanged)
        {
            m_valueChanged = true;
            RefreshText();
        }
    }

    void UIInputFieldComponent::OnRender2D(
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

        auto backgroundColor = m_focused
            ? m_focusedColor
            : m_backgroundColor;
        if (!m_interactable)
        {
            backgroundColor.w *= 0.5f;
        }
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

        const float padding = size.y * 0.15f;
        const bool showingPlaceholder = m_text.empty();
        const auto& texture = showingPlaceholder
            ? m_placeholderTexture
            : m_textTexture;
        // 文字テクスチャは白で焼いてあるので、本文と説明文で
        // 色を掛け分けます。
        const auto textTint = PremultipliedTextColor(
            showingPlaceholder
                ? m_placeholderColor
                : m_textColor);
        if (texture)
        {
            const float textWidth =
                size.x - padding * 2.0f;
            const float textHeight =
                size.y - padding * 2.0f;
            if (textWidth > 0.0f && textHeight > 0.0f)
            {
                spriteBatch.Draw(
                    texture->view.Get(),
                    XMFLOAT2{
                        rect.minimum.x + padding,
                        rect.minimum.y + padding },
                    nullptr,
                    textTint,
                    0.0f,
                    {},
                    XMFLOAT2{
                        textWidth
                            / static_cast<float>(
                                texture->width),
                        textHeight
                            / static_cast<float>(
                                texture->height)
                    });
            }
        }

        // フォーカス中は右端で点滅するキャレットを描きます。
        if (m_focused
            && std::fmod(m_caretTimer, 1.0f) < 0.5f)
        {
            const auto premultipliedCaret =
                Premultiply(m_textColor);
            spriteBatch.Draw(
                whiteTexture,
                XMFLOAT2{
                    rect.maximum.x - padding - 2.0f,
                    rect.minimum.y + padding },
                nullptr,
                XMLoadFloat4(&premultipliedCaret),
                0.0f,
                {},
                XMFLOAT2{
                    2.0f,
                    size.y - padding * 2.0f });
        }
    }

    void UIInputFieldComponent::RefreshText()
    {
        if (m_assets == nullptr)
        {
            m_textTexture.reset();
            m_placeholderTexture.reset();
            return;
        }
        const TextLayoutOptions layout{
            { 512.0f, 64.0f },
            TextHorizontalAlignment::Left,
            TextVerticalAlignment::Center,
            false };
        m_textTexture = m_text.empty()
            ? nullptr
            : m_assets->LoadTextTexture(
                m_text,
                m_fontFamily,
                m_fontSize,
                layout);
        m_placeholderTexture = m_placeholder.empty()
            ? nullptr
            : m_assets->LoadTextTexture(
                m_placeholder,
                m_fontFamily,
                m_fontSize,
                layout);
    }

    void UIInputFieldComponent::AppendCharacter(
        const wchar_t character)
    {
        // サロゲートペアはUTF-32へ合成してから追記します。
        if (character >= 0xD800 && character <= 0xDBFF)
        {
            m_pendingHighSurrogate = character;
            return;
        }
        char32_t codePoint = character;
        if (character >= 0xDC00 && character <= 0xDFFF)
        {
            if (m_pendingHighSurrogate == 0)
            {
                return;
            }
            codePoint = 0x10000
                + ((static_cast<char32_t>(
                        m_pendingHighSurrogate)
                    - 0xD800) << 10)
                + (static_cast<char32_t>(character)
                    - 0xDC00);
            m_pendingHighSurrogate = 0;
        }
        AppendUtf8(m_text, codePoint);
    }

    void UIInputFieldComponent::RemoveLastCodePoint()
    {
        // 末尾の継続バイト(10xxxxxx)を取り除いてから
        // 先頭バイトを1つ削除します。
        while (!m_text.empty()
            && (static_cast<unsigned char>(
                    m_text.back())
                & 0xC0) == 0x80)
        {
            m_text.pop_back();
        }
        if (!m_text.empty())
        {
            m_text.pop_back();
        }
    }
}
