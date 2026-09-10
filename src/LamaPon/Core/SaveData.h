#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class SaveDataStore final
    {
    public:
        explicit SaveDataStore(
            std::filesystem::path directory);

        // SaveDataStoreは値をキャッシュしないため、保存先の切り替えは
        // noexceptです。以後の操作だけが新しいディレクトリを使います。
        void Rebind(std::filesystem::path directory) noexcept;

        void SaveJson(
            std::string_view slot,
            std::string_view json);
        [[nodiscard]] std::optional<std::string>
            LoadJson(std::string_view slot) const;
        [[nodiscard]] bool HasSlot(
            std::string_view slot) const;
        bool DeleteSlot(std::string_view slot);
        [[nodiscard]] std::vector<std::string>
            ListSlots() const;
        [[nodiscard]] std::filesystem::path
            SlotPath(std::string_view slot) const;
        [[nodiscard]] const std::filesystem::path&
            Directory() const noexcept
        {
            return m_directory;
        }

    private:
        std::filesystem::path m_directory;
    };
}
