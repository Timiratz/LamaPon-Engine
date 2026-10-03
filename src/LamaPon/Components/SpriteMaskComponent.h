#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    enum class SpriteMaskShape
    {
        // XY軸に沿う矩形で切り抜きます。
        Rectangle,
        // Xサイズの半分を半径とする円で切り抜きます。
        Circle
    };

    // マスク内外の指定がある標準のワールドSpriteを最近傍1個の形状で切り抜きます。
    // 形状は回転・スケール・画像の透明度を使わず、サイズには有限値を指定します。
    class SpriteMaskComponent final : public Component
    {
    public:
        // 矩形または円で切り抜くマスクを作ります(shape: マスクの形状, size: ワールドXY全幅で円はXが直径)。
        explicit SpriteMaskComponent(
            SpriteMaskShape shape = SpriteMaskShape::Rectangle,
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f }) noexcept;

        // 切り抜く形状を設定します(shape: マスクの形状)。
        void SetShape(const SpriteMaskShape shape) noexcept
        {
            m_shape = shape;
        }
        // マスクの形状を返します。
        [[nodiscard]] SpriteMaskShape Shape() const noexcept
        {
            return m_shape;
        }

        // 各軸を1以上に制限して設定します(size: ワールドXY全幅で円はXが直径)。
        void SetSize(const DirectX::XMFLOAT2& size) noexcept;
        // マスクのワールドXY全幅を返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept
        {
            return m_size;
        }

        // マスク中心のワールドXY位置を返します。
        [[nodiscard]] DirectX::XMFLOAT2 WorldPosition() const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "SpriteMask";
        }

    protected:
        // マスク形状の輪郭を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // マスクの矩形・円の形状
        SpriteMaskShape m_shape;
        // ワールドXY全幅でXが円直径
        DirectX::XMFLOAT2 m_size;
    };
}
