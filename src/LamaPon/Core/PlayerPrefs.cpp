#include "LamaPon/Core/PlayerPrefs.h"

#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Online/CloudSave.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <variant>

namespace
{
    using Value = std::variant<
        std::int64_t,
        double,
        bool,
        std::string>;
    using ValueMap = std::map<
        std::string,
        Value,
        std::less<>>;

    // 設定キーの文字コードと長さを検証します(key: UTF-8の設定キー)。
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

    // 設定文書をフラッシュ後の置換で永続化します(path: 保存先, text: 保存文書の全文)。
    void WriteAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        LamaPon::Detail::DurablePublishLocalDocument(path, text);
    }

    // 設定ファイルを検証してキーと値を返します(filePath: 読み込み元のパス)。
    // ファイルがなければ空の設定とし、不正な文書や読み込み失敗は例外とします。
    ValueMap ReadValues(
        const std::filesystem::path& filePath)
    {
        // 読み込んだ設定キーと値の一覧
        ValueMap values;
        // 保存先ファイルの種別確認エラー
        std::error_code statusError;
        // リンクを追わない保存先の種別
        const auto status = std::filesystem::symlink_status(
            filePath,
            statusError);
        if (status.type() == std::filesystem::file_type::not_found
            || statusError
                == std::errc::no_such_file_or_directory
            || statusError.value() == ERROR_FILE_NOT_FOUND
            || statusError.value() == ERROR_PATH_NOT_FOUND)
        {
            return values;
        }
        if (statusError)
        {
            throw std::runtime_error(
                "Could not inspect PlayerPrefs file: "
                + LamaPon::PathToUtf8(filePath));
        }
        if (!std::filesystem::is_regular_file(status))
        {
            throw std::runtime_error(
                "PlayerPrefs path is not a regular file: "
                + LamaPon::PathToUtf8(filePath));
        }

        // 設定文書を読む入力ファイル
        std::ifstream input(filePath, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open PlayerPrefs file: "
                + LamaPon::PathToUtf8(filePath));
        }

        // ファイルから読み込む設定文書
        nlohmann::json document;
        input >> document;
        static_cast<void>(
            LamaPon::MigrateSerializedDocument(
                document,
                LamaPon::SerializedDocumentKind::PlayerPrefs));
        if (!document.contains("values")
            || !document["values"].is_object())
        {
            throw std::runtime_error(
                "Unsupported PlayerPrefs file: "
                + LamaPon::PathToUtf8(filePath));
        }
        // key: 設定キー
        // entry: 型名付きのJSON値
        for (const auto& [key, entry] :
            document["values"].items())
        {
            ValidateKey(key);
            // 記録された設定値の型名
            const auto type =
                entry.value("type", std::string{});
            if (type == "integer")
            {
                values[key] =
                    entry.at("value").get<std::int64_t>();
            }
            else if (type == "number")
            {
                // 有限性を検証する設定の実数値
                const double value =
                    entry.at("value").get<double>();
                if (!std::isfinite(value))
                {
                    throw std::runtime_error(
                        "PlayerPrefs contains a non-finite number.");
                }
                values[key] = value;
            }
            else if (type == "boolean")
            {
                values[key] = entry.at("value").get<bool>();
            }
            else if (type == "string")
            {
                values[key] =
                    entry.at("value").get<std::string>();
            }
            else
            {
                throw std::runtime_error(
                    "PlayerPrefs contains an unknown value type.");
            }
        }
        return values;
    }

    // 厳格な保存文書の検証後にキーと値を返します(text: 保存文書の全文)。
    ValueMap ReadStrictValues(const std::string_view text)
    {
        LamaPon::Detail::ValidatePlayerPrefsFullDocument(text);
        // 厳格検証済みの設定JSON文書
        const auto document = nlohmann::json::parse(text);
        // 読み込んだ設定キーと値の一覧
        ValueMap values;
        // key: 設定キー
        // entry: 型名付きのJSON値
        for (const auto& [key, entry] : document.at("values").items())
        {
            ValidateKey(key);
            // 記録された設定値の型名
            const auto type = entry.at("type").get<std::string>();
            if (type == "integer")
            {
                values[key] = entry.at("value").get<std::int64_t>();
            }
            else if (type == "number")
            {
                // 有限性を検証する設定の実数値
                const auto value = entry.at("value").get<double>();
                if (!std::isfinite(value))
                {
                    throw std::runtime_error(
                        "PlayerPrefs contains a non-finite number.");
                }
                values[key] = value;
            }
            else if (type == "boolean")
            {
                values[key] = entry.at("value").get<bool>();
            }
            else
            {
                values[key] = entry.at("value").get<std::string>();
            }
        }
        return values;
    }

    // ゲーム名から保存先のディレクトリ名を生成します(applicationName: ゲーム名)。
    std::wstring SafeDirectoryName(
        const std::string_view applicationName)
    {
        // 禁則文字を置換する保存先名
        std::wstring value =
            LamaPon::Utf8ToWide(applicationName);
        if (value.empty())
        {
            value = L"LamaPonGame";
        }
        // ファイル名へ使えない記号一覧
        constexpr std::wstring_view invalid =
            L"<>:\"/\\|?*";
        // 禁則文字かを照合する各文字
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
        // 現在の設定ファイルのパス
        std::filesystem::path filePath;
        // 読み込んだ設定キーと値の一覧
        // 設定キーと具体値の一覧
        ValueMap values;
        // 保存先の固定権の所有者識別子
        const void* bindingLeaseOwner{};
        // 未保存の変更があるか
        bool dirty{};
        // 読み込み失敗で上書き禁止か
        bool loadFailure{};
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
        Reload();
    }

    void PlayerPrefs::Reload()
    {
        if (m_implementation->loadFailure)
        {
            throw std::logic_error(
                "PlayerPrefs is blocked after a load failure; use an explicit recovery operation.");
        }

        // 成功後に交換する設定の準備状態
        auto replacement =
            std::make_unique<Implementation>();
        replacement->filePath =
            m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        try
        {
            replacement->values =
                ReadValues(replacement->filePath);
        }
        catch (...)
        {
            m_implementation->loadFailure = true;
            throw;
        }
        m_implementation.swap(replacement);
    }

    bool PlayerPrefs::HasLoadFailure() const noexcept
    {
        return m_implementation->loadFailure;
    }

    void PlayerPrefs::RecoverAfterLoadFailure()
    {
        if (!m_implementation->loadFailure)
        {
            throw std::logic_error(
                "PlayerPrefs does not have a load failure to recover.");
        }

        // 成功後に交換する設定の準備状態
        auto replacement =
            std::make_unique<Implementation>();
        replacement->filePath =
            m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        replacement->values =
            ReadValues(replacement->filePath);
        m_implementation.swap(replacement);
    }

    void PlayerPrefs::ResetAfterLoadFailure()
    {
        if (!m_implementation->loadFailure)
        {
            throw std::logic_error(
                "PlayerPrefs does not have a load failure to reset.");
        }
        m_implementation->values.clear();
        m_implementation->dirty = true;
        m_implementation->loadFailure = false;
    }

    void PlayerPrefs::Rebind(
        std::filesystem::path filePath)
    {
        if (m_implementation->bindingLeaseOwner)
        {
            throw std::logic_error(
                "PlayerPrefs binding is owned by online persistence.");
        }
        if (filePath == m_implementation->filePath)
        {
            return;
        }
        if (m_implementation->dirty)
        {
            throw std::logic_error(
                "PlayerPrefs must be saved or reloaded before rebinding.");
        }

        // 成功後に交換する設定の準備状態
        auto replacement =
            std::make_unique<Implementation>();
        replacement->filePath = std::move(filePath);
        replacement->values =
            ReadValues(replacement->filePath);
        m_implementation.swap(replacement);
    }

    void PlayerPrefs::RelocateBinding(
        std::filesystem::path filePath) noexcept
    {
        if (m_implementation->bindingLeaseOwner)
        {
            return;
        }
        m_implementation->filePath.swap(filePath);
    }

    void PlayerPrefs::SwapLoadedState(
        PlayerPrefs& other) noexcept
    {
        m_implementation.swap(other.m_implementation);
    }

    bool PlayerPrefs::AcquireBindingLease(
        const void* const owner) noexcept
    {
        if (!owner || m_implementation->bindingLeaseOwner)
        {
            return false;
        }
        m_implementation->bindingLeaseOwner = owner;
        return true;
    }

    bool PlayerPrefs::ReleaseBindingLease(
        const void* const owner) noexcept
    {
        if (!owner
            || m_implementation->bindingLeaseOwner != owner)
        {
            return false;
        }
        m_implementation->bindingLeaseOwner = nullptr;
        return true;
    }

    bool PlayerPrefs::IsBindingLeased() const noexcept
    {
        return m_implementation->bindingLeaseOwner != nullptr;
    }

    bool PlayerPrefs::BindingLeaseOwnedBy(
        const void* const owner) const noexcept
    {
        return owner
            && m_implementation->bindingLeaseOwner == owner;
    }

    void PlayerPrefs::LoadValidatedSnapshot(
        const std::string_view fullDocument,
        const bool missing)
    {
        // 成功後に交換する設定の準備状態
        auto replacement = std::make_unique<Implementation>();
        replacement->filePath = m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        if (missing)
        {
            if (!fullDocument.empty())
            {
                throw std::invalid_argument(
                    "A missing PlayerPrefs snapshot must not contain bytes.");
            }
        }
        else
        {
            replacement->values = ReadStrictValues(fullDocument);
        }
        m_implementation.swap(replacement);
    }

    std::string PlayerPrefs::SerializeToJson() const
    {
        // 設定キーと型名と値の保存文書
        nlohmann::json document{
            { "format", "LamaPonPlayerPrefs" },
            { "version", 1 },
            { "values", nlohmann::json::object() }
        };
        // key: 設定キー
        // value: メモリー内の設定値
        for (const auto& [key, value] :
            m_implementation->values)
        {
            // 型名と具体値を格納するJSON項目
            nlohmann::json entry;
            // 型名と値をJSONへ格納します(typedValue: variant内の具体型の設定値)。
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
        if (m_implementation->loadFailure)
        {
            throw std::logic_error(
                "PlayerPrefs cannot be saved after a load failure without explicit recovery.");
        }
        // 永続化後の設定保存の通知内容
        const Detail::LocalPersistenceCommitEvent event{
            Detail::LocalPersistenceResourceKind::PlayerPrefs,
            this,
            &m_implementation->filePath,
            {},
            false
        };
        WriteAtomically(
            m_implementation->filePath,
            SerializeToJson());
        m_implementation->dirty = false;
        Detail::NotifyLocalPersistenceCommit(event);
    }

    void PlayerPrefs::ApplyRemoteDocumentAtomically(
        const std::string_view fullDocument)
    {
        // 成功後に交換する設定の準備状態
        auto replacement = std::make_unique<Implementation>();
        replacement->filePath = m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        replacement->values = ReadStrictValues(fullDocument);
        WriteAtomically(replacement->filePath, fullDocument);
        m_implementation.swap(replacement);
    }

    void PlayerPrefs::DeleteRemoteDocumentAtomically()
    {
        // 成功後に交換する設定の準備状態
        auto replacement = std::make_unique<Implementation>();
        replacement->filePath = m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        (void)Detail::DurableDeleteLocalDocument(replacement->filePath);
        m_implementation.swap(replacement);
    }

    bool PlayerPrefs::ApplyRemoteDocumentAtomicallyIfUnchanged(
        const std::string_view fullDocument,
        const Detail::LocalPersistenceDocument& observed)
    {
        // 成功後に交換する設定の準備状態
        auto replacement = std::make_unique<Implementation>();
        replacement->filePath = m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        replacement->values = ReadStrictValues(fullDocument);
        // 観測状態を照合した永続操作の結果
        const auto result =
            Detail::DurablePublishLocalDocumentIfUnchanged(
                replacement->filePath,
                observed,
                fullDocument,
                CloudPreferencesMaxBytes);
        if (result
            == Detail::LocalPersistenceConditionalApplyResult::LocalChanged)
        {
            return false;
        }
        m_implementation.swap(replacement);
        return true;
    }

    bool PlayerPrefs::DeleteRemoteDocumentAtomicallyIfUnchanged(
        const Detail::LocalPersistenceDocument& observed)
    {
        // 成功後に交換する設定の準備状態
        auto replacement = std::make_unique<Implementation>();
        replacement->filePath = m_implementation->filePath;
        replacement->bindingLeaseOwner =
            m_implementation->bindingLeaseOwner;
        // 観測状態を照合した永続操作の結果
        const auto result =
            Detail::DurableDeleteLocalDocumentIfUnchanged(
                replacement->filePath,
                observed,
                CloudPreferencesMaxBytes);
        if (result
            == Detail::LocalPersistenceConditionalApplyResult::LocalChanged)
        {
            return false;
        }
        m_implementation.swap(replacement);
        return true;
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
        // 辞書順に返す設定キーの一覧
        std::vector<std::string> keys;
        keys.reserve(
            m_implementation->values.size());
        // key: 設定キー
        // value: メモリー内の設定値
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
        // 指定キーに対応する設定値の位置
        const auto iterator =
            m_implementation->values.find(key);
        if (iterator
            == m_implementation->values.end())
        {
            throw std::out_of_range(
                "PlayerPrefs key was not found.");
        }
        // 設定値に対応する型を返します(value: variant内の具体型の設定値)。
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
        // 指定キーに対応する設定値の位置
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
        // 指定キーに対応する設定値の位置
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
        // 指定キーに対応する設定値の位置
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
        // 指定キーに対応する設定値の位置
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
        // ユーザー用データ領域のパス
        std::wstring localAppData(32768, L'\0');
        // 環境変数から取得した文字数
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        // ゲーム保存先の親ディレクトリ
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
        // スクリプトが借用する設定管理
        // EXEとDLLで共有登録が分離しないよう、実体をCPPの1か所に置きます。
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
