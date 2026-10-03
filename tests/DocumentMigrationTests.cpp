#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
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

    // documentをpathへ整形JSONで保存します。
    // WriteJson(path: 出力先, document: JSON文書)
    void WriteJson(
        const std::filesystem::path& path,
        const nlohmann::json& document)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // JSON文書を書き込むバイナリ出力
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << document.dump(2) << '\n';
        // JSON書き込み失敗を呼び出し元へ通知
        if (!output)
        {
            throw std::runtime_error(
                "Could not write migration fixture.");
        }
    }
}

// 旧形式のシーン・設定・セーブを移行します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // entitiesと旧asset名を含むv0シーン
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
        // legacySceneの移行結果
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
        // 移行シーンから抽出した資産パス
        const auto assetPaths =
            LamaPon::CollectSerializedAssetPaths(
                legacyScene);
        Require(
            assetPaths.size() == 2
                && legacyScene["assetManifest"].size()
                    == 2,
            "Scene asset manifest was not deduplicated.");

        // entitiesを持つv0 prefab
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

        // 移行データの出力ルート
        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "document-migration";
        // 出力削除時のエラー状態
        std::error_code error;
        std::filesystem::remove_all(
            outputRoot,
            error);

        // 旧形式プロジェクトの入力ファイル
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
        // 移行して読み込んだプロジェクト設定
        const auto project =
            LamaPon::LoadProjectSettings(projectPath);
        Require(
            project.gameName == "Legacy Game"
                && project.windowWidth == 960
                && project.windowHeight == 540,
            "Legacy project settings migration failed.");

        // 旧形式PlayerPrefsの入力ファイル
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
        // 移行後のPlayerPrefs
        LamaPon::PlayerPrefs preferences(
            preferencesPath);
        preferences.Load();
        Require(
            preferences.GetInteger("score") == 42
                && preferences.GetBoolean("music")
                && preferences.GetString("name")
                    == "Legacy",
            "Legacy PlayerPrefs migration failed.");

        // 旧形式SaveDataの格納先
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
        // 旧形式SaveDataの読込サービス
        LamaPon::SaveDataStore saves(saveDirectory);
        // 移行後のセーブJSON
        const auto save = saves.LoadJson("legacy");
        Require(
            save.has_value()
                && nlohmann::json::parse(*save)
                    .at("level") == 7,
            "Legacy save-data migration failed.");

        // 将来version拒否の確認結果
        bool futureVersionRejected{};
        // 現行readerが対応しない将来version文書
        try
        {
            // 移行対象にしてはいけない将来version
            nlohmann::json future{
                { "format", "LamaPonScene" },
                { "version", 999 }
            };
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    future,
                    LamaPon::SerializedDocumentKind::Scene));
        }
        // 未対応versionエラーを拒否状態へ変換
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
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
