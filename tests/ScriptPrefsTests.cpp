#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Scripting/Script.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(
        const bool condition,
        const char* message)
    {
        // assertion失敗を例外で通知
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Save/LoadはOwner非依存で未アタッチでも利用可能
    class PrefsProbe final : public LamaPon::Script
    {
    public:
        using Script::CloudConflicts;
        using Script::CloudSyncStatus;
        using Script::DiscardPersistence;
        using Script::PersistenceRecoveryStatus;
        using Script::RequestCloudSync;
        using Script::ResolveCloudConflict;
        using Script::RestorePersistence;

        // 最高得点をPlayerPrefsへ保存します。
        // SaveBest(value: 保存する最高得点)
        void SaveBest(const std::int64_t value) const
        {
            SaveInteger("bestScore", value);
        }
        // 保存した最高得点を返します。
        // Best(fallback: キーがない場合の既定値)
        [[nodiscard]] std::int64_t Best(
            const std::int64_t fallback) const
        {
            return LoadInteger("bestScore", fallback);
        }
        // 最高得点キーの保存有無を返します。
        [[nodiscard]] bool HasBest() const
        {
            return HasSaved("bestScore");
        }
        // 最高得点キーを削除します。
        void ForgetBest() const
        {
            DeleteSaved("bestScore");
        }
        // プレイヤー名をPlayerPrefsへ保存します。
        // SaveName(value: 保存するプレイヤー名)
        void SaveName(std::string value) const
        {
            SaveText("playerName", std::move(value));
        }
        // 保存した名前、未保存なら既定名を返します。
        [[nodiscard]] std::string Name() const
        {
            return LoadText("playerName", "ななし");
        }
    };

    // fileへ保存し、別インスタンスでも読めることを確認します。
    // TestSavesThroughToDisk(file: PlayerPrefs保存先)
    void TestSavesThroughToDisk(
        const std::filesystem::path& file)
    {
        // テスト対象のPlayerPrefs
        LamaPon::PlayerPrefs prefs(file);
        LamaPon::SetActivePlayerPrefs(&prefs);

        // ScriptのPlayerPrefs APIを呼ぶprobe
        const PrefsProbe probe;
        Require(
            !probe.HasBest(),
            "a fresh file must not have the key yet");
        Require(
            probe.Best(-1) == -1,
            "missing keys must return the default");

        probe.SaveBest(1234);
        probe.SaveName("ゆうしゃ");
        Require(
            probe.Best(-1) == 1234,
            "the saved value must be readable back");
        Require(
            probe.HasBest(),
            "HasSaved must see the saved key");
        Require(
            probe.Name() == "ゆうしゃ",
            "text must round-trip (UTF-8)");

        // 別インスタンスでファイル保存結果を確認
        // 保存し忘れはこの読み直しで検出する
        LamaPon::PlayerPrefs reopened(file);
        reopened.Load();
        Require(
            reopened.GetInteger("bestScore", -1) == 1234,
            "the value must survive on disk");
        Require(
            reopened.GetString("playerName", {}) == "ゆうしゃ",
            "the text must survive on disk");

        probe.ForgetBest();
        Require(
            !probe.HasBest(),
            "DeleteSaved must remove the key");

        LamaPon::SetActivePlayerPrefs(nullptr);
    }

    // Application不在時もScriptの保存APIが安全に動くことを確認
    void TestWithoutApplicationIsHarmless()
    {
        LamaPon::SetActivePlayerPrefs(nullptr);
        LamaPon::SetActiveOnlineServices(nullptr);
        Require(
            LamaPon::ActivePlayerPrefs() == nullptr,
            "there must be no active prefs");

        // 未接続APIを検査するprobe
        const PrefsProbe probe;
        // 保存要求はactive PlayerPrefsがなければ無視
        probe.SaveBest(999);
        probe.ForgetBest();
        Require(
            probe.Best(42) == 42,
            "reads must fall back to the default");
        Require(
            !probe.HasBest(),
            "HasSaved must be false without prefs");
        Require(
            probe.Name() == "ななし",
            "text reads must fall back to the default");
        Require(
            probe.CloudSyncStatus().state
                    == LamaPon::OnlineCloudSyncState::Unavailable
                && probe.CloudConflicts().empty()
                && probe.PersistenceRecoveryStatus().state
                    == LamaPon::OnlinePersistenceRecoveryState::None,
            "cloud status must use safe defaults without an application");
        Require(
            probe.RequestCloudSync()
                    == LamaPon::OnlinePersistenceOperationResult::Unavailable
                && probe.ResolveCloudConflict(
                    "opaque-but-inactive",
                    LamaPon::OnlineCloudConflictResolution::UseRemote)
                    == LamaPon::OnlinePersistenceOperationResult::Unavailable
                && probe.RestorePersistence(1u)
                    == LamaPon::OnlinePersistenceOperationResult::Unavailable
                && probe.DiscardPersistence(1u)
                    == LamaPon::OnlinePersistenceOperationResult::Unavailable,
            "cloud operations must be harmless without an application");
    }
}

// ScriptのPlayerPrefs保存APIと未接続時の既定値を検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // テスト用PlayerPrefsファイルの親領域
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "script-prefs";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        TestSavesThroughToDisk(root / "PlayerPrefs.json");
        TestWithoutApplicationIsHarmless();
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& error)
    {
        std::cerr
            << "Script prefs tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "Script prefs tests passed.\n";
    return 0;
}
