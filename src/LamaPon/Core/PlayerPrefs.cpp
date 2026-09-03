#include "LamaPon/Core/PlayerPrefs.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <variant>

namespace
{
    using Value = std::variant<
        std::int64_t,
        double,
        bool,
        std::string>;

    void ValidateKey(const std::string_view key)
    {
        if (key.empty()
            || key.size() > 128
            || key.find_first_not_of(" \t\r\n")
                == std::string_view::npos
            || LamaPon::Utf8ToWide(key).empty())
        {
            throw std::invalid_argument(
                "PlayerPrefs key must be valid UTF-8 text with 1 to 128 bytes.");
        }
    }

    void WriteAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        auto temporary = path;
        temporary += L".tmp";
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create temporary save file: "
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
                "Could not replace save file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    std::wstring SafeDirectoryName(
        const std::string_view applicationName)
    {
        std::wstring value =
            LamaPon::Utf8ToWide(applicationName);
        if (value.empty())
        {
            value = L"LamaPonGame";
        }
        constexpr std::wstring_view invalid =
            L"<>:\"/\\|?*";
        for (auto& character : value)
        {
            if (character < 32
                || invalid.find(character)
                    != std::wstring_view::npos)
            {
                character = L'_';
            }
        }
        while (!value.empty()
            && (value.back() == L' '
                || value.back() == L'.'))
        {
            value.pop_back();
        }
        if (value.empty())
        {
            value = L"LamaPonGame";
        }
        if (value.size() > 64)
        {
            value.resize(64);
        }
        return value;
    }
}

namespace LamaPon
{
    struct PlayerPrefs::Implementation final
    {
        std::filesystem::path filePath;
        std::map<std::string, Value, std::less<>>
            values;
        bool dirty{};
    };

    PlayerPrefs::PlayerPrefs(
        std::filesystem::path filePath)
        : m_implementation(
            std::make_unique<Implementation>())
    {
        m_implementation->filePath =
            std::move(filePath);
    }

    PlayerPrefs::~PlayerPrefs() = default;

    void PlayerPrefs::Load()
    {
        auto& implementation = *m_implementation;
        implementation.values.clear();
        implementation.dirty = false;
        std::ifstream input(
            implementation.filePath,
            std::ios::binary);
        if (!input)
        {
            return;
        }
        nlohmann::json document;
        input >> document;
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::PlayerPrefs));
        if (!document.contains("values")
            || !document["values"].is_object())
        {
            throw std::runtime_error(
                "Unsupported PlayerPrefs file: "
                + PathToUtf8(
                    implementation.filePath));
        }
        for (const auto& [key, entry] :
            document["values"].items())
        {
            ValidateKey(key);
            const auto type =
                entry.value("type", std::string{});
            if (type == "integer")
            {
                implementation.values[key] =
                    entry.at("value").
                        get<std::int64_t>();
            }
            else if (type == "number")
            {
                const double value =
                    entry.at("value").get<double>();
                if (!std::isfinite(value))
                {
                    throw std::runtime_error(
                        "PlayerPrefs contains a non-finite number.");
                }
                implementation.values[key] = value;
            }
            else if (type == "boolean")
            {
                implementation.values[key] =
                    entry.at("value").get<bool>();
            }
            else if (type == "string")
            {
                implementation.values[key] =
                    entry.at("value").
                        get<std::string>();
            }
            else
            {
                throw std::runtime_error(
                    "PlayerPrefs contains an unknown value type.");
            }
        }
    }

    std::string PlayerPrefs::SerializeToJson() const
    {
        nlohmann::json document{
            { "format", "LamaPonPlayerPrefs" },
            { "version", 1 },
            { "values", nlohmann::json::object() }
        };
        for (const auto& [key, value] :
            m_implementation->values)
        {
            nlohmann::json entry;
            std::visit(
                [&entry](const auto& typedValue)
                {
                    using T =
                        std::decay_t<
                            decltype(typedValue)>;
                    if constexpr (
                        std::is_same_v<T, std::int64_t>)
                    {
                        entry["type"] = "integer";
                    }
                    else if constexpr (
                        std::is_same_v<T, double>)
                    {
                        entry["type"] = "number";
                    }
                    else if constexpr (
                        std::is_same_v<T, bool>)
                    {
                        entry["type"] = "boolean";
                    }
                    else
                    {
                        entry["type"] = "string";
                    }
                    entry["value"] = typedValue;
                },
                value);
            document["values"][key] =
                std::move(entry);
        }
        return document.dump(2) + '\n';
    }

    void PlayerPrefs::Save()
    {
        WriteAtomically(
            m_implementation->filePath,
            SerializeToJson());
        m_implementation->dirty = false;
    }

    bool PlayerPrefs::IsDirty() const noexcept
    {
        return m_implementation->dirty;
    }

    const std::filesystem::path&
        PlayerPrefs::FilePath() const noexcept
    {
        return m_implementation->filePath;
    }

    bool PlayerPrefs::HasKey(
        const std::string_view key) const
    {
        ValidateKey(key);
        return m_implementation->values.contains(key);
    }

    std::vector<std::string>
        PlayerPrefs::Keys() const
    {
        std::vector<std::string> keys;
        keys.reserve(
            m_implementation->values.size());
        for (const auto& [key, value] :
            m_implementation->values)
        {
            static_cast<void>(value);
            keys.push_back(key);
        }
        return keys;
    }

    PlayerPrefType PlayerPrefs::TypeOf(
        const std::string_view key) const
    {
        ValidateKey(key);
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
            == m_implementation->values.end())
        {
            throw std::out_of_range(
                "PlayerPrefs key was not found.");
        }
        return std::visit(
            [](const auto& value)
            {
                using T =
                    std::decay_t<decltype(value)>;
                if constexpr (
                    std::is_same_v<T, std::int64_t>)
                {
                    return PlayerPrefType::Integer;
                }
                else if constexpr (
                    std::is_same_v<T, double>)
                {
                    return PlayerPrefType::Number;
                }
                else if constexpr (
                    std::is_same_v<T, bool>)
                {
                    return PlayerPrefType::Boolean;
                }
                else
                {
                    return PlayerPrefType::String;
                }
            },
            iterator->second);
    }

    void PlayerPrefs::DeleteKey(
        const std::string_view key)
    {
        ValidateKey(key);
        m_implementation->dirty =
            m_implementation->values.erase(
                std::string(key)) != 0
            || m_implementation->dirty;
    }

    void PlayerPrefs::DeleteAll() noexcept
    {
        m_implementation->values.clear();
        m_implementation->dirty = true;
    }

    void PlayerPrefs::SetInteger(
        std::string key,
        const std::int64_t value)
    {
        ValidateKey(key);
        m_implementation->values[
            std::move(key)] = value;
        m_implementation->dirty = true;
    }

    std::int64_t PlayerPrefs::GetInteger(
        const std::string_view key,
        const std::int64_t defaultValue) const
    {
        ValidateKey(key);
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
                == m_implementation->values.end()
            || !std::holds_alternative<
                std::int64_t>(iterator->second))
        {
            return defaultValue;
        }
        return std::get<std::int64_t>(
            iterator->second);
    }

    void PlayerPrefs::SetNumber(
        std::string key,
        const double value)
    {
        ValidateKey(key);
        if (!std::isfinite(value))
        {
            throw std::invalid_argument(
                "PlayerPrefs numbers must be finite.");
        }
        m_implementation->values[
            std::move(key)] = value;
        m_implementation->dirty = true;
    }

    double PlayerPrefs::GetNumber(
        const std::string_view key,
        const double defaultValue) const
    {
        ValidateKey(key);
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
                == m_implementation->values.end()
            || !std::holds_alternative<double>(
                iterator->second))
        {
            return defaultValue;
        }
        return std::get<double>(iterator->second);
    }

    void PlayerPrefs::SetBoolean(
        std::string key,
        const bool value)
    {
        ValidateKey(key);
        m_implementation->values[
            std::move(key)] = value;
        m_implementation->dirty = true;
    }

    bool PlayerPrefs::GetBoolean(
        const std::string_view key,
        const bool defaultValue) const
    {
        ValidateKey(key);
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
                == m_implementation->values.end()
            || !std::holds_alternative<bool>(
                iterator->second))
        {
            return defaultValue;
        }
        return std::get<bool>(iterator->second);
    }

    void PlayerPrefs::SetString(
        std::string key,
        std::string value)
    {
        ValidateKey(key);
        if (!value.empty()
            && Utf8ToWide(value).empty())
        {
            throw std::invalid_argument(
                "PlayerPrefs string value must be valid UTF-8.");
        }
        m_implementation->values[
            std::move(key)] = std::move(value);
        m_implementation->dirty = true;
    }

    std::string PlayerPrefs::GetString(
        const std::string_view key,
        std::string defaultValue) const
    {
        ValidateKey(key);
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
                == m_implementation->values.end()
            || !std::holds_alternative<std::string>(
                iterator->second))
        {
            return defaultValue;
        }
        return std::get<std::string>(
            iterator->second);
    }

    std::filesystem::path UserDataDirectory(
        const std::string_view applicationName)
    {
        std::wstring localAppData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path root;
        if (length > 0
            && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            root =
                std::filesystem::temp_directory_path();
        }
        return root
            / L"LamaPon"
            / SafeDirectoryName(applicationName);
    }

    namespace
    {
        // ここに実体を1つだけ置きます。ヘッダのinline staticにすると
        // EXE側とDLL側で別々の実体になり、Scriptからの保存が無言で
        // 消えます（PhysicsSettingsで同じ罠を踏んでいます）。
        PlayerPrefs* g_activePlayerPrefs{};
    }

    PlayerPrefs* ActivePlayerPrefs() noexcept
    {
        return g_activePlayerPrefs;
    }

    void SetActivePlayerPrefs(PlayerPrefs* prefs) noexcept
    {
        g_activePlayerPrefs = prefs;
    }
}
