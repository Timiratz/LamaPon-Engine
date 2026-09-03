#include "LamaPon/Core/SaveData.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace
{
    void WriteAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::filesystem::create_directories(
            path.parent_path());
        auto temporary = path;
        temporary += L".tmp";
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create temporary save slot: "
                + LamaPon::PathToUtf8(temporary));
        }
        output << text;
        output.close();
        if (!output
            || !MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            throw std::runtime_error(
                "Could not replace save slot: "
                + LamaPon::PathToUtf8(path));
        }
    }
}

namespace LamaPon
{
    SaveDataStore::SaveDataStore(
        std::filesystem::path directory)
        : m_directory(std::move(directory))
    {
    }

    void SaveDataStore::ValidateSlot(
        const std::string_view slot)
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
            || Utf8ToWide(slot).empty())
        {
            throw std::invalid_argument(
                "Save slot must be valid UTF-8 filename text with 1 to 64 bytes.");
        }
    }

    std::filesystem::path SaveDataStore::SlotPath(
        const std::string_view slot) const
    {
        ValidateSlot(slot);
        return m_directory
            / (PathFromUtf8(slot).wstring()
                + L".save.json");
    }

    void SaveDataStore::SaveJson(
        const std::string_view slot,
        const std::string_view json)
    {
        const auto payload =
            nlohmann::json::parse(json);
        const nlohmann::json document{
            { "format", "LamaPonSaveData" },
            { "version", 1 },
            { "slot", slot },
            { "data", payload }
        };
        WriteAtomically(
            SlotPath(slot),
            document.dump(2) + '\n');
    }

    std::optional<std::string>
        SaveDataStore::LoadJson(
            const std::string_view slot) const
    {
        const auto path = SlotPath(slot);
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return std::nullopt;
        }
        nlohmann::json document;
        input >> document;
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::SaveData));
        if (!document.contains("data"))
        {
            throw std::runtime_error(
                "Unsupported save slot: "
                + PathToUtf8(path));
        }
        return document["data"].dump();
    }

    bool SaveDataStore::HasSlot(
        const std::string_view slot) const
    {
        return std::filesystem::is_regular_file(
            SlotPath(slot));
    }

    bool SaveDataStore::DeleteSlot(
        const std::string_view slot)
    {
        std::error_code error;
        const bool removed =
            std::filesystem::remove(
                SlotPath(slot),
                error);
        if (error)
        {
            throw std::runtime_error(
                "Could not delete save slot: "
                + error.message());
        }
        return removed;
    }

    std::vector<std::string>
        SaveDataStore::ListSlots() const
    {
        std::vector<std::string> slots;
        std::error_code error;
        if (!std::filesystem::is_directory(
                m_directory,
                error))
        {
            return slots;
        }
        constexpr std::wstring_view suffix =
            L".save.json";
        for (const auto& entry :
            std::filesystem::directory_iterator(
                m_directory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto name =
                entry.path().filename().wstring();
            if (name.size() <= suffix.size()
                || !name.ends_with(suffix))
            {
                continue;
            }
            slots.push_back(
                WideToUtf8(
                    std::wstring_view(name).substr(
                        0,
                        name.size()
                            - suffix.size())));
        }
        std::ranges::sort(slots);
        return slots;
    }
}
