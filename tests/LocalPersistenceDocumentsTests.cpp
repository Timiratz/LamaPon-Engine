#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSave.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Documents = LamaPon::Detail::LocalPersistenceDocuments;
    using State = LamaPon::Detail::LocalPersistenceDocumentState;

    // Require(condition: 条件, message: 失敗説明)でテスト失敗を通知します。
    void Require(const bool condition, const char* message)
    {
        // テスト条件の成否を判定します。
        if (!condition)
        {
            // 失敗条件を例外で通知します。
            throw std::runtime_error(message);
        }
    }

    // Throws(function: 実行処理)で例外の有無を返します。
    template<class Function>
    bool Throws(Function&& function)
    {
        // 例外発生時の結果を確認します。
        try
        {
            std::forward<Function>(function)();
            // 例外が出なかったことを返します。
            return false;
        }
        // 全例外を捕捉します。
        catch (...)
        {
            // 例外が発生したことを返します。
            return true;
        }
    }

    // テスト出力内の安全な保存先を返します。
    std::filesystem::path TestRoot()
    {
        // テスト出力ルート
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"local-persistence-documents").lexically_normal();
        Require(
            root.parent_path().filename() == L"test-output",
            "Local persistence test root escaped test-output.");
        // 正規化済みテストルートを返します。
        return root;
    }

    // テスト出力を初期状態へ戻します。
    void ResetRoot()
    {
        // 後始末エラー
        std::error_code error;
        std::filesystem::remove_all(TestRoot(), error);
        Require(!error, "Local persistence cleanup failed.");
        std::filesystem::create_directories(TestRoot(), error);
        Require(!error, "Local persistence test root creation failed.");
    }

    // ReadText(path: 読込ファイル)をバイト列どおり文字列化します。
    std::string ReadText(const std::filesystem::path& path)
    {
        // 入力ストリーム
        std::ifstream input(path, std::ios::binary);
        Require(input.good(), "Local persistence fixture could not be read.");
        // ファイル内容を返します。
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    // WriteText(path: 保存先, text: 書込内容)をそのまま保存します。
    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        // 出力ストリーム
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Require(output.good(), "Local persistence fixture could not be opened.");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        Require(output.good(), "Local persistence fixture could not be written.");
    }

    // Bytes(text: UTF-8文書)をbyte列へ変換します。
    std::vector<std::uint8_t> Bytes(const std::string_view text)
    {
        // 文書のbyte列を返します。
        return {
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data() + text.size())
        };
    }

    // AsText(bytes: 保存byte列)を文字列へ変換します。
    std::string AsText(const std::vector<std::uint8_t>& bytes)
    {
        // 変換した文書を返します。
        return {
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()
        };
    }

    // Suffix(path: 基準パス, suffix: 追加文字列)を結合します。
    std::filesystem::path Suffix(
        const std::filesystem::path& path,
        const std::wstring_view suffix)
    {
        // 変更用パス
        auto result = path;
        result += suffix;
        // 接尾辞付きパスを返します。
        return result;
    }

    // RemoveExact(path: 削除対象)で1ファイルを削除します。
    void RemoveExact(const std::filesystem::path& path)
    {
        // ファイル削除エラー
        std::error_code error;
        (void)std::filesystem::remove(path, error);
        Require(!error, "Local persistence fixture cleanup failed.");
    }

    // PlayerPrefsのリモート文書
    constexpr std::string_view RemotePreferences =
        R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9}}})";
    // 更新版PlayerPrefs文書
    constexpr std::string_view RemotePreferencesTwo =
        R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":10}}})";
    // SaveDataのリモート文書
    constexpr std::string_view RemoteSave =
        R"({"format":"LamaPonSaveData","version":1,"slot":"remote","data":{"level":8}})";

    struct ObserverProbe final
    {
        // 通知呼び出し数
        std::size_t calls{};
        // 最新commit番号
        std::uint64_t epoch{};
        // 通知対象の所有者
        const void* expectedSource{};
        // commit後の状態確認
        bool observedCommittedState{};
        // observer戻り値
        bool returnSuccess{ true };
    };

    // ObserveCommit(context: probe, epoch: 世代, event: 通知)でcommit結果を記録します。
    bool ObserveCommit(
        void* const context,
        const std::uint64_t epoch,
        const LamaPon::Detail::LocalPersistenceCommitEvent& event) noexcept
    {
        // observerの記録先
        auto& probe = *static_cast<ObserverProbe*>(context);
        ++probe.calls;
        probe.epoch = epoch;
        // 通知元と対象ファイルを照合します。
        if (event.source != probe.expectedSource || event.filePath == nullptr)
        {
            // 不一致通知を拒否します。
            return false;
        }
        // commit後の文書状態を読みます。
        try
        {
            // 対象文書の種別を判定します。
            if (event.kind
                == LamaPon::Detail::LocalPersistenceResourceKind::PlayerPrefs)
            {
                // PlayerPrefsの所有者
                const auto& prefs = *static_cast<const LamaPon::PlayerPrefs*>(
                    event.source);
                probe.observedCommittedState =
                    Documents::ReadPlayerPrefs(prefs).state == State::Loaded;
            }
            // 通知処理の失敗を拒否します。
            else
            {
                // SaveDataの所有者
                const auto& saves = *static_cast<const LamaPon::SaveDataStore*>(
                    event.source);
                // 保存後のSaveData状態
                const auto state = Documents::ReadSaveData(
                    saves,
                    event.slot).state;
                probe.observedCommittedState = event.deleted
                    ? state == State::Missing
                    : state == State::Loaded;
            }
        }
        // 通知処理例外を拒否します。
        catch (...)
        {
            // 例外を通知失敗にします。
            return false;
        }
        // commit状態に基づく結果を返します。
        return probe.returnSuccess && probe.observedCommittedState;
    }

    // 欠落・正常・不正な保存状態を区別します。
    void TestReadStatesAndStrictSchema()
    {
        // 状態試験用ディレクトリ
        const auto directory = TestRoot() / L"states";
        // 試験用PlayerPrefs
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        // 試験用SaveData
        LamaPon::SaveDataStore saves(directory / L"Saves");
        Require(
            Documents::ReadPlayerPrefs(prefs).state == State::Missing
                && Documents::ReadSaveData(saves, "slot").state
                    == State::Missing
                && Documents::ListSaveData(saves).state == State::Missing,
            "Missing local persistence was not distinguished.");

        prefs.SetInteger("level", 3);
        prefs.Save();
        saves.SaveJson("slot", R"({"level":3})");
        // PlayerPrefs読込結果
        const auto prefsRead = Documents::ReadPlayerPrefs(prefs);
        // SaveData読込結果
        const auto saveRead = Documents::ReadSaveData(saves, "slot");
        // SaveData一覧結果
        const auto listing = Documents::ListSaveData(saves);
        Require(
            prefsRead.state == State::Loaded
                && saveRead.state == State::Loaded
                && AsText(prefsRead.bytes).find("LamaPonPlayerPrefs")
                    != std::string::npos
                && AsText(saveRead.bytes).find("LamaPonSaveData")
                    != std::string::npos
                && listing.state == State::Loaded
                && listing.slots == std::vector<std::string>{ "slot" },
            "Valid full local documents were not returned.");

        // ファイル占有用のパス
        const auto unavailablePath = directory / L"not-a-directory";
        WriteText(unavailablePath, "file");
        // ファイルを親に持つSaveData
        LamaPon::SaveDataStore unavailable(unavailablePath);
        Require(
            Documents::ListSaveData(unavailable).state == State::Unavailable,
            "A non-directory SaveData root was treated as missing.");

        // 上限超過slot試験
        LamaPon::SaveDataStore tooMany(directory / L"TooManySaves");
        // slot番号
        for (std::size_t index = 0u;
             index <= LamaPon::CloudSaveMaxSlots;
             ++index)
        {
            tooMany.SaveJson("slot-" + std::to_string(index), "{}");
        }
        Require(
            Documents::ListSaveData(tooMany).state == State::Corrupt,
            "A 33rd cloud save slot was accepted by strict enumeration.");

        // 排他読込用ファイル
        const auto file = CreateFileW(
            prefs.FilePath().c_str(),
            GENERIC_READ,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Sharing fixture could not open.");
        // 共有拒否時の読込状態
        const auto unavailableRead = Documents::ReadPlayerPrefs(prefs).state;
        CloseHandle(file);
        Require(
            unavailableRead == State::Unavailable,
            "Sharing denial was treated as missing or corrupt.");

        WriteText(prefs.FilePath(), "{truncated");
        Require(
            Documents::ReadPlayerPrefs(prefs).state == State::Corrupt,
            "Malformed PlayerPrefs was not classified as corrupt.");
    }

    // 永続化とwriter競合のbarrierを検証します。
    void TestDurableBarriersAndConcurrentWriterContract()
    {
        // PlayerPrefs保存先
        const auto path = TestRoot() / L"durability" / L"PlayerPrefs.json";
        // 先行writer
        LamaPon::PlayerPrefs first(path);
        first.SetString("writer", "baseline");
        first.Save();
        // 保存前の文書
        const auto baseline = ReadText(path);
        Require(
            LamaPon::Detail::IsLocalPersistenceLockExclusiveForTesting(path),
            "Two production local persistence locks were acquired.");

        first.SetString("writer", "P1");
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::BeforeFlush);
        // Flush前の失敗で既存文書を保ちます。
        Require(
            Throws([&] { first.Save(); })
                && first.IsDirty()
                && ReadText(path) == baseline
                && std::filesystem::exists(Suffix(path, L".writing")),
            "A pre-flush failure changed the published PlayerPrefs.");

        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::
                AfterFlushBeforePublish);
        // 公開直前の失敗で既存文書を保ちます。
        Require(
            Throws([&] { first.Save(); })
                && first.IsDirty()
                && ReadText(path) == baseline,
            "A pre-publish failure changed the published PlayerPrefs.");

        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::
                BeforePublishSharingViolation);
        first.Save();
        // 1回だけの一時共有違反後も、完全な文書だけが公開されます。
        const auto retried = Documents::ReadPlayerPrefs(first);
        Require(
            !first.IsDirty()
                && first.GetString("writer") == "P1"
                && retried.state == State::Loaded
                && AsText(retried.bytes).find("P1") != std::string::npos,
            "A transient publish sharing violation was not retried safely.");

        // 後続writer
        LamaPon::PlayerPrefs second(path);
        second.Load();
        second.SetString("writer", "P2");
        second.Save();
        // 永続化済みの読込結果
        const auto durable = Documents::ReadPlayerPrefs(second);
        Require(
            durable.state == State::Loaded
                && second.GetString("writer") == "P2"
                && AsText(durable.bytes).find("P2") != std::string::npos,
            "A successful later writer did not return with durable P2 bytes.");

        // 保存barrier用SaveData
        LamaPon::SaveDataStore saves(TestRoot() / L"save-barrier" / L"Saves");
        saves.SaveJson("slot", R"({"writer":"old"})");
        // slot文書のパス
        const auto savePath = saves.SlotPath("slot");
        // 変更前のslot文書
        const auto oldSave = ReadText(savePath);
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::
                AfterFlushBeforePublish);
        // SaveData公開失敗で既存slotを保ちます。
        Require(
            Throws([&]
            {
                saves.SaveJson("slot", R"({"writer":"new"})");
            })
                && ReadText(savePath) == oldSave,
            "A failed SaveData publish changed the existing document.");
        saves.SaveJson("slot", R"({"writer":"new"})");
        Require(
            Documents::ReadSaveData(saves, "slot").state == State::Loaded
                && saves.LoadJson("slot")
                    == std::optional<std::string>(R"({"writer":"new"})"),
            "SaveData did not return after its full document became durable.");
    }

    // 不正文書の安全な破棄を検証します。
    void TestCorruptDocumentIdentitySupportsSafeDiscard()
    {
        // 復旧試験ディレクトリ
        const auto directory = TestRoot() / L"corrupt-identity";
        std::filesystem::create_directories(directory);
        // 復旧対象文書
        const auto path = directory / L"Recovery.prefs";
        // 復旧対象PlayerPrefs
        LamaPon::PlayerPrefs sidecar(path);

        WriteText(path, {});
        // 空文書の読込結果
        const auto empty = Documents::ReadPlayerPrefs(sidecar);
        Require(
            empty.state == State::Corrupt
                && empty.bytes.empty()
                && empty.identity.valid
                && empty.identity.completeBytes
                && empty.identity.byteLength == 0u,
            "An empty corrupt document was not recorded as complete bytes.");
        Require(
            LamaPon::Detail::DurableDeleteLocalDocumentIfUnchanged(
                path,
                empty,
                LamaPon::CloudPreferencesMaxBytes)
                    == LamaPon::Detail::
                        LocalPersistenceConditionalApplyResult::Applied
                && !std::filesystem::exists(path),
            "An unchanged empty corrupt document could not be discarded.");

        // 上限超過の文書本文
        const std::string oversized(
            LamaPon::CloudPreferencesMaxBytes + 1u,
            'a');
        WriteText(path, oversized);
        // 巨大文書の読込結果
        const auto large = Documents::ReadPlayerPrefs(sidecar);
        Require(
            large.state == State::Corrupt
                && large.bytes.empty()
                && large.identity.valid
                && !large.identity.completeBytes
                && large.identity.byteLength == oversized.size(),
            "An oversized document was fully read or lacked a bounded identity.");
        Require(
            LamaPon::Detail::DurableDeleteLocalDocumentIfUnchanged(
                path,
                large,
                LamaPon::CloudPreferencesMaxBytes)
                    == LamaPon::Detail::
                        LocalPersistenceConditionalApplyResult::Applied
                && !std::filesystem::exists(path),
            "An unchanged oversized corrupt document could not be discarded.");

        WriteText(path, oversized);
        // 置換前文書の識別情報
        const auto beforeReplacement = Documents::ReadPlayerPrefs(sidecar);
        // 置換用文書のパス
        const auto replacementPath = directory / L"replacement";
        WriteText(
            replacementPath,
            std::string(oversized.size(), 'b'));
        Require(
            MoveFileExW(
                replacementPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
            "Oversized replacement fixture could not be published.");
        Require(
            LamaPon::Detail::DurableDeleteLocalDocumentIfUnchanged(
                path,
                beforeReplacement,
                LamaPon::CloudPreferencesMaxBytes)
                    == LamaPon::Detail::
                        LocalPersistenceConditionalApplyResult::LocalChanged
                && std::filesystem::exists(path)
                && std::filesystem::file_size(path) == oversized.size(),
            "A replaced oversized document was deleted by stale identity.");

        RemoveExact(path);
        WriteText(path, RemotePreferences);
        // 置換後の読込結果
        const auto observed = Documents::ReadPlayerPrefs(sidecar);
        // 共有拒否用handle
        const auto blocker = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(
            blocker != INVALID_HANDLE_VALUE,
            "Conditional delete sharing fixture could not be opened.");
        // 削除拒否の結果を検証します。
        const bool rejected = Throws([&]
        {
            (void)LamaPon::Detail::
                DurableDeleteLocalDocumentIfUnchanged(
                    path,
                    observed,
                    LamaPon::CloudPreferencesMaxBytes);
        });
        CloseHandle(blocker);
        // 共有中の文書削除を拒否します。
        Require(
            rejected && std::filesystem::exists(path),
            "Conditional delete did not retain its DELETE-capable exact handle contract.");
    }

    // 不正なremote文書と適用時の原子性を検証します。
    void TestRemoteApplyStrongGuaranteeAndIdentity()
    {
        // remote apply試験ディレクトリ
        const auto directory = TestRoot() / L"remote";
        // 適用対象PlayerPrefs
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        // 対象の同一性
        auto* const address = &prefs;
        prefs.SetInteger("local", 4);
        prefs.Save();
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));
        Require(
            &prefs == address
                && prefs.GetInteger("remote") == 9
                && !prefs.IsDirty()
                && ReadText(prefs.FilePath()) == RemotePreferences,
            "Remote PlayerPrefs apply changed identity or reserialized bytes.");

        // int64上限値を含む設定文書
        constexpr std::string_view MaximumIntegerPreferences =
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9223372036854775807}}})";
        Documents::ApplyPlayerPrefs(prefs, Bytes(MaximumIntegerPreferences));
        Require(
            prefs.GetInteger("remote")
                == (std::numeric_limits<std::int64_t>::max)(),
            "INT64_MAX PlayerPrefs did not round-trip exactly.");
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));

        // 不正文書
        for (const auto invalid : {
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","format":"duplicate","version":1,"values":{}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":2,"values":{}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"bad":{"type":"integer","value":"x"}}})"),
                std::string(
                    R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":9223372036854775808}}})") })
        {
            // remote適用前の文書
            const auto before = ReadText(prefs.FilePath());
            // 不正remote設定でメモリと文書を保ちます。
            Require(
                Throws([&]
                {
                    Documents::ApplyPlayerPrefs(prefs, Bytes(invalid));
                })
                    && ReadText(prefs.FilePath()) == before
                    && prefs.GetInteger("remote") == 9,
                "Invalid remote PlayerPrefs mutated disk or memory.");
        }
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::BeforeFlush);
        // remote公開失敗でローカル状態を保ちます。
        Require(
            Throws([&]
            {
                Documents::ApplyPlayerPrefs(
                    prefs,
                    Bytes(RemotePreferencesTwo));
            })
                && prefs.GetInteger("remote") == 9
                && ReadText(prefs.FilePath()) == RemotePreferences,
            "Failed remote PlayerPrefs publish mutated disk or memory.");

        // 適用対象SaveData
        LamaPon::SaveDataStore saves(directory / L"Saves");
        saves.SaveJson("remote", R"({"level":1})");
        Documents::ApplySaveData(saves, "remote", Bytes(RemoteSave));
        Require(
            ReadText(saves.SlotPath("remote")) == RemoteSave
                && saves.LoadJson("remote")
                    == std::optional<std::string>(R"({"level":8})"),
            "Remote SaveData was wrapped twice or changed.");
        // 不正保存文書
        for (const auto invalid : {
                std::string(
                    R"({"format":"LamaPonSaveData","version":1,"slot":"wrong","data":{}})"),
                std::string(
                    R"({"format":"LamaPonSaveData","version":1,"slot":"remote","slot":"remote","data":{}})"),
                std::string(
                    R"({"format":"LamaPonSaveData","version":2,"slot":"remote","data":{}})" ) })
        {
            // remote適用前のslot文書
            const auto before = ReadText(saves.SlotPath("remote"));
            // 不正remote保存で既存slotを保ちます。
            Require(
                Throws([&]
                {
                    Documents::ApplySaveData(saves, "remote", Bytes(invalid));
                })
                    && ReadText(saves.SlotPath("remote")) == before,
                "Invalid remote SaveData mutated the existing slot.");
        }

        Documents::DeleteSaveData(saves, "remote");
        Require(
            Documents::ReadSaveData(saves, "remote").state == State::Missing,
            "Remote SaveData delete did not publish missing state.");
        WriteText(Suffix(saves.SlotPath("remote"), L".deleting"), "stale");
        Require(
            Documents::ReadSaveData(saves, "remote").state == State::Missing
                && Documents::ListSaveData(saves).slots.empty(),
            "A stale .deleting file was adopted as local data.");

        Documents::DeletePlayerPrefs(prefs);
        Require(
            &prefs == address
                && prefs.Keys().empty()
                && !prefs.IsDirty()
                && Documents::ReadPlayerPrefs(prefs).state == State::Missing,
            "Remote PlayerPrefs delete did not atomically clear disk and memory.");
    }

    // commit通知の順序と所有権を検証します。
    void TestObserverOrderingAndOwnership()
    {
        // 通知試験ディレクトリ
        const auto directory = TestRoot() / L"observer";
        // 通知対象PlayerPrefs
        LamaPon::PlayerPrefs prefs(directory / L"PlayerPrefs.json");
        // PlayerPrefs通知記録
        ObserverProbe probe;
        probe.expectedSource = &prefs;
        // 先行通知token
        const auto firstToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &probe,
                41u);
        prefs.SetInteger("value", 1);
        prefs.Save();
        Require(
            firstToken != 0u
                && probe.calls == 1u
                && probe.epoch == 41u
                && probe.observedCommittedState,
            "PlayerPrefs observer ran before durable commit.");

        // 置換後通知記録
        ObserverProbe replacement;
        replacement.expectedSource = &prefs;
        replacement.returnSuccess = false;
        // 後続通知token
        const auto secondToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &replacement,
                42u);
        Require(
            !LamaPon::Detail::DetachLocalPersistenceCommitObserver(firstToken),
            "A stale observer owner detached its replacement.");
        prefs.SetInteger("value", 2);
        prefs.Save();
        Require(
            replacement.calls == 1u
                && LamaPon::Detail::ConsumeLocalPersistenceObserverFailure(),
            "Observer failure changed local success or was not recorded.");

        {
            // observer抑止スコープ
            LamaPon::Detail::ScopedLocalPersistenceObserverSuppression suppress;
            prefs.SetInteger("value", 3);
            prefs.Save();
        }
        Documents::ApplyPlayerPrefs(prefs, Bytes(RemotePreferences));
        Require(
            replacement.calls == 1u
                && LamaPon::Detail::DetachLocalPersistenceCommitObserver(
                    secondToken),
            "Suppressed/remote commits emitted a local observer event.");

        // 通知対象SaveData
        LamaPon::SaveDataStore saves(directory / L"Saves");
        // SaveData通知記録
        ObserverProbe saveProbe;
        saveProbe.expectedSource = &saves;
        // SaveData通知token
        const auto saveToken =
            LamaPon::Detail::AttachLocalPersistenceCommitObserver(
                &ObserveCommit,
                &saveProbe,
                43u);
        saves.SaveJson("slot", "{}");
        Require(
            saveProbe.calls == 1u && saveProbe.observedCommittedState,
            "SaveData observer ran before durable commit.");
        Require(saves.DeleteSlot("slot"), "SaveData delete fixture failed.");
        Require(
            saveProbe.calls == 2u && saveProbe.observedCommittedState
                && LamaPon::Detail::DetachLocalPersistenceCommitObserver(
                    saveToken),
            "SaveData delete observer did not see committed missing state.");
    }

    // 新しいローカル保存へのremote上書きを拒否します。
    void TestConditionalRemoteApplyRejectsNewerLocalCommit()
    {
        // 条件付き適用試験ディレクトリ
        const auto directory = TestRoot() / L"conditional-apply";
        // 条件付き適用先
        const auto path = directory / L"PlayerPrefs.json";
        // 適用元PlayerPrefs
        LamaPon::PlayerPrefs active(path);
        active.SetString("writer", "P1");
        active.Save();
        // remote読込時の識別情報
        const auto observed = Documents::ReadPlayerPrefs(active);

        // 競合writer
        LamaPon::PlayerPrefs peer(path);
        peer.Load();
        peer.SetString("writer", "P2");
        // P2を確定し、remote上書きの拒否を検証します。
        peer.Save();
        // P2確定後の文書
        const auto p2Bytes = ReadText(path);

        // 条件付き適用結果
        const auto result = Documents::ApplyPlayerPrefsIfUnchanged(
            active,
            observed,
            Bytes(RemotePreferences));
        Require(
            result
                    == LamaPon::Detail::
                        LocalPersistenceConditionalApplyResult::LocalChanged
                && ReadText(path) == p2Bytes
                && peer.GetString("writer") == "P2"
                && active.GetString("writer") == "P1",
            "Conditional remote apply overwrote a newer local commit.");

        // 最新ローカル文書の状態
        const auto current = Documents::ReadPlayerPrefs(peer);
        // 条件付き削除結果
        const auto deleteResult = Documents::DeletePlayerPrefsIfUnchanged(
            peer,
            current);
        Require(
            deleteResult
                    == LamaPon::Detail::
                        LocalPersistenceConditionalApplyResult::Applied
                && Documents::ReadPlayerPrefs(peer).state == State::Missing
                && peer.Keys().empty(),
            "Conditional PlayerPrefs delete did not atomically update memory.");
    }

    // hard link拒否を検証します。
    void TestHardLinksFailBeforeMutation()
    {
        // hard link試験ディレクトリ
        const auto directory = TestRoot() / L"hard-links";
        // 適用対象文書
        const auto path = directory / L"PlayerPrefs.json";
        // hard link対象
        LamaPon::PlayerPrefs prefs(path);
        prefs.SetString("value", "old");
        prefs.Save();

        // target別名パス
        const auto targetAlias = directory / L"target-alias";
        Require(
            CreateHardLinkW(targetAlias.c_str(), path.c_str(), nullptr) != FALSE,
            "Target hard-link fixture could not be created.");
        prefs.SetString("value", "new");
        // 変更前の文書
        const auto before = ReadText(path);
        // hard link先を変更せず保存を拒否します。
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(targetAlias) == before,
            "A hard-linked final document was accepted or changed.");
        RemoveExact(targetAlias);

        // stage sidecarパス
        const auto stagePath = Suffix(path, L".writing");
        RemoveExact(stagePath);
        // stage側の保護対象
        const auto stageVictim = TestRoot() / L"stage-victim";
        WriteText(stageVictim, "must-not-change");
        Require(
            CreateHardLinkW(
                stagePath.c_str(),
                stageVictim.c_str(),
                nullptr) != FALSE,
            "Stage hard-link fixture could not be created.");
        // stage hard linkの切詰めを拒否します。
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(stageVictim) == "must-not-change",
            "A hard-linked stage was truncated before validation.");
        RemoveExact(stagePath);
        RemoveExact(stageVictim);

        // lock sidecarパス
        const auto lockPath = Suffix(path, L".lock");
        RemoveExact(lockPath);
        // lock側の保護対象
        const auto lockVictim = TestRoot() / L"lock-victim";
        WriteText(lockVictim, "must-remain-data");
        Require(
            CreateHardLinkW(
                lockPath.c_str(),
                lockVictim.c_str(),
                nullptr) != FALSE,
            "Lock hard-link fixture could not be created.");
        // lock hard linkを変更せず保存を拒否します。
        Require(
            Throws([&] { prefs.Save(); })
                && ReadText(lockVictim) == "must-remain-data",
            "A hard-linked persistence lock was accepted or changed.");
        RemoveExact(lockPath);
        RemoveExact(lockVictim);

        // hard link試験SaveData
        LamaPon::SaveDataStore saves(directory / L"Saves");
        saves.SaveJson("slot", R"({"value":"old"})");
        // slot文書パス
        const auto savePath = saves.SlotPath("slot");
        // slot別名パス
        const auto saveAlias = directory / L"save-target-alias";
        Require(
            CreateHardLinkW(
                saveAlias.c_str(),
                savePath.c_str(),
                nullptr) != FALSE,
            "Delete target hard-link fixture could not be created.");
        // slotの変更前文書
        const auto saveBefore = ReadText(savePath);
        // hard link先を変更せず削除を拒否します。
        Require(
            Throws([&] { (void)saves.DeleteSlot("slot"); })
                && ReadText(saveAlias) == saveBefore,
            "A hard-linked delete target was removed or changed.");
        RemoveExact(saveAlias);

        // deleting sidecarパス
        const auto deletingPath = Suffix(savePath, L".deleting");
        // deleting側の保護対象
        const auto deleteVictim = TestRoot() / L"delete-victim";
        WriteText(deleteVictim, "must-not-be-deleted");
        Require(
            CreateHardLinkW(
                deletingPath.c_str(),
                deleteVictim.c_str(),
                nullptr) != FALSE,
            "Delete stage hard-link fixture could not be created.");
        // deleting hard linkの削除を拒否します。
        Require(
            Throws([&] { (void)saves.DeleteSlot("slot"); })
                && ReadText(savePath) == saveBefore
                && ReadText(deleteVictim) == "must-not-be-deleted",
            "An unsafe delete stage changed target or victim data.");
        RemoveExact(deletingPath);
        RemoveExact(deleteVictim);
    }

    // 祖先reparse経由の保存を拒否します。
    void TestAncestorReparseIsRejectedWhenSupported()
    {
        // reparse linkの参照先
        const auto outside = TestRoot() / L"reparse-target";
        // 外部PlayerPrefs
        LamaPon::PlayerPrefs outsidePrefs(outside / L"PlayerPrefs.json");
        outsidePrefs.SetString("secret", "outside");
        outsidePrefs.Save();
        // 外部SaveData
        LamaPon::SaveDataStore outsideSaves(outside / L"Saves");
        outsideSaves.SaveJson("slot", "{}");
        // reparse試験前の文書
        const auto before = ReadText(outsidePrefs.FilePath());

        // reparse linkパス
        const auto link = TestRoot() / L"reparse-link";
        // 非昇格link作成flag
        constexpr DWORD AllowUnprivilegedCreate = 0x2u;
        // linkを作成できる環境で祖先reparseの隔離を検証します。
        if (CreateSymbolicLinkW(
                link.c_str(),
                outside.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | AllowUnprivilegedCreate) == FALSE)
        {
            // Developer Mode/権限がないWindowsでも他の回帰は実行します。
            return;
        }
        // link経由のPlayerPrefs
        LamaPon::PlayerPrefs linkedPrefs(link / L"PlayerPrefs.json");
        linkedPrefs.SetString("secret", "overwrite");
        // link経由のSaveData
        LamaPon::SaveDataStore linkedSaves(link / L"Saves");
        // 祖先link経由の保存と削除を拒否します。
        Require(
            Documents::ReadPlayerPrefs(linkedPrefs).state == State::Unavailable
                && Documents::ListSaveData(linkedSaves).state
                    == State::Unavailable
                && Throws([&] { linkedPrefs.Save(); })
                && Throws([&]
                {
                    Documents::DeletePlayerPrefs(linkedPrefs);
                })
                && ReadText(outsidePrefs.FilePath()) == before,
            "An ancestor reparse point escaped local persistence isolation.");
        RemoveExact(link);
    }
}

// 保存文書試験を実行します。
int main()
{
    // テスト群の例外を捕捉します。
    try
    {
        ResetRoot();
        // run(name: テスト名, test: 実行処理)で失敗へテスト名を付けます。
        const auto run = [](const char* name, const auto test)
        {
            // 個別テストの例外を捕捉します。
            try
            {
                test();
            }
            // テスト名付き例外を捕捉します(exception: 失敗理由)。
            catch (const std::exception& exception)
            {
                // テスト名付きで例外を再送出します。
                throw std::runtime_error(
                    std::string(name) + ": " + exception.what());
            }
        };
        // strict read、durability、remote適用、observer、file保護を検証します。
        run("strict read", TestReadStatesAndStrictSchema);
        run("durability", TestDurableBarriersAndConcurrentWriterContract);
        run("corrupt identity", TestCorruptDocumentIdentitySupportsSafeDiscard);
        run("remote apply", TestRemoteApplyStrongGuaranteeAndIdentity);
        run("conditional apply", TestConditionalRemoteApplyRejectsNewerLocalCommit);
        run("observer", TestObserverOrderingAndOwnership);
        run("hard links", TestHardLinksFailBeforeMutation);
        run("ancestor reparse", TestAncestorReparseIsRejectedWhenSupported);
        LamaPon::Detail::SetLocalPersistenceTestFailPoint(
            LamaPon::Detail::LocalPersistenceTestFailPoint::None);
        // 最終後始末エラー
        std::error_code cleanupError;
        std::filesystem::remove_all(TestRoot(), cleanupError);
        Require(!cleanupError, "Local persistence final cleanup failed.");
        std::cout << "Local persistence document tests passed.\n";
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
