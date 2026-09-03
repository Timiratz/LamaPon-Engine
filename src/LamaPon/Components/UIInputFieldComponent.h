#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    // 1行テキスト入力。クリックでフォーカスし、IME確定文字を含む
    // キーボード入力を受け付けます。Enterで確定します。
    class UIInputFieldComponent final : public Component
    {
    public:
        explicit UIInputFieldComponent(
            std::string text = {},
            std::string placeholder =
                "テキストを入力...");

        void SetText(std::string text);
        void SetPlaceholder(std::string placeholder);
        void SetFontFamily(std::string family);
        void SetFontSize(float size);
        void SetMaxLength(std::size_t maxLength) noexcept;
        void SetInteractable(bool interactable) noexcept;
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        void SetFocusedColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_focusedColor = color;
        }
        void SetTextColor(const DirectX::XMFLOAT4& color);
        void SetPlaceholderColor(
            const DirectX::XMFLOAT4& color);
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        void Focus() noexcept
        {
            m_focused = m_interactable;
        }
        void Blur() noexcept
        {
            m_focused = false;
        }

        [[nodiscard]] const std::string&
            Text() const noexcept
        {
            return m_text;
        }
        [[nodiscard]] const std::string&
            Placeholder() const noexcept
        {
            return m_placeholder;
        }
        [[nodiscard]] const std::string&
            FontFamily() const noexcept
        {
            return m_fontFamily;
        }
        [[nodiscard]] float FontSize() const noexcept
        {
            return m_fontSize;
        }
        [[nodiscard]] std::size_t MaxLength() const noexcept
        {
            return m_maxLength;
        }
        [[nodiscard]] bool IsFocused() const noexcept
        {
            return m_focused;
        }
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // テキストが変化していたらtrueを返し、フラグを消費します。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        // Enterで確定されていたらtrueを返し、フラグを消費します。
        [[nodiscard]] bool ConsumeSubmit() noexcept
        {
            const bool submitted = m_submitted;
            m_submitted = false;
            return submitted;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            FocusedColor() const noexcept
        {
            return m_focusedColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            PlaceholderColor() const noexcept
        {
            return m_placeholderColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIInputField";
        }
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(
            GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView*
                whiteTexture) override;

    private:
        void RefreshText();
        void AppendCharacter(wchar_t character);
        void RemoveLastCodePoint();

        std::string m_text;
        std::string m_placeholder;
        std::string m_fontFamily{ "Yu Gothic UI" };
        float m_fontSize{ 24.0f };
        std::size_t m_maxLength{ 256 };
        bool m_interactable{ true };
        bool m_focused{};
        bool m_valueChanged{};
        bool m_submitted{};
        float m_caretTimer{};
        wchar_t m_pendingHighSurrogate{};
        DirectX::XMFLOAT4 m_backgroundColor{
            0.10f, 0.12f, 0.16f, 0.96f };
        DirectX::XMFLOAT4 m_focusedColor{
            0.14f, 0.20f, 0.30f, 1.0f };
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 m_placeholderColor{
            0.65f, 0.68f, 0.75f, 0.8f };
        DirectX::XMFLOAT2 m_fallbackSize{
            280.0f, 44.0f };
        int m_sortOrder{};
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        std::shared_ptr<const TextTextureAsset>
            m_placeholderTexture;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
    };
}
