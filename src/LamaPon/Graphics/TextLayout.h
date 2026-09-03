#pragma once

#include <DirectXMath.h>

namespace LamaPon
{
    enum class TextHorizontalAlignment
    {
        Left,
        Center,
        Right
    };

    enum class TextVerticalAlignment
    {
        Top,
        Center,
        Bottom
    };

    struct TextLayoutOptions final
    {
        DirectX::XMFLOAT2 size{};
        TextHorizontalAlignment horizontalAlignment{
            TextHorizontalAlignment::Left
        };
        TextVerticalAlignment verticalAlignment{
            TextVerticalAlignment::Top
        };
        bool wordWrap{};
    };

    // 文字テクスチャは**白**で焼いてあるので、描くときに色を掛けます。
    // SpriteBatchはPremultiplied Alphaで描くため、RGBへあらかじめ
    // アルファを掛けた値を渡します（忘れると半透明のときに色が濃く
    // 出ます）。
    [[nodiscard]] inline DirectX::XMVECTOR PremultipliedTextColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        const DirectX::XMFLOAT4 premultiplied{
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
        return DirectX::XMLoadFloat4(&premultiplied);
    }
}
