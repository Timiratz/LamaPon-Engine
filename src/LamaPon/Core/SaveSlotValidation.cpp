#include "LamaPon/Core/SaveSlotValidation.h"

#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
    // ASCII制御文字かを返します(value: 判定するバイト)。
    bool IsAsciiControl(const unsigned char value) noexcept
    {
        return value < 0x20 || value == 0x7f;
    }

    // Windows予約名に該当するかを返します(slot: 保存スロット名)。
    bool IsReservedDeviceName(std::string_view slot)
    {
        // 最初の拡張子区切り位置
        const auto dot = slot.find('.');
        // 予約名と照合する拡張子前の部分
        std::string stem(slot.substr(0, dot));
        while (!stem.empty()
            && (stem.back() == ' ' || stem.back() == '.'))
        {
            stem.pop_back();
        }
        // ASCIIの小文字を大文字へ変換します(value: 名前の各バイト)。
        std::ranges::transform(
            stem,
            stem.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(
                    value >= 'a' && value <= 'z'
                        ? value - 'a' + 'A'
                        : value);
            });
        if (stem == "CON" || stem == "NUL"
            || stem == "AUX" || stem == "PRN"
            || stem == "CLOCK$" || stem == "CONIN$"
            || stem == "CONOUT$")
        {
            return true;
        }
        if (stem.size() == 4
            && (stem.starts_with("COM") || stem.starts_with("LPT"))
            && stem[3] >= '1' && stem[3] <= '9')
        {
            return true;
        }
        // Win32は上付き1/2/3もCOM・LPTの予約番号として扱います。
        return (stem.starts_with("COM") || stem.starts_with("LPT"))
            && (stem.substr(3) == "\xC2\xB9"
                || stem.substr(3) == "\xC2\xB2"
                || stem.substr(3) == "\xC2\xB3");
    }
}

namespace LamaPon::Detail
{
    bool IsValidSaveSlotName(const std::string_view slot) noexcept
    {
        try
        {
            if (slot.empty()
                || slot.size() > 64
                || slot == "."
                || slot == ".."
                || slot.find_first_of("<>:\"/\\|?*")
                    != std::string_view::npos
                || slot.find_first_not_of(" \t\r\n.")
                    == std::string_view::npos
                || slot.back() == ' '
                || slot.back() == '.'
                || std::ranges::any_of(slot, IsAsciiControl)
                || IsReservedDeviceName(slot))
            {
                return false;
            }
            return !Utf8ToWide(slot).empty();
        }
        catch (...)
        {
            return false;
        }
    }

    void ValidateSaveSlotName(const std::string_view slot)
    {
        if (!IsValidSaveSlotName(slot))
        {
            throw std::invalid_argument(
                "Save slot must be valid UTF-8 filename text with 1 to 64 bytes and must not be a Windows device name.");
        }
    }

    bool EquivalentSaveSlotNames(
        const std::string_view left,
        const std::string_view right) noexcept
    {
        if (!IsValidSaveSlotName(left)
            || !IsValidSaveSlotName(right))
        {
            return false;
        }
        try
        {
            // 比較元のUTF-16スロット名
            const auto wideLeft = Utf8ToWide(left);
            // 比較先のUTF-16スロット名
            const auto wideRight = Utf8ToWide(right);
            return CompareStringOrdinal(
                wideLeft.data(),
                static_cast<int>(wideLeft.size()),
                wideRight.data(),
                static_cast<int>(wideRight.size()),
                TRUE) == CSTR_EQUAL;
        }
        catch (...)
        {
            return false;
        }
    }
}
