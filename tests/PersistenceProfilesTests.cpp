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
    // Require(condition: 条件, message: 失敗説明)でテスト失敗を通知します。
    void Require(const bool condition, const char* message)
    {
        // テスト条件の成否を判定します。
        if (!condition)
        {
            // 条件違反をテスト失敗にします。
            throw std::runtime_error(message);
        }
    }

    // WriteText(path: 保存先, text: 内容)をテスト文書として保存します。
    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        // 出力ストリーム
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        // fixture書込の成否を調べます。
        if (!output)
        {
            // fixture書込失敗を通知します。
            throw std::runtime_error("Could not write test fixture.");
        }
    }

    // ReadText(path: 読込元)からファイル内容を返します。
    std::string ReadText(const std::filesystem::path& path)
    {
        // 入力ストリーム
        std::ifstream input(path, std::ios::binary);
        // 読み込んだ文字列を返します。
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    struct FileHandle final
    {
        // Win32ファイルhandle
        HANDLE value{ INVALID_HANDLE_VALUE };

        // ~FileHandle() 保持中のhandleを閉じます。
        ~FileHandle()
        {
            Close();
        }

        // Close() 有効なhandleを解放します。
        void Close() noexcept
        {
            // 有効なhandleだけを閉じます。
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    // IsLowerHexKey(value: 検査文字列)で小文字SHA-256形式を調べます。
    bool IsLowerHexKey(const std::string_view value)
    {
        // 64桁の小文字hexだけを受理します(character: 検査文字)。
        return value.size() == 64
            && std::ranges::all_of(
                value,
                [](const char character)
                {
                    // 検査文字が小文字hexか返します。
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                });
    }

    // 安定したaccountパスとguest互換性を検証します(root: 保存ルート)。
    void TestStableSafeAccountPaths(
        const std::filesystem::path& root)
    {
        // テスト対象プロフィール
        const LamaPon::PersistenceProfiles profiles(
            root,
            "game-A",
            "production");
        // guestプロフィール
        const auto guest = profiles.Guest();
        Require(
            guest.isGuest
                && guest.rootDirectory == root
                && guest.playerPrefsFile == root / "PlayerPrefs.json"
                && guest.saveDataDirectory == root / "Saves",
            "Guest profile did not preserve legacy paths.");

        // 不正account ID候補
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
        // 生成済みaccount key
        std::set<std::string> keys;
        // account ID候補
        for (const auto& id : adversarialIds)
        {
            // 1回目のaccount解決
            const auto first = profiles.Account(id);
            // 再解決したaccount
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

        // 別gameのプロフィール
        const LamaPon::PersistenceProfiles otherGame(
            root,
            "game-B",
            "production");
        // 別environmentのプロフィール
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

        // 表示名変更後のguest root
        const LamaPon::PersistenceProfiles renamedGuestFolder(
            root.parent_path() / "renamed-display-folder",
            "game-A",
            "production");
        Require(
            profiles.Account("same-player").rootDirectory
                == renamedGuestFolder.Account(
                    "same-player").rootDirectory,
            "Display-name folder changed the stable account path.");

        // 空ID拒否の結果
        bool emptyRejected{};
        // 空IDの拒否を捕捉します。
        try
        {
            static_cast<void>(profiles.Account(""));
        }
        // 無効IDの例外を捕捉します。
        catch (const std::invalid_argument&)
        {
            emptyRejected = true;
        }
        Require(emptyRejected, "Empty player id was accepted.");

        // 不正UTF-8拒否の結果
        bool invalidUtf8Rejected{};
        // 不正UTF-8の拒否を捕捉します。
        try
        {
            static_cast<void>(profiles.Account(
                std::string_view("\xff", 1)));
        }
        // 不正UTF-8例外を捕捉します。
        catch (const std::invalid_argument&)
        {
            invalidUtf8Rejected = true;
        }
        Require(invalidUtf8Rejected, "Invalid UTF-8 player id was accepted.");
    }

    // guestとaccountの再bind強保証を検証します(root: 保存ルート)。
    void TestStrongRebind(const std::filesystem::path& root)
    {
        // 再bind対象profiles
        const LamaPon::PersistenceProfiles profiles(root, "rebind-game");
        // guestプロフィール
        const auto guest = profiles.Guest();
        // account-Aプロフィール
        const auto accountA = profiles.Account("account-A");
        // account-Bプロフィール
        const auto accountB = profiles.Account("account-B");

        // guestのPlayerPrefs
        LamaPon::PlayerPrefs preferences(guest.playerPrefsFile);
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        // guestのSaveData
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
        // dirty状態拒否の結果
        bool dirtyRejected{};
        // dirty状態での再bind拒否を検証します。
        try
        {
            profiles.RebindAccount(preferences, saves, "account-A");
        }
        // logic_errorを記録します。
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
        // 破損文書拒否の結果
        bool corruptRejected{};
        // 破損プロフィールの読込拒否を検証します。
        try
        {
            profiles.RebindAccount(preferences, saves, "account-B");
        }
        // 読み込み例外を記録します。
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

        // reload用PlayerPrefsパス
        const auto reloadPath = root / "reload" / "PlayerPrefs.json";
        // reload検証対象
        LamaPon::PlayerPrefs reloadPreferences(reloadPath);
        reloadPreferences.Load();
        reloadPreferences.SetInteger("value", 1);
        reloadPreferences.Save();
        // 復旧可能な文書内容
        const auto validReloadJson =
            reloadPreferences.SerializeToJson();
        reloadPreferences.SetInteger("value", 2);
        WriteText(reloadPath, "{broken json");
        // reload拒否の結果
        bool reloadRejected{};
        // 破損後reloadの失敗を捕捉します。
        try
        {
            reloadPreferences.Reload();
        }
        // reload例外を記録します。
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

        // fail-closed保存拒否の結果
        bool blockedSaveRejected{};
        // 失敗状態での保存拒否を検証します。
        try
        {
            reloadPreferences.Save();
        }
        // logic_errorを記録します。
        catch (const std::logic_error&)
        {
            blockedSaveRejected = true;
        }
        Require(
            blockedSaveRejected
                && ReadText(reloadPath) == "{broken json",
            "Fail-closed PlayerPrefs overwrote an unreadable file.");

        WriteText(reloadPath, validReloadJson);
        // 通常reloadの再拒否結果
        bool ordinaryReloadStillBlocked{};
        // 回復前reloadの失敗を捕捉します。
        try
        {
            reloadPreferences.Reload();
        }
        // load failure維持の例外を記録します。
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
        // 再読込失敗を捕捉します。
        try
        {
            reloadPreferences.Reload();
        }
        // 文書解析例外を記録します。
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

    // open失敗をmissing扱いしないことを検証します(root: 保存ルート)。
    void TestOpenFailuresAreNotMissing(
        const std::filesystem::path& root)
    {
        // 欠落ファイルのパス
        const auto missingPath = root / "missing.json";
        // 欠落確認PlayerPrefs
        LamaPon::PlayerPrefs missing(missingPath);
        missing.Load();
        Require(
            !missing.HasLoadFailure()
                && missing.Keys().empty(),
            "Missing PlayerPrefs was not loaded as empty.");

        // ディレクトリの偽装パス
        const auto directoryPath = root / "directory.json";
        std::filesystem::create_directories(directoryPath);
        // 非regular file用prefs
        LamaPon::PlayerPrefs nonRegular(directoryPath);
        // directory拒否の結果
        bool directoryRejected{};
        // 非regular fileの読込失敗を捕捉します。
        try
        {
            nonRegular.Load();
        }
        // 読込例外を記録します。
        catch (const std::exception&)
        {
            directoryRejected = true;
        }
        Require(
            directoryRejected
                && nonRegular.HasLoadFailure(),
            "Non-regular PlayerPrefs was mistaken for a missing file.");

        // 共有ロック対象パス
        const auto lockedPath = root / "locked.json";
        {
            // 共有fixture
            LamaPon::PlayerPrefs fixture(lockedPath);
            fixture.Load();
            fixture.SetInteger("value", 7);
            fixture.Save();
        }
        // 排他handle
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
        // 共有拒否対象prefs
        LamaPon::PlayerPrefs inaccessible(lockedPath);
        // 共有失敗拒否の結果
        bool sharingFailureRejected{};
        // 共有中読込の失敗を捕捉します。
        try
        {
            inaccessible.Load();
        }
        // 共有違反例外を記録します。
        catch (const std::exception&)
        {
            sharingFailureRejected = true;
        }
        Require(
            sharingFailureRejected
                && inaccessible.HasLoadFailure(),
            "Sharing-denied PlayerPrefs was mistaken for missing.");
    }

    // guest文書の明示importを検証します(root: 保存ルート)。
    void TestExplicitGuestImport(const std::filesystem::path& root)
    {
        // import対象profiles
        const LamaPon::PersistenceProfiles profiles(root, "import-game");
        // guestプロフィール
        const auto guest = profiles.Guest();
        // import先account
        const auto account = profiles.Account("import-account");

        // guestのPlayerPrefs
        LamaPon::PlayerPrefs guestPreferences(guest.playerPrefsFile);
        guestPreferences.Load();
        guestPreferences.SetInteger("level", 12);
        // guestのSaveData
        LamaPon::SaveDataStore guestSaves(guest.saveDataDirectory);
        guestSaves.SaveJson("slot", R"({"chapter":4})");

        // 残存import staging
        auto staleStaging = account.rootDirectory;
        staleStaging +=
            L".importing.00112233445566778899aabbccddeeff";
        WriteText(
            staleStaging / "incomplete.tmp",
            "stale import fixture");

        // guest import結果
        const auto imported =
            profiles.ImportGuestToAccount(
                guestPreferences,
                guestSaves,
                "import-account");
        // 失敗時の診断情報を出力します。
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

        // bind済み拒否結果
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
        // 既存accountデータの拒否結果
        const auto refused =
            profiles.ImportGuestToAccount(
                guestPreferences,
                guestSaves,
                "import-account");
        // 確認用account prefs
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

    // 不正guest文書のimport拒否を検証します(root: 保存ルート)。
    void TestImportRejectsInvalidDocuments(
        const std::filesystem::path& root)
    {
        {
            // 不正文書試験profiles
            const LamaPon::PersistenceProfiles profiles(
                root / "bad-preferences",
                "bad-preferences-game");
            // guestプロフィール
            const auto guest = profiles.Guest();
            // import先account
            const auto account = profiles.Account("account");
            // guestのPlayerPrefs
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            // guestのSaveData
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            // 不正prefs本文
            constexpr std::string_view invalid = "{broken prefs";
            WriteText(guest.playerPrefsFile, invalid);
            // 不正prefsのimport結果
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
            // 将来版save試験profiles
            const LamaPon::PersistenceProfiles profiles(
                root / "future-save",
                "future-save-game");
            // guestプロフィール
            const auto guest = profiles.Guest();
            // import先account
            const auto account = profiles.Account("account");
            // guestのPlayerPrefs
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            // guestのSaveData
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            // 将来版slotパス
            const auto savePath =
                guest.saveDataDirectory / "slot.save.json";
            // 未対応版のsave本文
            constexpr std::string_view futureDocument =
                R"({"format":"LamaPonSaveData","version":999,"slot":"slot","data":{"level":4}})";
            WriteText(savePath, futureDocument);
            // 将来版saveのimport結果
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
            // slot不一致試験profiles
            const LamaPon::PersistenceProfiles profiles(
                root / "mismatched-save",
                "mismatched-save-game");
            // guestプロフィール
            const auto guest = profiles.Guest();
            // import先account
            const auto account = profiles.Account("account");
            // guestのPlayerPrefs
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            // guestのSaveData
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            // 不一致slotパス
            const auto savePath =
                guest.saveDataDirectory / "slot.save.json";
            WriteText(
                savePath,
                R"({"format":"LamaPonSaveData","version":1,"slot":"other","data":{"level":4}})");
            // 直接load拒否の結果
            bool directLoadRejected{};
            // slot不一致loadを拒否します。
            try
            {
                static_cast<void>(activeSaves.LoadJson("slot"));
            }
            // runtime_errorを記録します。
            catch (const std::runtime_error&)
            {
                directLoadRejected = true;
            }
            // slot不一致import結果
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
            // fail-closed用profiles
            const LamaPon::PersistenceProfiles profiles(
                root / "blocked-preferences",
                "blocked-preferences-game");
            // guestプロフィール
            const auto guest = profiles.Guest();
            // import先account
            const auto account = profiles.Account("account");
            // guestのPlayerPrefs
            LamaPon::PlayerPrefs activePreferences(
                guest.playerPrefsFile);
            activePreferences.Load();
            // guestのSaveData
            LamaPon::SaveDataStore activeSaves(
                guest.saveDataDirectory);
            // 破損prefs本文
            constexpr std::string_view invalid = "{blocked prefs";
            WriteText(guest.playerPrefsFile, invalid);
            // 破損reloadの例外を捕捉します。
            try
            {
                activePreferences.Reload();
            }
            // 文書読込失敗を記録します。
            catch (const std::exception&)
            {
            }
            // fail-closed import結果
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

        // 不正ID試験profiles
        const LamaPon::PersistenceProfiles profiles(
            root / "invalid-id",
            "invalid-id-game");
        // ID検証対象guest
        const auto invalidIdGuest = profiles.Guest();
        // 不正ID用PlayerPrefs
        LamaPon::PlayerPrefs invalidIdPreferences(
            invalidIdGuest.playerPrefsFile);
        invalidIdPreferences.Load();
        // 不正ID用SaveData
        LamaPon::SaveDataStore invalidIdSaves(
            invalidIdGuest.saveDataDirectory);
        // 不正IDのimport結果
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

    // 空importと失敗rollbackを検証します(root: 保存ルート)。
    void TestNothingAndRollback(const std::filesystem::path& root)
    {
        {
            // 空guest試験profiles
            const LamaPon::PersistenceProfiles profiles(
                root / "empty",
                "empty-game");
            // 空guestプロフィール
            const auto guest = profiles.Guest();
            // guestのPlayerPrefs
            LamaPon::PlayerPrefs preferences(
                guest.playerPrefsFile);
            preferences.Load();
            // guestのSaveData
            LamaPon::SaveDataStore saves(
                guest.saveDataDirectory);
            // 空importの結果
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

        // rollback用保存ルート
        const auto lockedRoot = root / "rollback";
        // rollback用profiles
        const LamaPon::PersistenceProfiles profiles(
            lockedRoot,
            "rollback-game");
        // guestプロフィール
        const auto guest = profiles.Guest();
        // 既存accountプロフィール
        const auto account = profiles.Account("locked-account");
        // rollback対象prefs
        LamaPon::PlayerPrefs preferences(guest.playerPrefsFile);
        preferences.Load();
        preferences.SetString("state", "must-survive");
        preferences.Save();
        preferences.SetString("state", "unsaved-change");
        // guestのSaveData
        LamaPon::SaveDataStore saves(guest.saveDataDirectory);

        // 保存lock用handle
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

        // rollback import結果
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

// 保存プロフィール試験を実行します。
int main()
{
    // テストroot
    const auto root =
        std::filesystem::current_path()
        / "test-output"
        / "profiles";
    // OnlineProfiles掃除先
    const auto onlineProfiles = root / "OnlineProfiles";
    // テスト前後の掃除を保護します。
    try
    {
        Require(
            onlineProfiles.parent_path() == root,
            "Online profile cleanup target escaped the test root.");
        std::filesystem::remove_all(onlineProfiles);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        // path安全性、再bind、import、失敗rollbackを順に検証します。
        TestStableSafeAccountPaths(root / "paths");
        TestStrongRebind(root / "rebind");
        TestOpenFailuresAreNotMissing(root / "open-failures");
        TestExplicitGuestImport(root / "import");
        TestImportRejectsInvalidDocuments(root / "invalid-import");
        TestNothingAndRollback(root);

        std::filesystem::remove_all(onlineProfiles);
        std::filesystem::remove_all(root);
        std::cout << "Persistence profile tests passed.\n";
        // 全テストの成功を返します。
        return 0;
    }
    // mainの例外を捕捉します(exception: 失敗理由)。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返します。
        return 1;
    }
}
