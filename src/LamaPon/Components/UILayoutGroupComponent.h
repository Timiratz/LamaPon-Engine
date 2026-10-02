#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    enum class UILayoutAxis
    {
        // 左から右へ子を並べます。
        Horizontal,
        // 上から下へ子を並べます。
        Vertical
    };

    enum class UILayoutAlignment
    {
        // 並び方向と直角の始端に合わせます。
        Start,
        // 並び方向と直角の中央に合わせます。
        Center,
        // 並び方向と直角の終端に合わせます。
        End
    };

    // 整列領域にSizeDeltaを使うためアンカーで伸縮しない矩形を使い、数値には有限値を指定します。
    class UILayoutGroupComponent final : public Component
    {
    public:
        // 直下の子の整列を設定します(axis: 子を並べる方向, spacing: 拡縮前の非負にする間隔)。
        explicit UILayoutGroupComponent(
            UILayoutAxis axis =
                UILayoutAxis::Vertical,
            float spacing = 8.0f) noexcept;

        // 子を並べる方向を設定します(axis: 整列方向)。
        void SetAxis(const UILayoutAxis axis) noexcept
        {
            m_axis = axis;
        }
        // 間隔を非負に制限して設定します(spacing: 拡縮前の間隔)。
        void SetSpacing(float spacing) noexcept;
        // 内側余白を非負にして設定します(padding: 拡縮前の左・上・右・下の余白)。
        void SetPadding(
            const DirectX::XMFLOAT4& padding) noexcept;
        // 並び方向と直角の位置合わせを設定します(alignment: 始端・中央・終端の指定)。
        void SetChildAlignment(
            const UILayoutAlignment alignment) noexcept
        {
            m_childAlignment = alignment;
        }

        // 子を並べる方向を返します。
        [[nodiscard]] UILayoutAxis Axis() const noexcept
        {
            return m_axis;
        }
        // 拡縮前の子の間隔を返します。
        [[nodiscard]] float Spacing() const noexcept
        {
            return m_spacing;
        }
        // 拡縮前の左・上・右・下の余白を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            Padding() const noexcept
        {
            return m_padding;
        }
        // 並び方向と直角の位置合わせを返します。
        [[nodiscard]] UILayoutAlignment
            ChildAlignment() const noexcept
        {
            return m_childAlignment;
        }

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UILayoutGroup";
        }

        // 直下の有効な子の矩形を順番に整列します。
        // 子のアンカーとピボットを左上へ設定し、既存の基準点位置を上書きします。
        void ApplyLayout();

    protected:
        // 子の矩形を整列します(deltaTime: 使用しない経過秒数)。
        void OnUpdate(float deltaTime) override;

    private:
        // 子を並べる方向
        UILayoutAxis m_axis;
        // 拡縮前の子の間隔
        float m_spacing;
        // 拡縮前の左・上・右・下余白
        DirectX::XMFLOAT4 m_padding{
            8.0f, 8.0f, 8.0f, 8.0f };
        // 並び方向と直角の位置合わせ
        UILayoutAlignment m_childAlignment{
            UILayoutAlignment::Start };
    };
}
