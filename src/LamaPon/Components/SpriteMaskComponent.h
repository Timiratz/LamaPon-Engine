#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    enum class SpriteMaskShape
    {
        Rectangle,
        Circle
    };

    // GameObjectの位置を中心に、矩形または円の形でSprite Rendererの
    // 表示範囲を切り抜きます（フォグ・オブ・ウォーの視界、懐中電灯の
    // 明かり、ウィンドウ状の演出などに使えます）。対象になるのは
    // SpriteRendererComponent::SetMaskInteractionで
    // VisibleInsideMask／VisibleOutsideMaskを設定したSpriteだけです。
    // マスクは境界がくっきりしたクリップ（半透明フェードなし）で、
    // 任意画像の透明部分ではなくこの矩形／円の形状のみに対応します。
    class SpriteMaskComponent final : public Component
    {
    public:
        explicit SpriteMaskComponent(
            SpriteMaskShape shape = SpriteMaskShape::Rectangle,
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f }) noexcept;

        void SetShape(const SpriteMaskShape shape) noexcept
        {
            m_shape = shape;
        }
        [[nodiscard]] SpriteMaskShape Shape() const noexcept
        {
            return m_shape;
        }

        void SetSize(const DirectX::XMFLOAT2& size) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept
        {
            return m_size;
        }

        // ワールド座標系のXY（このエンジンの2DはワールドXY＝画面
        // ピクセルの1対1対応です）。
        [[nodiscard]] DirectX::XMFLOAT2 WorldPosition() const noexcept;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpriteMask";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        SpriteMaskShape m_shape;
        DirectX::XMFLOAT2 m_size;
    };
}
