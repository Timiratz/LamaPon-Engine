#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    // 密に並ぶRGBA8画素を不透明なPNGへ保存する(path: 保存ファイル, width: 画像幅, height: 画像高さ, rgbaPixels: 行順の画素バイト列)。
    // 呼出スレッドのCOMを初期化済みとし、サイズは非ゼロで転送バイト数をUINT内に収める。
    // バッファー不足やWICの失敗はruntime_errorとし、保存失敗時の部分ファイルは削除しない。
    void SavePng(
        const std::filesystem::path& path,
        std::uint32_t width,
        std::uint32_t height,
        const std::vector<std::uint8_t>& rgbaPixels);
}
