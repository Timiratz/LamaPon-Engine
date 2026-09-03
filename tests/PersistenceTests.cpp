#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

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
}

int main()
{
    try
    {
        const auto directory =
            std::filesystem::current_path()
            / "test-output"
            / "persistence";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);

        const auto preferencesPath =
            directory / "PlayerPrefs.json";
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

        bool invalidNumberRejected{};
        try
        {
            restored.SetNumber(
                "invalid",
                std::numeric_limits<double>::
                    quiet_NaN());
        }
        catch (const std::invalid_argument&)
        {
            invalidNumberRejected = true;
        }
        Require(
            invalidNumberRejected,
            "PlayerPrefs accepted a non-finite number.");

        bool invalidKeyRejected{};
        try
        {
            restored.SetInteger("", 1);
        }
        catch (const std::invalid_argument&)
        {
            invalidKeyRejected = true;
        }
        Require(
            invalidKeyRejected,
            "PlayerPrefs accepted an empty key.");

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
        const auto saveJson =
            saves.LoadJson("スロット1");
        Require(
            saveJson.has_value(),
            "Save slot could not be loaded.");
        const auto payload =
            nlohmann::json::parse(*saveJson);
        Require(
            payload["level"] == 3
                && payload["name"] == "勇者",
            "Save slot payload changed.");

        bool traversalRejected{};
        try
        {
            saves.SaveJson(
                "../escape",
                "{}");
        }
        catch (const std::invalid_argument&)
        {
            traversalRejected = true;
        }
        Require(
            traversalRejected,
            "SaveData accepted path traversal.");

        bool invalidJsonRejected{};
        try
        {
            saves.SaveJson(
                "broken",
                "{not json}");
        }
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
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
