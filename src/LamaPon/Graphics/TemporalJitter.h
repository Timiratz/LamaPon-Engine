#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    // TAA用のサブピクセルずらし量を返します。
    //
    // Halton列（基数2と3）を使います。乱数ではなく「均等に散る」列で、
    // 少ない枚数でもピクセル内へ偏りなく並ぶのが利点です。乱数だと
    // 同じ場所に固まる回があり、そのフレームだけアンチエイリアスが
    // 効かずにちらついて見えます。
    //
    // 返すのは -0.5〜+0.5 ピクセルの範囲です。射影行列へ足す前に
    // 2/幅・2/高さを掛けてクリップ空間へ直してください。
    [[nodiscard]] float HaltonSequence(
        std::uint32_t index,
        std::uint32_t base) noexcept;

    // indexは0から数えるフレーム番号です。内部で長さ8へ折り返します
    // （8枚で1周。長くすると収束は滑らかになりますが、動きの後の
    // 復帰が遅くなります）。
    [[nodiscard]] DirectX::XMFLOAT2 TemporalJitterOffset(
        std::uint32_t index) noexcept;

    // ずらしを織り込んだ射影行列を返します。行ベクトル規約なので、
    // クリップ空間のx/yへ足すには_31/_32へ加算します。
    [[nodiscard]] DirectX::XMMATRIX ApplyTemporalJitter(
        DirectX::FXMMATRIX projection,
        const DirectX::XMFLOAT2& jitterPixels,
        std::uint32_t width,
        std::uint32_t height) noexcept;
}
