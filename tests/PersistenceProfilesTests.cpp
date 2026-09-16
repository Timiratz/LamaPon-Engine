#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        if (!output)
        {
            throw std::runtime_error("Could not write test fixture.");
        }
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    struct FileHandle final
    {
        HANDLE value{ INVALID_HANDLE_VALUE };

        ~FileHandle()
        {
            Close();
        }

        void Close() noexcept
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    bool IsLowerHexKey(const std::string_view value)
    {
        return value.size() == 64
            && std::ranges::all_of(
                value,
                [](const char character)
                {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                });
    }

    void TestStableSafeAccountPaths(
        const std::filesystem::path& root)
    {
        const LamaPon::PersistenceProfiles profiles(
            root,
            "game-A",
            "production");
        const auto guest = profiles.Guest();
        Require(
            guest.isGuest
                && guest.rootDirectory == root
                && guest.playerPrefsFile == root / "PlayerPrefs.json"
                && guest.saveDataDirectory == root / "Saves",
            "Guest profile did not preserve legacy paths.");

        const std::vector<std::string> adversarialIds{
            ".",
            "..",
            "CON",
            "discord-user",
            "Discord-user",
            "discord-user.",
            "../discord-user",
            "discord-user ",
            "NUL:profile"
        };
        std::set<std::string> keys;
        for (const auto& id : adversarialIds)
        {
            const auto first = profiles.Account(id);
            const auto second = profiles.Account(id);
            Require(
                !first.isGuest
                    && IsLowerHexKey(first.accountStorageKey)
                    && first.accountStorageKey
                        == second.accountStorageKey
                    && first.rootDirectory.parent_path()
                        == root.parent_path() / "OnlineProfiles"
                    && first.rootDirectory.filename()
                        == first.accountStorageKey
                    && first.playerPrefsFile
                        == first.rootDirectory / "PlayerPrefs.json"
                    && first.saveDataDirectory
                        == first.rootDirectory / "Saves",
                "Account profile path was unstable or unsafe.");
            keys.insert(first.accountStorageKey);
        }
        Require(
            keys.size() == adversarialIds.size(),
            "Adversarial account ids aliased to one profile path.");

        const LamaPon::PersistenceProfiles otherGame(
            root,
            "game-B",
            "production");
        const LamaPon::PersistenceProfiles otherEnvironment(
            root,
            "game-A",
            "staging");
        Require(
            profiles.Account("same-player").accountStorageKey
                    == "ad2f75bbaa021fc5eed07fe4e131edddd1343f8ed9da54500925305d82e1432f"
                && profiles.Account("same-player").accountStorageKey
                    != otherGame.Account("same-player").accountStorageKey
                && profiles.Account("same-player").accountStorageKey
                    != otherEnvironment.Account("same-player").accountStorageKey
                && otherGame.Account("same-player").accountStorageKey
                    != otherEnvironment.Account("same-player").accountStorageKey,
            "Game or environment namespaces aliased account storage.");

        const LamaPon::PersistenceProfiles renamedGuestFolder(
            root.parent_path() / "renamed-display-folder",
            "game-A",
            "production");
        Require(
            profiles.Account("same-player").rootDirectory
                == renamedGuestFolder.Account(
                    "same-player").rootDirectory,
            "Display-name folder changed the stable account path.");

        bool emptyRejected{};
        try
        {
            static_cast<void>(profiles.Account(""));
        }
        catch (const std::invalid_argument&)
        {
            emptyRejected = true;
        }
        Require(emptyRejected, "Empty player id was accepted.");

        bool invalidUtf8Rejected{};
        try
        {
            static_cast<void>(profiles.Account(
                std::string_view("\xff", 1)));
        }
        catch (const std::invalid_argument&)
        {
            invalidUtf8Rejected = true;
        }
        Require(invalidUtf8Rejected, "Invalid UTF-8 player id was accepted.");
    }

    void TestStrongRebind(const std::filesystem::path& root)
    {
        const LamaPon::PersistenceProfiles profiles(root, "rebind-game");
        const auto guest = profiles.Guest();
        const auto accountA = profiles.Account("account-A");
        const auto accountB = profiles.Account("account-B");

        LamaPon::PlayerPrefs preferences(guest.playerPrefsFile);
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        LamaPon::SaveDataStore saves(guest.saveDataDirectory);
        saves.SaveJson("progress", R"({"owner":"guest"})");

        profiles.RebindAccount(preferences, saves, "account-A");
        Require(
            preferences.FilePath() == accountA.playerPrefsFile
                && preferences.Keys().empty()
                && saves.Directory() == accountA.saveDataDirectory
                && !saves.HasSlot("progress"),
            "Account rebind leaked guest values.");
        preferences.SetString("owner", "account-A");
        preferences.Save();
        saves.SaveJson("progress", R"({"owner":"account-A"})");

        profiles.RebindGuest(preferences, saves);
        Require(
            preferences.GetString("owner") == "guest"
                && saves.HasSlot("progress"),
            "Guest profile was not restored after account switch.");

        preferences.SetInteger("unsaved", 1);
        bool dirtyRejected{};
        try
        {
            profiles.RebindAccount(preferences, saves, "account-A");
        }
        catch (const std::logic_error&)
        {
            dirtyRejected = true;
        }
        Require(
            dirtyRejected
                && preferences.FilePath() == guest.playerPrefsFile
                && preferences.GetInteger("unsaved") == 1
                && saves.Directory() == guest.saveDataDirectory,
            "Dirty PlayerPrefs rebind changed active persistence.");
        preferences.Reload();

        WriteText(accountB.playerPrefsFile, "{broken json");
        bool corruptRejected{};
        try
        {
            profiles.RebindAccount(preferences, saves, "account-B");
        }
        catch (const std::exception&)
        {
            corruptRejected = true;
        }
        Require(
            corruptRejected
                && preferences.FilePath() == guest.playerPrefsFile
                && preferences.GetString("owner") == "guest"
                && !preferences.HasLoadFailure()
                && saves.Directory() == guest.saveDataDirectory,
            "Failed profile load partially changed active persistence.");

        profiles.RebindAccount(preferences, saves, "account-A");
        Require(
            preferences.GetString("owner") == "account-A"
                && saves.HasSlot("progress"),
            "Account-local persistence did not survive rebind.");

        const auto reloadPath = root / "reload" / "PlayerPrefs.json";
        LamaPon::PlayerPrefs reloadPreferences(reloadPath);
        reloadPreferences.Load();
        reloadPreferences.SetInteger("value", 1);
        reloadPreferences.Save();
        const auto validReloadJson =
            reloadPreferences.SerializeToJson();
        reloadPreferences.SetInteger("value", 2);
        WriteText(reloadPath, "{broken json");
        bool reloadRejected{};
        try
        {
            reloadPreferences.Reload();
        }
        catch (const std::exception&)
        {
            reloadRejected = true;
        }
        Require(
            reloadRejected
                && reloadPreferences.FilePath() == reloadPath
                && reloadPreferences.GetInteger("value") == 2
                && reloadPreferences.IsDirty()
                && reloadPreferences.HasLoadFailure(),
            "Failed reload did not preserve in-memory PlayerPrefs state.");

        bool blockedSaveRejected{};
        try
        {
            reloadPreferences.Save();
        }
        catch (const std::logic_error&)
        {
            blockedSaveRejected = true;
        }
        Require(
            blockedSaveRejected
                && ReadText(reloadPath) == "{broken json",
            "Fail-closed PlayerPrefs overwrote an unreadable file.");

        WriteText(reloadPath, validReloadJson);
        bool ordinaryReloadStillBlocked{};
        try
        {
            reloadPreferences.Reload();
        }
        catch (const std::logic_error&)
        {
            ordinaryReloadStillBlocked = true;
        }
        Require(
            ordinaryReloadStillBlocked
                && reloadPreferences.HasLoadFailure(),
            "Ordinary reload silently cleared fail-closed state.");
        reloadPreferences.RecoverAfterLoadFailure();
        Require(
            !reloadPreferences.HasLoadFailure()
                && !reloadPreferences.IsDirty()
                && reloadPreferences.GetInteger("value") == 1,
            "Explicit PlayerPrefs recovery failed.");

        WriteText(reloadPath, "{broken again");
        try
        {
            reloadPreferences.Reload();
        }
        catch (const std::exception&)
        {
        }
        Require(
            reloadPreferences.HasLoadFailure(),
            "Second load failure was not recorded.");
        reloadPreferences.ResetAfterLoadFailure();
        Require(
            !reloadPreferences.HasLoadFailure()
                && reloadPreferences.IsDirty()
                && reloadPreferences.Keys().empty(),
            "Explicit PlayerPrefs reset did not clear failed data.");
        reloadPreferences.Save();
        Require(
            !ReadText(reloadPath).starts_with("{broken"),
            "Explicit reset could not replace the failed file.");
    }

    void TestOpenFailuresAreNotMissing(
        const std::filesystem::path& root)
    {
        const auto missingPath = root / "missing.json";
        LamaPon::PlayerPrefs missing(missingPath);
        missing.Load();
        Require(
            !missing.HasLoadFailure()
                && missing.Keys().empty(),
            "Missing PlayerPrefs was not loaded as empty.");

        const auto directoryPath = root / "directory.json";
        std::filesystem::create_directories(directoryPath);
        LamaPon::PlayerPrefs nonRegular(directoryPath);
        bool directoryRejected{};
        try
        {
            nonRegular.Load();
        }
        catch (const std::exception&)
        {
            directoryRejected = true;
        }
        Require(
            directoryRejected
                && nonRegular.HasLoadFailure(),
            "Non-regular PlayerPrefs was mistaken for a missing file.");

        const auto lockedPath = root / "locked.json";
        {
            LamaPon::PlayerPrefs fixture(lockedPath);
            fixture.Load();
            fixture.SetInteger("value", 7);
            fixture.Save();
        }
        FileHandle locked;
        locked.value = CreateFileW(
            lockedPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            locked.value != INVALID_HANDLE_VALUE,
            "Could not lock PlayerPrefs open-failure fixture.");
        LamaPon::PlayerPrefs inaccessible(lockedPath);
        bool sharingFailureRejected{};
        try
        {
            inaccessible.Load();
        }
        catch (const std::exception&)
        {
            sharingFailureRejected = true;
        }
        Require(
            sharingFailureRejected
                && inaccessible.HasLoadFailure(),
            "Sharing-denied PlayerPrefs was mistaken for missing.");
    }

    void TestExplicitGuestImport(const std::filesystem::path& root)
    {
        const LamaPon::PersistenceProfiles profiles(root, "import-game");
        const auto guest = profiles.Guest();
        const auto account = profiles.Account("import-account");

        LamaPon::PlayerPrefs guestPreferences(guest.playerPrefsFile);
        guestPreferences.Load();
        guestPreferences.SetInteger("level", 12);
        LamaPon::SaveDataStore guestSaves(guest.saveDataDirectory);
        guestSaves.SaveJson("slot", R"({"chapter":4})");

        auto staleStaging = account.rootDirectory;
        staleStaging +=
            L".importing.00112233445566778899aabbccddeeff";
        WriteText(
            staleStaging / "incomplete.tmp",
            "stale import fixture");

        const auto imported =
            profiles.ImportGuestToAccount(
                guestPreferences,
                guestSaves,
                "import-account");
        if (!imported.Succeeded())
        {
            std::cerr
                << "Import status="
                << static_cast<int>(imported.status)
                << " error=" << imported.error << '\n';
        }
        Require(
            imported.status
                    == LamaPon::GuestPersistenceImportStatus::Imported
                && imported.Succeeded()
                && imported.error.empty()
                && std::filesystem::exists(staleStaging)
                && std::filesystem::is_regular_file(
                    guest.playerPrefsFile)
                && guestPreferences.FilePath()
                    == account.playerPrefsFile
                && guestPreferences.GetInteger("level") == 12
                && guestSaves.Directory()
                    == account.saveDataDirectory
                && guestSaves.HasSlot("slot"),
            "Guest import did not atomically activate its snapshot.");

        const auto reboundRefused =
            profiles.ImportGuestToAccount(
                guestPreferences,
                guestSaves,
                "import-account");
        Require(
            reboundRefused.status
                    == LamaPon::GuestPersistenceImportStatus::Failed
                && guestPreferences.FilePath()
                    == account.playerPrefsFile
                && guestSaves.Directory()
                    == account.saveDataDirectory,
            "Import accepted persistence already bound to an account.");

        guestPreferences.SetInteger("level", 99);
        guestPreferences.Save();
        profiles.RebindGuest(guestPreferences, guestSaves);
        const auto refused =
            profiles.ImportGuestToAccount(
                guestPreferences,
                guestSaves,
                "import-account");
        LamaPon::PlayerPrefs accountVerification(
            account.playerPrefsFile);
        accountVerification.Load();
        Require(
            refused.status
                    == LamaPon::GuestPersistenceImportStatus::AccountAlreadyHasData
                && guestPreferences.FilePath()
                    == guest.playerPrefsFile
                && guestPreferences.GetInteger("level") == 12
                && accountVerification.GetInteger("level") == 99
                && std::filesystem::exists(staleStaging),
            "Guest import silently overwrote account data.");
    }

    void TestImportRejectsInvalidDocuments(
        const std::filesystem::path& root)
    {
        {
            const LamaPon::PersistenceProfiles profiles(
                root / "bad-preferences",
                "bad-preferences-game");
            const auto guest = profiles.Guest();
            const auto account = profiles.Account("account");
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            constexpr std::string_view invalid = "{broken prefs";
            WriteText(guest.playerPrefsFile, invalid);
            const auto result =
                profiles.ImportGuestToAccount(
                    activePreferences,
                    activeSaves,
                    "account");
            Require(
                result.status
                        == LamaPon::GuestPersistenceImportStatus::Failed
                    && ReadText(guest.playerPrefsFile) == invalid
                    && !std::filesystem::exists(account.rootDirectory),
                "Invalid guest PlayerPrefs mutated account persistence.");
        }

        {
            const LamaPon::PersistenceProfiles profiles(
                root / "future-save",
                "future-save-game");
            const auto guest = profiles.Guest();
            const auto account = profiles.Account("account");
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            const auto savePath =
                guest.saveDataDirectory / "slot.save.json";
            constexpr std::string_view futureDocument =
                R"({"format":"LamaPonSaveData","version":999,"slot":"slot","data":{"level":4}})";
            WriteText(savePath, futureDocument);
            const auto result =
                profiles.ImportGuestToAccount(
                    activePreferences,
                    activeSaves,
                    "account");
            Require(
                result.status
                        == LamaPon::GuestPersistenceImportStatus::Failed
                    && ReadText(savePath) == futureDocument
                    && !std::filesystem::exists(account.rootDirectory),
                "Future guest SaveData mutated account persistence.");
        }

        {
            const LamaPon::PersistenceProfiles profiles(
                root / "mismatched-save",
                "mismatched-save-game");
            const auto guest = profiles.Guest();
            const auto account = profiles.Account("account");
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            const auto savePath =
                guest.saveDataDirectory / "slot.save.json";
            WriteText(
                savePath,
                R"({"format":"LamaPonSaveData","version":1,"slot":"other","data":{"level":4}})");
            bool directLoadRejected{};
            try
            {
                static_cast<void>(activeSaves.LoadJson("slot"));
            }
            catch (const std::runtime_error&)
            {
                directLoadRejected = true;
            }
            const auto result =
                profiles.ImportGuestToAccount(
                    activePreferences,
                    activeSaves,
                    "account");
            Require(
                directLoadRejected
                    && result.status
                        == LamaPon::GuestPersistenceImportStatus::Failed
                    && !std::filesystem::exists(account.rootDirectory),
                "Mismatched save-slot identity was imported.");
        }

        {
            const LamaPon::PersistenceProfiles profiles(
                root / "blocked-preferences",
                "blocked-preferences-game");
            const auto guest = profiles.Guest();
            const auto account = profiles.Account("account");
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            constexpr std::string_view invalid = "{blocked prefs";
            WriteText(guest.playerPrefsFile, invalid);
            try
            {
                activePreferences.Reload();
            }
            catch (const std::exception&)
            {
            }
            const auto result =
                profiles.ImportGuestToAccount(
                    activePreferences,
                    activeSaves,
                    "account");
            Require(
                activePreferences.HasLoadFailure()
                    && result.status
                        == LamaPon::GuestPersistenceImportStatus::Failed
                    && ReadText(guest.playerPrefsFile) == invalid
                    && !std::filesystem::exists(account.rootDirectory),
                "Import accepted fail-closed guest PlayerPrefs.");
        }

        const LamaPon::PersistenceProfiles profiles(
            root / "invalid-id",
            "invalid-id-game");
        const auto invalidIdGuest = profiles.Guest();
        LamaPon::PlayerPrefs invalidIdPreferences(
            invalidIdGuest.playerPrefsFile);
        invalidIdPreferences.Load();
        LamaPon::SaveDataStore invalidIdSaves(
            invalidIdGuest.saveDataDirectory);
        const auto invalidIdResult =
            profiles.ImportGuestToAccount(
                invalidIdPreferences,
                invalidIdSaves,
                "");
        Require(
            invalidIdResult.status
                    == LamaPon::GuestPersistenceImportStatus::Failed
                && !invalidIdResult.error.empty(),
            "Invalid import player id escaped the result contract.");
    }

    void TestNothingAndRollback(const std::filesystem::path& root)
    {
        {
            const LamaPon::PersistenceProfiles profiles(
                root / "empty",
                "empty-game");
            const auto guest = profiles.Guest();
            LamaPon::PlayerPrefs preferences(
                guest.playerPrefsFile);
            preferences.Load();
            LamaPon::SaveDataStore saves(
                guest.saveDataDirectory);
            const auto result =
                profiles.ImportGuestToAccount(
                    preferences,
                    saves,
                    "empty-account");
            Require(
                result.status
                    == LamaPon::GuestPersistenceImportStatus::NothingToImport,
                "Empty guest profile was treated as importable data.");
        }

        const auto lockedRoot = root / "rollback";
        const LamaPon::PersistenceProfiles profiles(
            lockedRoot,
            "rollback-game");
        const auto guest = profiles.Guest();
        const auto account = profiles.Account("locked-account");
        LamaPon::PlayerPrefs preferences(guest.playerPrefsFile);
        preferences.Load();
        preferences.SetString("state", "must-survive");
        preferences.Save();
        preferences.SetString("state", "unsaved-change");
        LamaPon::SaveDataStore saves(guest.saveDataDirectory);

        FileHandle locked;
        locked.value = CreateFileW(
            guest.playerPrefsFile.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            locked.value != INVALID_HANDLE_VALUE,
            "Could not lock rollback fixture.");

        const auto result =
            profiles.ImportGuestToAccount(
                preferences,
                saves,
                "locked-account");
        locked.Close();
        Require(
            result.status
                    == LamaPon::GuestPersistenceImportStatus::Failed
                && !result.error.empty()
                && !std::filesystem::exists(account.rootDirectory)
                && preferences.IsDirty()
                && preferences.GetString("state")
                    == "unsaved-change"
                && ReadText(guest.playerPrefsFile).find(
                    "must-survive") != std::string::npos
                && std::filesystem::is_regular_file(
                    guest.playerPrefsFile),
            "Failed guest snapshot mutated account persistence.");
    }
}

int main()
{
    const auto root =
        std::filesystem::current_path()
        / "test-output"
        / "profiles";
    const auto onlineProfiles = root / "OnlineProfiles";
    try
    {
        Require(
            onlineProfiles.parent_path() == root,
            "Online profile cleanup target escaped the test root.");
        std::filesystem::remove_all(onlineProfiles);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        TestStableSafeAccountPaths(root / "paths");
        TestStrongRebind(root / "rebind");
        TestOpenFailuresAreNotMissing(root / "open-failures");
        TestExplicitGuestImport(root / "import");
        TestImportRejectsInvalidDocuments(root / "invalid-import");
        TestNothingAndRollback(root);

        std::filesystem::remove_all(onlineProfiles);
        std::filesystem::remove_all(root);
        std::cout << "Persistence profile tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
