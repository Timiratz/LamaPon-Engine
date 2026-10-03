#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace LamaPon::Cli
{
    // build出力のerror診断行を判定します(line: 出力行)
    [[nodiscard]] inline bool IsBuildErrorLine(
        const std::string_view line)
    {
        // 大文字小文字を無視して照合する行です。
        std::string lower;
        lower.reserve(line.size());
        // ASCII文字を小文字へ変換します(value: 入力byte)
        for (const unsigned char value : line)
        {
            lower.push_back(static_cast<char>(std::tolower(value)));
        }

        // 前後空白を除いた先頭indexです。
        const auto first = lower.find_first_not_of(" \t");
        // 診断prefix判定に使うtrimmed viewです。
        const std::string_view trimmed =
            first == std::string::npos
                ? std::string_view{}
                : std::string_view{ lower }.substr(first);
        // 行頭に現れるerror表記を検出します。
        if (trimmed.starts_with("error:")
            || trimmed.starts_with("error ")
            || trimmed.starts_with("fatal error")
            || trimmed.starts_with("cmake error"))
        {
            return true;
        }

        // コンパイラー診断で場所や重大度の後に現れるエラー表記だけを検出します。
        // winerror.hなど、パスに含まれる語は除外します。
        // compiler診断で使うerror substringです。
        constexpr std::array<std::string_view, 4> patterns{
            ": error:",
            ": error ",
            ": fatal error:",
            ": fatal error ",
        };
        // error表記patternを順に照合します。
        for (const auto pattern : patterns)
        {
            // 行内に診断patternがあればerror行です。
            if (lower.find(pattern) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
}
