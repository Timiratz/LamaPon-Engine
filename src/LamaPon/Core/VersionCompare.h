#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // "v2026.7.31" 形式のバージョン文字列を数値列へ分解します。
    // 先頭の v/V は無視し、数値とドット以外を含む場合や空の場合は
    // 空を返します。
    [[nodiscard]] std::vector<std::uint32_t>
        ParseVersionNumbers(std::string_view version);

    // latestがcurrentより新しいバージョンかを返します。
    // 数値成分の辞書順比較で、桁数が違う場合は不足分を0として
    // 扱います（2026.7.31 < 2026.7.31.1）。どちらかが解釈できない
    // 場合はfalseです。
    [[nodiscard]] bool IsNewerVersion(
        std::string_view current,
        std::string_view latest);
}
