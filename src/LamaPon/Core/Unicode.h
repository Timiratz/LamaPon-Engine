#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace LamaPon::Detail
{
    // UTF-8をUTF-16またはUTF-32へ変換し、不正な入力は全体を拒否する(value: UTF-8文字列)。
    template<typename Character>
    [[nodiscard]] std::basic_string<Character> Utf8ToCodeUnits(
        const std::string_view value)
    {
        static_assert(sizeof(Character) == 2 || sizeof(Character) == 4);
        std::basic_string<Character> result;
        result.reserve(value.size());
        for (std::size_t offset = 0; offset < value.size();)
        {
            const auto first = static_cast<unsigned char>(value[offset++]);
            std::uint32_t scalar{};
            std::uint32_t minimum{};
            unsigned trailing{};
            if (first < 0x80) scalar = first;
            else if (first >= 0xc2 && first <= 0xdf)
            {
                scalar = first & 0x1f; trailing = 1; minimum = 0x80;
            }
            else if (first >= 0xe0 && first <= 0xef)
            {
                scalar = first & 0x0f; trailing = 2; minimum = 0x800;
            }
            else if (first >= 0xf0 && first <= 0xf4)
            {
                scalar = first & 0x07; trailing = 3; minimum = 0x10000;
            }
            else return {};

            if (trailing > value.size() - offset) return {};
            for (unsigned index = 0; index < trailing; ++index)
            {
                const auto next = static_cast<unsigned char>(value[offset++]);
                if ((next & 0xc0) != 0x80) return {};
                scalar = (scalar << 6) | (next & 0x3f);
            }
            if (scalar < minimum || scalar > 0x10ffff
                || (scalar >= 0xd800 && scalar <= 0xdfff)) return {};

            if constexpr (sizeof(Character) == 2)
            {
                if (scalar > 0xffff)
                {
                    scalar -= 0x10000;
                    result.push_back(static_cast<Character>(0xd800 + (scalar >> 10)));
                    result.push_back(static_cast<Character>(0xdc00 + (scalar & 0x3ff)));
                    continue;
                }
            }
            result.push_back(static_cast<Character>(scalar));
        }
        return result;
    }

    // UTF-16またはUTF-32からUTF-8へ変換し、不正な入力は全体を拒否する(value: コード単位列)。
    template<typename Character>
    [[nodiscard]] std::string CodeUnitsToUtf8(
        const std::basic_string_view<Character> value)
    {
        static_assert(sizeof(Character) == 2 || sizeof(Character) == 4);
        std::string result;
        result.reserve(value.size());
        for (std::size_t offset = 0; offset < value.size(); ++offset)
        {
            auto scalar = static_cast<std::uint32_t>(
                static_cast<std::make_unsigned_t<Character>>(value[offset]));
            if constexpr (sizeof(Character) == 2)
            {
                if (scalar >= 0xd800 && scalar <= 0xdbff)
                {
                    if (++offset >= value.size()) return {};
                    const auto low = static_cast<std::uint32_t>(
                        static_cast<std::make_unsigned_t<Character>>(value[offset]));
                    if (low < 0xdc00 || low > 0xdfff) return {};
                    scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
                }
                else if (scalar >= 0xdc00 && scalar <= 0xdfff) return {};
            }
            if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return {};
            if (scalar < 0x80) result.push_back(static_cast<char>(scalar));
            else if (scalar < 0x800)
            {
                result.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
                result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
            }
            else if (scalar < 0x10000)
            {
                result.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
                result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
            }
            else
            {
                result.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
                result.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
            }
        }
        return result;
    }
}
