#pragma once

#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    class TextRendererComponent final : public Component
    {
    public:
        explicit TextRendererComponent(
            std::string text = "日本語テキスト",
            std::string fontFamily = "Yu Gothic UI",
            float fontSize = 32.0f,
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            DirectX::XMFLOAT2 layoutSize = { 0.0f, 0.0f },
            bool wordWrap = false,
            TextHorizontalAlignment horizontalAlignment =
                TextHorizontalAlignment::Left,
            TextVerticalAlignment verticalAlignment =
                TextVerticalAlignment::Top);

        void SetText(std::string text);
        [[nodiscard]] const std::string& Text() const noexcept { return m_text; }

        void SetFontFamily(std::string fontFamily);
        [[nodiscard]] const std::string& FontFamily() const noexcept
        {
            return m_fontFamily;
        }

        void SetFontSize(float fontSize);
        [[nodiscard]] float FontSize() const noexcept { return m_fontSize; }

        void SetColor(const DirectX::XMFLOAT4& color);
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }

        void SetLayoutSize(const DirectX::XMFLOAT2& size);
        [[nodiscard]] const DirectX::XMFLOAT2& LayoutSize() const noexcept
        {
            return m_layout.size;
        }

        void SetWordWrap(bool wordWrap);
        [[nodiscard]] bool WordWrap() const noexcept { return m_layout.wordWrap; }

        void SetHorizontalAlignment(TextHorizontalAlignment alignment);
        [[nodiscard]] TextHorizontalAlignment HorizontalAlignment() const noexcept
        {
            return m_layout.horizontalAlignment;
        }

        void SetVerticalAlignment(TextVerticalAlignment alignment);
        [[nodiscard]] TextVerticalAlignment VerticalAlignment() const noexcept
        {
            return m_layout.verticalAlignment;
        }

        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        [[nodiscard]] int SortOrder() const noexcept { return m_sortOrder; }

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "TextRenderer";
        }
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnRender2D(
            DirectX::SpriteBatch& spriteBatch,
            ID3D11ShaderResourceView* whiteTexture) override;

    private:
        void RefreshTexture();

        std::string m_text;
        std::string m_fontFamily;
        float m_fontSize;
        DirectX::XMFLOAT4 m_color;
        TextLayoutOptions m_layout;
        int m_sortOrder{};
        AssetManager* m_assets{};
        GraphicsDevice* m_graphics{};
        std::shared_ptr<const TextTextureAsset> m_texture;
        // RefreshTextureが失敗したとき、同じ値のSetterでも再試行します。
        bool m_textureRefreshPending{};
    };
}
