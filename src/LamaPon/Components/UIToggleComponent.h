#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    // チェックボックス型のオン／オフUI。クリックで切り替わります。
    class UIToggleComponent final : public Component
    {
    public:
        explicit UIToggleComponent(
            std::string label = "トグル",
            bool isOn = false);

        void SetIsOn(bool isOn) noexcept;
        void SetLabel(std::string label);
        void SetFontFamily(std::string family);
        void SetFontSize(float size);
        void SetInteractable(bool interactable) noexcept;
        void SetBoxColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_boxColor = color;
        }
        void SetCheckColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_checkColor = color;
        }
        void SetTextColor(const DirectX::XMFLOAT4& color);
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        [[nodiscard]] bool IsOn() const noexcept
        {
            return m_isOn;
        }
        // 値が変化していたらtrueを返し、フラグを消費します。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        [[nodiscard]] const std::string&
            Label() const noexcept
        {
            return m_label;
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
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            BoxColor() const noexcept
        {
            return m_boxColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CheckColor() const noexcept
        {
            return m_checkColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
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
            return "UIToggle";
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

        std::string m_label;
        std::string m_fontFamily{ "Yu Gothic UI" };
        float m_fontSize{ 24.0f };
        bool m_isOn{};
        bool m_interactable{ true };
        bool m_hovered{};
        bool m_pressedInside{};
        bool m_valueChanged{};
        DirectX::XMFLOAT4 m_boxColor{
            0.16f, 0.20f, 0.28f, 0.96f };
        DirectX::XMFLOAT4 m_checkColor{
            0.30f, 0.75f, 0.40f, 1.0f };
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT2 m_fallbackSize{
            220.0f, 40.0f };
        int m_sortOrder{};
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        GraphicsDevice* m_graphics{};
        AssetManager* m_assets{};
    };
}
