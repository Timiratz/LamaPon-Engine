#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

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
}

// PlayerPrefsとSaveDataの永続化契約を検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // PlayerPrefsとSaveDataのテスト領域
        const auto directory =
            std::filesystem::current_path()
            / "test-output"
            / "persistence";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);

        // PlayerPrefsの保存先
        const auto preferencesPath =
            directory / "PlayerPrefs.json";
        // 初期値を読み込む設定ストア
        LamaPon::PlayerPrefs preferences(
            preferencesPath);
        preferences.Load();
        Require(
            preferences.Keys().empty()
                && !preferences.IsDirty(),
            "Missing PlayerPrefs did not load as empty.");

        preferences.SetInteger("highScore", 4200);
        preferences.SetNumber("volume", 0.75);
        preferences.SetBoolean("subtitles", true);
        preferences.SetString(
            "playerName",
            "トライデント");
        Require(
            preferences.IsDirty()
                && preferences.GetInteger(
                    "highScore") == 4200
                && std::abs(
                    preferences.GetNumber("volume")
                    - 0.75) < 0.0001
                && preferences.GetBoolean("subtitles")
                && preferences.GetString("playerName")
                    == "トライデント",
            "PlayerPrefs values were not retained.");
        Require(
            preferences.GetString(
                "highScore",
                "type mismatch")
                == "type mismatch",
            "PlayerPrefs type mismatch did not return the default.");

        preferences.Save();
        Require(
            std::filesystem::is_regular_file(
                preferencesPath)
                && !std::filesystem::exists(
                    preferencesPath.string()
                    + ".tmp")
                && !preferences.IsDirty(),
            "PlayerPrefs atomic save failed.");

        // 保存内容を読み直す設定ストア
        LamaPon::PlayerPrefs restored(
            preferencesPath);
        restored.Load();
        Require(
            restored.GetInteger("highScore") == 4200
                && restored.GetString("playerName")
                    == "トライデント"
                && restored.TypeOf("volume")
                    == LamaPon::PlayerPrefType::Number,
            "PlayerPrefs reload failed.");
        restored.DeleteKey("volume");
        Require(
            !restored.HasKey("volume")
                && restored.IsDirty(),
            "PlayerPrefs key deletion failed.");

        // 非有限数の拒否状態
        bool invalidNumberRejected{};
        // NaNを拒否する設定更新
        try
        {
            restored.SetNumber(
                "invalid",
                std::numeric_limits<double>::
                    quiet_NaN());
        }
        // 非有限値エラーを拒否状態へ変換
        catch (const std::invalid_argument&)
        {
            invalidNumberRejected = true;
        }
        Require(
            invalidNumberRejected,
            "PlayerPrefs accepted a non-finite number.");

        // 空キーの拒否状態
        bool invalidKeyRejected{};
        // 空キーを拒否する設定更新
        try
        {
            restored.SetInteger("", 1);
        }
        // 空キーエラーを拒否状態へ変換
        catch (const std::invalid_argument&)
        {
            invalidKeyRejected = true;
        }
        Require(
            invalidKeyRejected,
            "PlayerPrefs accepted an empty key.");

        // セーブスロットの保存領域
        LamaPon::SaveDataStore saves(
            directory / "Saves");
        saves.SaveJson(
            "スロット1",
            R"({"level":3,"position":[1,2,3],"name":"勇者"})");
        Require(
            saves.HasSlot("スロット1")
                && saves.ListSlots().size() == 1
                && saves.ListSlots().front()
                    == "スロット1",
            "Japanese save slot was not listed.");
        // 読み込んだセーブJSON文字列
        const auto saveJson =
            saves.LoadJson("スロット1");
        Require(
            saveJson.has_value(),
            "Save slot could not be loaded.");
        // JSONから解析したセーブ内容
        const auto payload =
            nlohmann::json::parse(*saveJson);
        Require(
            payload["level"] == 3
                && payload["name"] == "勇者",
            "Save slot payload changed.");

        // パストラバーサル拒否状態
        bool traversalRejected{};
        // プロジェクト外への保存を拒否
        try
        {
            saves.SaveJson(
                "../escape",
                "{}");
        }
        // 不正パスエラーを拒否状態へ変換
        catch (const std::invalid_argument&)
        {
            traversalRejected = true;
        }
        Require(
            traversalRejected,
            "SaveData accepted path traversal.");

        // 不正JSONの拒否状態
        bool invalidJsonRejected{};
        // JSON構文エラーの保存を拒否
        try
        {
            saves.SaveJson(
                "broken",
                "{not json}");
        }
        // JSON構文エラーを拒否状態へ変換
        catch (const nlohmann::json::exception&)
        {
            invalidJsonRejected = true;
        }
        Require(
            invalidJsonRejected
                && !saves.HasSlot("broken"),
            "SaveData accepted invalid JSON.");
        Require(
            saves.DeleteSlot("スロット1")
                && !saves.HasSlot("スロット1")
                && saves.ListSlots().empty(),
            "Save slot deletion failed.");

        // 無効文字を除去したユーザーデータ先
        const auto userDirectory =
            LamaPon::UserDataDirectory(
                "Invalid:/Game*Name");
        Require(
            userDirectory.filename()
                == L"Invalid__Game_Name",
            "User data directory was not sanitized.");

        std::filesystem::remove_all(directory);
        std::cout
            << "PlayerPrefs and SaveData tests passed.\n";
        return 0;
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
    // 想定外のテスト例外を失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
