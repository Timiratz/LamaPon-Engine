#include "SceneCommands.h"
#include "ComponentSchemas.h"
#include "JsonFiles.h"
#include "ProjectPaths.h"
#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectSettings.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LamaPon::Cli
{
    namespace
    {
        struct SceneSource final
        {
            std::filesystem::path projectRoot;
            std::filesystem::path relativeScene;
            std::filesystem::path sceneFile;
            nlohmann::json document;
        };

        [[nodiscard]] bool TryReadObjectId(
            const nlohmann::json& value,
            std::uint64_t& result)
        {
            if (!value.is_number_integer()
                && !value.is_number_unsigned())
            {
                return false;
            }
            try
            {
                result = value.get<std::uint64_t>();
                return result != 0;
            }
            catch (const nlohmann::json::exception&)
            {
                return false;
            }
        }

        [[nodiscard]] SceneSource LoadSceneSource(
            const std::filesystem::path& requestedProject,
            const std::filesystem::path& requestedScene)
        {
            const auto projectRoot =
                std::filesystem::weakly_canonical(
                    std::filesystem::absolute(requestedProject));
            const auto settingsPath =
                projectRoot / L".lamapon" / L"project.json";
            if (!std::filesystem::is_regular_file(settingsPath))
            {
                throw std::runtime_error(
                    "The folder is not a LamaPon project"
                    " (missing .lamapon/project.json): "
                    + LamaPon::PathToUtf8(projectRoot));
            }

            std::filesystem::path requested = requestedScene;
            if (requested.empty())
            {
                requested =
                    LamaPon::LoadProjectSettings(settingsPath)
                        .startupScene;
            }
            const auto relativeScene = NormalizeScenePath(
                projectRoot,
                requested).lexically_normal();
            const auto assetsRoot =
                std::filesystem::weakly_canonical(
                    projectRoot / L"assets");
            const auto sceneFile =
                std::filesystem::weakly_canonical(
                    assetsRoot / relativeScene);
            const auto insideAssets =
                sceneFile.lexically_relative(assetsRoot);
            if (insideAssets.empty()
                || insideAssets.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "The scene is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(sceneFile));
            }
            if (!std::filesystem::is_regular_file(sceneFile))
            {
                throw std::runtime_error(
                    "Could not open scene for reading: "
                    + LamaPon::PathToUtf8(sceneFile));
            }

            std::ifstream input(sceneFile, std::ios::binary);
            const std::string contents{
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{} };
            auto document = nlohmann::json::parse(contents);
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    document,
                    LamaPon::SerializedDocumentKind::Scene));
            return {
                projectRoot,
                relativeScene,
                sceneFile,
                std::move(document),
            };
        }

        [[nodiscard]] nlohmann::json SceneProblem(
            const char* kind,
            const std::string& detail,
            const std::uint64_t objectId = 0,
            const std::string& objectName = {})
        {
            nlohmann::json problem{
                { "severity", "error" },
                { "kind", kind },
                { "detail", detail },
            };
            if (objectId != 0)
            {
                problem["objectId"] = objectId;
            }
            if (!objectName.empty())
            {
                problem["object"] = objectName;
            }
            return problem;
        }

        struct PrefabSource final
        {
            std::filesystem::path projectRoot;
            std::filesystem::path relativePrefab;
            std::filesystem::path prefabFile;
            nlohmann::json document;
        };

        [[nodiscard]] PrefabSource LoadPrefabSource(
            const std::filesystem::path& requestedProject,
            const std::filesystem::path& requestedPrefab)
        {
            const auto projectRoot = CanonicalProjectRoot(requestedProject);
            if (requestedPrefab.empty())
            {
                throw std::invalid_argument(
                    "prefab requires --path.");
            }
            const auto relativePrefab = NormalizeAssetPath(
                projectRoot,
                requestedPrefab).lexically_normal();
            const auto assetsRoot =
                std::filesystem::weakly_canonical(
                    projectRoot / L"assets");
            const auto prefabFile =
                std::filesystem::weakly_canonical(
                    assetsRoot / relativePrefab);
            const auto insideAssets =
                prefabFile.lexically_relative(assetsRoot);
            if (insideAssets.empty()
                || insideAssets.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "The prefab is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(prefabFile));
            }
            if (!std::filesystem::is_regular_file(prefabFile))
            {
                throw std::runtime_error(
                    "Could not open prefab for reading: "
                    + LamaPon::PathToUtf8(prefabFile));
            }
            auto document = ReadJsonFile(prefabFile);
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    document,
                    LamaPon::SerializedDocumentKind::Prefab));
            return {
                projectRoot,
                relativePrefab,
                prefabFile,
                std::move(document),
            };
        }

        [[nodiscard]] nlohmann::json AnalyzePrefab(
            const PrefabSource& source)
        {
            const auto& document = source.document;
            nlohmann::json problems = nlohmann::json::array();
            nlohmann::json warnings = nlohmann::json::array();
            nlohmann::json objects = nlohmann::json::array();
            std::unordered_map<std::uint64_t, std::size_t> indices;
            std::unordered_set<std::uint64_t> ids;
            std::size_t componentCount{};
            std::size_t rootCount{};
            std::size_t nestedPrefabCount{};

            if (document.value("format", std::string{})
                != "LamaPonPrefab")
            {
                problems.push_back(SceneProblem(
                    "invalid-format",
                    "The document format must be LamaPonPrefab."));
            }
            if (!document.contains("objects")
                || !document.at("objects").is_array())
            {
                problems.push_back(SceneProblem(
                    "invalid-objects",
                    "The prefab must contain an objects array."));
            }
            else
            {
                const auto& serializedObjects =
                    document.at("objects");
                for (std::size_t index{};
                    index < serializedObjects.size();
                    ++index)
                {
                    const auto& object = serializedObjects.at(index);
                    if (!object.is_object())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object",
                            "Every prefab object must be an object."));
                        continue;
                    }
                    const auto objectName = object.value(
                        "name",
                        std::string{});
                    std::uint64_t objectId{};
                    if (!object.contains("id")
                        || !TryReadObjectId(
                            object.at("id"),
                            objectId))
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object-id",
                            "Every prefab object must have a positive integer id.",
                            0,
                            objectName));
                    }
                    else if (!ids.insert(objectId).second)
                    {
                        problems.push_back(SceneProblem(
                            "duplicate-object-id",
                            "Prefab object ids must be unique.",
                            objectId,
                            objectName));
                    }
                    else
                    {
                        indices.emplace(objectId, index);
                    }

                    const auto parent = object.find("parent");
                    if (parent == object.end()
                        || parent->is_null())
                    {
                        ++rootCount;
                    }
                    else
                    {
                        std::uint64_t parentId{};
                        if (!TryReadObjectId(*parent, parentId))
                        {
                            problems.push_back(SceneProblem(
                                "invalid-parent",
                                "The prefab parent must be null or a positive object id.",
                                objectId,
                                objectName));
                        }
                        else if (parentId == objectId)
                        {
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "A prefab object cannot be its own parent.",
                                objectId,
                                objectName));
                        }
                    }

                    if (object.contains("prefabAsset")
                        && object.at("prefabAsset").is_string())
                    {
                        ++nestedPrefabCount;
                    }
                    nlohmann::json inspected{
                        { "id", objectId },
                        { "name", objectName },
                        { "enabled", object.value("enabled", true) },
                        { "parent", parent == object.end()
                            || parent->is_null()
                            ? nlohmann::json(nullptr)
                            : *parent },
                        { "components", nlohmann::json::array() },
                    };
                    if (const auto prefabAsset = object.find("prefabAsset");
                        prefabAsset != object.end())
                    {
                        inspected["prefabAsset"] = *prefabAsset;
                    }
                    if (const auto transform = object.find("transform");
                        transform != object.end())
                    {
                        inspected["transform"] = *transform;
                    }
                    const auto components = object.find("components");
                    if (components == object.end()
                        || !components->is_array())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-components",
                            "The prefab components field must be an array.",
                            objectId,
                            objectName));
                    }
                    else
                    {
                        componentCount += components->size();
                        for (const auto& component : *components)
                        {
                            if (!component.is_object()
                                || !component.contains("type")
                                || !component.at("type").is_string())
                            {
                                problems.push_back(SceneProblem(
                                    "invalid-component",
                                    "Every prefab component must have a string type.",
                                    objectId,
                                    objectName));
                                continue;
                            }
                            inspected["components"].push_back({
                                { "type", component.at("type") },
                                { "enabled",
                                    component.value("enabled", true) },
                                { "data", component },
                            });
                        }
                    }
                    objects.push_back(std::move(inspected));
                }

                for (const auto& [objectId, index] : indices)
                {
                    const auto& object = serializedObjects.at(index);
                    const auto parent = object.find("parent");
                    if (parent == object.end() || parent->is_null())
                    {
                        continue;
                    }
                    std::uint64_t parentId{};
                    if (TryReadObjectId(*parent, parentId)
                        && !indices.contains(parentId))
                    {
                        problems.push_back(SceneProblem(
                            "missing-parent",
                            "The prefab parent object does not exist.",
                            objectId,
                            object.value("name", std::string{})));
                    }
                }

                for (const auto& [objectId, index] : indices)
                {
                    std::unordered_set<std::uint64_t> visited;
                    auto current = objectId;
                    while (indices.contains(current))
                    {
                        if (!visited.insert(current).second)
                        {
                            const auto& object =
                                serializedObjects.at(index);
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "The prefab hierarchy contains a cycle.",
                                objectId,
                                object.value("name", std::string{})));
                            break;
                        }
                        const auto& currentObject =
                            serializedObjects.at(indices.at(current));
                        const auto parent = currentObject.find("parent");
                        if (parent == currentObject.end()
                            || parent->is_null()
                            || !TryReadObjectId(*parent, current))
                        {
                            break;
                        }
                    }
                }
            }

            std::uint64_t rootId{};
            if (!document.contains("root")
                || !TryReadObjectId(document.at("root"), rootId)
                || !indices.contains(rootId))
            {
                problems.push_back(SceneProblem(
                    "invalid-root",
                    "root must reference an existing prefab object."));
            }
            else
            {
                const auto& root = document.at("objects")
                    .at(indices.at(rootId));
                const auto rootParent = root.find("parent");
                if (rootParent != root.end() && !rootParent->is_null())
                {
                    problems.push_back(SceneProblem(
                        "root-has-parent",
                        "The prefab root object must not have a parent.",
                        rootId,
                        root.value("name", std::string{})));
                }
                for (const auto& [objectId, index] : indices)
                {
                    std::unordered_set<std::uint64_t> visited;
                    auto current = objectId;
                    bool reachesRoot{};
                    while (indices.contains(current))
                    {
                        if (!visited.insert(current).second)
                        {
                            break;
                        }
                        if (current == rootId)
                        {
                            reachesRoot = true;
                            break;
                        }
                        const auto currentParent = document.at("objects")
                            .at(indices.at(current)).find("parent");
                        if (currentParent == document.at("objects")
                                .at(indices.at(current)).end()
                            || currentParent->is_null()
                            || !TryReadObjectId(*currentParent, current))
                        {
                            break;
                        }
                    }
                    if (!reachesRoot)
                    {
                        const auto& object = document.at("objects")
                            .at(index);
                        problems.push_back(SceneProblem(
                            "outside-root",
                            "The prefab object is not under root.",
                            objectId,
                            object.value("name", std::string{})));
                    }
                }
            }

            nlohmann::json assets = nlohmann::json::array();
            std::unordered_set<std::string> seenAssets;
            for (const auto& assetPath :
                LamaPon::CollectSerializedAssetPaths(document))
            {
                const auto normalized = LamaPon::PathToUtf8(assetPath);
                if (!seenAssets.insert(normalized).second)
                {
                    continue;
                }
                const auto resolved = assetPath.is_absolute()
                    ? assetPath
                    : source.projectRoot / L"assets" / assetPath;
                const bool exists =
                    std::filesystem::is_regular_file(resolved);
                assets.push_back({
                    { "path", normalized },
                    { "exists", exists },
                });
                if (!exists)
                {
                    problems.push_back(SceneProblem(
                        "missing-asset",
                        "A serialized prefab asset reference does not exist: "
                            + normalized));
                }
            }

            return {
                { "format", document.value("format", std::string{}) },
                { "version", document.value("version", 0u) },
                { "root", rootId },
                { "rootCount", rootCount },
                { "objectCount", objects.size() },
                { "componentCount", componentCount },
                { "nestedPrefabCount", nestedPrefabCount },
                { "objects", std::move(objects) },
                { "assets", std::move(assets) },
                { "problems", std::move(problems) },
                { "warnings", std::move(warnings) },
            };
        }

        [[nodiscard]] nlohmann::json AnalyzeScene(
            const SceneSource& source)
        {
            const auto& document = source.document;
            nlohmann::json problems = nlohmann::json::array();
            nlohmann::json warnings = nlohmann::json::array();
            nlohmann::json objects = nlohmann::json::array();
            std::unordered_map<std::uint64_t, std::size_t> indices;
            std::unordered_set<std::uint64_t> ids;
            std::size_t componentCount{};
            std::size_t rootCount{};

            if (document.value("format", std::string{})
                != "LamaPonScene")
            {
                problems.push_back(SceneProblem(
                    "invalid-format",
                    "The document format must be LamaPonScene."));
            }
            if (!document.contains("objects")
                || !document.at("objects").is_array())
            {
                problems.push_back(SceneProblem(
                    "invalid-objects",
                    "The scene must contain an objects array."));
            }
            else
            {
                const auto& serializedObjects =
                    document.at("objects");
                for (std::size_t index{};
                    index < serializedObjects.size();
                    ++index)
                {
                    const auto& object = serializedObjects.at(index);
                    if (!object.is_object())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object",
                            "Every entry in objects must be an object."));
                        continue;
                    }
                    const auto objectName = object.value(
                        "name",
                        std::string{});
                    std::uint64_t objectId{};
                    if (!object.contains("id")
                        || !TryReadObjectId(
                            object.at("id"),
                            objectId))
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object-id",
                            "Every object must have a positive integer id.",
                            0,
                            objectName));
                    }
                    else if (!ids.insert(objectId).second)
                    {
                        problems.push_back(SceneProblem(
                            "duplicate-object-id",
                            "Object ids must be unique.",
                            objectId,
                            objectName));
                    }
                    else
                    {
                        indices.emplace(objectId, index);
                    }

                    const auto parent = object.find("parent");
                    if (parent == object.end()
                        || parent->is_null())
                    {
                        ++rootCount;
                    }
                    else
                    {
                        std::uint64_t parentId{};
                        if (!TryReadObjectId(*parent, parentId))
                        {
                            problems.push_back(SceneProblem(
                                "invalid-parent",
                                "The parent must be null or a positive object id.",
                                objectId,
                                objectName));
                        }
                        else if (parentId == objectId)
                        {
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "An object cannot be its own parent.",
                                objectId,
                                objectName));
                        }
                    }

                    nlohmann::json inspected{
                        { "id", objectId },
                        { "name", objectName },
                        { "enabled", object.value("enabled", true) },
                        { "parent", parent == object.end()
                            || parent->is_null()
                            ? nlohmann::json(nullptr)
                            : *parent },
                        { "components", nlohmann::json::array() },
                    };
                    if (const auto transform = object.find("transform");
                        transform != object.end())
                    {
                        inspected["transform"] = *transform;
                    }
                    const auto components = object.find("components");
                    if (components == object.end()
                        || !components->is_array())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-components",
                            "The components field must be an array.",
                            objectId,
                            objectName));
                    }
                    else
                    {
                        componentCount += components->size();
                        for (const auto& component : *components)
                        {
                            if (!component.is_object()
                                || !component.contains("type")
                                || !component.at("type").is_string())
                            {
                                problems.push_back(SceneProblem(
                                    "invalid-component",
                                    "Every component must have a string type.",
                                    objectId,
                                    objectName));
                                continue;
                            }
                            inspected["components"].push_back({
                                { "type", component.at("type") },
                                { "enabled",
                                    component.value("enabled", true) },
                                { "data", component },
                            });
                        }
                    }
                    objects.push_back(std::move(inspected));
                }

                for (const auto& [objectId, index] : indices)
                {
                    const auto& object = serializedObjects.at(index);
                    const auto parent = object.find("parent");
                    if (parent == object.end() || parent->is_null())
                    {
                        continue;
                    }
                    std::uint64_t parentId{};
                    if (TryReadObjectId(*parent, parentId)
                        && !indices.contains(parentId))
                    {
                        problems.push_back(SceneProblem(
                            "missing-parent",
                            "The parent object does not exist.",
                            objectId,
                            object.value("name", std::string{})));
                    }
                }

                for (const auto& [objectId, index] : indices)
                {
                    std::unordered_set<std::uint64_t> visited;
                    auto current = objectId;
                    while (indices.contains(current))
                    {
                        if (!visited.insert(current).second)
                        {
                            const auto& object =
                                serializedObjects.at(index);
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "The object hierarchy contains a cycle.",
                                objectId,
                                object.value(
                                    "name",
                                    std::string{})));
                            break;
                        }
                        const auto& currentObject =
                            serializedObjects.at(indices.at(current));
                        const auto parent =
                            currentObject.find("parent");
                        if (parent == currentObject.end()
                            || parent->is_null()
                            || !TryReadObjectId(*parent, current))
                        {
                            break;
                        }
                    }
                }
            }

            if (const auto mainCamera = document.find("mainCamera");
                mainCamera != document.end() && !mainCamera->is_null())
            {
                std::uint64_t cameraId{};
                if (!TryReadObjectId(*mainCamera, cameraId)
                    || !indices.contains(cameraId))
                {
                    problems.push_back(SceneProblem(
                        "missing-main-camera",
                        "mainCamera must reference an existing object."));
                }
                else
                {
                    const auto& cameraObject =
                        document.at("objects").at(indices.at(cameraId));
                    bool hasCamera{};
                    if (const auto components =
                            cameraObject.find("components");
                        components != cameraObject.end()
                        && components->is_array())
                    {
                        for (const auto& component : *components)
                        {
                            if (component.is_object()
                                && component.value(
                                    "type",
                                    std::string{})
                                    == "Camera")
                            {
                                hasCamera = true;
                                break;
                            }
                        }
                    }
                    if (!hasCamera)
                    {
                        problems.push_back(SceneProblem(
                            "main-camera-component-missing",
                            "mainCamera references an object without a Camera component.",
                            cameraId,
                            cameraObject.value(
                                "name",
                                std::string{})));
                    }
                }
            }

            nlohmann::json assets = nlohmann::json::array();
            std::unordered_set<std::string> seenAssets;
            for (const auto& assetPath :
                LamaPon::CollectSerializedAssetPaths(document))
            {
                const auto normalized =
                    LamaPon::PathToUtf8(assetPath);
                if (!seenAssets.insert(normalized).second)
                {
                    continue;
                }
                const auto resolved = assetPath.is_absolute()
                    ? assetPath
                    : source.projectRoot / L"assets" / assetPath;
                const bool exists =
                    std::filesystem::is_regular_file(resolved);
                assets.push_back({
                    { "path", normalized },
                    { "exists", exists },
                });
                if (!exists)
                {
                    problems.push_back(SceneProblem(
                        "missing-asset",
                        "A serialized asset reference does not exist: "
                            + normalized));
                }
            }

            return {
                { "format", document.value("format", std::string{}) },
                { "version", document.value("version", 0u) },
                { "objectCount", objects.size() },
                { "rootCount", rootCount },
                { "componentCount", componentCount },
                { "objects", std::move(objects) },
                { "assets", std::move(assets) },
                { "problems", std::move(problems) },
                { "warnings", std::move(warnings) },
            };
        }

    }

    [[nodiscard]] int RunInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        const auto source = LoadSceneSource(projectRoot, scene);
        auto analysis = AnalyzeScene(source);
        analysis["command"] = "inspect";
        analysis["ok"] = true;
        analysis["project"] =
            LamaPon::PathToUtf8(source.projectRoot);
        analysis["scene"] =
            LamaPon::PathToUtf8(source.relativeScene);
        analysis["document"] = source.document;
        std::cout
            << analysis.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        const auto source = LoadSceneSource(projectRoot, scene);
        auto analysis = AnalyzeScene(source);
        const bool valid =
            analysis.at("problems").empty();
        analysis["command"] = "validate";
        analysis["ok"] = valid;
        analysis["project"] =
            LamaPon::PathToUtf8(source.projectRoot);
        analysis["scene"] =
            LamaPon::PathToUtf8(source.relativeScene);
        std::cout
            << analysis.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return valid ? 0 : 1;
    }

    [[nodiscard]] int RunPrefabInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab)
    {
        const auto source = LoadPrefabSource(projectRoot, prefab);
        auto analysis = AnalyzePrefab(source);
        analysis["command"] = "prefab inspect";
        analysis["ok"] = true;
        analysis["project"] =
            LamaPon::PathToUtf8(source.projectRoot);
        analysis["prefab"] =
            LamaPon::PathToUtf8(source.relativePrefab);
        analysis["document"] = source.document;
        std::cout
            << analysis.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunPrefabValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab)
    {
        const auto source = LoadPrefabSource(projectRoot, prefab);
        auto analysis = AnalyzePrefab(source);
        const bool valid =
            analysis.at("problems").empty();
        analysis["command"] = "prefab validate";
        analysis["ok"] = valid;
        analysis["project"] =
            LamaPon::PathToUtf8(source.projectRoot);
        analysis["prefab"] =
            LamaPon::PathToUtf8(source.relativePrefab);
        std::cout
            << analysis.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return valid ? 0 : 1;
    }

    namespace
    {
        [[nodiscard]] std::vector<std::string> SplitPatchPath(
            const std::string& path)
        {
            if (path.empty())
            {
                throw std::invalid_argument(
                    "A patch path must not be empty.");
            }
            std::vector<std::string> parts;
            std::size_t begin{};
            while (begin <= path.size())
            {
                const auto end = path.find('.', begin);
                const auto length = end == std::string::npos
                    ? path.size() - begin
                    : end - begin;
                if (length == 0)
                {
                    throw std::invalid_argument(
                        "A patch path must not contain empty segments: "
                        + path);
                }
                parts.push_back(path.substr(begin, length));
                if (end == std::string::npos)
                {
                    break;
                }
                begin = end + 1;
            }
            return parts;
        }

        [[nodiscard]] std::size_t PatchArrayIndex(
            const std::string& token,
            const std::string& path)
        {
            try
            {
                std::size_t consumed{};
                const auto value = std::stoull(token, &consumed);
                if (consumed != token.size()
                    || value > std::numeric_limits<std::size_t>::max())
                {
                    throw std::invalid_argument("not an array index");
                }
                return static_cast<std::size_t>(value);
            }
            catch (const std::exception&)
            {
                throw std::invalid_argument(
                    "Patch path segment is not a valid array index: "
                    + token + " (path: " + path + ")");
            }
        }

        void SetPatchValue(
            nlohmann::json& root,
            const std::string& path,
            const nlohmann::json& value)
        {
            const auto parts = SplitPatchPath(path);
            nlohmann::json* current = &root;
            for (std::size_t index{}; index < parts.size(); ++index)
            {
                const bool last = index + 1 == parts.size();
                const auto& part = parts.at(index);
                if (current->is_object())
                {
                    if (last)
                    {
                        (*current)[part] = value;
                        return;
                    }
                    if (!current->contains(part))
                    {
                        (*current)[part] = nlohmann::json::object();
                    }
                    current = &(*current)[part];
                    continue;
                }
                if (current->is_array())
                {
                    const auto arrayIndex = PatchArrayIndex(part, path);
                    if (arrayIndex >= current->size())
                    {
                        throw std::invalid_argument(
                            "Patch path array index is out of range: "
                            + path);
                    }
                    if (last)
                    {
                        current->at(arrayIndex) = value;
                        return;
                    }
                    current = &current->at(arrayIndex);
                    continue;
                }
                throw std::invalid_argument(
                    "Patch path crosses a scalar value: " + path);
            }
        }

        [[nodiscard]] const nlohmann::json* FindPatchValue(
            const nlohmann::json& root,
            const std::string& path)
        {
            const auto parts = SplitPatchPath(path);
            const nlohmann::json* current = &root;
            for (const auto& part : parts)
            {
                if (current->is_object())
                {
                    const auto iterator = current->find(part);
                    if (iterator == current->end())
                    {
                        return nullptr;
                    }
                    current = &*iterator;
                    continue;
                }
                if (current->is_array())
                {
                    const auto arrayIndex = PatchArrayIndex(part, path);
                    if (arrayIndex >= current->size())
                    {
                        return nullptr;
                    }
                    current = &current->at(arrayIndex);
                    continue;
                }
                return nullptr;
            }
            return current;
        }

        [[nodiscard]] std::vector<std::size_t> FindPatchTargets(
            const nlohmann::json& document,
            const nlohmann::json& target)
        {
            const auto objects = document.find("objects");
            if (objects == document.end() || !objects->is_array())
            {
                throw std::invalid_argument(
                    "Scene objects must be an array before applying a patch.");
            }

            std::optional<std::uint64_t> id;
            std::optional<std::string> name;
            if (target.is_object())
            {
                if (const auto value = target.find("id");
                    value != target.end())
                {
                    std::uint64_t parsed{};
                    if (!TryReadObjectId(*value, parsed))
                    {
                        throw std::invalid_argument(
                            "Patch target id must be a positive integer.");
                    }
                    id = parsed;
                }
                if (const auto value = target.find("name");
                    value != target.end())
                {
                    if (!value->is_string())
                    {
                        throw std::invalid_argument(
                            "Patch target name must be a string.");
                    }
                    name = value->get<std::string>();
                }
            }
            else if (target.is_number_integer()
                || target.is_number_unsigned())
            {
                std::uint64_t parsed{};
                if (!TryReadObjectId(target, parsed))
                {
                    throw std::invalid_argument(
                        "Patch target id must be a positive integer.");
                }
                id = parsed;
            }
            else if (target.is_string())
            {
                name = target.get<std::string>();
            }
            if (!id.has_value() && !name.has_value())
            {
                throw std::invalid_argument(
                    "Patch target must contain id or name.");
            }

            std::vector<std::size_t> matches;
            for (std::size_t index{}; index < objects->size(); ++index)
            {
                const auto& object = objects->at(index);
                if (!object.is_object())
                {
                    continue;
                }
                std::uint64_t objectId{};
                const bool idMatches = id.has_value()
                    && object.contains("id")
                    && TryReadObjectId(object.at("id"), objectId)
                    && objectId == id.value();
                const bool nameMatches = name.has_value()
                    && object.value("name", std::string{}) == name.value();
                if ((id.has_value() && !idMatches)
                    || (name.has_value() && !nameMatches))
                {
                    continue;
                }
                matches.push_back(index);
            }
            return matches;
        }

        [[nodiscard]] std::size_t FindSinglePatchTarget(
            const nlohmann::json& document,
            const nlohmann::json& target)
        {
            const auto matches = FindPatchTargets(document, target);
            if (matches.empty())
            {
                throw std::invalid_argument(
                    "Patch target did not match any object.");
            }
            if (matches.size() != 1)
            {
                throw std::invalid_argument(
                    "Patch target matched multiple objects; use a unique id.");
            }
            return matches.front();
        }

        [[nodiscard]] nlohmann::json PatchParentId(
            const nlohmann::json& document,
            const nlohmann::json& parent)
        {
            if (parent.is_null())
            {
                return nullptr;
            }
            const auto index = FindSinglePatchTarget(document, parent);
            return document.at("objects").at(index).at("id");
        }

        [[nodiscard]] std::uint64_t AllocatePatchObjectId(
            const nlohmann::json& document,
            std::uint64_t& nextId)
        {
            const auto& objects = document.at("objects");
            while (true)
            {
                if (nextId == 0
                    || nextId == std::numeric_limits<std::uint64_t>::max())
                {
                    throw std::runtime_error(
                        "No unused object id is available.");
                }
                bool used{};
                for (const auto& object : objects)
                {
                    std::uint64_t objectId{};
                    if (object.is_object()
                        && object.contains("id")
                        && TryReadObjectId(object.at("id"), objectId)
                        && objectId == nextId)
                    {
                        used = true;
                        break;
                    }
                }
                if (!used)
                {
                    return nextId++;
                }
                ++nextId;
            }
        }

        [[nodiscard]] std::size_t ApplyPatchOperation(
            nlohmann::json& document,
            const nlohmann::json& operation,
            std::uint64_t& nextId)
        {
            if (!operation.is_object()
                || !operation.contains("op")
                || !operation.at("op").is_string())
            {
                throw std::invalid_argument(
                    "Every patch operation must contain a string op.");
            }
            const auto kind = operation.at("op").get<std::string>();
            if (kind == "set-scene")
            {
                if (!operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    throw std::invalid_argument(
                        "set-scene requires path and value.");
                }
                SetPatchValue(
                    document,
                    operation.at("path").get<std::string>(),
                    operation.at("value"));
                return 1;
            }

            if (kind == "add-object")
            {
                nlohmann::json object = operation.value(
                    "object",
                    nlohmann::json::object());
                if (!object.is_object())
                {
                    throw std::invalid_argument(
                        "add-object object must be a JSON object.");
                }
                std::uint64_t objectId{};
                if (object.contains("id"))
                {
                    if (!TryReadObjectId(object.at("id"), objectId))
                    {
                        throw std::invalid_argument(
                            "add-object id must be a positive integer.");
                    }
                    for (const auto& existing : document.at("objects"))
                    {
                        std::uint64_t existingId{};
                        if (existing.is_object()
                            && existing.contains("id")
                            && TryReadObjectId(
                                existing.at("id"),
                                existingId)
                            && existingId == objectId)
                        {
                            throw std::invalid_argument(
                                "add-object id is already in use.");
                        }
                    }
                    if (objectId >= nextId)
                    {
                        nextId = objectId + 1;
                    }
                }
                else
                {
                    objectId = AllocatePatchObjectId(document, nextId);
                    object["id"] = objectId;
                }
                if (!object.contains("name"))
                {
                    object["name"] = "GameObject";
                }
                if (operation.contains("name"))
                {
                    if (!operation.at("name").is_string())
                    {
                        throw std::invalid_argument(
                            "add-object name must be a string.");
                    }
                    object["name"] = operation.at("name");
                }
                if (!object.contains("enabled"))
                {
                    object["enabled"] = true;
                }
                if (operation.contains("enabled"))
                {
                    if (!operation.at("enabled").is_boolean())
                    {
                        throw std::invalid_argument(
                            "add-object enabled must be boolean.");
                    }
                    object["enabled"] = operation.at("enabled");
                }
                if (operation.contains("parent"))
                {
                    object["parent"] = PatchParentId(
                        document,
                        operation.at("parent"));
                }
                else if (object.contains("parent"))
                {
                    object["parent"] = PatchParentId(
                        document,
                        object.at("parent"));
                }
                else
                {
                    object["parent"] = nullptr;
                }
                if (!object.contains("transform"))
                {
                    object["transform"] = {
                        { "position", { 0.0, 0.0, 0.0 } },
                        { "rotation", { 0.0, 0.0, 0.0 } },
                        { "scale", { 1.0, 1.0, 1.0 } },
                    };
                }
                if (operation.contains("transform"))
                {
                    object["transform"] = operation.at("transform");
                }
                if (!object.contains("components"))
                {
                    object["components"] = nlohmann::json::array();
                }
                if (operation.contains("components"))
                {
                    object["components"] = operation.at("components");
                }
                document["objects"].push_back(std::move(object));
                return 1;
            }

            if (!operation.contains("target"))
            {
                throw std::invalid_argument(
                    kind + " requires target.");
            }
            const auto targetIndex = FindSinglePatchTarget(
                document,
                operation.at("target"));
            auto& object = document.at("objects").at(targetIndex);

            if (kind == "set")
            {
                if (!operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    throw std::invalid_argument(
                        "set requires path and value.");
                }
                const auto path = operation.at("path").get<std::string>();
                if (path == "id" || path == "parent")
                {
                    throw std::invalid_argument(
                        "Use add-object or reparent to change object hierarchy and ids.");
                }
                SetPatchValue(object, path, operation.at("value"));
                return 1;
            }

            if (kind == "rename")
            {
                if (!operation.contains("name")
                    || !operation.at("name").is_string())
                {
                    throw std::invalid_argument(
                        "rename requires a string name.");
                }
                object["name"] = operation.at("name");
                return 1;
            }

            if (kind == "reparent")
            {
                if (!operation.contains("parent"))
                {
                    throw std::invalid_argument(
                        "reparent requires parent, or null for a root object.");
                }
                object["parent"] = PatchParentId(
                    document,
                    operation.at("parent"));
                return 1;
            }

            if (kind == "add-component")
            {
                if (!operation.contains("type")
                    || !operation.at("type").is_string()
                    || operation.at("type").get<std::string>().empty())
                {
                    throw std::invalid_argument(
                        "add-component requires a non-empty type.");
                }
                nlohmann::json component = operation.value(
                    "data",
                    nlohmann::json::object());
                if (!component.is_object())
                {
                    throw std::invalid_argument(
                        "add-component data must be a JSON object.");
                }
                component["type"] = operation.at("type");
                if (!component.contains("enabled"))
                {
                    component["enabled"] = true;
                }
                ValidateComponentObject(
                    operation.at("type").get<std::string>(),
                    component);
                if (!object.contains("components")
                    || !object.at("components").is_array())
                {
                    object["components"] = nlohmann::json::array();
                }
                object["components"].push_back(std::move(component));
                return 1;
            }

            if (kind == "remove-component")
            {
                if (!operation.contains("type")
                    || !operation.at("type").is_string())
                {
                    throw std::invalid_argument(
                        "remove-component requires a type.");
                }
                auto& components = object["components"];
                if (!components.is_array())
                {
                    throw std::invalid_argument(
                        "The target object's components must be an array.");
                }
                const auto type = operation.at("type").get<std::string>();
                std::vector<std::size_t> matches;
                for (std::size_t componentIndex{};
                    componentIndex < components.size();
                    ++componentIndex)
                {
                    const auto& component = components.at(componentIndex);
                    if (component.is_object()
                        && component.value("type", std::string{}) == type)
                    {
                        matches.push_back(componentIndex);
                    }
                }
                if (matches.empty())
                {
                    throw std::invalid_argument(
                        "remove-component did not match the requested type.");
                }
                if (matches.size() > 1
                    && !operation.value("all", false))
                {
                    throw std::invalid_argument(
                        "remove-component matched multiple components; use all:true.");
                }
                for (auto iterator = matches.rbegin();
                    iterator != matches.rend();
                    ++iterator)
                {
                    components.erase(components.begin() + *iterator);
                }
                return 1;
            }

            if (kind == "set-component")
            {
                if (!operation.contains("type")
                    || !operation.at("type").is_string()
                    || !operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    throw std::invalid_argument(
                        "set-component requires type, path, and value.");
                }
                if (operation.at("path").get<std::string>() == "type")
                {
                    throw std::invalid_argument(
                        "set-component cannot change component type.");
                }
                auto& components = object["components"];
                if (!components.is_array())
                {
                    throw std::invalid_argument(
                        "The target object's components must be an array.");
                }
                const auto type = operation.at("type").get<std::string>();
                std::vector<std::size_t> matches;
                for (std::size_t componentIndex{};
                    componentIndex < components.size();
                    ++componentIndex)
                {
                    const auto& component = components.at(componentIndex);
                    if (component.is_object()
                        && component.value("type", std::string{}) == type)
                    {
                        matches.push_back(componentIndex);
                    }
                }
                if (matches.size() != 1)
                {
                    throw std::invalid_argument(
                        "set-component requires exactly one matching component.");
                }
                ValidateComponentValue(
                    type,
                    operation.at("path").get<std::string>(),
                    operation.at("value"));
                SetPatchValue(
                    components.at(matches.front()),
                    operation.at("path").get<std::string>(),
                    operation.at("value"));
                return 1;
            }

            if (kind == "remove-object")
            {
                std::uint64_t removedId{};
                if (!object.contains("id")
                    || !TryReadObjectId(object.at("id"), removedId))
                {
                    throw std::invalid_argument(
                        "remove-object target must have a valid id.");
                }
                const bool recursive = operation.value("recursive", false);
                std::unordered_set<std::uint64_t> removedIds{ removedId };
                bool changed{};
                do
                {
                    changed = false;
                    for (const auto& candidate : document.at("objects"))
                    {
                        if (!candidate.is_object()
                            || !candidate.contains("id"))
                        {
                            continue;
                        }
                        std::uint64_t candidateId{};
                        if (!TryReadObjectId(
                                candidate.at("id"),
                                candidateId)
                            || removedIds.contains(candidateId))
                        {
                            continue;
                        }
                        const auto parent = candidate.find("parent");
                        std::uint64_t parentId{};
                        if (parent != candidate.end()
                            && !parent->is_null()
                            && TryReadObjectId(*parent, parentId)
                            && removedIds.contains(parentId))
                        {
                            removedIds.insert(candidateId);
                            changed = true;
                        }
                    }
                } while (recursive && changed);

                if (!recursive && removedIds.size() == 1)
                {
                    for (const auto& candidate : document.at("objects"))
                    {
                        if (!candidate.is_object())
                        {
                            continue;
                        }
                        std::uint64_t parentId{};
                        const auto parent = candidate.find("parent");
                        if (parent != candidate.end()
                            && !parent->is_null()
                            && TryReadObjectId(*parent, parentId)
                            && parentId == removedId)
                        {
                            throw std::invalid_argument(
                                "remove-object has children; use recursive:true.");
                        }
                    }
                }

                auto& objects = document["objects"];
                for (auto iterator = objects.begin();
                    iterator != objects.end();)
                {
                    std::uint64_t candidateId{};
                    if (iterator->is_object()
                        && iterator->contains("id")
                        && TryReadObjectId(
                            iterator->at("id"),
                            candidateId)
                        && removedIds.contains(candidateId))
                    {
                        iterator = objects.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
                if (document.contains("mainCamera")
                    && !document.at("mainCamera").is_null())
                {
                    std::uint64_t cameraId{};
                    if (TryReadObjectId(
                            document.at("mainCamera"),
                            cameraId)
                        && removedIds.contains(cameraId))
                    {
                        document["mainCamera"] = nullptr;
                    }
                }
                return 1;
            }

            throw std::invalid_argument(
                "Unknown patch operation: " + kind);
        }

        [[nodiscard]] std::filesystem::path ResolvePatchOutput(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& requested)
        {
            const auto candidate = requested.is_absolute()
                ? requested
                : projectRoot / requested;
            const auto output = std::filesystem::weakly_canonical(candidate);
            const auto relative = output.lexically_relative(projectRoot);
            if (relative.empty()
                || relative.native().starts_with(L".."))
            {
                throw std::invalid_argument(
                    "Patch output must stay inside the project: "
                    + LamaPon::PathToUtf8(output));
            }
            return output;
        }

    }

    [[nodiscard]] int RunPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun)
    {
        auto source = LoadSceneSource(projectRoot, scene);
        const auto operationsFile = operationsPath.is_absolute()
            ? operationsPath
            : source.projectRoot / operationsPath;
        const auto operationsDocument = ReadJsonFile(operationsFile);
        const auto operations = operationsDocument.is_array()
            ? operationsDocument
            : operationsDocument.value(
                "operations",
                nlohmann::json::array());
        if (!operations.is_array())
        {
            throw std::invalid_argument(
                "Patch file must be an array or contain an operations array.");
        }
        if (!source.document.contains("objects")
            || !source.document.at("objects").is_array())
        {
            throw std::invalid_argument(
                "Scene objects must be an array before applying a patch.");
        }

        std::uint64_t nextId{ 1 };
        for (const auto& object : source.document.at("objects"))
        {
            std::uint64_t objectId{};
            if (object.is_object()
                && object.contains("id")
                && TryReadObjectId(object.at("id"), objectId)
                && objectId >= nextId)
            {
                if (objectId == std::numeric_limits<std::uint64_t>::max())
                {
                    nextId = 1;
                }
                else
                {
                    nextId = objectId + 1;
                }
            }
        }

        const auto before = AnalyzeScene(source);
        std::size_t operationsApplied{};
        for (const auto& operation : operations)
        {
            operationsApplied += ApplyPatchOperation(
                source.document,
                operation,
                nextId);
        }
        SceneSource resultSource = source;
        const auto after = AnalyzeScene(resultSource);
        const bool valid = after.at("problems").empty();

        nlohmann::json report{
            { "command", "patch" },
            { "ok", valid },
            { "dryRun", dryRun },
            { "project", LamaPon::PathToUtf8(source.projectRoot) },
            { "scene", LamaPon::PathToUtf8(source.relativeScene) },
            { "operationsFile", LamaPon::PathToUtf8(operationsFile) },
            { "operationsApplied", operationsApplied },
            { "changed", operationsApplied != 0 },
            { "before", before },
            { "after", after },
        };

        if (!valid)
        {
            report["error"] =
                "The patch would leave the scene invalid; no file was written.";
        }
        else if (!dryRun && operationsApplied != 0)
        {
            const auto output = outputPath.empty()
                ? source.sceneFile
                : ResolvePatchOutput(source.projectRoot, outputPath);
            std::filesystem::path backup;
            if (std::filesystem::is_regular_file(output))
            {
                backup = output.wstring()
                    + L".bak-"
                    + std::to_wstring(GetTickCount64());
                std::error_code copyError;
                std::filesystem::copy_file(
                    output,
                    backup,
                    std::filesystem::copy_options::none,
                    copyError);
                if (copyError)
                {
                    throw std::runtime_error(
                        "Could not create scene backup: "
                        + copyError.message());
                }
            }
            WriteJsonFile(output, source.document);
            report["output"] = LamaPon::PathToUtf8(output);
            report["backup"] = backup.empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(LamaPon::PathToUtf8(backup));
        }
        else
        {
            report["output"] = nullptr;
            report["backup"] = nullptr;
        }

        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return valid ? 0 : 1;
    }

    [[nodiscard]] int RunPrefabPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun)
    {
        auto source = LoadPrefabSource(projectRoot, prefab);
        const auto operationsFile = operationsPath.is_absolute()
            ? operationsPath
            : source.projectRoot / operationsPath;
        const auto operationsDocument = ReadJsonFile(operationsFile);
        const auto operations = operationsDocument.is_array()
            ? operationsDocument
            : operationsDocument.value(
                "operations",
                nlohmann::json::array());
        if (!operations.is_array())
        {
            throw std::invalid_argument(
                "Prefab patch file must be an array or contain an operations array.");
        }
        if (!source.document.contains("objects")
            || !source.document.at("objects").is_array())
        {
            throw std::invalid_argument(
                "Prefab objects must be an array before applying a patch.");
        }

        std::uint64_t nextId{ 1 };
        for (const auto& object : source.document.at("objects"))
        {
            std::uint64_t objectId{};
            if (object.is_object()
                && object.contains("id")
                && TryReadObjectId(object.at("id"), objectId)
                && objectId >= nextId)
            {
                nextId = objectId == std::numeric_limits<std::uint64_t>::max()
                    ? 1
                    : objectId + 1;
            }
        }

        const auto before = AnalyzePrefab(source);
        std::size_t operationsApplied{};
        for (const auto& operation : operations)
        {
            operationsApplied += ApplyPatchOperation(
                source.document,
                operation,
                nextId);
        }
        PrefabSource resultSource = source;
        const auto after = AnalyzePrefab(resultSource);
        const bool valid = after.at("problems").empty();

        nlohmann::json report{
            { "command", "prefab patch" },
            { "ok", valid },
            { "dryRun", dryRun },
            { "project", LamaPon::PathToUtf8(source.projectRoot) },
            { "prefab", LamaPon::PathToUtf8(source.relativePrefab) },
            { "operationsFile", LamaPon::PathToUtf8(operationsFile) },
            { "operationsApplied", operationsApplied },
            { "changed", operationsApplied != 0 },
            { "before", before },
            { "after", after },
        };
        if (!valid)
        {
            report["error"] =
                "The patch would leave the prefab invalid; no file was written.";
        }
        else if (!dryRun && operationsApplied != 0)
        {
            const auto output = outputPath.empty()
                ? source.prefabFile
                : ResolvePatchOutput(source.projectRoot, outputPath);
            std::filesystem::path backup;
            if (std::filesystem::is_regular_file(output))
            {
                backup = output.wstring()
                    + L".bak-"
                    + std::to_wstring(GetTickCount64());
                std::error_code copyError;
                std::filesystem::copy_file(
                    output,
                    backup,
                    std::filesystem::copy_options::none,
                    copyError);
                if (copyError)
                {
                    throw std::runtime_error(
                        "Could not create prefab backup: "
                        + copyError.message());
                }
            }
            WriteJsonFile(output, source.document);
            report["output"] = LamaPon::PathToUtf8(output);
            report["backup"] = backup.empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(LamaPon::PathToUtf8(backup));
        }
        else
        {
            report["output"] = nullptr;
            report["backup"] = nullptr;
        }

        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return valid ? 0 : 1;
    }

    [[nodiscard]] int RunSceneTests(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& specificationPath,
        const std::filesystem::path& reportPath)
    {
        const auto source = LoadSceneSource(projectRoot, scene);
        const auto specificationFile = specificationPath.is_absolute()
            ? specificationPath
            : source.projectRoot / specificationPath;
        const auto specification = ReadJsonFile(specificationFile);
        const auto tests = specification.is_array()
            ? specification
            : specification.value(
                "tests",
                nlohmann::json::array());
        if (!tests.is_array())
        {
            throw std::invalid_argument(
                "Test specification must be an array or contain a tests array.");
        }

        const auto analysis = AnalyzeScene(source);
        nlohmann::json assertions = nlohmann::json::array();
        std::size_t passedCount{};
        for (std::size_t index{}; index < tests.size(); ++index)
        {
            const auto& test = tests.at(index);
            const auto name = test.is_object()
                ? test.value(
                    "name",
                    "test-" + std::to_string(index + 1))
                : "test-" + std::to_string(index + 1);
            const auto kind = test.is_object()
                ? test.value("kind", std::string{})
                : std::string{};
            bool passed{};
            std::string detail;
            nlohmann::json actual = nullptr;
            try
            {
                if (!test.is_object()
                    || kind.empty())
                {
                    detail = "Each test requires a kind.";
                }
                else if (kind == "scene-valid"
                    || kind == "no-problems")
                {
                    passed = analysis.at("problems").empty();
                    actual = analysis.at("problems");
                    detail = passed
                        ? "The scene has no validation problems."
                        : "The scene contains validation problems.";
                }
                else if (kind == "object-exists")
                {
                    if (!test.contains("target"))
                    {
                        detail = "object-exists requires target.";
                    }
                    else
                    {
                        const auto matches = FindPatchTargets(
                            source.document,
                            test.at("target"));
                        passed = !matches.empty();
                        actual = matches.size();
                        detail = passed
                            ? "The target object exists."
                            : "The target object does not exist.";
                    }
                }
                else if (kind == "component-exists")
                {
                    if (!test.contains("target")
                        || !test.contains("type")
                        || !test.at("type").is_string())
                    {
                        detail =
                            "component-exists requires target and type.";
                    }
                    else
                    {
                        const auto matches = FindPatchTargets(
                            source.document,
                            test.at("target"));
                        for (const auto objectIndex : matches)
                        {
                            const auto& object = source.document
                                .at("objects")
                                .at(objectIndex);
                            const auto components = object.find("components");
                            if (components == object.end()
                                || !components->is_array())
                            {
                                continue;
                            }
                            for (const auto& component : *components)
                            {
                                if (component.is_object()
                                    && component.value(
                                        "type",
                                        std::string{})
                                        == test.at("type").get<std::string>())
                                {
                                    passed = true;
                                    break;
                                }
                            }
                            if (passed)
                            {
                                break;
                            }
                        }
                        actual = passed;
                        detail = passed
                            ? "The requested component exists."
                            : "The requested component does not exist.";
                    }
                }
                else if (kind == "object-count")
                {
                    const auto count = analysis.at("objectCount").get<std::size_t>();
                    actual = count;
                    if (test.contains("expected")
                        && test.at("expected").is_number_integer())
                    {
                        const auto expected = test.at("expected").get<std::size_t>();
                        passed = count == expected;
                        detail = "Expected object count "
                            + std::to_string(expected) + ".";
                    }
                    else
                    {
                        const auto minimum = test.value(
                            "min",
                            static_cast<std::size_t>(0));
                        const auto maximum = test.value(
                            "max",
                            std::numeric_limits<std::size_t>::max());
                        passed = count >= minimum && count <= maximum;
                        detail = "Expected object count in range.";
                    }
                }
                else if (kind == "asset-exists")
                {
                    if (!test.contains("path")
                        || !test.at("path").is_string())
                    {
                        detail = "asset-exists requires path.";
                    }
                    else
                    {
                        const auto requested = test.at("path").get<std::string>();
                        for (const auto& asset : analysis.at("assets"))
                        {
                            if (asset.value("path", std::string{}) == requested)
                            {
                                passed = asset.value("exists", false);
                                actual = asset;
                                break;
                            }
                        }
                        detail = passed
                            ? "The requested asset exists."
                            : "The requested asset is missing.";
                    }
                }
                else if (kind == "value-equals")
                {
                    if (!test.contains("target")
                        || !test.contains("path")
                        || !test.at("path").is_string()
                        || !test.contains("value"))
                    {
                        detail =
                            "value-equals requires target, path, and value.";
                    }
                    else
                    {
                        const auto targetIndex = FindSinglePatchTarget(
                            source.document,
                            test.at("target"));
                        const auto actualValue = FindPatchValue(
                            source.document.at("objects").at(targetIndex),
                            test.at("path").get<std::string>());
                        if (actualValue != nullptr)
                        {
                            actual = *actualValue;
                            passed = *actualValue == test.at("value");
                        }
                        detail = passed
                            ? "The value matches."
                            : "The value does not match.";
                    }
                }
                else
                {
                    detail = "Unknown test kind: " + kind;
                }
            }
            catch (const std::exception& exception)
            {
                detail = exception.what();
                passed = false;
            }
            if (passed)
            {
                ++passedCount;
            }
            assertions.push_back({
                { "name", name },
                { "kind", kind },
                { "ok", passed },
                { "detail", detail },
                { "actual", std::move(actual) },
            });
        }

        const bool allPassed = passedCount == assertions.size();
        nlohmann::json report{
            { "command", "test" },
            { "ok", allPassed },
            { "project", LamaPon::PathToUtf8(source.projectRoot) },
            { "scene", LamaPon::PathToUtf8(source.relativeScene) },
            { "specification", LamaPon::PathToUtf8(specificationFile) },
            { "passed", passedCount },
            { "failed", assertions.size() - passedCount },
            { "assertions", std::move(assertions) },
        };
        if (!reportPath.empty())
        {
            const auto output = ResolvePatchOutput(
                source.projectRoot,
                reportPath);
            report["report"] = LamaPon::PathToUtf8(output);
            WriteJsonFile(output, report);
        }
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return allPassed ? 0 : 1;
    }

}
