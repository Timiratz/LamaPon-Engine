#pragma once

#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    // 縦スクロールのコンテナ。自身のUIRectTransformが表示領域、
    // 子GameObjectがコンテンツになります。表示領域の外は
    // シザー矩形でクリッピングされます。
    class UIScrollViewComponent final : public Component
    {
    public:
        UIScrollViewComponent() = default;

        // 現在のスクロール量（キャンバススケール前の単位）。
        [[nodiscard]] float ScrollOffset() const noexcept
        {
            return m_scrollOffset;
        }
        void SetScrollOffset(float offset) noexcept;
        // ホイール1ノッチあたりのスクロール量。
        void SetScrollSpeed(float speed) noexcept;
        [[nodiscard]] float ScrollSpeed() const noexcept
        {
            return m_scrollSpeed;
        }
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        void SetScrollbarColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_scrollbarColor = color;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            ScrollbarColor() const noexcept
        {
            return m_scrollbarColor;
        }
        void SetInteractable(
            const bool interactable) noexcept
        {
            m_interactable = interactable;
        }
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }

        // 子のRectから求めたコンテンツ全体の高さ（スケール前）。
        [[nodiscard]] float ContentHeight() const noexcept
        {
            return m_contentHeight;
        }
        [[nodiscard]] float
            MaximumScrollOffset() const noexcept;

        // 表示領域のワールドUI矩形（クリッピングに使用）。
        [[nodiscard]] UIRect ViewRect(
            const GraphicsDevice& graphics) const noexcept;

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIScrollView";
        }
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        [[nodiscard]] int SortOrder() const noexcept
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
        void RefreshContentHeight() noexcept;
        [[nodiscard]] float CanvasScale() const noexcept;

        float m_scrollOffset{};
        float m_scrollSpeed{ 48.0f };
        float m_contentHeight{};
        bool m_interactable{ true };
        bool m_dragging{};
        int m_sortOrder{};
        DirectX::XMFLOAT4 m_backgroundColor{
            0.08f, 0.09f, 0.12f, 0.9f };
        DirectX::XMFLOAT4 m_scrollbarColor{
            0.6f, 0.65f, 0.75f, 0.9f };
        GraphicsDevice* m_graphics{};
    };
}
