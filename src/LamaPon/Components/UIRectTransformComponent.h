#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

namespace LamaPon
{
    struct UIRect final
    {
        // 矩形のXY始点
        DirectX::XMFLOAT2 minimum{};
        // 矩形のXY終点
        DirectX::XMFLOAT2 maximum{};

        // 矩形の幅・高さを返します。
        [[nodiscard]] DirectX::XMFLOAT2 Size() const noexcept
        {
            return {
                maximum.x - minimum.x,
                maximum.y - minimum.y
            };
        }
        // 矩形の境界上を含めて点が内部か返します(point: 矩形と同じ座標系の点)。
        [[nodiscard]] bool Contains(
            const DirectX::XMFLOAT2& point) const noexcept
        {
            return point.x >= minimum.x
                && point.x <= maximum.x
                && point.y >= minimum.y
                && point.y <= maximum.y;
        }
    };

    // 配置と表示サイズの数値には有限値を指定します。
    class UIRectTransformComponent final : public Component
    {
    public:
        // UIの矩形配置を設定します(anchorMin: 親矩形の始点比率, anchorMax: 親矩形の終点比率, pivot: 自身の基準点比率, anchoredPosition: 拡縮前の基準点移動量, sizeDelta: 拡縮前のサイズ増減量)。
        explicit UIRectTransformComponent(
            DirectX::XMFLOAT2 anchorMin =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchorMax =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 pivot =
                { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchoredPosition = {},
            DirectX::XMFLOAT2 sizeDelta =
                { 220.0f, 56.0f }) noexcept;

        // 始点比率を0〜1に制限し必要なら終点も広げます(value: 親矩形のXY始点比率)。
        void SetAnchorMin(
            const DirectX::XMFLOAT2& value) noexcept;
        // 終点比率を0〜1に制限し必要なら始点も縮めます(value: 親矩形のXY終点比率)。
        void SetAnchorMax(
            const DirectX::XMFLOAT2& value) noexcept;
        // 自身の基準点比率を0〜1に制限します(value: XY基準点比率)。
        void SetPivot(
            const DirectX::XMFLOAT2& value) noexcept;
        // 基準点の移動量を設定します(value: 拡縮前のXY移動量)。
        void SetAnchoredPosition(
            const DirectX::XMFLOAT2& value) noexcept;
        // アンカー領域からのサイズ増減量を設定します(value: 拡縮前の幅・高さ増減量)。
        void SetSizeDelta(
            const DirectX::XMFLOAT2& value) noexcept;

        // 親矩形のXY始点比率を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchorMin() const noexcept { return m_anchorMin; }
        // 親矩形のXY終点比率を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchorMax() const noexcept { return m_anchorMax; }
        // 自身のXY基準点比率を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            Pivot() const noexcept { return m_pivot; }
        // 拡縮前の基準点移動量を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            AnchoredPosition() const noexcept
        {
            return m_anchoredPosition;
        }
        // 拡縮前の幅・高さ増減量を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            SizeDelta() const noexcept { return m_sizeDelta; }

        // 親矩形・キャンバス倍率・スクロールから表示矩形を求めます(viewportWidth: 表示幅ピクセル, viewportHeight: 表示高さピクセル)。
        // 座標は左上原点のピクセルで、GameObjectの3D変換は使用しません。
        [[nodiscard]] UIRect Resolve(
            float viewportWidth,
            float viewportHeight) const noexcept;

        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIRectTransform";
        }

    private:
        // 親矩形のXY始点比率
        DirectX::XMFLOAT2 m_anchorMin;
        // 親矩形のXY終点比率
        DirectX::XMFLOAT2 m_anchorMax;
        // 自身のXY基準点比率
        DirectX::XMFLOAT2 m_pivot;
        // 拡縮前の基準点移動量
        DirectX::XMFLOAT2 m_anchoredPosition;
        // 拡縮前の幅・高さ増減量
        DirectX::XMFLOAT2 m_sizeDelta;
    };
}
