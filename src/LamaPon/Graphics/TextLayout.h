#pragma once

#include <DirectXMath.h>

namespace LamaPon
{
    // 幅を指定した文字領域内での横位置合わせです。
    enum class TextHorizontalAlignment
    {
        // 左へ合わせます。
        Left,
        // 中央へ合わせます。
        Center,
        // 右へ合わせます。
        Right
    };

    // 高さを指定した文字領域内での縦位置合わせです。
    enum class TextVerticalAlignment
    {
        // 上へ合わせます。
        Top,
        // 中央へ合わせます。
        Center,
        // 下へ合わせます。
        Bottom
    };

    struct TextLayoutOptions final
    {
        // 文字の配置幅・高さで0は自動
        DirectX::XMFLOAT2 size{};
        // 幅指定時の横位置合わせ
        TextHorizontalAlignment horizontalAlignment{
            TextHorizontalAlignment::Left
        };
        // 高さ指定時の縦位置合わせ
        TextVerticalAlignment verticalAlignment{
            TextVerticalAlignment::Top
        };
        // 幅指定時の自動折り返し
        bool wordWrap{};
    };

    // 白い文字画像に乗算する描画色を作ります(color: アルファ乗算前のRGBA色)。
    [[nodiscard]] inline DirectX::XMVECTOR PremultipliedTextColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        // アルファ乗算済みのRGBA色
        const DirectX::XMFLOAT4 premultiplied{
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
        return DirectX::XMLoadFloat4(&premultiplied);
    }
}
