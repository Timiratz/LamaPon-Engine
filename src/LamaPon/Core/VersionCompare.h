#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // バージョンを数値列へ分解します(version: ドット区切りのバージョン文字列)。
    // 先頭のv/Vは無視し、空成分・数字以外・32ビット整数の範囲外を含む場合は空を返します。
    [[nodiscard]] std::vector<std::uint32_t>
        ParseVersionNumbers(std::string_view version);

    // 更新版の方が新しいかを返します(current: 現行バージョン, latest: 更新候補のバージョン)。
    // 数値成分の辞書順で比較し、不足する成分は0として扱います。
    // どちらかが解釈できない場合はfalseです。
    [[nodiscard]] bool IsNewerVersion(
        std::string_view current,
        std::string_view latest);
}
