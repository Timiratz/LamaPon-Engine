#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    enum class UILayoutAxis
    {
        Horizontal,
        Vertical
    };

    enum class UILayoutAlignment
    {
        Start,
        Center,
        End
    };

    // 直下の子GameObjectのUIRectTransformを主軸方向へ整列させます。
    // 自身のUIRectTransformのsizeDeltaを整列領域として使うため、
    // アンカーでストレッチしていないRectで使用してください。
    class UILayoutGroupComponent final : public Component
    {
    public:
        explicit UILayoutGroupComponent(
            UILayoutAxis axis =
                UILayoutAxis::Vertical,
            float spacing = 8.0f) noexcept;

        void SetAxis(const UILayoutAxis axis) noexcept
        {
            m_axis = axis;
        }
        void SetSpacing(float spacing) noexcept;
        // 左・上・右・下の順の内側余白。
        void SetPadding(
            const DirectX::XMFLOAT4& padding) noexcept;
        void SetChildAlignment(
            const UILayoutAlignment alignment) noexcept
        {
            m_childAlignment = alignment;
        }

        [[nodiscard]] UILayoutAxis Axis() const noexcept
        {
            return m_axis;
        }
        [[nodiscard]] float Spacing() const noexcept
        {
            return m_spacing;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            Padding() const noexcept
        {
            return m_padding;
        }
        [[nodiscard]] UILayoutAlignment
            ChildAlignment() const noexcept
        {
            return m_childAlignment;
        }

        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UILayoutGroup";
        }

        // 子の整列を即時実行します。エディターの編集モード
        // プレビューでも呼ばれます。
        void ApplyLayout();

    protected:
        void OnUpdate(float deltaTime) override;

    private:
        UILayoutAxis m_axis;
        float m_spacing;
        DirectX::XMFLOAT4 m_padding{
            8.0f, 8.0f, 8.0f, 8.0f };
        UILayoutAlignment m_childAlignment{
            UILayoutAlignment::Start };
    };
}
