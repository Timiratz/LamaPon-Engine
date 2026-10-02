#include "LamaPon/Online/CloudSaveJournal.h"

#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using Journal = LamaPon::Detail::CloudSaveJournal;

    // テスト用のSHA-256形式アカウント識別子
    constexpr std::string_view AccountKey =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    // レベル本文のSHA-256
    constexpr std::string_view LevelHash =
        "fUV07UsUNLX3A0GWwak_tdwTcH_rWlJ1CqBLNiwLRwY";
    // 空JSON本文のSHA-256
    constexpr std::string_view EmptyHash =
        "RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o";
    // 1件目の冪等性識別子
    constexpr std::string_view MutationOne =
        "123e4567-e89b-42d3-a456-426614174000";
    // 2件目の冪等性識別子
    constexpr std::string_view MutationTwo =
        "123e4567-e89b-42d3-b456-426614174001";
    // 3件目の冪等性識別子
    constexpr std::string_view MutationThree =
        "123e4567-e89b-42d3-8456-426614174002";

    // 条件を満たさなければテストを失敗させます。
    // Require(condition: 成功条件, message: 失敗理由)
    void Require(const bool condition, const char* message)
    {
        // 失敗条件
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // UTF-8本文のSHA-256を小文字16進数へ変換します。
    // Sha256LowerHex(value: ハッシュ対象のバイト列)
    std::string Sha256LowerHex(const std::string_view value)
    {
        // SHA-256プロバイダー
        BCRYPT_ALG_HANDLE algorithm{};
        Require(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0u) >= 0,
            "Journal test SHA-256 provider failed.");
        // SHA-256ダイジェスト
        std::array<std::uint8_t, 32u> digest{};
        // BCryptHashの成否
        const auto status = BCryptHash(
            algorithm,
            nullptr,
            0u,
            value.empty()
                ? nullptr
                : reinterpret_cast<PUCHAR>(
                    const_cast<char*>(value.data())),
            static_cast<ULONG>(value.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0u);
        Require(status >= 0, "Journal test SHA-256 failed.");
        // 小文字16進数の変換表
        constexpr char Digits[] = "0123456789abcdef";
        // 結果文字列
        std::string result;
        result.reserve(digest.size() * 2u);
        // ダイジェスト各バイトを16進数2桁に変換
        for (const auto byte : digest)
        {
            result.push_back(Digits[(byte >> 4u) & 0x0fu]);
            result.push_back(Digits[byte & 0x0fu]);
        }
        return result;
    }

    // 関数が例外を送出するかを返します。
    // Throws(function: 実行する処理)
    template<class Function>
    bool Throws(Function&& function)
    {
        // 例外の有無を確認
        try
        {
            std::forward<Function>(function)();
            return false;
        }
        // 送出された例外を期待結果として扱う
        catch (...)
        {
            return true;
        }
    }

    // ファイル所有者が現在のユーザーか検査します。
    // IsOwnedByCurrentUser(path: 所有者を検査するパス)
    bool IsOwnedByCurrentUser(const std::filesystem::path& path)
    {
        // 現在プロセスのアクセストークン
        HANDLE token{};
        // トークンを取得できない場合
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        {
            return false;
        }
        // TokenUser情報に必要なバイト数
        DWORD bytes{};
        GetTokenInformation(token, TokenUser, nullptr, 0u, &bytes);
        // TokenUser情報を取得できない場合
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
            || bytes < sizeof(TOKEN_USER))
        {
            CloseHandle(token);
            return false;
        }
        // TokenUser情報を格納する領域
        std::vector<std::uint8_t> tokenUser(bytes);
        // ユーザーSIDを取得できない場合
        if (GetTokenInformation(
                token,
                TokenUser,
                tokenUser.data(),
                bytes,
                &bytes) == FALSE)
        {
            CloseHandle(token);
            return false;
        }
        CloseHandle(token);
        // 現在ユーザーのSID
        const auto currentUser =
            reinterpret_cast<const TOKEN_USER*>(tokenUser.data())->User.Sid;
        // 対象ファイルの所有SID
        PSID owner{};
        // 所有者情報を保持するセキュリティ記述子
        PSECURITY_DESCRIPTOR descriptor{};
        // Windows所有者照会の結果コード
        const auto result = GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,
            &owner,
            nullptr,
            nullptr,
            nullptr,
            &descriptor);
        // 照会成功かつ所有SIDが現在ユーザーと一致するか
        const bool matches = result == ERROR_SUCCESS
            && descriptor != nullptr
            && owner != nullptr
            && EqualSid(owner, currentUser) != FALSE;
        // Windows APIが割り当てた記述子を解放
        if (descriptor != nullptr)
        {
            LocalFree(descriptor);
        }
        return matches;
    }

    // 現在のテスト専用作業ディレクトリを返します。
    std::filesystem::path TestRoot()
    {
        // test-output配下へ正規化した作業ルート
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"cloud-save-journal").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Journal test cleanup root escaped test-output.");
        return root;
    }

    // テスト作業ルートを空にして作成します。
    void ResetTestRoot()
    {
        // ファイル操作のエラー出力
        std::error_code error;
        // 前回のテストデータを削除
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Journal test cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Journal test root creation failed.");
    }

    // テスト名から信頼済みルートを作ります。
    // TrustedPath(name: テスト用サブディレクトリ名)
    std::filesystem::path TrustedPath(const std::string_view name)
    {
        return TestRoot()
            / std::filesystem::path(
                std::u8string(
                    reinterpret_cast<const char8_t*>(name.data()),
                    reinterpret_cast<const char8_t*>(name.data() + name.size())))
            / L"GuestDisplayName";
    }

    // 信頼済みルートに対応するアカウントパスを作ります。
    // Profile(trusted: 信頼済みルート, key: アカウント識別子)
    LamaPon::PersistenceProfilePaths Profile(
        const std::filesystem::path& trusted,
        const std::string_view key = AccountKey)
    {
        // アカウント固有のプロファイルルート
        const auto root = trusted.parent_path()
            / L"OnlineProfiles"
            / std::filesystem::path(
                std::u8string(
                    reinterpret_cast<const char8_t*>(key.data()),
                    reinterpret_cast<const char8_t*>(key.data() + key.size())));
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::string(key),
            false
        };
    }

    // テスト用のアカウントジャーナルを開きます。
    // MakeJournal(trusted: 信頼済みルート, backend: バックエンドURL)
    std::unique_ptr<Journal> MakeJournal(
        const std::filesystem::path& trusted,
        const std::string_view backend = "https://online.example.test/tenant-a/")
    {
        return std::make_unique<Journal>(
            trusted,
            Profile(trusted),
            "journal-game",
            "staging",
            std::string(backend));
    }

    // ハッシュ固定のレベルJSON本文を返します。
    std::vector<std::uint8_t> LevelContent()
    {
        return { '{', '"', 'l', 'e', 'v', 'e', 'l', '"', ':', '7', '}' };
    }

    // 空のJSONオブジェクト本文を返します。
    std::vector<std::uint8_t> EmptyContent()
    {
        return { '{', '}' };
    }

    // 生存中のクラウド保存スナップショットを作ります。
    // LiveSnapshot(resource: リソース識別子, etag: 版番号, content: 本文, hash: SHA-256)
    LamaPon::CloudSaveSnapshot LiveSnapshot(
        LamaPon::CloudSaveResource resource,
        std::string etag,
        std::vector<std::uint8_t> content,
        const std::string_view hash)
    {
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            std::string(hash)
        };
    }

    // 削除済みクラウド保存スナップショットを作ります。
    // Tombstone(resource: リソース識別子, etag: 版番号)
    LamaPon::CloudSaveSnapshot Tombstone(
        LamaPon::CloudSaveResource resource,
        std::string etag)
    {
        return {
            std::move(resource),
            std::move(etag),
            true,
            {},
            {}
        };
    }

    // テストファイルをバイナリのまま文字列で読みます。
    // ReadText(path: 読み込むファイル)
    std::string ReadText(const std::filesystem::path& path)
    {
        // 改行変換を行わない入力ストリーム
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "Journal fixture could not be read.");
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    // 文字列を指定先へバイナリ上書きします。
    // WriteText(path: 書き込むファイル, text: 保存本文)
    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        // 既存内容を置き換える出力ストリーム
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(output.good(), "Journal fixture could not be opened.");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(output.good(), "Journal fixture could not be written.");
    }

    // パス末尾に候補ファイルの拡張子を加えます。
    // Suffix(path: 基本パス, suffix: 追加する接尾辞)
    std::filesystem::path Suffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        // 接尾辞を追加する複製パス
        auto result = path;
        result += suffix;
        return result;
    }

    // 指定したテストファイルだけを削除します。
    // RemoveExact(path: 削除対象ファイル)
    void RemoveExact(const std::filesystem::path& path)
    {
        // ファイル操作のエラー出力
        std::error_code error;
        (void)std::filesystem::remove(path, error);
        Require(!error, "Journal fixture cleanup failed.");
    }

    // 書込先行記録、競合解決、再起動復元を検証します。
    void TestWriteAheadRoundTripAndConflict()
    {
        // テスト専用の信頼済みディレクトリ
        const auto trusted = TrustedPath("roundtrip");
        // アカウント永続化先の期待パス
        const auto profile = Profile(trusted);
        Require(
            !std::filesystem::exists(profile.rootDirectory),
            "Account fixture unexpectedly existed.");

        // ジャーナル実ファイルのパス
        std::filesystem::path journalPath;
        {
            // 初回書込対象のジャーナル
            auto journal = MakeJournal(trusted);
            journalPath = journal->FilePath();
            Require(
                journal->Generation() == 0u
                    && !std::filesystem::exists(journalPath)
                    && !std::filesystem::exists(profile.rootDirectory)
                    && journalPath.parent_path().parent_path().filename()
                        == L"OnlineState",
                "Journal construction created account persistence.");
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                LevelContent(),
                MutationOne);
            Require(
                journal->Generation() == 1u
                    && std::filesystem::exists(journalPath)
                    && !std::filesystem::exists(profile.rootDirectory),
                "Write-ahead journal was not durably separated from account data.");
        }

        {
            // 再起動後に復元したジャーナル
            auto journal = MakeJournal(trusted);
            // 復元された未送信変更
            const auto pending = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                journal->Generation() == 1u
                    && pending
                    && pending->mutationId == MutationOne
                    && !pending->baseEtag
                    && pending->content == LevelContent()
                    && pending->sha256 == LevelHash
                    && journal->Dispatchable().size() == 1u,
                "Pending mutation did not survive restart exactly.");
            journal->RecordConflict(
                LamaPon::CloudSaveResource::Preferences(),
                MutationOne,
                LiveSnapshot(
                    LamaPon::CloudSaveResource::Preferences(),
                    "\"remote-1\"",
                    EmptyContent(),
                    EmptyHash));
        }

        {
            // 競合記録を復元するジャーナル
            auto journal = MakeJournal(trusted);
            // 再送前の保留中変更
            const auto pending = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            // サーバー側で検出した版競合
            const auto conflict = journal->Conflict(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                pending && pending->mutationId == MutationOne
                    && conflict && conflict->etag == "\"remote-1\""
                    && journal->Dispatchable().empty()
                    && journal->Resources().size() == 1u,
                "Conflict or its immutable pending mutation was not restored.");
            // 古い画面操作が持つ世代番号
            const auto generation = journal->Generation();
            Require(
                Throws([&]
                {
                    journal->ResolveConflict(
                        LamaPon::CloudSaveResource::Preferences(),
                        MutationTwo,
                        LamaPon::Detail::CloudSaveConflictResolution::RetryLocal,
                        MutationThree);
                })
                    && journal->Generation() == generation,
                "A stale conflict UI mutation id changed the journal.");
            journal->ResolveConflict(
                LamaPon::CloudSaveResource::Preferences(),
                MutationOne,
                LamaPon::Detail::CloudSaveConflictResolution::RetryLocal,
                MutationTwo);
            // 競合解決後の再送変更
            const auto retried = journal->Pending(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                retried && retried->mutationId == MutationTwo
                    && retried->baseEtag == "\"remote-1\""
                    && retried->content == LevelContent()
                    && !journal->Conflict(
                        LamaPon::CloudSaveResource::Preferences())
                    && journal->Dispatchable().size() == 1u,
                "RetryLocal changed content or failed to advance CAS metadata.");
            journal->RecordSuccess(
                LamaPon::CloudSaveResource::Preferences(),
                MutationTwo,
                LiveSnapshot(
                    LamaPon::CloudSaveResource::Preferences(),
                    "\"accepted-2\"",
                    LevelContent(),
                    LevelHash));
        }

        {
            // 成功応答を復元するジャーナル
            auto journal = MakeJournal(trusted);
            // サーバーで確定した基準スナップショット
            const auto baseline = journal->Baseline(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                baseline && baseline->etag == "\"accepted-2\""
                    && baseline->content == LevelContent()
                    && !journal->HasPending(
                        LamaPon::CloudSaveResource::Preferences()),
                "Mutation success was not retained as a full baseline.");
        }

        // ディスクへ保存されたジャーナル本文
        const auto persisted = ReadText(journalPath);
        // 検証対象のジャーナルJSON
        const auto document = Json::parse(persisted);
        Require(
            document.contains("parentGeneration")
                && document.contains("parentChecksum")
                && document.contains("checksum")
                && persisted.find("journal-game") == std::string::npos
                && persisted.find("staging") == std::string::npos
                && persisted.find("online.example.test") == std::string::npos
                && persisted.find("token") == std::string::npos
                && persisted.find("discord") == std::string::npos,
            "Journal schema omitted its chain or exposed namespace/identity data.");
    }

    // 削除競合でのリモート採用と識別子一意性を検証します。
    void TestDeleteUseRemoteAndGlobalMutationIds()
    {
        // テスト専用の信頼済みディレクトリ
        const auto trusted = TrustedPath("delete");
        // 削除変更を記録するジャーナル
        auto journal = MakeJournal(trusted);
        // 大文字小文字を無視する保存スロット
        const auto slot = LamaPon::CloudSaveResource::SaveSlot("Slot-A");
        journal->RecordBaseline(Tombstone(slot, "\"old\""));
        journal->QueueDelete(slot, MutationOne, "\"old\"");
        // 識別子重複検査前の世代
        const auto before = journal->Generation();
        Require(
            Throws([&]
            {
                journal->QueuePut(
                    LamaPon::CloudSaveResource::Preferences(),
                    EmptyContent(),
                    MutationOne);
            })
                && journal->Generation() == before,
            "A mutation id was reused by a different resource.");
        journal->RecordConflict(
            slot,
            MutationOne,
            LiveSnapshot(slot, "\"remote\"", EmptyContent(), EmptyHash));
        journal.reset();

        journal = MakeJournal(trusted);
        Require(
            journal->Pending(
                LamaPon::CloudSaveResource::SaveSlot("slot-a"))
                && journal->Conflict(
                    LamaPon::CloudSaveResource::SaveSlot("slot-a")),
            "Case-insensitive slot identity was not restored.");
        journal->ResolveConflict(
            LamaPon::CloudSaveResource::SaveSlot("slot-a"),
            MutationOne,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        // サーバー側削除状態を採用した基準スナップショット
        const auto baseline = journal->Baseline(slot);
        Require(
            baseline && !baseline->deleted
                && baseline->etag == "\"remote\""
                && !journal->Pending(slot),
            "UseRemote did not finalize the retained full remote snapshot.");
    }

    // 古いジャーナル実体の世代更新を拒否することを検証します。
    void TestGenerationCasBlocksStaleInstance()
    {
        // 両実体で共有する信頼済みルート
        const auto trusted = TrustedPath("cas");
        // 世代を先に更新するジャーナル
        auto first = MakeJournal(trusted);
        // 更新前の世代を読み込んだ古い実体
        auto stale = MakeJournal(trusted);
        first->RecordBaseline(Tombstone(
            LamaPon::CloudSaveResource::SaveSlot("first"),
            "\"one\""));
        Require(
            Throws([&]
            {
                stale->RecordBaseline(Tombstone(
                    LamaPon::CloudSaveResource::SaveSlot("stale"),
                    "\"two\""));
            }),
            "A stale journal instance bypassed generation CAS.");
        Require(
            Throws([&] { (void)stale->Dispatchable(); })
                && Throws([&] { (void)stale->Generation(); }),
            "A stale instance remained usable after an ambiguous CAS failure.");
        // 永続状態から再読込した正本
        auto reopened = MakeJournal(trusted);
        Require(
            reopened->Resources().size() == 1u
                && reopened->Resources()[0].slot == "first",
            "A CAS loser changed persistent state.");
    }

    // OSロック競合時の失敗と後続再試行を検証します。
    void TestOperatingSystemLockFailsClosed()
    {
        // ロック検証用の信頼済みルート
        const auto trusted = TrustedPath("os-lock");
        // OSロックを使うジャーナル
        auto journal = MakeJournal(trusted);
        // 排他ロックファイル
        const auto lockPath =
            journal->FilePath().parent_path() / L"CloudSaveJournal.lock";
        Require(
            LamaPon::Detail::IsCloudSaveJournalLockExclusiveForTesting(
                lockPath),
            "Two production journal locks were acquired concurrently.");
        // 他プロセスを模した保持中のWindowsハンドル
        const auto lock = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(lock != INVALID_HANDLE_VALUE, "Journal OS lock fixture failed.");
        // ロック中の書込が拒否されたか
        const bool rejected = Throws([&]
        {
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
        });
        CloseHandle(lock);
        Require(
            rejected && journal->Generation() == 0u
                && journal->Resources().empty(),
            "An initial lock contention permanently blocked a clean instance.");
        journal->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        // ロック解除後に永続記録を再確認
        auto reopened = MakeJournal(trusted);
        Require(
            reopened->Generation() == 1u
                && reopened->Pending(
                    LamaPon::CloudSaveResource::Preferences()),
            "A journal operation could not retry after lock contention ended.");
    }

    // 書込先、候補、ロックのハードリンクを更新前に拒否します。
    void TestHardLinksAreRejectedBeforeMutation()
    {
        // ハードリンク検証用ルート
        const auto trusted = TrustedPath("hard-links");
        // 検証対象ジャーナル
        auto journal = MakeJournal(trusted);
        // 公開済みジャーナルファイル
        const auto finalPath = journal->FilePath();
        // 書込途中ファイル
        const auto writingPath = Suffix(finalPath, L".writing");
        Require(
            IsOwnedByCurrentUser(finalPath.parent_path().parent_path())
                && IsOwnedByCurrentUser(finalPath.parent_path())
                && IsOwnedByCurrentUser(
                    finalPath.parent_path() / L"CloudSaveJournal.lock"),
            "A newly created restricted journal object has an unsafe owner.");
        // ハードリンク先の保護対象ファイル
        const auto writeVictim = TestRoot() / L"hard-link-write-victim.txt";
        // 保護対象へ事前に保存する本文
        constexpr std::string_view WriteVictimText = "must-not-be-truncated";
        WriteText(writeVictim, WriteVictimText);
        Require(
            CreateHardLinkW(
                writingPath.c_str(),
                writeVictim.c_str(),
                nullptr) != FALSE,
            "Journal write hard-link fixture could not be created.");
        Require(
            Throws([&]
            {
                journal->QueuePut(
                    LamaPon::CloudSaveResource::Preferences(),
                    EmptyContent(),
                    MutationOne);
            })
                && ReadText(writeVictim) == WriteVictimText,
            "A hard-linked write target was modified before validation.");
        RemoveExact(writingPath);
        RemoveExact(writeVictim);
        journal->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        Require(
            IsOwnedByCurrentUser(finalPath),
            "A newly published journal has an unsafe owner.");
        journal.reset();

        // 読込候補の別名ハードリンク
        const auto candidateAlias = Suffix(finalPath, L".candidate-alias");
        // 変更されてはならない公開本文
        const auto finalDocument = ReadText(finalPath);
        Require(
            CreateHardLinkW(
                candidateAlias.c_str(),
                finalPath.c_str(),
                nullptr) != FALSE,
            "Journal candidate hard-link fixture could not be created.");
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(finalPath) == finalDocument,
            "A hard-linked journal candidate was accepted or modified.");
        RemoveExact(candidateAlias);

        // ハードリンクを検査するロックファイル
        const auto lockPath =
            finalPath.parent_path() / L"CloudSaveJournal.lock";
        RemoveExact(lockPath);
        // ロック別名が壊してはならないデータファイル
        const auto lockVictim = TestRoot() / L"hard-link-lock-victim.txt";
        // 保護対象ファイルに書く初期本文
        constexpr std::string_view LockVictimText = "must-remain-a-data-file";
        WriteText(lockVictim, LockVictimText);
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Journal lock hard-link fixture could not be created.");
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(lockVictim) == LockVictimText,
            "A hard-linked journal lock was accepted or modified.");
        RemoveExact(lockPath);
        RemoveExact(lockVictim);
    }

    // 書込途中ファイルを復旧対象にせずflush成功後を確定します。
    void TestDurabilityBarrierAndWritingCandidate()
    {
        // flush故障注入用の信頼済みルート
        const auto trusted = TrustedPath("flush-barrier");
        // 復旧時に選択する最終ファイル
        std::filesystem::path finalPath;
        {
            // flush失敗を注入するジャーナル
            auto journal = MakeJournal(trusted);
            finalPath = journal->FilePath();
            LamaPon::Detail::SetCloudSaveJournalTestFailPoint(
                LamaPon::Detail::CloudSaveJournalTestFailPoint::
                    BeforeNextFlush);
            Require(
                Throws([&]
                {
                    journal->QueuePut(
                        LamaPon::CloudSaveResource::Preferences(),
                        EmptyContent(),
                        MutationOne);
                })
                    && journal->Generation() == 0u
                    && !journal->Pending(
                        LamaPon::CloudSaveResource::Preferences())
                    && !std::filesystem::exists(finalPath)
                    && std::filesystem::exists(
                        Suffix(finalPath, L".writing")),
                "A mutation without a successful flush became dispatchable.");
        }
        {
            // 書込途中ファイルを無視して再開する実体
            auto reopened = MakeJournal(trusted);
            Require(
                reopened->Generation() == 0u
                    && reopened->Resources().empty(),
                "A selector adopted the non-candidate .writing file.");
            LamaPon::Detail::SetCloudSaveJournalTestFailPoint(
                LamaPon::Detail::CloudSaveJournalTestFailPoint::
                    AfterNextFlush);
            reopened->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
            Require(
                reopened->Generation() == 1u
                    && reopened->Pending(
                        LamaPon::CloudSaveResource::Preferences()),
                "A flushed .next candidate was not recovered as committed.");
        }
    }

    // 保存スロット上限と不正リソースを検証します。
    void TestResourceLimits()
    {
        // 上限検査用の信頼済みルート
        const auto trusted = TrustedPath("limits");
        // 保存スロットを上限まで保持するジャーナル
        auto journal = MakeJournal(trusted);
        journal->RecordBaseline(Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"preferences\""));
        // 全スロット上限分の基準を登録
        for (std::size_t index = 0u;
             index < LamaPon::CloudSaveMaxSlots;
             ++index)
        {
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::SaveSlot(
                    "slot-" + std::to_string(index)),
                "\"v-" + std::to_string(index) + "\""));
        }
        // 上限超過操作前の世代番号
        const auto generation = journal->Generation();
        Require(
            journal->Resources().size()
                    == LamaPon::CloudSaveMaxSlots + 1u
                && Throws([&]
                {
                    journal->RecordBaseline(Tombstone(
                        LamaPon::CloudSaveResource::SaveSlot("slot-overflow"),
                        "\"overflow\""));
                })
                && journal->Generation() == generation,
            "Journal save-slot limits were not enforced atomically.");
        // 列挙範囲外のリソース種別
        LamaPon::CloudSaveResource invalid;
        invalid.kind = static_cast<LamaPon::CloudSaveResourceKind>(255u);
        Require(
            Throws([&]
            {
                journal->RecordBaseline(Tombstone(invalid, "\"invalid\""));
            }),
            "An invalid resource kind was accepted.");
    }

    // 世代候補の復旧順位と分岐検出を検証します。
    void TestRecoveryTopologyAndSplitBrain()
    {
        // 世代候補を作る信頼済みルート
        const auto trusted = TrustedPath("recovery");
        // ジャーナルの最終パス
        std::filesystem::path finalPath;
        // 親世代の保存本文
        std::string generationOne;
        // 子世代の保存本文
        std::string generationTwo;
        {
            // 2世代の候補を作成するジャーナル
            auto journal = MakeJournal(trusted);
            finalPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"one\""));
            generationOne = ReadText(finalPath);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"two\""));
            generationTwo = ReadText(finalPath);
        }
        Require(
            ReadText(Suffix(finalPath, L".bak")) == generationOne,
            "Normal publish did not retain the parent generation.");
        WriteText(Suffix(finalPath, L".next"), generationTwo);
        WriteText(finalPath, "{truncated");
        {
            // flush済み子世代を復旧する実体
            auto recovered = MakeJournal(trusted);
            Require(
                recovered->Generation() == 2u
                    && recovered->Baseline(
                        LamaPon::CloudSaveResource::Preferences())->etag
                        == "\"two\""
                    && ReadText(Suffix(finalPath, L".bak")) == generationOne,
                "A flushed child was not recovered without destroying its valid parent.");
        }
        // final(g2)を古い同一next/backup(g1)で巻き戻さない
        WriteText(Suffix(finalPath, L".next"), generationOne);
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(finalPath) == generationTwo,
            "A stale identical next/backup pair rolled back a newer final.");
        WriteText(finalPath, generationOne);
        WriteText(Suffix(finalPath, L".next"), generationOne);
        WriteText(Suffix(finalPath, L".bak"), generationTwo);
        Require(
            Throws([&] { (void)MakeJournal(trusted); })
                && ReadText(Suffix(finalPath, L".bak")) == generationTwo,
            "An identical old final/next pair hid a newer backup.");

        // backup候補を復旧する別ルート
        const auto backupTrusted = TrustedPath("backup-recovery");
        // backup復旧用の最終パス
        std::filesystem::path backupFinal;
        // 有効なbackup世代の本文
        std::string backupDocument;
        {
            // backup復旧データを作るジャーナル
            auto journal = MakeJournal(backupTrusted);
            backupFinal = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"backup-one\""));
            backupDocument = ReadText(backupFinal);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"discarded-child\""));
        }
        WriteText(backupFinal, "{truncated");
        WriteText(Suffix(backupFinal, L".bak"), backupDocument);
        // backup復旧直後に同じ世代をnextへflushした状態
        WriteText(Suffix(backupFinal, L".next"), backupDocument);
        {
            // backupとnextが同じ世代の復旧結果
            auto recovered = MakeJournal(backupTrusted);
            Require(
                recovered->Generation() == 1u
                    && recovered->Baseline(
                        LamaPon::CloudSaveResource::Preferences())->etag
                        == "\"backup-one\"",
                "Equivalent backup/next recovery copies were treated as split brain.");
        }

        // 同世代分岐を検証するルート
        const auto splitTrusted = TrustedPath("split-brain");
        // 分岐候補を置く最終パス
        std::filesystem::path splitFinal;
        // 分岐Aの親世代本文
        std::string branchA;
        // 分岐Aの子世代本文
        std::string branchAChild;
        // 分岐Bの世代本文
        std::string branchB;
        {
            // 分岐Aの2世代を作るジャーナル
            auto journal = MakeJournal(splitTrusted);
            splitFinal = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-a\""));
            branchA = ReadText(splitFinal);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-a-child\""));
            branchAChild = ReadText(splitFinal);
        }
        RemoveExact(splitFinal);
        RemoveExact(Suffix(splitFinal, L".bak"));
        RemoveExact(Suffix(splitFinal, L".next"));
        {
            // 分岐Bを作るジャーナル
            auto journal = MakeJournal(splitTrusted);
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"branch-b\""));
            branchB = ReadText(splitFinal);
        }
        WriteText(splitFinal, branchA);
        WriteText(Suffix(splitFinal, L".next"), branchB);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Same-generation split brain was accepted.");
        WriteText(splitFinal, branchAChild);
        WriteText(Suffix(splitFinal, L".next"), branchAChild);
        WriteText(Suffix(splitFinal, L".bak"), branchB);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Identical final/next bypassed an unrelated parent candidate.");
        WriteText(splitFinal, branchB);
        WriteText(Suffix(splitFinal, L".next"), branchAChild);
        WriteText(Suffix(splitFinal, L".bak"), branchAChild);
        Require(
            Throws([&] { (void)MakeJournal(splitTrusted); }),
            "Identical next/backup bypassed an unrelated parent candidate.");
    }

    // バックエンド結合、未知版、重複キーを拒否することを検証します。
    void TestBindingSchemaAndContentFailClosed()
    {
        // バインディング検証用の信頼済みルート
        const auto trusted = TrustedPath("binding");
        // 作成するジャーナル本文のパス
        std::filesystem::path filePath;
        {
            // 重複JSONキー検査用ジャーナル
            auto journal = MakeJournal(trusted);
            filePath = journal->FilePath();
            journal->QueuePut(
                LamaPon::CloudSaveResource::Preferences(),
                EmptyContent(),
                MutationOne);
            // 重複キー検査前の世代
            const auto generation = journal->Generation();
            // 重複キーを持つ不正JSON本文
            const std::vector<std::uint8_t> duplicate{
                '{', '"', 'x', '"', ':', '1', ',',
                '"', 'x', '"', ':', '2', '}'
            };
            Require(
                Throws([&]
                {
                    journal->QueuePut(
                        LamaPon::CloudSaveResource::SaveSlot("dup"),
                        duplicate,
                        MutationTwo);
                })
                    && journal->Generation() == generation,
                "Duplicate keys in embedded JSON were accepted.");
        }
        Require(
            Throws([&]
            {
                (void)MakeJournal(
                    trusted,
                    "https://online.example.test/tenant-b/");
            }),
            "A different backend base path reused a bound journal.");

        // 将来版を模した保存文書
        auto future = Json::parse(ReadText(filePath));
        future["version"] = 3;
        WriteText(Suffix(filePath, L".next"), future.dump());
        Require(
            Throws([&] { (void)MakeJournal(trusted); }),
            "A future journal version was ignored in favor of an old candidate.");

        // 未知フィールド検査用ルート
        const auto schemaTrusted = TrustedPath("unknown-schema");
        // スキーマ検査対象のファイル
        std::filesystem::path schemaPath;
        {
            // 既知スキーマの基準を作るジャーナル
            auto journal = MakeJournal(schemaTrusted);
            schemaPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"schema\""));
        }
        // 未知フィールドを持つ保存文書
        auto unknownSchema = Json::parse(ReadText(schemaPath));
        unknownSchema["futureField"] = true;
        WriteText(Suffix(schemaPath, L".next"), unknownSchema.dump());
        Require(
            Throws([&] { (void)MakeJournal(schemaTrusted); }),
            "An unknown journal schema was ignored in favor of an old candidate.");

        // 重複キー検査用の信頼済みルート
        const auto corruptTrusted = TrustedPath("corrupt");
        // 不正JSONを書き込むファイル
        std::filesystem::path corruptPath;
        {
            // 正しい本文を作るジャーナル
            auto journal = MakeJournal(corruptTrusted);
            corruptPath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"valid\""));
        }
        // 重複キーを挿入する保存本文
        auto corrupt = ReadText(corruptPath);
        // 先頭のformatフィールド位置
        const auto position = corrupt.find("\"format\"");
        Require(position != std::string::npos, "Journal fixture had no format field.");
        corrupt.insert(position, "\"format\":\"duplicate\",");
        WriteText(corruptPath, corrupt);
        Require(
            Throws([&] { (void)MakeJournal(corruptTrusted); }),
            "Duplicate journal keys were accepted as an empty state.");
    }

    // 信頼済みパス制約とアカウント領域の非作成を検証します。
    void TestTrustedPathAndAccountNonCreation()
    {
        // 正当なテスト用ルート
        const auto trusted = TrustedPath("paths");
        // 正当なアカウントパス集合
        auto profile = Profile(trusted);
        // 信頼範囲外に置いた偽装アカウントパス
        const auto arbitrary = TestRoot()
            / L"arbitrary"
            / L"OnlineProfiles"
            / std::filesystem::path(std::string(AccountKey));
        // 信頼ルート外を指す偽装プロファイル
        auto forged = profile;
        forged.rootDirectory = arbitrary;
        forged.playerPrefsFile = arbitrary / L"PlayerPrefs.json";
        forged.saveDataDirectory = arbitrary / L"Saves";
        Require(
            Throws([&]
            {
                // 偽装されたパスを拒否する生成対象
                Journal journal(
                    trusted,
                    forged,
                    "game",
                    "production",
                    "https://online.example.test");
            })
                && !std::filesystem::exists(arbitrary),
            "A forged account root escaped the trusted anchor.");

        // ゲスト指定のプロファイル
        auto guest = profile;
        guest.isGuest = true;
        Require(
            Throws([&]
            {
                // ゲストプロファイルを拒否する生成対象
                Journal journal(
                    trusted,
                    guest,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A guest profile was accepted by the account journal.");
        Require(
            Throws([&]
            {
                // 相対パスの信頼アンカーを拒否する生成対象
                Journal journal(
                    L"relative-user-data",
                    profile,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A relative trusted anchor was accepted.");
        Require(
            Throws([&]
            {
                // ネットワーク共有の信頼アンカーを拒否する生成対象
                Journal journal(
                    L"\\\\server\\share\\Guest",
                    profile,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A network trusted anchor was accepted.");

        // 大小文字差の検査用ルート
        const auto caseTrusted = TrustedPath("case-spelling");
        // 大小文字だけを変えた偽装プロファイル
        auto caseForged = Profile(caseTrusted);
        caseForged.playerPrefsFile =
            caseForged.rootDirectory / L"playerprefs.json";
        Require(
            Throws([&]
            {
                // 偽装パスを拒否する生成対象
                Journal journal(
                    caseTrusted,
                    caseForged,
                    "game",
                    "production",
                    "https://online.example.test");
            }),
            "A case-only forged account profile path was accepted.");

        // 正当なルートで作るジャーナル
        auto valid = MakeJournal(trusted);
        valid->QueuePut(
            LamaPon::CloudSaveResource::Preferences(),
            EmptyContent(),
            MutationOne);
        Require(
            !std::filesystem::exists(profile.rootDirectory)
                && !std::filesystem::exists(
                    trusted.parent_path() / L"OnlineProfiles"),
            "Journal activity changed guest-import account existence.");
    }

    // 削除意図の永続性と版1から版2への移行を検証します。
    void TestLocalDeleteIntentAndVersionOneMigration()
    {
        // 移行試験用の信頼済みルート
        const auto trusted = TrustedPath("version-migration");
        // 移行対象ジャーナルのパス
        std::filesystem::path filePath;
        {
            // 版1へ変換する基準ジャーナル
            auto journal = MakeJournal(trusted);
            filePath = journal->FilePath();
            journal->RecordBaseline(Tombstone(
                LamaPon::CloudSaveResource::Preferences(),
                "\"v1-baseline\""));
        }

        // 版1形式へ書き換えるジャーナル文書
        auto versionOne = Json::parse(ReadText(filePath));
        versionOne["version"] = 1u;
        // 版1に存在しない削除意図属性を除去
        for (auto& entry : versionOne.at("entries"))
        {
            entry.erase("localDeleteIntent");
        }
        versionOne.erase("checksum");
        versionOne["checksum"] = Sha256LowerHex(versionOne.dump());
        WriteText(filePath, versionOne.dump());
        RemoveExact(Suffix(filePath, L".next"));
        RemoveExact(Suffix(filePath, L".bak"));

        {
            // 版1から移行するジャーナル
            auto migrated = MakeJournal(trusted);
            Require(
                migrated->Generation() == 1u
                    && migrated->Baseline(
                        LamaPon::CloudSaveResource::Preferences())
                        ->etag == "\"v1-baseline\"",
                "A valid version-1 journal could not be loaded.");
            migrated->RecordLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                migrated->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "A local delete intent was not durable in memory.");
        }
        Require(
            Json::parse(ReadText(filePath)).at("version") == 2u,
            "A version-1 journal was not durably migrated on mutation.");
        {
            // 移行後の永続値を再読込するジャーナル
            auto reopened = MakeJournal(trusted);
            Require(
                reopened->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "A local delete intent did not survive restart.");
            reopened->ClearLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences());
            Require(
                !reopened->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences())
                    && reopened->Baseline(
                        LamaPon::CloudSaveResource::Preferences()),
                "Clearing a local delete intent discarded its baseline.");
        }
    }

    // 削除意図の一括適用が一世代で確定することを検証します。
    void TestLocalDeleteIntentBatchIsSingleGeneration()
    {
        // 一括操作用の信頼済みルート
        const auto trusted = TrustedPath("delete-intent-batch");
        // 一括操作を記録するジャーナル
        auto journal = MakeJournal(trusted);
        // 設定領域の識別子
        const auto preferences =
            LamaPon::CloudSaveResource::Preferences();
        // 保存スロットの識別子
        const auto slot =
            LamaPon::CloudSaveResource::SaveSlot("batch-slot");
        // 一世代で反映する削除意図操作列
        const std::vector<LamaPon::Detail::CloudSaveDeleteIntentOperation>
            records{
                {
                    preferences,
                    LamaPon::Detail::
                        CloudSaveDeleteIntentOperationKind::Record
                },
                {
                    slot,
                    LamaPon::Detail::
                        CloudSaveDeleteIntentOperationKind::Record
                }
            };

        // 失敗注入前の世代番号
        const auto initialGeneration = journal->Generation();
        LamaPon::Detail::SetCloudSaveJournalTestFailPoint(
            LamaPon::Detail::CloudSaveJournalTestFailPoint::BeforeNextFlush);
        Require(
            Throws([&]
            {
                journal->ApplyLocalDeleteIntentOperations(records);
            })
                && journal->Generation() == initialGeneration
                && !journal->HasLocalDeleteIntent(preferences)
                && !journal->HasLocalDeleteIntent(slot),
            "A failed delete-intent batch published a partial operation list.");

        journal = MakeJournal(trusted);
        journal->ApplyLocalDeleteIntentOperations(records);
        Require(
            journal->Generation() == initialGeneration + 1u
                && journal->HasLocalDeleteIntent(preferences)
                && journal->HasLocalDeleteIntent(slot),
            "A delete-intent batch did not publish in one generation.");

        // 記録済み削除意図の世代番号
        const auto recordedGeneration = journal->Generation();
        Require(
            Throws([&]
            {
                journal->ApplyLocalDeleteIntentOperations({
                    {
                        LamaPon::CloudSaveResource::SaveSlot("Batch-Slot"),
                        LamaPon::Detail::
                            CloudSaveDeleteIntentOperationKind::Record
                    },
                    {
                        LamaPon::CloudSaveResource::SaveSlot("batch-slot"),
                        LamaPon::Detail::
                            CloudSaveDeleteIntentOperationKind::Clear
                    }
                });
            })
                && journal->Generation() == recordedGeneration,
            "An ambiguous delete-intent batch changed the journal.");

        journal->ApplyLocalDeleteIntentOperations({
            {
                preferences,
                LamaPon::Detail::
                    CloudSaveDeleteIntentOperationKind::Clear
            },
            {
                slot,
                LamaPon::Detail::
                    CloudSaveDeleteIntentOperationKind::Clear
            }
        });
        Require(
            journal->Generation() == recordedGeneration + 1u
                && !journal->HasLocalDeleteIntent(preferences)
                && !journal->HasLocalDeleteIntent(slot),
            "A delete-intent clear batch was not atomic.");
    }
}

// クラウド保存ジャーナルの永続化検証を実行します。
int main()
{
    // テスト失敗を例外で収集
    try
    {
        ResetTestRoot();
        TestWriteAheadRoundTripAndConflict();
        TestDeleteUseRemoteAndGlobalMutationIds();
        TestGenerationCasBlocksStaleInstance();
        TestOperatingSystemLockFailsClosed();
        TestHardLinksAreRejectedBeforeMutation();
        TestDurabilityBarrierAndWritingCandidate();
        TestResourceLimits();
        TestRecoveryTopologyAndSplitBrain();
        TestBindingSchemaAndContentFailClosed();
        TestTrustedPathAndAccountNonCreation();
        TestLocalDeleteIntentAndVersionOneMigration();
        TestLocalDeleteIntentBatchIsSingleGeneration();
        // 作業ルート削除の結果
        std::error_code cleanupError;
        std::filesystem::remove_all(TestRoot(), cleanupError);
        Require(!cleanupError, "Journal test final cleanup failed.");
        std::cout << "Cloud save journal durability tests passed.\n";
        // 全検証と後片付けの成功
        return 0;
    }
    // テスト例外を標準エラーへ出力
    // catch(exception: 検証中に発生した例外)
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
