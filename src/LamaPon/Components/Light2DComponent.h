#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 標準のSprite・Tilemapへ加算照明を与え、粒子と独自シェーダーには適用しません。
    // 数値設定には有限値を指定します。
    class Light2DComponent final : public Component
    {
    public:
        // 色・強度・半径を制限して2D光源を作ります(color: RGB色, intensity: 加算する強度, radius: 影響半径)。
        explicit Light2DComponent(
            DirectX::XMFLOAT3 color = { 1.0f, 0.9f, 0.7f },
            float intensity = 1.0f,
            float radius = 150.0f) noexcept;

        // RGBを0〜1に制限して設定します(color: RGB色)。
        void SetColor(const DirectX::XMFLOAT3& color) noexcept;
        // RGB色を返します。
        [[nodiscard]] const DirectX::XMFLOAT3& Color() const noexcept
        {
            return m_color;
        }

        // 強度を0〜16に制限して設定します(intensity: 加算する強度)。
        void SetIntensity(float intensity) noexcept;
        // 加算する光の強度を返します。
        [[nodiscard]] float Intensity() const noexcept
        {
            return m_intensity;
        }

        // 半径を1〜100000に制限して設定します(radius: ワールドXY単位の半径)。
        void SetRadius(float radius) noexcept;
        // 光の影響半径をワールドXY単位で返します。
        [[nodiscard]] float Radius() const noexcept
        {
            return m_radius;
        }

        // UIへの加算照明を設定します(enabled: UIを照らす指定)。
        void SetAffectsUI(const bool enabled) noexcept
        {
            m_affectsUI = enabled;
        }
        // UIを照らす指定を返します。
        [[nodiscard]] bool AffectsUI() const noexcept
        {
            return m_affectsUI;
        }

        // 光源のワールドXY位置を返します。
        [[nodiscard]] DirectX::XMFLOAT2 WorldPosition() const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "Light2D";
        }

    protected:
        // 光の影響円を描画します(graphics: 描画装置, view: ビュー行列, projection: 射影行列)。
        void OnRenderDebug3D(
            GraphicsDevice& graphics,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // 加算光のRGB色
        DirectX::XMFLOAT3 m_color;
        // 加算する光の強度
        float m_intensity;
        // 影響半径ワールドXY単位
        float m_radius;
        // DLLのメンバー位置の互換性のため末尾配置を維持します。
        // UIを照らす指定
        bool m_affectsUI{ false };
    };
}
