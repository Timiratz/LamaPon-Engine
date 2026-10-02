#pragma once

#include <DirectXMath.h>

#include <cstdint>

namespace LamaPon
{
    // 基数の桁を反転した小数を返す(index: 列内の位置, base: 基数)。
    // 基数が2未満なら0を返す。
    [[nodiscard]] float HaltonSequence(
        std::uint32_t index,
        std::uint32_t base) noexcept;

    // 8フレーム周期のHalton列から半画素以内のずらし量を返す(index: 0始まりのフレーム番号)。
    [[nodiscard]] DirectX::XMFLOAT2 TemporalJitterOffset(
        std::uint32_t index) noexcept;

    // 行ベクトル射影の_31・_32へ画素ずらしを加える(projection: 元の射影行列, jitterPixels: 画素単位のずらし量, width: 出力幅, height: 出力高さ)。
    // 出力サイズは最小1とし、画素座標の下向きYをクリップ空間の上向きYへ変換する。
    [[nodiscard]] DirectX::XMMATRIX ApplyTemporalJitter(
        DirectX::FXMMATRIX projection,
        const DirectX::XMFLOAT2& jitterPixels,
        std::uint32_t width,
        std::uint32_t height) noexcept;
}
