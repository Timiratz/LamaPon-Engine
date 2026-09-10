#include "LamaPon/Core/SaveData.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/SaveSlotValidation.h"

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
        LamaPon::Detail::DurablePublishLocalDocument(path, text);
    }
}

namespace LamaPon
{
    SaveDataStore::SaveDataStore(
        std::filesystem::path directory)
        : m_directory(std::move(directory))
    {
    }

    void SaveDataStore::Rebind(
        std::filesystem::path directory) noexcept
    {
        if (m_bindingLeaseOwner)
        {
            return;
        }
        m_directory.swap(directory);
    }

    bool SaveDataStore::AcquireBindingLease(
        const void* const owner) noexcept
    {
        if (!owner || m_bindingLeaseOwner)
        {
            return false;
        }
        m_bindingLeaseOwner = owner;
        return true;
    }

    bool SaveDataStore::ReleaseBindingLease(
        const void* const owner) noexcept
    {
        if (!owner || m_bindingLeaseOwner != owner)
        {
            return false;
        }
        m_bindingLeaseOwner = nullptr;
        return true;
    }

    bool SaveDataStore::IsBindingLeased() const noexcept
    {
        return m_bindingLeaseOwner != nullptr;
    }

    bool SaveDataStore::BindingLeaseOwnedBy(
        const void* const owner) const noexcept
    {
        return owner && m_bindingLeaseOwner == owner;
    }

    void SaveDataStore::RelocateBinding(
        std::filesystem::path directory,
        const void* const owner) noexcept
    {
        if (!BindingLeaseOwnedBy(owner))
        {
            return;
        }
        m_directory.swap(directory);
    }

    std::filesystem::path SaveDataStore::SlotPath(
        const std::string_view slot) const
    {
        Detail::ValidateSaveSlotName(slot);
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
        const auto path = SlotPath(slot);
        const Detail::LocalPersistenceCommitEvent event{
            Detail::LocalPersistenceResourceKind::SaveData,
            this,
            &path,
            slot,
            false
        };
        WriteAtomically(
            path,
            document.dump(2) + '\n');
        Detail::NotifyLocalPersistenceCommit(event);
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
        if (!document.contains("slot")
            || !document["slot"].is_string()
            || document["slot"].get<std::string>() != slot)
        {
            throw std::runtime_error(
                "Save slot identity does not match its filename: "
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
        const auto path = SlotPath(slot);
        const Detail::LocalPersistenceCommitEvent event{
            Detail::LocalPersistenceResourceKind::SaveData,
            this,
            &path,
            slot,
            true
        };
        const bool removed = Detail::DurableDeleteLocalDocument(path);
        if (removed)
        {
            Detail::NotifyLocalPersistenceCommit(event);
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
