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

    // 文字テクスチャは白で焼いてあるので、描くときに色を掛けます。
    // 既存Sprite passのtint規約に合わせ、RGBへあらかじめアルファを
    // 掛けた値を渡します（忘れると半透明のときに色が濃く出ます）。
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
