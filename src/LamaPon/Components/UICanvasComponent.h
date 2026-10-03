#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    // 解像度・拡縮比率・表示サイズには有限値を指定します。
    class UICanvasComponent final : public Component
    {
    public:
        // 基準解像度と拡縮の比率を設定します(referenceResolution: 1以上に制限する基準幅・高さ, matchWidthOrHeight: 0は幅基準で1は高さ基準)。
        explicit UICanvasComponent(
            DirectX::XMFLOAT2 referenceResolution =
                { 1280.0f, 720.0f },
            float matchWidthOrHeight = 0.5f) noexcept;

        // 基準解像度の各軸を1以上にして設定します(resolution: 基準幅・高さ)。
        void SetReferenceResolution(
            const DirectX::XMFLOAT2& resolution) noexcept;
        // 拡縮の比率を0〜1に制限して設定します(match: 0は幅基準で1は高さ基準)。
        void SetMatchWidthOrHeight(float match) noexcept;

        // 拡縮の基準幅・高さを返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            ReferenceResolution() const noexcept
        {
            return m_referenceResolution;
        }
        // 幅基準から高さ基準への拡縮比率を返します。
        [[nodiscard]] float
            MatchWidthOrHeight() const noexcept
        {
            return m_matchWidthOrHeight;
        }
        // 幅・高さの比率を対数で補間した拡縮倍率を返します(viewportWidth: 表示幅ピクセル, viewportHeight: 表示高さピクセル)。
        [[nodiscard]] float ScaleFactor(
            float viewportWidth,
            float viewportHeight) const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UICanvas";
        }

    private:
        // 拡縮の基準幅・高さ
        DirectX::XMFLOAT2 m_referenceResolution;
        // 幅から高さ基準への比率
        float m_matchWidthOrHeight;
    };
}
