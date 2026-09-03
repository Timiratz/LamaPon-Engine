#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    class GraphicsDevice;

    // 水平スライダー。バーをドラッグして値を変更します。
    class UISliderComponent final : public Component
    {
    public:
        explicit UISliderComponent(
            float minimumValue = 0.0f,
            float maximumValue = 1.0f,
            float value = 0.5f) noexcept;

        void SetRange(
            float minimumValue,
            float maximumValue) noexcept;
        void SetValue(float value) noexcept;
        void SetNormalizedValue(float normalized) noexcept;
        void SetWholeNumbers(bool wholeNumbers) noexcept;
        void SetInteractable(bool interactable) noexcept;
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        void SetFillColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_fillColor = color;
        }
        void SetHandleColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_handleColor = color;
        }
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        [[nodiscard]] float MinimumValue() const noexcept
        {
            return m_minimumValue;
        }
        [[nodiscard]] float MaximumValue() const noexcept
        {
            return m_maximumValue;
        }
        [[nodiscard]] float Value() const noexcept
        {
            return m_value;
        }
        [[nodiscard]] float NormalizedValue() const noexcept;
        [[nodiscard]] bool WholeNumbers() const noexcept
        {
            return m_wholeNumbers;
        }
        // 値が変化していたらtrueを返し、フラグを消費します。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        [[nodiscard]] bool IsDragging() const noexcept
        {
            return m_dragging;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            FillColor() const noexcept
        {
            return m_fillColor;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            HandleColor() const noexcept
        {
            return m_handleColor;
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
            return "UISlider";
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
        [[nodiscard]] float ApplyConstraints(
            float value) const noexcept;

        float m_minimumValue;
        float m_maximumValue;
        float m_value;
        bool m_wholeNumbers{};
        bool m_interactable{ true };
        bool m_dragging{};
        bool m_valueChanged{};
        DirectX::XMFLOAT4 m_backgroundColor{
            0.14f, 0.16f, 0.22f, 0.96f };
        DirectX::XMFLOAT4 m_fillColor{
            0.12f, 0.42f, 0.76f, 1.0f };
        DirectX::XMFLOAT4 m_handleColor{
            0.92f, 0.94f, 0.98f, 1.0f };
        DirectX::XMFLOAT2 m_fallbackSize{
            220.0f, 24.0f };
        int m_sortOrder{};
        GraphicsDevice* m_graphics{};
    };
}
