#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
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

    void WriteJson(
        const std::filesystem::path& path,
        const nlohmann::json& document)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << document.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Could not write migration fixture.");
        }
    }
}

int main()
{
    try
    {
        nlohmann::json legacyScene{
            { "format", "LamaPonScene" },
            { "version", 0 },
            {
                "entities",
                nlohmann::json::array({
                    {
                        { "id", 1 },
                        {
                            "components",
                            nlohmann::json::array({
                                {
                                    { "type", "SpriteRenderer" },
                                    { "texture", "textures/hero.png" }
                                },
                                {
                                    { "type", "AudioSource" },
                                    { "audio", "audio/theme.ogg" }
                                },
                                {
                                    { "type", "Duplicate" },
                                    { "texture", "textures/HERO.png" }
                                }
                            })
                        }
                    }
                })
            }
        };
        const auto sceneMigration =
            LamaPon::MigrateSerializedDocument(
                legacyScene,
                LamaPon::SerializedDocumentKind::Scene);
        Require(
            sceneMigration.changed
                && sceneMigration.sourceVersion == 0
                && sceneMigration.targetVersion == 1
                && legacyScene.contains("objects")
                && !legacyScene.contains("entities"),
            "Legacy scene migration failed.");

        LamaPon::RefreshSerializedAssetManifest(
            legacyScene);
        const auto assetPaths =
            LamaPon::CollectSerializedAssetPaths(
                legacyScene);
        Require(
            assetPaths.size() == 2
                && legacyScene["assetManifest"].size()
                    == 2,
            "Scene asset manifest was not deduplicated.");

        nlohmann::json legacyPrefab{
            { "format", "LamaPonPrefab" },
            { "version", 0 },
            { "root", 1 },
            { "entities", nlohmann::json::array() }
        };
        static_cast<void>(
            LamaPon::MigrateSerializedDocument(
                legacyPrefab,
                LamaPon::SerializedDocumentKind::Prefab));
        Require(
            legacyPrefab.contains("objects")
                && legacyPrefab["version"] == 1,
            "Legacy prefab migration failed.");

        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "document-migration";
        std::error_code error;
        std::filesystem::remove_all(
            outputRoot,
            error);

        const auto projectPath =
            outputRoot / "project.json";
        WriteJson(
            projectPath,
            {
                { "format", "LamaPonProject" },
                { "version", 0 },
                { "title", "Legacy Game" },
                { "windowWidth", 960 },
                { "windowHeight", 540 },
                {
                    "startupScene",
                    "scenes/legacy.scene.json"
                }
            });
        const auto project =
            LamaPon::LoadProjectSettings(projectPath);
        Require(
            project.gameName == "Legacy Game"
                && project.windowWidth == 960
                && project.windowHeight == 540,
            "Legacy project settings migration failed.");

        const auto preferencesPath =
            outputRoot / "PlayerPrefs.json";
        WriteJson(
            preferencesPath,
            {
                { "format", "LamaPonPlayerPrefs" },
                { "version", 0 },
                {
                    "values",
                    {
                        { "score", 42 },
                        { "music", true },
                        { "name", "Legacy" }
                    }
                }
            });
        LamaPon::PlayerPrefs preferences(
            preferencesPath);
        preferences.Load();
        Require(
            preferences.GetInteger("score") == 42
                && preferences.GetBoolean("music")
                && preferences.GetString("name")
                    == "Legacy",
            "Legacy PlayerPrefs migration failed.");

        const auto saveDirectory =
            outputRoot / "Saves";
        WriteJson(
            saveDirectory / "legacy.save.json",
            {
                { "format", "LamaPonSaveData" },
                { "version", 0 },
                { "slot", "legacy" },
                {
                    "payload",
                    {
                        { "level", 7 }
                    }
                }
            });
        LamaPon::SaveDataStore saves(saveDirectory);
        const auto save = saves.LoadJson("legacy");
        Require(
            save.has_value()
                && nlohmann::json::parse(*save)
                    .at("level") == 7,
            "Legacy save-data migration failed.");

        bool futureVersionRejected{};
        try
        {
            nlohmann::json future{
                { "format", "LamaPonScene" },
                { "version", 999 }
            };
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    future,
                    LamaPon::SerializedDocumentKind::Scene));
        }
        catch (const std::runtime_error&)
        {
            futureVersionRejected = true;
        }
        Require(
            futureVersionRejected,
            "A future document version was accepted.");

        std::filesystem::remove_all(
            outputRoot,
            error);
        std::cout
            << "Document migration tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
