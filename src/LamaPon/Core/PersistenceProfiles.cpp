#include "LamaPon/Core/PersistenceProfiles.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    constexpr std::string_view SaveSuffix = ".save.json";
    constexpr std::array<std::uint8_t, 29> ProfileHashDomain{
        'L', 'a', 'm', 'a', 'P', 'o', 'n', '.',
        'P', 'e', 'r', 's', 'i', 's', 't', 'e', 'n', 'c', 'e', '.',
        'P', 'r', 'o', 'f', 'i', 'l', 'e', '.', '1'
    };

    class AlgorithmHandle final
    {
    public:
        ~AlgorithmHandle()
        {
            if (value != nullptr)
            {
                BCryptCloseAlgorithmProvider(value, 0);
            }
        }

        BCRYPT_ALG_HANDLE value{};
    };

    class HashHandle final
    {
    public:
        ~HashHandle()
        {
            if (value != nullptr)
            {
                BCryptDestroyHash(value);
            }
        }

        BCRYPT_HASH_HANDLE value{};
    };

    void RequireNtSuccess(
        const NTSTATUS status,
        const char* operation)
    {
        if (status < 0)
        {
            throw std::runtime_error(
                std::string{ operation } + " failed.");
        }
    }

    std::array<std::uint8_t, 8> EncodeLength(
        const std::size_t length)
    {
        std::array<std::uint8_t, 8> bytes{};
        auto value = static_cast<std::uint64_t>(length);
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            bytes[bytes.size() - index - 1] =
                static_cast<std::uint8_t>(value & 0xff);
            value >>= 8;
        }
        return bytes;
    }

    void AppendHashData(
        const BCRYPT_HASH_HANDLE hash,
        const std::uint8_t* data,
        const std::size_t size)
    {
        if (size == 0)
        {
            return;
        }
        if (size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument("Persistence player id is too long.");
        }
        RequireNtSuccess(
            BCryptHashData(
                hash,
                const_cast<PUCHAR>(data),
                static_cast<ULONG>(size),
                0),
            "BCryptHashData(profile id)");
    }

    void ValidateOpaqueId(
        const std::string_view value,
        const std::size_t maximumBytes,
        const char* message)
    {
        if (value.empty()
            || value.size() > maximumBytes
            || LamaPon::Utf8ToWide(value).empty())
        {
            throw std::invalid_argument(message);
        }
    }

    void AppendLengthTagged(
        const BCRYPT_HASH_HANDLE hash,
        const std::string_view value)
    {
        const auto length = EncodeLength(value.size());
        AppendHashData(hash, length.data(), length.size());
        AppendHashData(
            hash,
            reinterpret_cast<const std::uint8_t*>(value.data()),
            value.size());
    }

    bool HasSaveSuffix(const std::filesystem::path& path)
    {
        const auto name = LamaPon::PathToUtf8(path.filename());
        return name.size() >= SaveSuffix.size()
            && name.ends_with(SaveSuffix);
    }

    LamaPon::GuestPersistenceImportResult FailedImport(
        std::string message)
    {
        return {
            LamaPon::GuestPersistenceImportStatus::Failed,
            std::move(message)
        };
    }

    std::string RandomStageId()
    {
        std::array<std::uint8_t, 16> random{};
        RequireNtSuccess(
            BCryptGenRandom(
                nullptr,
                random.data(),
                static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG),
            "BCryptGenRandom(persistence import)");

        constexpr char Hex[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(random.size() * 2);
        for (const auto byte : random)
        {
            encoded.push_back(Hex[byte >> 4]);
            encoded.push_back(Hex[byte & 0x0f]);
        }
        SecureZeroMemory(random.data(), random.size());
        return encoded;
    }
}

namespace LamaPon
{
    PersistenceProfiles::PersistenceProfiles(
        std::filesystem::path userDataDirectory,
        std::string gameId,
        std::string environmentId)
        : m_userDataDirectory(std::move(userDataDirectory)),
          m_gameId(std::move(gameId)),
          m_environmentId(std::move(environmentId))
    {
        if (m_userDataDirectory.empty())
        {
            throw std::invalid_argument(
                "Persistence user data directory must not be empty.");
        }
        ValidateOpaqueId(
            m_gameId,
            128,
            "Persistence game id must be valid UTF-8 text with 1 to 128 bytes.");
        ValidateOpaqueId(
            m_environmentId,
            64,
            "Persistence environment id must be valid UTF-8 text with 1 to 64 bytes.");
    }

    const std::filesystem::path&
        PersistenceProfiles::UserDataDirectory() const noexcept
    {
        return m_userDataDirectory;
    }

    PersistenceProfilePaths PersistenceProfiles::Guest() const
    {
        return {
            m_userDataDirectory,
            m_userDataDirectory / L"PlayerPrefs.json",
            m_userDataDirectory / L"Saves",
            {},
            true
        };
    }

    std::string PersistenceProfiles::AccountStorageKey(
        const std::string_view playerId)
        const
    {
        ValidateOpaqueId(
            playerId,
            1024,
            "Persistence player id must be valid UTF-8 text with 1 to 1024 bytes.");

        AlgorithmHandle algorithm;
        RequireNtSuccess(
            BCryptOpenAlgorithmProvider(
                &algorithm.value,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider(profile id)");
        HashHandle hash;
        RequireNtSuccess(
            BCryptCreateHash(
                algorithm.value,
                &hash.value,
                nullptr,
                0,
                nullptr,
                0,
                0),
            "BCryptCreateHash(profile id)");

        AppendHashData(
            hash.value,
            ProfileHashDomain.data(),
            ProfileHashDomain.size());
        AppendLengthTagged(hash.value, m_gameId);
        AppendLengthTagged(hash.value, m_environmentId);
        AppendLengthTagged(hash.value, playerId);

        std::array<std::uint8_t, 32> digest{};
        RequireNtSuccess(
            BCryptFinishHash(
                hash.value,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0),
            "BCryptFinishHash(profile id)");

        constexpr char Hex[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(digest.size() * 2);
        for (const auto byte : digest)
        {
            encoded.push_back(Hex[byte >> 4]);
            encoded.push_back(Hex[byte & 0x0f]);
        }
        SecureZeroMemory(digest.data(), digest.size());
        return encoded;
    }

    PersistenceProfilePaths PersistenceProfiles::Account(
        const std::string_view playerId) const
    {
        auto key = AccountStorageKey(playerId);
        const auto root =
            m_userDataDirectory.parent_path()
            / L"OnlineProfiles"
            / PathFromUtf8(key);
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::move(key),
            false
        };
    }

    void PersistenceProfiles::Rebind(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        const PersistenceProfilePaths& profile) const
    {
        if (preferences.IsBindingLeased()
            || saves.IsBindingLeased())
        {
            throw std::logic_error(
                "Persistence binding is owned by online persistence.");
        }
        // pathのコピーで起こり得るallocation failureを、PlayerPrefsの
        // state交換より前にすべて済ませます。
        auto preferencesPath = profile.playerPrefsFile;
        auto savesDirectory = profile.saveDataDirectory;
        // Rebind(PlayerPrefs)は読み込みを完了してから状態を交換し、
        // Rebind(SaveDataStore)はnoexceptです。この順なら2オブジェクト
        // の切り替えも強い例外安全性を持ちます。
        preferences.Rebind(std::move(preferencesPath));
        saves.Rebind(std::move(savesDirectory));
    }

    void PersistenceProfiles::RebindGuest(
        PlayerPrefs& preferences,
        SaveDataStore& saves) const
    {
        Rebind(preferences, saves, Guest());
    }

    void PersistenceProfiles::RebindAccount(
        PlayerPrefs& preferences,
        SaveDataStore& saves,
        const std::string_view playerId) const
    {
        Rebind(preferences, saves, Account(playerId));
    }

    GuestPersistenceImportResult
        PersistenceProfiles::ImportGuestToAccount(
            PlayerPrefs& activePreferences,
            SaveDataStore& activeSaves,
            const std::string_view playerId) const
    {
        PersistenceProfilePaths guest;
        PersistenceProfilePaths account;
        std::filesystem::path staging;
        bool ownsStaging{};
        bool attemptedFinalRename{};
        try
        {
            if (activePreferences.IsBindingLeased()
                || activeSaves.IsBindingLeased())
            {
                return FailedImport(
                    "Persistence binding is owned by online persistence.");
            }
            guest = Guest();
            account = Account(playerId);

            if (activePreferences.FilePath()
                    != guest.playerPrefsFile
                || activeSaves.Directory()
                    != guest.saveDataDirectory)
            {
                return FailedImport(
                    "Guest persistence must be active before import.");
            }
            if (activePreferences.HasLoadFailure())
            {
                return FailedImport(
                    "Guest PlayerPrefs must be recovered before import.");
            }

            if (std::filesystem::exists(account.rootDirectory))
            {
                return {
                    GuestPersistenceImportStatus::AccountAlreadyHasData,
                    {}
                };
            }

            if (activePreferences.IsDirty())
            {
                // snapshotが確定しない限り、account側のdirectoryすら
                // 作りません。Save失敗時もdirty状態は維持されます。
                activePreferences.Save();
            }

            bool copyPreferences{};
            const auto preferencesStatus =
                std::filesystem::symlink_status(
                    guest.playerPrefsFile);
            if (std::filesystem::exists(preferencesStatus))
            {
                if (!std::filesystem::is_regular_file(
                        preferencesStatus))
                {
                    return FailedImport(
                        "Guest PlayerPrefs is not a regular file.");
                }
                copyPreferences = true;
            }

            std::vector<std::pair<
                std::filesystem::path,
                std::string>> saveFiles;
            const auto savesStatus =
                std::filesystem::symlink_status(
                    guest.saveDataDirectory);
            if (std::filesystem::exists(savesStatus))
            {
                if (!std::filesystem::is_directory(savesStatus))
                {
                    return FailedImport(
                        "Guest save data is not a regular directory.");
                }
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        guest.saveDataDirectory))
                {
                    if (!HasSaveSuffix(entry.path()))
                    {
                        continue;
                    }
                    const auto status = entry.symlink_status();
                    if (!std::filesystem::is_regular_file(status))
                    {
                        return FailedImport(
                            "Guest save data contains an unsafe entry.");
                    }
                    const auto fileName =
                        entry.path().filename().wstring();
                    constexpr std::wstring_view wideSuffix =
                        L".save.json";
                    saveFiles.emplace_back(
                        entry.path(),
                        WideToUtf8(
                            std::wstring_view(fileName).substr(
                                0,
                                fileName.size()
                                    - wideSuffix.size())));
                }
            }

            if (!copyPreferences && saveFiles.empty())
            {
                return {
                    GuestPersistenceImportStatus::NothingToImport,
                    {}
                };
            }

            // コピー先を一切作る前に、実際の永続化loaderで全文書を
            // 読みます。JSON破損・未知format・未来version・不正slotは
            // ここで失敗し、正式accountにもstagingにも触れません。
            if (copyPreferences)
            {
                PlayerPrefs validation(guest.playerPrefsFile);
                validation.Load();
            }
            SaveDataStore saveValidation(
                guest.saveDataDirectory);
            for (const auto& [source, slot] : saveFiles)
            {
                static_cast<void>(source);
                if (!saveValidation.LoadJson(slot).has_value())
                {
                    throw std::runtime_error(
                        "Guest save data disappeared during validation.");
                }
            }

            std::filesystem::create_directories(
                account.rootDirectory.parent_path());

            // 他プロセスや以前のstageには触れません。暗号学的乱数を
            // 含む、自分だけのstageを作成します。万一名前が既存でも
            // 新しい乱数で再試行します。
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                auto candidate = account.rootDirectory;
                candidate += L".importing.";
                candidate += PathFromUtf8(RandomStageId());
                if (std::filesystem::create_directory(candidate))
                {
                    staging = std::move(candidate);
                    ownsStaging = true;
                    break;
                }
            }
            if (!ownsStaging)
            {
                return FailedImport(
                    "A unique guest import stage could not be created.");
            }

            if (copyPreferences)
            {
                std::filesystem::copy_file(
                    guest.playerPrefsFile,
                    staging / L"PlayerPrefs.json");
            }
            if (!saveFiles.empty())
            {
                const auto stagingSaves = staging / L"Saves";
                std::filesystem::create_directory(stagingSaves);
                for (const auto& [source, slot] : saveFiles)
                {
                    static_cast<void>(slot);
                    std::filesystem::copy_file(
                        source,
                        stagingSaves / source.filename());
                }
            }

            // 検証後にコピー元が変更された場合も、不完全・破損内容を
            // 正式accountとして公開しないようstageを再検証します。
            // このinstanceをfinal rename後にactiveへnoexcept swapする
            // ため、公開後にもう一度loadが失敗する窓もありません。
            PlayerPrefs preparedPreferences(
                staging / L"PlayerPrefs.json");
            preparedPreferences.Load();
            SaveDataStore stagedSaveValidation(
                staging / L"Saves");
            for (const auto& [source, slot] : saveFiles)
            {
                static_cast<void>(source);
                if (!stagedSaveValidation.LoadJson(slot).has_value())
                {
                    throw std::runtime_error(
                        "Staged save data failed validation.");
                }
            }

            // final rename後に行う処理が例外を投げないよう、pathの
            // allocationと状態loadをすべて先に完了させます。
            auto finalPreferencesPath = account.playerPrefsFile;
            auto finalSavesDirectory = account.saveDataDirectory;
            preparedPreferences.RelocateBinding(
                std::move(finalPreferencesPath));

            // 同じ親ディレクトリ内なので、不完全なステージを経由せず
            // 完成済みディレクトリを一度に公開します。
            attemptedFinalRename = true;
            std::filesystem::rename(
                staging,
                account.rootDirectory);
            ownsStaging = false;

            // ここからreturnまではnoexceptです。同じPlayerPrefs/
            // SaveDataStoreオブジェクトのため、ScriptやEditorの参照も
            // 有効なまま、import snapshotを直ちに読めます。
            activePreferences.SwapLoadedState(preparedPreferences);
            activeSaves.Rebind(std::move(finalSavesDirectory));
            return {
                GuestPersistenceImportStatus::Imported,
                {}
            };
        }
        catch (const std::exception&)
        {
            if (ownsStaging)
            {
                std::error_code rollbackError;
                std::filesystem::remove_all(
                    staging,
                    rollbackError);
                if (rollbackError)
                {
                    return FailedImport(
                        "Guest import failed and staged data could not be removed.");
                }
            }
            if (attemptedFinalRename)
            {
                std::error_code targetError;
                const bool targetExists =
                    std::filesystem::exists(
                        account.rootDirectory,
                        targetError);
                if (!targetError && targetExists)
                {
                    return {
                        GuestPersistenceImportStatus::AccountAlreadyHasData,
                        {}
                    };
                }
            }
            return FailedImport("Guest persistence import failed.");
        }
    }
}
