// C++ Scriptから設定値を保存できること（ハイスコアなど）を確かめます。
//
// 何を守るテストか: 以前は`PlayerPrefs`がApplicationの持ち物で、
// Scriptから触る方法がありませんでした（ドキュメントにも「Scriptから
// 直接アクセスできません」と書いてあった）。そのため、ワンボタン
// ゲームでハイスコアを残すことすらできませんでした。
//
// ここでは Script の Save/Load 系が
//   1) 実際にファイルへ書けること（別インスタンスから読めること）
//   2) Applicationが無い環境（CLIなど）でも落ちず、既定値を返すこと
// を確認します。2はCLIやテストで必ず通る道なので重要です。
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Scripting/Script.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Save/Load系はOwnerを触らないので、アタッチせずに呼べます。
    class PrefsProbe final : public LamaPon::Script
    {
    public:
        void SaveBest(const std::int64_t value) const
        {
            SaveInteger("bestScore", value);
        }
        [[nodiscard]] std::int64_t Best(
            const std::int64_t fallback) const
        {
            return LoadInteger("bestScore", fallback);
        }
        [[nodiscard]] bool HasBest() const
        {
            return HasSaved("bestScore");
        }
        void ForgetBest() const
        {
            DeleteSaved("bestScore");
        }
        void SaveName(std::string value) const
        {
            SaveText("playerName", std::move(value));
        }
        [[nodiscard]] std::string Name() const
        {
            return LoadText("playerName", "ななし");
        }
    };

    void TestSavesThroughToDisk(
        const std::filesystem::path& file)
    {
        LamaPon::PlayerPrefs prefs(file);
        LamaPon::SetActivePlayerPrefs(&prefs);

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

        // 別インスタンスで読み直して、本当にファイルへ書けたかを見ます
        // （Save()を忘れているとここで落ちます）。
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

    // Applicationが無い環境（CLIのrenderなど）でも落ちないこと。
    void TestWithoutApplicationIsHarmless()
    {
        LamaPon::SetActivePlayerPrefs(nullptr);
        Require(
            LamaPon::ActivePlayerPrefs() == nullptr,
            "there must be no active prefs");

        const PrefsProbe probe;
        probe.SaveBest(999);          // 黙って無視されるだけ
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
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "script-prefs";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        TestSavesThroughToDisk(root / "PlayerPrefs.json");
        TestWithoutApplicationIsHarmless();
    }
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
