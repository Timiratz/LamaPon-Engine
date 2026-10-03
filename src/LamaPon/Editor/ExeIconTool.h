#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    // ICOに格納する上から下の行順の32bit BGRA画像。
    struct IconImage final
    {
        // 画像の幅・1から256ピクセル
        std::uint32_t width{};
        // 画像の高さ・1から256ピクセル
        std::uint32_t height{};
        // 上から下へ並ぶ32bit BGRA画素
        std::vector<std::byte> bgraPixels;
    };

    // 画像数は16bitに収まる範囲とし、各辺は1～256ピクセル、画素数は幅×高さ×4バイトを要求する。
    // BGRA画像を32bit DIBとANDマスクとしてICO文書にまとめる(images: 1枚以上のBGRA画像一覧)。
    [[nodiscard]] std::vector<std::byte> BuildIcoFileBytes(
        const std::vector<IconImage>& images);

    // ICOは位置情報を検証して返し他の画像はWICで標準寸法のICOへ変換する(imagePath: 読込・変換する画像のパス)。
    [[nodiscard]] std::vector<std::byte> BuildIcoFromImageFile(
        const std::filesystem::path& imagePath);

    // 停止中のexeの固定アイコングループを検証済みICOへ置換し不在なら追加する(executablePath: 実行中でないexeのパス, icoBytes: ICO文書の全バイト)。
    void ReplaceExecutableIcon(
        const std::filesystem::path& executablePath,
        const std::vector<std::byte>& icoBytes);
}
