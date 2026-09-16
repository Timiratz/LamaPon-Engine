#pragma once

#include <string_view>

namespace LamaPon::Detail
{
    // ローカル保存とcloud resourceで共有するslot名規約です。
    // Windows上で別名になるdevice名や大文字小文字も考慮します。
    [[nodiscard]] bool IsValidSaveSlotName(
        std::string_view slot) noexcept;
    void ValidateSaveSlotName(std::string_view slot);
    [[nodiscard]] bool EquivalentSaveSlotNames(
        std::string_view left,
        std::string_view right) noexcept;
}
