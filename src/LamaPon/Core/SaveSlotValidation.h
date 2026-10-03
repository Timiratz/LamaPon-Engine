#pragma once

#include <string_view>

namespace LamaPon::Detail
{
    // 保存スロット名が使用可能かを返します(slot: UTF-8のスロット名)。
    // 1～64バイトのファイル名に限り、Windows予約名・禁止文字・末尾の空白やドットを拒否します。
    [[nodiscard]] bool IsValidSaveSlotName(
        std::string_view slot) noexcept;
    // 保存スロット名を検証します(slot: UTF-8のスロット名)。
    // 使用できない名前にはstd::invalid_argumentを送出します。
    void ValidateSaveSlotName(std::string_view slot);
    // 保存先が同じ名前かを返します(left: 比較元のスロット名, right: 比較先のスロット名)。
    // 有効な名前同士をWindowsの大文字小文字を区別しない序数比較で照合します。
    [[nodiscard]] bool EquivalentSaveSlotNames(
        std::string_view left,
        std::string_view right) noexcept;
}
