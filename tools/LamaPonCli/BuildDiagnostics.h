#pragma once

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace LamaPon::Cli
{
    [[nodiscard]] inline bool IsBuildErrorLine(
        const std::string_view line)
    {
        std::string lower;
        lower.reserve(line.size());
        for (const unsigned char value : line)
        {
            lower.push_back(static_cast<char>(std::tolower(value)));
        }

        const auto first = lower.find_first_not_of(" \t");
        const std::string_view trimmed =
            first == std::string::npos
                ? std::string_view{}
                : std::string_view{ lower }.substr(first);
        if (trimmed.starts_with("error:")
            || trimmed.starts_with("error ")
            || trimmed.starts_with("fatal error")
            || trimmed.starts_with("cmake error"))
        {
            return true;
        }

        // Compiler diagnostics put `error` after a location or severity.
        // A plain substring check also matched paths such as winerror.h and
        // incorrectly reported a successful build as containing errors.
        constexpr std::array<std::string_view, 4> patterns{
            ": error:",
            ": error ",
            ": fatal error:",
            ": fatal error ",
        };
        for (const auto pattern : patterns)
        {
            if (lower.find(pattern) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
}
