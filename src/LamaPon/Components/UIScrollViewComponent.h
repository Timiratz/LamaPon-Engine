#pragma once

#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    // 自身のUI矩形で子を切り抜く縦スクロールで、伸縮しない自身の矩形と左上基準の子を使います。
    // 移動量・速度・色は有限値とし、内容高は更新時に再計算します。
    class UIScrollViewComponent final : public Component
    {
    public:
        // 縦スクロールの初期設定を作ります。
        UIScrollViewComponent() = default;

        // 拡縮前のスクロール量を返します。
        [[nodiscard]] float ScrollOffset() const noexcept
        {
            return m_scrollOffset;
        }
        // 更新済みの範囲内にスクロール量を制限します(offset: 拡縮前の移動量)。
        void SetScrollOffset(float offset) noexcept;
        // ホイール移動量を1以上に制限して設定します(speed: 1ノッチの拡縮前の移動量)。
        void SetScrollSpeed(float speed) noexcept;
        // ホイール1ノッチの拡縮前の移動量を返します。
        [[nodiscard]] float ScrollSpeed() const noexcept
        {
            return m_scrollSpeed;
        }
        // 背景の色を設定します(color: アルファ乗算前のRGBA色)。
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        // アルファ乗算前の背景RGBA色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        // スクロールバーの色を設定します(color: アルファ乗算前のRGBA色)。
        void SetScrollbarColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_scrollbarColor = color;
        }
        // アルファ乗算前のスクロールバーRGBA色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            ScrollbarColor() const noexcept
        {
            return m_scrollbarColor;
        }
        // ホイールとドラッグの入力を設定します(interactable: 入力を受ける指定)。
        void SetInteractable(
            const bool interactable) noexcept
        {
            m_interactable = interactable;
        }
        // 入力を受ける指定を返します。
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }

        // 直近の更新で求めた拡縮前のコンテンツ高を返します。
        [[nodiscard]] float ContentHeight() const noexcept
        {
            return m_contentHeight;
        }
        // 保持中の内容高から自身のSizeDelta高を引いた非負の上限を返します。
        [[nodiscard]] float
            MaximumScrollOffset() const noexcept;

        // 自身の表示矩形を返し矩形部品がなければゼロを返します(graphics: UI表示サイズを持つ描画装置)。
        [[nodiscard]] UIRect ViewRect(
            const GraphicsDevice& graphics) const noexcept;

        // 描画内容と並び順の説明を設定してtrueを返します(description: 出力する説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIScrollView";
        }
        // 2D描画の並び順を返します。
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }
        // 2D描画の並び順を設定します(sortOrder: 小さいほど先に描く順序)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // 設定した2D描画の並び順を返します。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

    protected:
        // UI表示サイズと入力の描画装置を借用します(graphics: 借用する描画装置)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 内容高を更新しホイール・ドラッグと範囲制限を処理します(deltaTime: 使用しない経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 背景と右端のスクロールバーを描画します(sprites: スプライト描画の実行先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 直下の有効な子の位置と高さから内容高を更新します。
        // 子のアンカーとピボットは左上とし、サイズと位置はキャンバス拡縮前の単位で使います。
        void RefreshContentHeight() noexcept;
        // 最寄りのキャンバス倍率を返し取得できなければ1を返します。
        [[nodiscard]] float CanvasScale() const noexcept;

        // 拡縮前の上向き移動量
        float m_scrollOffset{};
        // 1ノッチの拡縮前の移動量
        float m_scrollSpeed{ 48.0f };
        // 更新時の拡縮前の内容高
        float m_contentHeight{};
        // ホイール・ドラッグ入力の許可
        bool m_interactable{ true };
        // 内容をドラッグしている状態
        bool m_dragging{};
        // 2D描画の並び順
        int m_sortOrder{};
        // アルファ乗算前の背景RGBA
        DirectX::XMFLOAT4 m_backgroundColor{
            0.08f, 0.09f, 0.12f, 0.9f };
        // アルファ乗算前のバーRGBA
        DirectX::XMFLOAT4 m_scrollbarColor{
            0.6f, 0.65f, 0.75f, 0.9f };
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
    };
}
