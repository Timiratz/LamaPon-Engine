#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    // RGBA8のピクセル列をPNGへ書き出します。WIC（Windows標準）を
    // 使うので追加のライブラリは要りません。
    //
    // LamaPonCliのスクリーンショットとエディターのスクリーンショット
    // モードの両方が使うため、ランタイムに置いています。
    // アルファは不透明（255）へ倒して書きます——バックバッファの
    // アルファには描画の都合の値が残っていて、そのまま書くと
    // 「半透明のスクリーンショット」になるためです。
    //
    // 失敗は std::runtime_error で投げます。
    void SavePng(
        const std::filesystem::path& path,
        std::uint32_t width,
        std::uint32_t height,
        const std::vector<std::uint8_t>& rgbaPixels);
}
