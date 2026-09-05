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
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create test asset.");
        }
        output << contents;
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "asset-database";
        std::filesystem::remove_all(root);

        const auto originalAsset =
            root / "textures" / "sample.bin";
        const auto renamedAsset =
            root / "textures" / "renamed.bin";
        const auto scenePath =
            root / "scenes" / "sample.scene.json";
        WriteFile(originalAsset, "asset");
        WriteFile(
            scenePath,
            R"({"format":"Test","texture":"textures/sample.bin"})");

        LamaPon::AssetDatabase database;
        database.SetAssetRoot(root);
        const auto first = database.Refresh(true);
        Require(
            first.assetCount == 2
                && first.createdMetaCount == 2
                && first.dependencyCount == 1,
            "Initial asset database scan failed.");

        const auto* original =
            database.FindByPath(
                "textures/sample.bin");
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
        const auto afterMove =
            database.Refresh(true);
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

        const auto remap =
            database.RemapJsonReferences(
                "textures/sample.bin",
                "textures/renamed.bin");
        Require(
            remap.fileCount == 1
                && remap.referenceCount == 1,
            "JSON asset reference was not remapped.");
        nlohmann::json remappedScene;
        {
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

        const auto duplicateAsset =
            root / "textures" / "duplicate.bin";
        WriteFile(duplicateAsset, "duplicate");
        std::filesystem::copy_file(
            LamaPon::AssetDatabase::MetaPathFor(
                renamedAsset),
            LamaPon::AssetDatabase::MetaPathFor(
                duplicateAsset));
        bool duplicateRejected = false;
        try
        {
            static_cast<void>(
                database.Refresh(true));
        }
        catch (const std::exception&)
        {
            duplicateRejected = true;
        }
        Require(
            duplicateRejected,
            "Duplicate asset GUID was accepted.");

        // 1件の破損でプロジェクト全体を開けなくしないよう、壊れた
        // ファイルだけを読み飛ばし、残りのアセットを走査します。
        {
            std::filesystem::remove_all(root);
            const auto healthy = root / "textures" / "ok.bin";
            const auto brokenJson =
                root / "scenes" / "broken.scene.json";
            WriteFile(healthy, "asset");
            WriteFile(brokenJson, "{ not valid json at all ");

            LamaPon::AssetDatabase resilient;
            resilient.SetAssetRoot(root);
            bool refreshed = true;
            try
            {
                static_cast<void>(resilient.Refresh(true));
            }
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

            // 壊れた.metaも同じ。ただし作り直しはしません
            // （GUIDが変わると他からの参照が黙って切れるため）。
            WriteFile(
                LamaPon::AssetDatabase::MetaPathFor(healthy),
                "{ not valid json either ");
            bool refreshedAgain = true;
            try
            {
                static_cast<void>(resilient.Refresh(true));
            }
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
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
