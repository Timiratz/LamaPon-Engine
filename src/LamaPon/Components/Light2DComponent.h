#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // ワールド空間のSprite RendererとTilemapへ加算式の光を与えます
    // （Particle Systemや、独自Shaderを持つSpriteRendererは対象外）。
    // UIは既定では照らしません（文字が読めなくなるほうが困るため）。
    // AffectsUIを立てたLight2Dが1つでもあると、UIも照らされます。
    // 光源から遠いスプライトは通常どおりの明るさのままで、暗くなることは
    // ありません（近くのLight2Dが色と明るさを加算するだけの仕組みです）。
    class Light2DComponent final : public Component
    {
    public:
        explicit Light2DComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.9f, 0.7f },
            float intensity = 1.0f,
            float radius = 150.0f) noexcept;

        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        void SetIntensity(float intensity) noexcept;
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        void SetRadius(float radius) noexcept;
        [[nodiscard]] float Radius() const noexcept
        {
            return m_radius;
        }

        // UI（Rect Transform付きのSprite Renderer）も照らすかどうか。
        // 既定はfalseです。UIは「読めること」が最優先で、勝手に色が
        // 乗ると困る場面のほうが多いためです。ランタンで照らされる
        // メニューのような演出をしたいときだけ立ててください。
        void SetAffectsUI(const bool enabled) noexcept
        {
            m_affectsUI = enabled;
        }
        [[nodiscard]] bool AffectsUI() const noexcept
        {
            return m_affectsUI;
        }

        // ワールド座標系のXY（このエンジンの2DはワールドXY=画面ピクセルの
        // 1対1対応です。SpriteRendererComponent::OnRender2Dと同じ変換）。
        [[nodiscard]] DirectX::XMFLOAT2 WorldPosition() const noexcept;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Light2D";
        }

    protected:
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        DirectX::XMFLOAT3 m_color;
        float m_intensity;
        float m_radius;
        // 末尾に足しています。既存メンバーの位置がずれないので、
        // 作り直していないGame Module DLLでも読み書きが壊れません。
        bool m_affectsUI{ false };
    };
}
