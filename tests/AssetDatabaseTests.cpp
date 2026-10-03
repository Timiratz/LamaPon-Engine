#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

    // contentsをpathへ書き込むテスト資産を作ります。
    // WriteFile(path: 出力先, contents: ファイル内容)
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // contentsを書き込むバイナリ出力
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        // 資産fixtureの作成失敗を通知
        if (!output)
        {
            throw std::runtime_error(
                "Could not create test asset.");
        }
        output << contents;
    }
}

// asset GUID・参照解決・再マップ・破損ファイルを検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // AssetDatabase用テストroot
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "asset-database";
        std::filesystem::remove_all(root);

        // 元のtexture asset
        const auto originalAsset =
            root / "textures" / "sample.bin";
        // rename後のtexture asset
        const auto renamedAsset =
            root / "textures" / "renamed.bin";
        // texture参照を持つscene fixture
        const auto scenePath =
            root / "scenes" / "sample.scene.json";
        WriteFile(originalAsset, "asset");
        WriteFile(
            scenePath,
            R"({"format":"Test","texture":"textures/sample.bin"})");

        // GUIDと依存を追跡するasset database
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(root);
        // 初回走査結果
        const auto first = database.Refresh(true);
        Require(
            first.assetCount == 2
                && first.createdMetaCount == 2
                && first.dependencyCount == 1,
            "Initial asset database scan failed.");

        // GUIDが維持される元asset記録
        const auto* original =
            database.FindByPath(
                "textures/sample.bin");
        // textureへ依存するscene記録
        const auto* scene =
            database.FindByPath(
                "scenes/sample.scene.json");
        Require(
            original != nullptr
                && scene != nullptr
                && LamaPon::AssetDatabase::IsValidGuid(
                    original->guid)
                && scene->dependencies.size() == 1
                && scene->dependencies[0]
                    == original->guid
                && original->dependents.size() == 1
                && original->dependents[0]
                    == scene->guid,
            "Dependency graph was not built.");
        // rename前のtexture GUID
        const std::string originalGuid =
            original->guid;

        std::filesystem::rename(
            originalAsset,
            renamedAsset);
        std::filesystem::rename(
            LamaPon::AssetDatabase::MetaPathFor(
                originalAsset),
            LamaPon::AssetDatabase::MetaPathFor(
                renamedAsset));
        // move後のdatabase走査結果
        const auto afterMove =
            database.Refresh(true);
        // 新pathで解決されるasset記録
        const auto* renamed =
            database.FindByPath(
                "textures/renamed.bin");
        Require(
            afterMove.createdMetaCount == 0
                && renamed != nullptr
                && renamed->guid == originalGuid
                && database.ResolveGuid(originalGuid)
                    == std::filesystem::path(
                        "textures/renamed.bin"),
            "GUID was not preserved after moving the meta file.");

        // asset GUIDで解決するAnimatorController
        auto controller =
            LamaPon::AnimatorController::FromJson(
                nlohmann::json{
                    {
                        "format",
                        "LamaPonAnimatorController"
                    },
                    { "version", 1 },
                    { "entry", "Idle" },
                    {
                        "states",
                        nlohmann::json::array({
                            {
                                { "name", "Idle" },
                                {
                                    "clip",
                                    "textures/sample.bin"
                                },
                                {
                                    "clipGuid",
                                    originalGuid
                                },
                                { "speed", 1.0f },
                                { "loop", true }
                            }
                        })
                    },
                    {
                        "transitions",
                        nlohmann::json::array()
                    }
                }.dump());
        controller.ResolveAssetReferences(database);
        Require(
            controller.States().at(0).clipPath
                == std::filesystem::path(
                    "textures/renamed.bin"),
            "Animator GUID reference did not resolve the moved asset.");

        // GUID参照のテストmaterial path
        const auto materialPath =
            root / "test.material.json";
        WriteFile(
            materialPath,
            nlohmann::json{
                {
                    "type",
                    "LamaPonLitMaterial"
                },
                { "version", 1 },
                {
                    "albedoTexture",
                    "textures/sample.bin"
                },
                {
                    "albedoTextureGuid",
                    originalGuid
                },
                { "normalTexture", "" },
                { "shader", "textures/sample.bin" },
                { "shaderGuid", originalGuid }
            }.dump());
        // databaseのGUID解決を適用したmaterial
        const auto material =
            LamaPon::LoadLitMaterialAsset(
                materialPath,
                &database);
        Require(
            material.AlbedoTexture()
                == std::filesystem::path(
                    "textures/renamed.bin"),
            "Material GUID reference did not resolve the moved asset.");
        Require(
            material.Shader()
                == std::filesystem::path(
                    "textures/renamed.bin"),
            "Material shader GUID reference did not resolve the moved asset.");

        // scene JSONへ適用したasset path置換結果
        const auto remap =
            database.RemapJsonReferences(
                "textures/sample.bin",
                "textures/renamed.bin");
        Require(
            remap.fileCount == 1
                && remap.referenceCount == 1,
            "JSON asset reference was not remapped.");
        // remap後scene JSONの読み込み先
        nlohmann::json remappedScene;
        {
            // 更新されたscene fixture
            std::ifstream input(
                scenePath,
                std::ios::binary);
            input >> remappedScene;
        }
        Require(
            remappedScene.at("texture")
                    .get<std::string>()
                == "textures/renamed.bin",
            "Remapped JSON contains the old path.");
        // remap後のdependency graph記録
        const auto* remappedRecord =
            database.FindByPath(
                "scenes/sample.scene.json");
        Require(
            remappedRecord != nullptr
                && remappedRecord->dependencies.size()
                    == 1
                && remappedRecord->dependencies[0]
                    == originalGuid,
            "Dependency graph was not refreshed after remap.");

        // duplicate GUIDを割り当てる新asset
        const auto duplicateAsset =
            root / "textures" / "duplicate.bin";
        WriteFile(duplicateAsset, "duplicate");
        std::filesystem::copy_file(
            LamaPon::AssetDatabase::MetaPathFor(
                renamedAsset),
            LamaPon::AssetDatabase::MetaPathFor(
                duplicateAsset));
        // duplicate GUID走査を拒否したか
        bool duplicateRejected = false;
        // duplicate GUIDのdatabase更新を拒否
        try
        {
            static_cast<void>(
                database.Refresh(true));
        }
        // GUID重複エラーを拒否状態へ変換
        catch (const std::exception&)
        {
            duplicateRejected = true;
        }
        Require(
            duplicateRejected,
            "Duplicate asset GUID was accepted.");

        // 壊れたJSONを除外し、正常assetの走査を継続
        {
            std::filesystem::remove_all(root);
            // 走査継続を確かめる正常asset
            const auto healthy = root / "textures" / "ok.bin";
            // 不正JSONのscene fixture
            const auto brokenJson =
                root / "scenes" / "broken.scene.json";
            WriteFile(healthy, "asset");
            WriteFile(brokenJson, "{ not valid json at all ");

            // malformed fileを個別に除外するdatabase
            LamaPon::AssetDatabase resilient;
            resilient.SetAssetRoot(root);
            // malformed JSON後も走査が完了したか
            bool refreshed = true;
            // malformed JSONを含むrootを走査
            try
            {
                static_cast<void>(resilient.Refresh(true));
            }
            // malformed JSONだけを除外した走査結果
            catch (const std::exception&)
            {
                refreshed = false;
            }
            Require(
                refreshed,
                "A malformed JSON asset must not fail the scan.");
            Require(
                resilient.FindByPath(
                    std::filesystem::path{ "textures" }
                        / "ok.bin") != nullptr,
                "Healthy assets must still be indexed.");

            // 壊れた.metaはGUID保護のため再生成せず当該assetを除外
            WriteFile(
                LamaPon::AssetDatabase::MetaPathFor(healthy),
                "{ not valid json either ");
            // malformed meta後も走査が完了したか
            bool refreshedAgain = true;
            // malformed metaを含むrootを走査
            try
            {
                static_cast<void>(resilient.Refresh(true));
            }
            // malformed metaだけを除外した走査結果
            catch (const std::exception&)
            {
                refreshedAgain = false;
            }
            Require(
                refreshedAgain,
                "A malformed .meta must not fail the scan.");
            Require(
                resilient.FindByPath(
                    std::filesystem::path{ "textures" }
                        / "ok.bin") == nullptr,
                "An asset with an unreadable .meta is skipped.");
        }

        std::filesystem::remove_all(root);
        std::cout
            << "Asset database tests passed.\n";
        return 0;
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
