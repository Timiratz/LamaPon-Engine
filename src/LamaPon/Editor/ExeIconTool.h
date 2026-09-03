#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    // ICOへ格納する1画像。32bit BGRA、上から下の行順で保持します。
    struct IconImage final
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::byte> bgraPixels;
    };

    // IconImage列から.icoファイル全体のバイト列を組み立てます。
    // 画像はICO内へ32bit DIB（BITMAPINFOHEADER＋ANDマスク）として
    // 格納します。1～256ピクセル以外のサイズは受け付けません。
    [[nodiscard]] std::vector<std::byte> BuildIcoFileBytes(
        const std::vector<IconImage>& images);

    // 画像ファイルから.icoバイト列を作ります。.icoはヘッダー検証の上
    // そのまま返し、PNG/JPG/BMP等はWICでデコードして標準サイズ
    // （16/24/32/48/256）へ縮小したマルチサイズICOへ変換します。
    [[nodiscard]] std::vector<std::byte> BuildIcoFromImageFile(
        const std::filesystem::path& imagePath);

    // 実行ファイルのアイコングループ（IDI_LAMAPON_ENGINE）を
    // .icoバイト列の内容へ差し替えます。グループが無い実行ファイルには
    // 新規に埋め込みます。実行中でないファイルにのみ使用できます。
    void ReplaceExecutableIcon(
        const std::filesystem::path& executablePath,
        const std::vector<std::byte>& icoBytes);
}
