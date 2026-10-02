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
            // projectRoot: scene所属projectのcanonical root。
            std::filesystem::path projectRoot;
            // relativeScene: assetsからのscene相対path。
            std::filesystem::path relativeScene;
            // sceneFile: 読み込むscene fileのcanonical path。
            std::filesystem::path sceneFile;
            // document: migration済みscene JSON。
            nlohmann::json document;
        };

        // TryReadObjectId(value: JSON値, result: 読み取ったID): 正の64bit object IDだけを受理します。
        [[nodiscard]] bool TryReadObjectId(
            const nlohmann::json& value,
            std::uint64_t& result)
        {
            // JSON値が符号付き・符号なし整数かを確認します。
            if (!value.is_number_integer()
                && !value.is_number_unsigned())
            {
                // 整数でない値はobject IDとして拒否します。
                return false;
            }
            // 範囲外の整数変換を捕捉します。
            try
            {
                result = value.get<std::uint64_t>();
                // 0を予約値として拒否し、正のIDだけ成功にします。
                return result != 0;
            }
            // 範囲外のJSON整数は読み取り失敗として扱います。
            catch (const nlohmann::json::exception&)
            {
                // 変換失敗はfalseで呼び出し元へ返します。
                return false;
            }
        }

        // LoadSceneSource(requestedProject: project path, requestedScene: scene path): assets内sceneを読み込みmigrationします。
        [[nodiscard]] SceneSource LoadSceneSource(
            const std::filesystem::path& requestedProject,
            const std::filesystem::path& requestedScene)
        {
            // projectとsceneの基準になるcanonical root。
            const auto projectRoot =
                std::filesystem::weakly_canonical(
                    std::filesystem::absolute(requestedProject));
            // project設定ファイルのpath。
            const auto settingsPath =
                projectRoot / L".lamapon" / L"project.json";
            // 指定pathがLamaPon projectか確認します。
            if (!std::filesystem::is_regular_file(settingsPath))
            {
                // project設定がない場合は診断付きで中断します。
                throw std::runtime_error(
                    "The folder is not a LamaPon project"
                    " (missing .lamapon/project.json): "
                    + LamaPon::PathToUtf8(projectRoot));
            }

            // requested: 引数またはproject設定から選んだscene path。
            std::filesystem::path requested = requestedScene;
            // scene引数が省略された場合はstartup sceneを使います。
            if (requested.empty())
            {
                requested =
                    LamaPon::LoadProjectSettings(settingsPath)
                        .startupScene;
            }
            // project root基準にscene pathを正規化します。
            const auto relativeScene = NormalizeScenePath(
                projectRoot,
                requested).lexically_normal();
            // path containment検証に使うcanonical assets root。
            const auto assetsRoot =
                std::filesystem::weakly_canonical(
                    projectRoot / L"assets");
            // 読み込むscene fileのcanonical path。
            const auto sceneFile =
                std::filesystem::weakly_canonical(
                    assetsRoot / relativeScene);
            // sceneがassets rootから外れていないかrelative pathで確認します。
            const auto insideAssets =
                sceneFile.lexically_relative(assetsRoot);
            // scene pathがassets外へ出ていないか検査します。
            if (insideAssets.empty()
                || insideAssets.native().starts_with(L".."))
            {
                // assets外のscene pathを診断付きで拒否します。
                throw std::invalid_argument(
                    "The scene is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(sceneFile));
            }
            // scene fileが通常fileとして存在するか確認します。
            if (!std::filesystem::is_regular_file(sceneFile))
            {
                // 読めないscene fileを診断付きで拒否します。
                throw std::runtime_error(
                    "Could not open scene for reading: "
                    + LamaPon::PathToUtf8(sceneFile));
            }

            // input: scene JSONを読むbinary stream。
            std::ifstream input(sceneFile, std::ios::binary);
            // file全体を保持してJSON parserへ渡すbyte列。
            const std::string contents{
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{} };
            // document: parse直後のscene JSON。
            auto document = nlohmann::json::parse(contents);
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    document,
                    LamaPon::SerializedDocumentKind::Scene));
            // migration後のscene sourceを返します。
            return {
                projectRoot,
                relativeScene,
                sceneFile,
                std::move(document),
            };
        }

        // SceneProblem(kind: 種別, detail: 説明, objectId: 対象ID, objectName: 対象名): JSON診断entryを作ります。
        [[nodiscard]] nlohmann::json SceneProblem(
            const char* kind,
            const std::string& detail,
            const std::uint64_t objectId = 0,
            const std::string& objectName = {})
        {
            // severity・kind・detailを持つ問題entry。
            nlohmann::json problem{
                { "severity", "error" },
                { "kind", kind },
                { "detail", detail },
            };
            // object IDがある場合だけ診断へ含めます。
            if (objectId != 0)
            {
                problem["objectId"] = objectId;
            }
            // object名がある場合だけ診断へ含めます。
            if (!objectName.empty())
            {
                problem["object"] = objectName;
            }
            // 完成した診断entryを返します。
            return problem;
        }

        struct PrefabSource final
        {
            // projectRoot: prefab所属projectのcanonical root。
            std::filesystem::path projectRoot;
            // relativePrefab: assetsからのprefab相対path。
            std::filesystem::path relativePrefab;
            // prefabFile: 読み込むprefab fileのcanonical path。
            std::filesystem::path prefabFile;
            // document: migration済みprefab JSON。
            nlohmann::json document;
        };

        // LoadPrefabSource(requestedProject: project path, requestedPrefab: prefab path): assets内prefabを読み込みmigrationします。
        [[nodiscard]] PrefabSource LoadPrefabSource(
            const std::filesystem::path& requestedProject,
            const std::filesystem::path& requestedPrefab)
        {
            // prefabとprojectの基準になるcanonical root。
            const auto projectRoot = CanonicalProjectRoot(requestedProject);
            // prefab pathの必須引数を検査します。
            if (requestedPrefab.empty())
            {
                // path省略時は不足引数を通知して中断します。
                throw std::invalid_argument(
                    "prefab requires --path.");
            }
            // assets基準にprefab pathを正規化します。
            const auto relativePrefab = NormalizeAssetPath(
                projectRoot,
                requestedPrefab).lexically_normal();
            // path containment検証に使うcanonical assets root。
            const auto assetsRoot =
                std::filesystem::weakly_canonical(
                    projectRoot / L"assets");
            // 読み込むprefab fileのcanonical path。
            const auto prefabFile =
                std::filesystem::weakly_canonical(
                    assetsRoot / relativePrefab);
            // prefabがassets rootから外れていないかrelative pathで確認します。
            const auto insideAssets =
                prefabFile.lexically_relative(assetsRoot);
            // prefab pathがassets外へ出ていないか検査します。
            if (insideAssets.empty()
                || insideAssets.native().starts_with(L".."))
            {
                // assets外のprefab pathを診断付きで拒否します。
                throw std::invalid_argument(
                    "The prefab is outside the project's assets folder: "
                    + LamaPon::PathToUtf8(prefabFile));
            }
            // prefab fileが通常fileとして存在するか確認します。
            if (!std::filesystem::is_regular_file(prefabFile))
            {
                // 読めないprefab fileを診断付きで拒否します。
                throw std::runtime_error(
                    "Could not open prefab for reading: "
                    + LamaPon::PathToUtf8(prefabFile));
            }
            // document: parse直後のprefab JSON。
            auto document = ReadJsonFile(prefabFile);
            static_cast<void>(
                LamaPon::MigrateSerializedDocument(
                    document,
                    LamaPon::SerializedDocumentKind::Prefab));
            // migration後のprefab sourceを返します。
            return {
                projectRoot,
                relativePrefab,
                prefabFile,
                std::move(document),
            };
        }

        // AnalyzePrefab(source: migration済みprefab): object階層・components・asset参照を診断します。
        [[nodiscard]] nlohmann::json AnalyzePrefab(
            const PrefabSource& source)
        {
            // sourceから検証対象のprefab documentを参照します。
            const auto& document = source.document;
            // 問題・警告・検査済みobjectを蓄積する配列。
            nlohmann::json problems = nlohmann::json::array();
            // 今後のprefab警告を返す配列。
            nlohmann::json warnings = nlohmann::json::array();
            // 出力用に整形したobject一覧。
            nlohmann::json objects = nlohmann::json::array();
            // object IDからserializedObjects indexへの対応表。
            std::unordered_map<std::uint64_t, std::size_t> indices;
            // duplicate ID検出に使う集合。
            std::unordered_set<std::uint64_t> ids;
            // 検査したcomponent数。
            std::size_t componentCount{};
            // parentを持たないroot数。
            std::size_t rootCount{};
            // nested prefab reference数。
            std::size_t nestedPrefabCount{};

            // 文書formatがprefab用markerと一致するか検査します。
            if (document.value("format", std::string{})
                != "LamaPonPrefab")
            {
                problems.push_back(SceneProblem(
                    "invalid-format",
                    "The document format must be LamaPonPrefab."));
            }
            // objects fieldがobject配列か検査します。
            if (!document.contains("objects")
                || !document.at("objects").is_array())
            {
                problems.push_back(SceneProblem(
                    "invalid-objects",
                    "The prefab must contain an objects array."));
            }
            // object配列があれば各entryを検証します。
            else
            {
                // serializedObjects: 検査対象object配列。
                const auto& serializedObjects =
                    document.at("objects");
                // index: object IDと親子関係を検証する通番。
                for (std::size_t index{};
                    index < serializedObjects.size();
                    ++index)
                {
            // object: 現在検査しているserialized object entry。
                    const auto& object = serializedObjects.at(index);
                    // serialized objectがJSON objectか判定します。
                    if (!object.is_object())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object",
                            "Every prefab object must be an object."));
                        // object形式でないentryは処理対象から外します。
                        continue;
                    }
                    // objectName: 診断に表示するobject名。
                    const auto objectName = object.value(
                        "name",
                        std::string{});
                    // objectId: 読み取った正のobject ID。
                    std::uint64_t objectId{};
                    // IDが欠落または不正なら問題を記録します。
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
                    // IDが既出なら重複問題を記録します。
                    else if (!ids.insert(objectId).second)
                    {
                        problems.push_back(SceneProblem(
                            "duplicate-object-id",
                            "Prefab object ids must be unique.",
                            objectId,
                            objectName));
                    }
                    // 有効で一意なIDをindex表へ登録します。
                    else
                    {
                        indices.emplace(objectId, index);
                    }

                    // parent fieldを検査するためのiterator。
                    const auto parent = object.find("parent");
                    // parentがnullまたは欠落ならroot objectです。
                    if (parent == object.end()
                        || parent->is_null())
                    {
                        ++rootCount;
                    }
                    // parentがあるobjectのIDを検証します。
                    else
                    {
                        // parent fieldから読み取るobject ID。
                        std::uint64_t parentId{};
                        // parentが正の整数IDか確認します。
                        if (!TryReadObjectId(*parent, parentId))
                        {
                            problems.push_back(SceneProblem(
                                "invalid-parent",
                                "The prefab parent must be null or a positive object id.",
                                objectId,
                                objectName));
                        }
                        // object自身をparentに指定した循環を報告します。
                        else if (parentId == objectId)
                        {
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "A prefab object cannot be its own parent.",
                                objectId,
                                objectName));
                        }
                    }

                    // nested prefab pathを持つobjectを数えます。
                    if (object.contains("prefabAsset")
                        && object.at("prefabAsset").is_string())
                    {
                        ++nestedPrefabCount;
                    }
                    // inspection結果として返すobject概要。
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
                    // prefabAsset: nested prefab参照があれば出力へ保持します。
                    if (const auto prefabAsset = object.find("prefabAsset");
                        prefabAsset != object.end())
                    {
                        inspected["prefabAsset"] = *prefabAsset;
                    }
                    // transform: 明示値があればinspection結果へ保持します。
                    if (const auto transform = object.find("transform");
                        transform != object.end())
                    {
                        inspected["transform"] = *transform;
                    }
                    // component配列を取り出すiterator。
                    // components: objectのcomponent array field。
                    const auto components = object.find("components");
                    // components fieldがJSON配列か検査します。
                    if (components == object.end()
                        || !components->is_array())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-components",
                            "The prefab components field must be an array.",
                            objectId,
                            objectName));
                    }
                    // 有効なcomponent配列を数えて検査します。
                    else
                    {
                        componentCount += components->size();
                        // component: component配列内のentryをtype条件と照合します。
                        for (const auto& component : *components)
                        {
                            // componentがobjectでstring typeを持つか判定します。
                            if (!component.is_object()
                                || !component.contains("type")
                                || !component.at("type").is_string())
                            {
                                problems.push_back(SceneProblem(
                                    "invalid-component",
                                    "Every prefab component must have a string type.",
                                    objectId,
                                    objectName));
                                // 不正componentは出力へ含めず次へ進みます。
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

                // objectId/index: 全objectの親参照先が存在するか検査します。
                for (const auto& [objectId, index] : indices)
                {
                    // object: 親参照を確認する対象object。
                    const auto& object = serializedObjects.at(index);
                    // 親参照の有無を判定するiterator。
                    const auto parent = object.find("parent");
                    // parentがないroot objectには存在確認が不要です。
                    if (parent == object.end() || parent->is_null())
                    {
                        // root objectの親参照検査を飛ばします。
                        continue;
                    }
                    // 存在確認に使うparent object ID。
                    std::uint64_t parentId{};
                    // 親IDが有効で対応objectがない場合を報告します。
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

                // objectId/index: 各objectからrootまでの親chainを検査します。
                for (const auto& [objectId, index] : indices)
                {
                    // 循環検出用の訪問済みID集合。
                    std::unordered_set<std::uint64_t> visited;
                    // 親chainをたどる現在のobject ID。
                    auto current = objectId;
                    // 現在IDがprefab内にある間、親を追います。
                    while (indices.contains(current))
                    {
                        // IDを再訪した場合は親chainに循環があります。
                        if (!visited.insert(current).second)
                        {
                            // object: 循環を報告する開始object entry。
                            const auto& object =
                                serializedObjects.at(index);
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "The prefab hierarchy contains a cycle.",
                                objectId,
                                object.value("name", std::string{})));
                            // 循環を報告した後、このchainの探索を終えます。
                            break;
                        }
                        // currentObject: 親chain上の現在object entry。
                        const auto& currentObject =
                            serializedObjects.at(indices.at(current));
                        // 現在objectのparent field。
                        const auto parent = currentObject.find("parent");
                        // 親がない・不正な場合はchainがここで終わります。
                        if (parent == currentObject.end()
                            || parent->is_null()
                            || !TryReadObjectId(*parent, current))
                        {
                            // rootまたは不正parentで親chainを終えます。
                            break;
                        }
                    }
                }
            }

            // prefab documentが指すroot object ID。
            std::uint64_t rootId{};
            // root IDが既存objectを指すか検証します。
            if (!document.contains("root")
                || !TryReadObjectId(document.at("root"), rootId)
                || !indices.contains(rootId))
            {
                problems.push_back(SceneProblem(
                    "invalid-root",
                    "root must reference an existing prefab object."));
            }
            // root objectが解決できた場合、親と到達性を調べます。
            else
            {
                // root: rootIdで識別したroot object entry。
                const auto& root = document.at("objects")
                    .at(indices.at(rootId));
                // root objectが誤って親を持たないか確認するiterator。
                const auto rootParent = root.find("parent");
                // root objectにparentがあれば形式違反です。
                if (rootParent != root.end() && !rootParent->is_null())
                {
                    problems.push_back(SceneProblem(
                        "root-has-parent",
                        "The prefab root object must not have a parent.",
                        rootId,
                        root.value("name", std::string{})));
                }
                // objectId/index: 全objectが指定root配下に属するか検査します。
                for (const auto& [objectId, index] : indices)
                {
                    // 循環検出用の訪問済みID集合。
                    std::unordered_set<std::uint64_t> visited;
                    // rootまでたどる現在のobject ID。
                    auto current = objectId;
                    // 親chainがrootへ到達したかを記録します。
                    bool reachesRoot{};
                    // object IDがprefab内にある間、rootまで親をたどります。
                    while (indices.contains(current))
                    {
                        // 親IDを再訪した場合は循環なので探索を止めます。
                        if (!visited.insert(current).second)
                        {
                            // parent chainでIDを再訪したため、循環として探索を止めます。
                            break;
                        }
                        // currentがroot IDなら親chainの検証を完了します。
                        if (current == rootId)
                        {
                            reachesRoot = true;
                            // root到達を記録し、このobjectの探索を終えます。
                            break;
                        }
                        // 現在objectの親参照先。
                        const auto currentParent = document.at("objects")
                            .at(indices.at(current)).find("parent");
                        // 親が欠落・null・不正ならchainが終端です。
                        if (currentParent == document.at("objects")
                                .at(indices.at(current)).end()
                            || currentParent->is_null()
                            || !TryReadObjectId(*currentParent, current))
                        {
                            // 有効な親がないためroot探索を終えます。
                            break;
                        }
                    }
                    // rootへ到達しないobjectを問題として報告します。
                    if (!reachesRoot)
                    {
                        // object: rootへ到達できなかったobject entry。
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

            // documentから集めたasset参照の出力配列。
            nlohmann::json assets = nlohmann::json::array();
            // seenAssets: 重複asset pathを除くための集合。
            std::unordered_set<std::string> seenAssets;
            // assetPath: prefabが参照するassetを一件ずつ検証します。
            for (const auto& assetPath :
                LamaPon::CollectSerializedAssetPaths(document))
            {
                // 比較と出力に使うUTF-8 path。
                const auto normalized = LamaPon::PathToUtf8(assetPath);
                // 同じasset pathを重複出力しないよう判定します。
                if (!seenAssets.insert(normalized).second)
                {
                    // 重複assetは二重に確認せず次へ進みます。
                    continue;
                }
                // absolute pathは維持し、relative pathはassets基準で解決します。
                const auto resolved = assetPath.is_absolute()
                    ? assetPath
                    : source.projectRoot / L"assets" / assetPath;
                // 解決先のfile存在状態。
                const bool exists =
                    std::filesystem::is_regular_file(resolved);
                assets.push_back({
                    { "path", normalized },
                    { "exists", exists },
                });
                // 参照fileがない場合は診断を追加します。
                if (!exists)
                {
                    problems.push_back(SceneProblem(
                        "missing-asset",
                        "A serialized prefab asset reference does not exist: "
                            + normalized));
                }
            }

            // prefab解析結果と診断一覧を返します。
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

        // AnalyzeScene(source: migration済みscene): object階層・camera・component・asset参照を診断します。
        [[nodiscard]] nlohmann::json AnalyzeScene(
            const SceneSource& source)
        {
        // sourceから検査対象scene documentを参照します。
            const auto& document = source.document;
            // sceneで見つけたerror診断。
            nlohmann::json problems = nlohmann::json::array();
            // sceneに付随するwarning一覧。
            nlohmann::json warnings = nlohmann::json::array();
            // inspection出力用のobject概要一覧。
            nlohmann::json objects = nlohmann::json::array();
            // object IDからserialized object indexへの対応表。
            std::unordered_map<std::uint64_t, std::size_t> indices;
            // duplicate ID検出に使う集合。
            std::unordered_set<std::uint64_t> ids;
            // scene componentの総数。
            std::size_t componentCount{};
            // parentを持たないroot object数。
            std::size_t rootCount{};

            // document formatがscene用markerか検査します。
            if (document.value("format", std::string{})
                != "LamaPonScene")
            {
                problems.push_back(SceneProblem(
                    "invalid-format",
                    "The document format must be LamaPonScene."));
            }
            // objects fieldがJSON配列か検査します。
            if (!document.contains("objects")
                || !document.at("objects").is_array())
            {
                problems.push_back(SceneProblem(
                    "invalid-objects",
                    "The scene must contain an objects array."));
            }
            // object配列があれば各entryを検証します。
            else
            {
                // serializedObjects: 検査対象object配列。
                const auto& serializedObjects =
                    document.at("objects");
                // index: object IDと親子関係を検証する通番。
                for (std::size_t index{};
                    index < serializedObjects.size();
                    ++index)
                {
                    // object: sceneの現在のserialized object entry。
                    const auto& object = serializedObjects.at(index);
                    // serialized entryがJSON objectか判定します。
                    if (!object.is_object())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-object",
                            "Every entry in objects must be an object."));
                        // object形式でないentryを飛ばします。
                        continue;
                    }
                    // objectName: error診断に表示するobject名。
                    const auto objectName = object.value(
                        "name",
                        std::string{});
                    // objectId: 読み取った正のobject ID。
                    std::uint64_t objectId{};
                    // ID欠落または不正な整数を問題にします。
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
                    // 既出IDをduplicateとして報告します。
                    else if (!ids.insert(objectId).second)
                    {
                        problems.push_back(SceneProblem(
                            "duplicate-object-id",
                            "Object ids must be unique.",
                            objectId,
                            objectName));
                    }
                    // 有効かつ一意なIDをindex表に登録します。
                    else
                    {
                        indices.emplace(objectId, index);
                    }

                    // parent fieldを検査するiterator。
                    const auto parent = object.find("parent");
                    // parentなしまたはnullのobjectをrootとして数えます。
                    if (parent == object.end()
                        || parent->is_null())
                    {
                        ++rootCount;
                    }
                    // parent参照があるobjectのIDを検査します。
                    else
                    {
                        // parent fieldから読み取るobject ID。
                        std::uint64_t parentId{};
                        // parent値が正の整数IDでない場合を報告します。
                        if (!TryReadObjectId(*parent, parentId))
                        {
                            problems.push_back(SceneProblem(
                                "invalid-parent",
                                "The parent must be null or a positive object id.",
                                objectId,
                                objectName));
                        }
                        // object自身へのparent参照を循環として報告します。
                        else if (parentId == objectId)
                        {
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "An object cannot be its own parent.",
                                objectId,
                                objectName));
                        }
                    }

                    // inspection結果に含めるobject概要。
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
                    // transform: 明示された値があればinspection出力へ含めます。
                    if (const auto transform = object.find("transform");
                        transform != object.end())
                    {
                        inspected["transform"] = *transform;
                    }
                    // components fieldを検査するiterator。
                    // components: objectのcomponent array field。
                    const auto components = object.find("components");
                    // components fieldがJSON配列か確認します。
                    if (components == object.end()
                        || !components->is_array())
                    {
                        problems.push_back(SceneProblem(
                            "invalid-components",
                            "The components field must be an array.",
                            objectId,
                            objectName));
                    }
                    // 有効なcomponent配列を数えて出力します。
                    else
                    {
                        componentCount += components->size();
                        // component: component配列内のentryをtype条件と照合します。
                        for (const auto& component : *components)
                        {
                            // componentがobjectでstring typeを持つか判定します。
                            if (!component.is_object()
                                || !component.contains("type")
                                || !component.at("type").is_string())
                            {
                                problems.push_back(SceneProblem(
                                    "invalid-component",
                                    "Every component must have a string type.",
                                    objectId,
                                    objectName));
                                // 不正componentを出力せず次のentryへ進みます。
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

                // objectId/index: 全parent参照先の存在を検査します。
                for (const auto& [objectId, index] : indices)
                {
                    // object: 親参照を確認するscene object。
                    const auto& object = serializedObjects.at(index);
                    // serialized parent fieldを検索します。
                    const auto parent = object.find("parent");
                    // root objectはparent存在確認を省きます。
                    if (parent == object.end() || parent->is_null())
                    {
                        // root objectの参照先検査を飛ばします。
                        continue;
                    }
                    // 参照先として検証するparent ID。
                    std::uint64_t parentId{};
                    // parent IDが有効でscene内にない場合を報告します。
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

                // objectId/index: 各objectからrootまでparent chainを検査します。
                for (const auto& [objectId, index] : indices)
                {
                    // cycle検出用の訪問済みobject ID。
                    std::unordered_set<std::uint64_t> visited;
                    // parent chainをたどる現在のobject ID。
                    auto current = objectId;
                    // parent chainがscene内にある間たどり続けます。
                    while (indices.contains(current))
                    {
                        // 同じIDを再訪したらparent cycleです。
                        if (!visited.insert(current).second)
                        {
                            // object: 循環を報告する開始object entry。
                            const auto& object =
                                serializedObjects.at(index);
                            problems.push_back(SceneProblem(
                                "parent-cycle",
                                "The object hierarchy contains a cycle.",
                                objectId,
                                object.value(
                                    "name",
                                    std::string{})));
                            // cycleを報告してこのchainの探索を終えます。
                            break;
                        }
                        // currentObject: sceneの親chain上の現在object entry。
                        const auto& currentObject =
                            serializedObjects.at(indices.at(current));
                        // 現在objectのparent fieldを検索します。
                        const auto parent =
                            currentObject.find("parent");
                        // parentが欠落・null・不正ならchainが終端です。
                        if (parent == currentObject.end()
                            || parent->is_null()
                            || !TryReadObjectId(*parent, current))
                        {
                            // 有効なparentがないためchain探索を止めます。
                            break;
                        }
                    }
                }
            }

            // mainCamera: 設定がある場合は参照先objectを検証します。
            if (const auto mainCamera = document.find("mainCamera");
                mainCamera != document.end() && !mainCamera->is_null())
            {
                // sceneのmainCamera object ID。
                std::uint64_t cameraId{};
                // mainCameraが有効なscene objectを指すか確認します。
                if (!TryReadObjectId(*mainCamera, cameraId)
                    || !indices.contains(cameraId))
                {
                    problems.push_back(SceneProblem(
                        "missing-main-camera",
                        "mainCamera must reference an existing object."));
                }
                // mainCamera objectが解決した場合componentを調べます。
                else
                {
                    // cameraObject: main camera IDが指すobject entry。
                    const auto& cameraObject =
                        document.at("objects").at(indices.at(cameraId));
                    // objectにCamera componentがあるかの判定。
                    bool hasCamera{};
                    // components: camera objectにcomponent配列があるか調べます。
                    if (const auto components =
                            cameraObject.find("components");
                        components != cameraObject.end()
                        && components->is_array())
                    {
                        // component: component配列内のentryをtype条件と照合します。
                        for (const auto& component : *components)
                        {
                            // componentがCamera typeならmainCamera契約を満たします。
                            if (component.is_object()
                                && component.value(
                                    "type",
                                    std::string{})
                                    == "Camera")
                            {
                                hasCamera = true;
                                // Camera componentを見つけたら探索を終えます。
                                break;
                            }
                        }
                    }
                    // Camera componentがないobject参照を問題にします。
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

            // sceneから収集したasset参照の出力配列。
            nlohmann::json assets = nlohmann::json::array();
            // 重複asset pathを省くための集合。
            std::unordered_set<std::string> seenAssets;
            // assetPath: sceneが参照するassetを一件ずつ検証します。
            for (const auto& assetPath :
                LamaPon::CollectSerializedAssetPaths(document))
            {
                // 比較・出力用のUTF-8 asset path。
                const auto normalized =
                    LamaPon::PathToUtf8(assetPath);
                // 同じpathの重複検証を防ぎます。
                if (!seenAssets.insert(normalized).second)
                {
                    // 既出assetを再確認せず次へ進みます。
                    continue;
                }
                // absolute pathを保ち、relative pathはassets基準で解決します。
                const auto resolved = assetPath.is_absolute()
                    ? assetPath
                    : source.projectRoot / L"assets" / assetPath;
                // 解決先asset fileの存在状態。
                const bool exists =
                    std::filesystem::is_regular_file(resolved);
                assets.push_back({
                    { "path", normalized },
                    { "exists", exists },
                });
                // 存在しないasset referenceをerrorに追加します。
                if (!exists)
                {
                    problems.push_back(SceneProblem(
                        "missing-asset",
                        "A serialized asset reference does not exist: "
                            + normalized));
                }
            }

            // scene解析結果と診断を返します。
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

    // RunInspect(projectRoot: project root, scene: 相対scene path): scene解析reportをJSON出力します。
    [[nodiscard]] int RunInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        // source: source: 読み込んだsceneと解析対象documentを保持します。
        const auto source = LoadSceneSource(projectRoot, scene);
        // inspection JSONに追加するscene診断。
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
        // inspectが成功したことをexit code 0で返します。
        return 0;
    }

    // RunValidate(projectRoot: project root, scene: 相対scene path): sceneのerror診断を検証しexit codeを返します。
    [[nodiscard]] int RunValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene)
    {
        // source: source: 読み込んだsceneと解析対象documentを保持します。
        const auto source = LoadSceneSource(projectRoot, scene);
        // validation結果へcommand・path情報を追加します。
        auto analysis = AnalyzeScene(source);
        // error診断が空ならsceneは有効です。
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
        // sceneにerrorがあれば1、なければ0を返します。
        return valid ? 0 : 1;
    }

    // RunPrefabInspect(projectRoot: project root, prefab: 相対prefab path): prefab解析reportをJSON出力します。
    [[nodiscard]] int RunPrefabInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab)
    {
        // source: inspect対象prefabを読み込みます。
        const auto source = LoadPrefabSource(projectRoot, prefab);
        // inspection JSONにprefab解析結果を追加します。
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
        // prefab inspect成功をexit code 0で返します。
        return 0;
    }

    // RunPrefabValidate(projectRoot: project root, prefab: 相対prefab path): prefab診断を検証しexit codeを返します。
    [[nodiscard]] int RunPrefabValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab)
    {
        // source: validate対象prefabを読み込みます。
        const auto source = LoadPrefabSource(projectRoot, prefab);
        // validation結果へcommand・path情報を追加します。
        auto analysis = AnalyzePrefab(source);
        // error診断が空ならprefabは有効です。
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
        // prefabにerrorがあれば1、なければ0を返します。
        return valid ? 0 : 1;
    }

    namespace
    {
        // SplitPatchPath(path: JSON dot path): key/index segmentへ分割し、空segmentを拒否します。
        [[nodiscard]] std::vector<std::string> SplitPatchPath(
            const std::string& path)
        {
            // 空pathは分割可能なpatch pathではありません。
            if (path.empty())
            {
                // 必須pathの不足を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "A patch path must not be empty.");
            }
            // parts: pathから分けたkey/index segment列。
            std::vector<std::string> parts;
            // begin: 現在segmentの開始byte offset。
            std::size_t begin{};
            // pathの全segmentを走査します。
            while (begin <= path.size())
            {
                // 次のdotまたはpath末尾までの位置。
                const auto end = path.find('.', begin);
                // segmentの文字数。
                const auto length = end == std::string::npos
                    ? path.size() - begin
                    : end - begin;
                // 空segmentを含むpathを拒否します。
                if (length == 0)
                {
                    // 不正pathを入力値と共に拒否します。
                    throw std::invalid_argument(
                        "A patch path must not contain empty segments: "
                        + path);
                }
                parts.push_back(path.substr(begin, length));
                // 最終segmentに達したかを確認します。
                if (end == std::string::npos)
                {
                    // 残りsegmentがないため分割を終えます。
                    break;
                }
                begin = end + 1;
            }
            // 検証済みpath segment列を返します。
            return parts;
        }

        // PatchArrayIndex(token: index候補, path: 元path): 配列indexを検証して返します。
        [[nodiscard]] std::size_t PatchArrayIndex(
            const std::string& token,
            const std::string& path)
        {
            // tokenを整数に変換してindex範囲を検査します。
            try
            {
                // 数値parserが消費したtoken文字数。
                std::size_t consumed{};
                // tokenをunsigned integerとして解析した値。
                const auto value = std::stoull(token, &consumed);
                // token全体が有効なsize_t範囲の整数か検査します。
                if (consumed != token.size()
                    || value > std::numeric_limits<std::size_t>::max())
                {
                    // 配列indexでない数値を拒否します。
                    throw std::invalid_argument("not an array index");
                }
                // 検証済み配列indexをsize_tへ変換して返します。
                return static_cast<std::size_t>(value);
            }
            // 数値変換失敗をpatch pathの診断へ置き換えます。
            catch (const std::exception&)
            {
                // 不正な配列indexとpathを示して拒否します。
                throw std::invalid_argument(
                    "Patch path segment is not a valid array index: "
                    + token + " (path: " + path + ")");
            }
        }

        // SetPatchValue(root: 更新対象JSON, path: dot path, value: 置換値): 指定pathのJSON valueを更新します。
        void SetPatchValue(
            nlohmann::json& root,
            const std::string& path,
            const nlohmann::json& value)
        {
            // parts: SplitPatchPathが返したkey/index segment列。
            const auto parts = SplitPatchPath(path);
            // current: 更新対象をたどるJSON node。
            nlohmann::json* current = &root;
            // index: path segmentの位置を先頭から進めます。
            for (std::size_t index{}; index < parts.size(); ++index)
            {
                // 現在segmentが指定pathの最後かを判定します。
                const bool last = index + 1 == parts.size();
                // part: 現在処理するobject keyまたはarray index。
                const auto& part = parts.at(index);
                // object nodeならkeyとして現在segmentを使います。
                if (current->is_object())
                {
                    // final keyでは既存値をreplacementで置き換えます。
                    if (last)
                    {
                        (*current)[part] = value;
                        // 更新完了後にcallerへ戻ります。
                        return;
                    }
                    // 中間object keyがない場合は空objectを作ります。
                    if (!current->contains(part))
                    {
                        (*current)[part] = nlohmann::json::object();
                    }
                    current = &(*current)[part];
                    // object nodeのkey更新後、次segmentへ進みます。
                    continue;
                }
                // array nodeならsegmentを数値indexとして解釈します。
                if (current->is_array())
                {
                    // arrayIndex: 検証済みの配列要素番号。
                    const auto arrayIndex = PatchArrayIndex(part, path);
                    // array indexが現在の配列範囲内か検査します。
                    if (arrayIndex >= current->size())
                    {
                        // 範囲外indexをpath付きで拒否します。
                        throw std::invalid_argument(
                            "Patch path array index is out of range: "
                            + path);
                    }
                    // final array indexでは要素を置き換えます。
                    if (last)
                    {
                        current->at(arrayIndex) = value;
                        // 更新完了後にcallerへ戻ります。
                        return;
                    }
                    current = &current->at(arrayIndex);
                    // array nodeの要素を選択して次segmentへ進みます。
                    continue;
                }
                // scalar nodeを横切るpathは更新できません。
                throw std::invalid_argument(
                    "Patch path crosses a scalar value: " + path);
            }
        }

        // FindPatchValue(root: 検索対象JSON, path: dot path): pathが解決するvalueまたはnullptrを返します。
        [[nodiscard]] const nlohmann::json* FindPatchValue(
            const nlohmann::json& root,
            const std::string& path)
        {
            // parts: SplitPatchPathが返したkey/index segment列。
            const auto parts = SplitPatchPath(path);
            // current: 検索中のJSON node。
            const nlohmann::json* current = &root;
            // 各segmentを順に解決してpathをたどります。
            // part: rootから順に解決するkey/index segment。
            for (const auto& part : parts)
            {
                // object nodeからkeyを検索します。
                if (current->is_object())
                {
                    // 現在object内のkey検索結果。
                    const auto iterator = current->find(part);
                    // keyがなければpathは未解決です。
                    if (iterator == current->end())
                    {
                        // 欠落keyをnull pointerで示します。
                        return nullptr;
                    }
                    current = &*iterator;
                    // object keyを解決し次segmentへ進みます。
                    continue;
                }
                // array nodeからindexを解決します。
                if (current->is_array())
                {
                    // arrayIndex: 範囲検査用の数値index。
                    const auto arrayIndex = PatchArrayIndex(part, path);
                    // indexが配列範囲を超えていないか確認します。
                    if (arrayIndex >= current->size())
                    {
                        // 範囲外indexは未解決pathとして返します。
                        return nullptr;
                    }
                    current = &current->at(arrayIndex);
                    // array elementを選び次segmentへ進みます。
                    continue;
                }
                // objectでもarrayでもないnodeでpathが終わります。
                return nullptr;
            }
            // 全segmentを解決したnodeを返します。
            return current;
        }

        // FindPatchTargets(document: scene JSON, target: object selector): 条件に一致するobject indexを返します。
        [[nodiscard]] std::vector<std::size_t> FindPatchTargets(
            const nlohmann::json& document,
            const nlohmann::json& target)
        {
            // document内のobjects fieldを探します。
            const auto objects = document.find("objects");
            // patch前提となるobjects arrayを検証します。
            if (objects == document.end() || !objects->is_array())
            {
                // object配列がなければpatch targetを解決できません。
                throw std::invalid_argument(
                    "Scene objects must be an array before applying a patch.");
            }


            // id: targetで指定されたobject ID。
            std::optional<std::uint64_t> id;
            // name: targetで指定されたobject名。
            std::optional<std::string> name;
            // object targetからidまたはnameを読み取ります。
            if (target.is_object())
            {
                // target id fieldが指定されているか確認します。
                // value: selector内で見つけたid field。
                if (const auto value = target.find("id");
                    value != target.end())
                {
                    // parsed: positive integerとして読んだtarget ID。
                    std::uint64_t parsed{};
                    // target idは正の整数でなければなりません。
                    if (!TryReadObjectId(*value, parsed))
                    {
                        // 不正なtarget IDを入力エラーとして拒否します。
                        throw std::invalid_argument(
                            "Patch target id must be a positive integer.");
                    }
                    id = parsed;
                }
                // target name fieldが指定されているか確認します。
                // value: selector内で見つけたname field。
                if (const auto value = target.find("name");
                    value != target.end())
                {
                    // target nameをstring以外で指定していないか検査します。
                    if (!value->is_string())
                    {
                        // 不正なtarget nameを入力エラーとして拒否します。
                        throw std::invalid_argument(
                            "Patch target name must be a string.");
                    }
                    name = value->get<std::string>();
                }
            }
            // integer targetをobject IDとして扱います。
            else if (target.is_number_integer()
                || target.is_number_unsigned())
            {
                // parsed: target JSONから読み取ったobject ID。
                std::uint64_t parsed{};
                // target JSONが正のobject IDか確認します。
                if (!TryReadObjectId(target, parsed))
                {
                    // 不正なinteger targetを拒否します。
                    throw std::invalid_argument(
                        "Patch target id must be a positive integer.");
                }
                id = parsed;
            }
            // string targetをobject nameとして扱います。
            else if (target.is_string())
            {
                name = target.get<std::string>();
            }
            // targetにidかnameの少なくとも一方があるか確認します。
            if (!id.has_value() && !name.has_value())
            {
                // 識別条件のないtargetを拒否します。
                throw std::invalid_argument(
                    "Patch target must contain id or name.");
            }


            // 一致したobject indexを蓄積する配列。
            std::vector<std::size_t> matches;
            // index/object: 各scene objectをselector条件と照合します。
            for (std::size_t index{}; index < objects->size(); ++index)
            {
                // object: selector条件と比較するscene entry。
                const auto& object = objects->at(index);
                // target候補がJSON objectであることを確認します。
                if (!object.is_object())
                {
                    // JSON objectでないentryは候補から外します。
                    continue;
                }
                // objectId: 比較用に読み取ったpositive object ID。
                std::uint64_t objectId{};
                // idMatches: target IDとobject IDが一致したか。
                const bool idMatches = id.has_value()
                    && object.contains("id")
                    && TryReadObjectId(object.at("id"), objectId)
                    && objectId == id.value();
                // nameMatches: target nameとobject nameが一致したか。
                const bool nameMatches = name.has_value()
                    && object.value("name", std::string{}) == name.value();
                // 指定した全条件を満たさないobjectは除外します。
                if ((id.has_value() && !idMatches)
                    || (name.has_value() && !nameMatches))
                {
                    // 一致しないobjectを結果から外します。
                    continue;
                }
                matches.push_back(index);
            }
            // target条件に一致したobject index列を返します。
            return matches;
        }

        // FindSinglePatchTarget(document: scene JSON, target: selector): 一意に一致するobject indexを返します。
        [[nodiscard]] std::size_t FindSinglePatchTarget(
            const nlohmann::json& document,
            const nlohmann::json& target)
        {
            // matches: selectorに一致したobject index列。
            const auto matches = FindPatchTargets(document, target);
            // 一致objectがない場合はtarget errorにします。
            if (matches.empty())
            {
                // 解決できないtargetを呼び出し元へ通知します。
                throw std::invalid_argument(
                    "Patch target did not match any object.");
            }
            // 一致objectが一件だけか検査します。
            if (matches.size() != 1)
            {
                // 曖昧なtargetを一意IDで指定するよう求めます。
                throw std::invalid_argument(
                    "Patch target matched multiple objects; use a unique id.");
            }
            // 唯一一致したobject indexを返します。
            return matches.front();
        }

        // PatchParentId(document: scene JSON, parent: parent selector): nullまたはresolved parent IDを返します。
        [[nodiscard]] nlohmann::json PatchParentId(
            const nlohmann::json& document,
            const nlohmann::json& parent)
        {
            // parentがnullならroot parentとしてそのまま返します。
            if (parent.is_null())
            {
                // 親selectorがnullならJSON nullを返します。
                return nullptr;
            }
            // index: parent selectorで解決したobject位置。
            const auto index = FindSinglePatchTarget(document, parent);
            // 解決したparent objectのIDを返します。
            return document.at("objects").at(index).at("id");
        }

        // AllocatePatchObjectId(document: scene JSON, nextId: 採番状態): 未使用の正のobject IDを返します。
        [[nodiscard]] std::uint64_t AllocatePatchObjectId(
            const nlohmann::json& document,
            std::uint64_t& nextId)
        {
            // objects: ID重複を調べるserialized object配列。
            const auto& objects = document.at("objects");
            // 未使用IDが見つかるまで採番候補を進めます。
            while (true)
            {
                // 0と最大値は次IDへ進めないため拒否します。
                if (nextId == 0
                    || nextId == std::numeric_limits<std::uint64_t>::max())
                {
                    // ID空間を使い切ったことを呼び出し元へ通知します。
                    throw std::runtime_error(
                        "No unused object id is available.");
                }
                // used: candidate IDが既存objectに使われたか。
                bool used{};
                // object: candidateとのID重複を調べるentry。
                for (const auto& object : objects)
                {
                    // objectId: 現在entryから読み取ったID。
                    std::uint64_t objectId{};
                    // 既存entryがcandidate IDを使っているか調べます。
                    if (object.is_object()
                        && object.contains("id")
                        && TryReadObjectId(object.at("id"), objectId)
                        && objectId == nextId)
                    {
                        used = true;
                        // candidateを使用中と判定したら残りを調べません。
                        break;
                    }
                }
                // candidate IDが未使用なら呼び出し元へ返します。
                if (!used)
                {
                    // 未使用IDを返し、次回用の採番値を進めます。
                    return nextId++;
                }
                ++nextId;
            }
        }

        // ApplyPatchOperation(document: mutable scene JSON, operation: patch JSON, nextId: 次のobject ID): operationを検証して適用し、変更件数を返します。
        [[nodiscard]] std::size_t ApplyPatchOperation(
            nlohmann::json& document,
            const nlohmann::json& operation,
            std::uint64_t& nextId)
        {
            // operationがJSON objectでstring opを持つか検査します。
            if (!operation.is_object()
                || !operation.contains("op")
                || !operation.at("op").is_string())
            {
                // 不正なoperation descriptorを入力errorとして拒否します。
                throw std::invalid_argument(
                    "Every patch operation must contain a string op.");
            }
            // kind: dispatchするpatch operation名。
            const auto kind = operation.at("op").get<std::string>();
            // set-scene operationのdocument更新を処理します。
            if (kind == "set-scene")
            {
                // pathとvalueが更新要求に含まれるか検証します。
                if (!operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    // 不足または不正なscene path/valueを拒否します。
                    throw std::invalid_argument(
                        "set-scene requires path and value.");
                }
                SetPatchValue(
                    document,
                    operation.at("path").get<std::string>(),
                    operation.at("value"));
                // scene valueを一件更新したことを返します。
                return 1;
            }

            // add-object operationの新規object作成を処理します。
            if (kind == "add-object")
            {
                // object: operationから作成するserialized object。
                nlohmann::json object = operation.value(
                    "object",
                    nlohmann::json::object());
                // 新objectのdescriptorがJSON objectか確認します。
                if (!object.is_object())
                {
                    // object形式でない新規objectを拒否します。
                    throw std::invalid_argument(
                        "add-object object must be a JSON object.");
                }
                // objectId: 新規objectに設定する正のID。
                std::uint64_t objectId{};
                // 明示IDがある場合は形式と重複を検査します。
                if (object.contains("id"))
                {
                    // 指定object IDが正の整数か確認します。
                    if (!TryReadObjectId(object.at("id"), objectId))
                    {
                        // 不正なobject IDを入力errorとして拒否します。
                        throw std::invalid_argument(
                            "add-object id must be a positive integer.");
                    }
                    // existing: 新規IDとの重複を調べるobject。
                    for (const auto& existing : document.at("objects"))
                    {
                        // existingId: 現在の既存object ID。
                        std::uint64_t existingId{};
                        // existing objectが新IDを使用中か確認します。
                        if (existing.is_object()
                            && existing.contains("id")
                            && TryReadObjectId(
                                existing.at("id"),
                                existingId)
                            && existingId == objectId)
                        {
                            // 使用中のobject IDを再利用させず拒否します。
                            throw std::invalid_argument(
                                "add-object id is already in use.");
                        }
                    }
                    // 明示IDが採番候補を越えた場合は次IDを進めます。
                    if (objectId >= nextId)
                    {
                        nextId = objectId + 1;
                    }
                }
                // ID省略時は未使用IDを割り当てます。
                else
                {
                    objectId = AllocatePatchObjectId(document, nextId);
                    object["id"] = objectId;
                }
                // object name省略時の既定値を補います。
                if (!object.contains("name"))
                {
                    object["name"] = "GameObject";
                }
                // operation側にname overrideがあるか確認します。
                if (operation.contains("name"))
                {
                    // override nameがstringか検証します。
                    if (!operation.at("name").is_string())
                    {
                        // stringでないobject nameを拒否します。
                        throw std::invalid_argument(
                            "add-object name must be a string.");
                    }
                    object["name"] = operation.at("name");
                }
                // enabled省略時はobjectを有効にします。
                if (!object.contains("enabled"))
                {
                    object["enabled"] = true;
                }
                // operation側のenabled overrideを検査します。
                if (operation.contains("enabled"))
                {
                    // enabled overrideがbooleanか確認します。
                    if (!operation.at("enabled").is_boolean())
                    {
                        // booleanでないenabled overrideを拒否します。
                        throw std::invalid_argument(
                            "add-object enabled must be boolean.");
                    }
                    object["enabled"] = operation.at("enabled");
                }
                // operationがparent selectorを指定したか確認します。
                if (operation.contains("parent"))
                {
                    object["parent"] = PatchParentId(
                        document,
                        operation.at("parent"));
                }
                // operationにparentがなければobject側の値を解決します。
                else if (object.contains("parent"))
                {
                    object["parent"] = PatchParentId(
                        document,
                        object.at("parent"));
                }
                // どちらにもparentがなければroot objectにします。
                else
                {
                    object["parent"] = nullptr;
                }
                // transform省略時はscene既定値を補います。
                if (!object.contains("transform"))
                {
                    object["transform"] = {
                        { "position", { 0.0, 0.0, 0.0 } },
                        { "rotation", { 0.0, 0.0, 0.0 } },
                        { "scale", { 1.0, 1.0, 1.0 } },
                    };
                }
                // operationにtransform overrideがあれば適用します。
                if (operation.contains("transform"))
                {
                    object["transform"] = operation.at("transform");
                }
                // components省略時は空配列を補います。
                if (!object.contains("components"))
                {
                    object["components"] = nlohmann::json::array();
                }
                // operation側にcomponents overrideがあれば適用します。
                if (operation.contains("components"))
                {
                    object["components"] = operation.at("components");
                }
                document["objects"].push_back(std::move(object));
                // 新objectを追加したことを返します。
                return 1;
            }

            // target selectorが指定されているか確認します。
            if (!operation.contains("target"))
            {
                // 対象がないpatch operationを拒否します。
                throw std::invalid_argument(
                    kind + " requires target.");
            }
            // target selectorで解決したscene object index。
            // targetIndex: assertion targetで解決したobject位置。
            const auto targetIndex = FindSinglePatchTarget(
                document,
                operation.at("target"));
            // object: target selectorで解決したscene entry。
            auto& object = document.at("objects").at(targetIndex);

            // 既存objectのproperty更新を処理します。
            if (kind == "set")
            {
                // pathとreplacement valueの指定を検証します。
                if (!operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    // 不完全なset operationを拒否します。
                    throw std::invalid_argument(
                        "set requires path and value.");
                }
                // JSON rootから更新するdot path。
                const auto path = operation.at("path").get<std::string>();
                // idとparentの直接変更を禁止します。
                if (path == "id" || path == "parent")
                {
                    // hierarchy fieldの変更を専用operationへ誘導します。
                    throw std::invalid_argument(
                        "Use add-object or reparent to change object hierarchy and ids.");
                }
                SetPatchValue(object, path, operation.at("value"));
                // object propertyを一件更新したことを返します。
                return 1;
            }

            // object name変更operationを処理します。
            if (kind == "rename")
            {
                // nameがstringとして指定されているか確認します。
                if (!operation.contains("name")
                    || !operation.at("name").is_string())
                {
                    // 不正なrename要求を拒否します。
                    throw std::invalid_argument(
                        "rename requires a string name.");
                }
                object["name"] = operation.at("name");
                // object nameを変更したことを返します。
                return 1;
            }

            // parent変更operationを処理します。
            if (kind == "reparent")
            {
                // parent selectorまたはnullが指定されているか確認します。
                if (!operation.contains("parent"))
                {
                    // parent省略のreparent要求を拒否します。
                    throw std::invalid_argument(
                        "reparent requires parent, or null for a root object.");
                }
                object["parent"] = PatchParentId(
                    document,
                    operation.at("parent"));
                // object parentを更新したことを返します。
                return 1;
            }

            // component追加operationを処理します。
            if (kind == "add-component")
            {
                // 追加componentのtypeが非空stringか検査します。
                if (!operation.contains("type")
                    || !operation.at("type").is_string()
                    || operation.at("type").get<std::string>().empty())
                {
                    // typeのないcomponent追加を拒否します。
                    throw std::invalid_argument(
                        "add-component requires a non-empty type.");
                }
                // component: operationから作るcomponent JSON。
                nlohmann::json component = operation.value(
                    "data",
                    nlohmann::json::object());
                // component dataがJSON objectか確認します。
                if (!component.is_object())
                {
                    // objectでないcomponent dataを拒否します。
                    throw std::invalid_argument(
                        "add-component data must be a JSON object.");
                }
                component["type"] = operation.at("type");
                // enabled省略時のcomponent既定値を補います。
                if (!component.contains("enabled"))
                {
                    component["enabled"] = true;
                }
                ValidateComponentObject(
                    operation.at("type").get<std::string>(),
                    component);
                // component配列がなければ空配列を初期化します。
                if (!object.contains("components")
                    || !object.at("components").is_array())
                {
                    object["components"] = nlohmann::json::array();
                }
                object["components"].push_back(std::move(component));
                // componentを追加したことを返します。
                return 1;
            }

            // remove-component operationで対象typeのcomponentを削除します。
            if (kind == "remove-component")
            {
                // typeがcomponent selectorとして指定されているか確認します。
                if (!operation.contains("type")
                    || !operation.at("type").is_string())
                {
                    // typeのないremove-component要求を拒否します。
                    throw std::invalid_argument(
                        "remove-component requires a type.");
                }
                // components: 対象objectのcomponent配列。
                auto& components = object["components"];
                // 対象objectのcomponents fieldが配列か検証します。
                if (!components.is_array())
                {
                    // 壊れたcomponent配列を削除前に拒否します。
                    throw std::invalid_argument(
                        "The target object's components must be an array.");
                }
                // type: 削除するcomponent type名。
                const auto type = operation.at("type").get<std::string>();
                // matches: 指定typeに一致するcomponent index列。
                std::vector<std::size_t> matches;
                // componentIndex: 各componentをtype selectorと照合します。
                for (std::size_t componentIndex{};
                    componentIndex < components.size();
                    ++componentIndex)
                {
                    // component: operationのtypeと照合するentry。
                    const auto& component = components.at(componentIndex);
                    // componentがobjectで指定typeに一致するか調べます。
                    if (component.is_object()
                        && component.value("type", std::string{}) == type)
                    {
                        matches.push_back(componentIndex);
                    }
                }
                // selectorに一致するcomponentがあるか確認します。
                if (matches.empty())
                {
                    // 一致componentがない削除要求を拒否します。
                    throw std::invalid_argument(
                        "remove-component did not match the requested type.");
                }
                // 複数一致時にall:trueが指定されたか確認します。
                if (matches.size() > 1
                    && !operation.value("all", false))
                {
                    // 曖昧なcomponent削除をall指定なしで拒否します。
                    throw std::invalid_argument(
                        "remove-component matched multiple components; use all:true.");
                }
                // matchesを逆順に消して後続indexのずれを防ぎます。
                // iterator: 削除するcomponent indexを逆順にたどります。
                for (auto iterator = matches.rbegin();
                    iterator != matches.rend();
                    ++iterator)
                {
                    components.erase(components.begin() + *iterator);
                }
                // component削除後の変更件数を返します。
                return 1;
            }

            // set-component operationで単一componentのfieldを更新します。
            if (kind == "set-component")
            {
                // type・path・valueが揃った更新要求か検証します。
                if (!operation.contains("type")
                    || !operation.at("type").is_string()
                    || !operation.contains("path")
                    || !operation.at("path").is_string()
                    || !operation.contains("value"))
                {
                    // 必要なfieldがないset-component要求を拒否します。
                    throw std::invalid_argument(
                        "set-component requires type, path, and value.");
                }
                // component typeはfield更新で変更できないか確認します。
                if (operation.at("path").get<std::string>() == "type")
                {
                    // type変更を専用operationなしで行う要求を拒否します。
                    throw std::invalid_argument(
                        "set-component cannot change component type.");
                }
                // components: 対象objectのcomponent配列。
                auto& components = object["components"];
                // 対象objectのcomponents fieldが配列か確認します。
                if (!components.is_array())
                {
                    // 壊れたcomponent配列への更新を拒否します。
                    throw std::invalid_argument(
                        "The target object's components must be an array.");
                }
                // type: 更新対象componentのtype名。
                const auto type = operation.at("type").get<std::string>();
                // matches: 指定typeに一致するcomponent index列。
                std::vector<std::size_t> matches;
                // componentIndex: selectorに一致するcomponent位置を探します。
                for (std::size_t componentIndex{};
                    componentIndex < components.size();
                    ++componentIndex)
                {
                    // component: operationのtypeと照合するentry。
                    const auto& component = components.at(componentIndex);
                    // componentが指定typeに一致するか調べます。
                    if (component.is_object()
                        && component.value("type", std::string{}) == type)
                    {
                        matches.push_back(componentIndex);
                    }
                }
                // 更新先componentが一件だけか検証します。
                if (matches.size() != 1)
                {
                    // 一致なし・複数一致の更新先を拒否します。
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
                // component fieldを更新したことを返します。
                return 1;
            }

            // remove-object operationでobjectと必要な子孫を削除します。
            if (kind == "remove-object")
            {
                // removedId: 削除対象objectの正のID。
                std::uint64_t removedId{};
                // 削除対象に有効なobject IDがあるか確認します。
                if (!object.contains("id")
                    || !TryReadObjectId(object.at("id"), removedId))
                {
                    // IDを特定できないobject削除を拒否します。
                    throw std::invalid_argument(
                        "remove-object target must have a valid id.");
                }
                // recursive: 子孫objectも削除するかのflag。
                const bool recursive = operation.value("recursive", false);
                // removedIds: 削除するobjectと子孫のID集合。
                std::unordered_set<std::uint64_t> removedIds{ removedId };
                // changed: 直近の反復で子孫が追加されたか。
                bool changed{};
                // recursive時は削除対象の子孫ID集合を固定点まで広げます。
                do
                {
                    changed = false;
                    // candidate: removedIdsの子か調べるscene object。
                    for (const auto& candidate : document.at("objects"))
                    {
                        // candidateがobject IDを持つJSON objectか確認します。
                        if (!candidate.is_object()
                            || !candidate.contains("id"))
                        {
                            // 不正entryは子孫検査から除外します。
                            continue;
                        }
                        // candidateId: 現在のchild candidate ID。
                        std::uint64_t candidateId{};
                        // ID不正または既に削除対象のcandidateを飛ばします。
                        if (!TryReadObjectId(
                                candidate.at("id"),
                                candidateId)
                            || removedIds.contains(candidateId))
                        {
                            // 既存または不正なcandidateを次へ進めます。
                            continue;
                        }
                        // candidateのparent field検索結果。
                        const auto parent = candidate.find("parent");
                        // parentId: 削除対象集合との照合に使う親ID。
                        std::uint64_t parentId{};
                        // candidateの親が削除対象ならchild IDを追加します。
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

                // 非recursive削除で子を残す状態にならないか調べます。
                if (!recursive && removedIds.size() == 1)
                {
                    // candidate: 削除対象objectへの子参照を探します。
                    for (const auto& candidate : document.at("objects"))
                    {
                        // objectでないentryは子判定から除外します。
                        if (!candidate.is_object())
                        {
                            // 不正entryのparent fieldを検査しません。
                            continue;
                        }
                        // parentId: 削除対象objectとの比較に使う親ID。
                        std::uint64_t parentId{};
                        // candidateの親参照を検索します。
                        const auto parent = candidate.find("parent");
                        // 直接の子が残る場合は削除を拒否します。
                        if (parent != candidate.end()
                            && !parent->is_null()
                            && TryReadObjectId(*parent, parentId)
                            && parentId == removedId)
                        {
                            // 子を残さないようrecursive削除を要求します。
                            throw std::invalid_argument(
                                "remove-object has children; use recursive:true.");
                        }
                    }
                }

                // objects: 削除対象を除去するscene object配列。
                auto& objects = document["objects"];
                // objects配列から削除対象IDを消します。
                // iterator: 削除対象をeraseしながらobjectsを走査します。
                for (auto iterator = objects.begin();
                    iterator != objects.end();)
                {
                    // candidateId: 現在のiterator entryから読むID。
                    std::uint64_t candidateId{};
                    // object IDがremovedIdsに含まれるか検査します。
                    if (iterator->is_object()
                        && iterator->contains("id")
                        && TryReadObjectId(
                            iterator->at("id"),
                            candidateId)
                        && removedIds.contains(candidateId))
                    {
                        iterator = objects.erase(iterator);
                    }
                    // 削除対象外のentryはそのまま残します。
                    else
                    {
                        ++iterator;
                    }
                }
                // mainCameraが削除対象objectを指すか確認します。
                if (document.contains("mainCamera")
                    && !document.at("mainCamera").is_null())
                {
                    // cameraId: sceneが参照するmain camera ID。
                    std::uint64_t cameraId{};
                    // 削除対象cameraへの参照をnullへ差し替えます。
                    if (TryReadObjectId(
                            document.at("mainCamera"),
                            cameraId)
                        && removedIds.contains(cameraId))
                    {
                        document["mainCamera"] = nullptr;
                    }
                }
                // object削除を適用したことを返します。
                return 1;
            }

            // 未対応operation名を明示して拒否します。
            throw std::invalid_argument(
                "Unknown patch operation: " + kind);
        }

        // ResolvePatchOutput(projectRoot: project root, requested: 出力path): project内のcanonical output pathを返します。
        [[nodiscard]] std::filesystem::path ResolvePatchOutput(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& requested)
        {
            // candidate: project rootを基準に解決するrequested path。
            const auto candidate = requested.is_absolute()
                ? requested
                : projectRoot / requested;
            // output: 正規化後に返すcanonical path。
            const auto output = std::filesystem::weakly_canonical(candidate);
            // relative: outputがproject root内か検査するpath。
            const auto relative = output.lexically_relative(projectRoot);
            // output pathがproject root内に収まるか検証します。
            if (relative.empty()
                || relative.native().starts_with(L".."))
            {
                // project外へのpatch出力を拒否します。
                throw std::invalid_argument(
                    "Patch output must stay inside the project: "
                    + LamaPon::PathToUtf8(output));
            }
            // canonical化したpatch出力pathを返します。
            return output;
        }

    }

    // RunPatch(projectRoot: project root, scene: scene path, operationsPath: patch JSON, outputPath: 保存先, dryRun: 書込抑止flag): scene patchを適用・検証しreportを出力します。
    [[nodiscard]] int RunPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun)
    {
        // source: patch前後のscene documentとpathを保持します。
        auto source = LoadSceneSource(projectRoot, scene);
        // operationsFile: project基準で解決したpatch JSON path。
        const auto operationsFile = operationsPath.is_absolute()
            ? operationsPath
            : source.projectRoot / operationsPath;
        // operationsDocument: ReadJsonFileが返したpatch JSON。
        const auto operationsDocument = ReadJsonFile(operationsFile);
        // operations: 配列直下またはoperations fieldから得た処理列。
        const auto operations = operationsDocument.is_array()
            ? operationsDocument
            : operationsDocument.value(
                "operations",
                nlohmann::json::array());
        // patch documentがoperation配列か検査します。
        if (!operations.is_array())
        {
            // 配列でないpatch fileを入力errorとして拒否します。
            throw std::invalid_argument(
                "Patch file must be an array or contain an operations array.");
        }
        // scene objects fieldがpatch可能な配列か確認します。
        if (!source.document.contains("objects")
            || !source.document.at("objects").is_array())
        {
            // object配列でないsceneをpatch前に拒否します。
            throw std::invalid_argument(
                "Scene objects must be an array before applying a patch.");
        }

        // nextId: 既存IDを避ける次回採番値。
        std::uint64_t nextId{ 1 };
        // object: nextId更新用に既存IDを調べるscene entry。
        for (const auto& object : source.document.at("objects"))
        {
            // objectId: 現在entryから読み取ったscene ID。
            std::uint64_t objectId{};
            // valid IDの最大値から次回採番値を決めます。
            if (object.is_object()
                && object.contains("id")
                && TryReadObjectId(object.at("id"), objectId)
                && objectId >= nextId)
            {
                // 最大IDではoverflowを避けて採番を巻き戻します。
                if (objectId == std::numeric_limits<std::uint64_t>::max())
                {
                    nextId = 1;
                }
                // 最大値未満なら現在IDの次を採番値にします。
                else
                {
                    nextId = objectId + 1;
                }
            }
        }

        // before: operation適用前のscene解析結果。
        const auto before = AnalyzeScene(source);
        // operationsApplied: 適用したpatch operation数。
        std::size_t operationsApplied{};
        // operation: 各patchを順に適用して変更数を集計します。
        for (const auto& operation : operations)
        {
            operationsApplied += ApplyPatchOperation(
                source.document,
                operation,
                nextId);
        }
        // resultSource: operation適用後のscene解析用source。
        SceneSource resultSource = source;
        // after: operation適用後のscene解析結果。
        const auto after = AnalyzeScene(resultSource);
        // valid: patch後にerror診断が残らないか。
        const bool valid = after.at("problems").empty();

        // before/afterと書込状態を返すreport JSON。
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

        // errorが残るpatchはfileへ書き込みません。
        if (!valid)
        {
            report["error"] =
                "The patch would leave the scene invalid; no file was written.";
        }
        // dry-runでなく変更がある場合に限り保存します。
        else if (!dryRun && operationsApplied != 0)
        {
            // output: 明示pathまたは元scene fileを選択します。
            const auto output = outputPath.empty()
                ? source.sceneFile
                : ResolvePatchOutput(source.projectRoot, outputPath);
            // backup: 既存outputを上書き前に退避するpath。
            std::filesystem::path backup;
            // backup作成が必要な既存outputか確認します。
            if (std::filesystem::is_regular_file(output))
            {
                backup = output.wstring()
                    + L".bak-"
                    + std::to_wstring(GetTickCount64());
                // copyError: backup file作成時のOS error。
                std::error_code copyError;
                std::filesystem::copy_file(
                    output,
                    backup,
                    std::filesystem::copy_options::none,
                    copyError);
                // file copy中のOS errorを検査します。
                if (copyError)
                {
                    // backupを保全できない場合はscene更新を中断します。
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
        // dry-runまたは変更なしではoutputをnullにします。
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
        // validation成否をcommand exit codeで返します。
        return valid ? 0 : 1;
    }

    // RunPrefabPatch(projectRoot: project root, prefab: prefab path, operationsPath: patch JSON, outputPath: 保存先, dryRun: 書込抑止flag): prefab patchを適用・検証します。
    [[nodiscard]] int RunPrefabPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun)
    {
        // source: patch前後のprefab documentとpathを保持します。
        auto source = LoadPrefabSource(projectRoot, prefab);
        // operationsFile: project基準で解決したpatch JSON path。
        const auto operationsFile = operationsPath.is_absolute()
            ? operationsPath
            : source.projectRoot / operationsPath;
        // operationsDocument: ReadJsonFileが返したpatch JSON。
        const auto operationsDocument = ReadJsonFile(operationsFile);
        // operations: 配列直下またはoperations fieldから得た処理列。
        const auto operations = operationsDocument.is_array()
            ? operationsDocument
            : operationsDocument.value(
                "operations",
                nlohmann::json::array());
        // patch documentがoperation配列か検査します。
        if (!operations.is_array())
        {
            // 配列でないpatch fileを入力errorとして拒否します。
            throw std::invalid_argument(
                "Prefab patch file must be an array or contain an operations array.");
        }
        // prefab objects fieldがpatch可能な配列か確認します。
        if (!source.document.contains("objects")
            || !source.document.at("objects").is_array())
        {
            // object配列でないprefabをpatch前に拒否します。
            throw std::invalid_argument(
                "Prefab objects must be an array before applying a patch.");
        }

        // nextId: 既存IDを避ける次回採番値。
        std::uint64_t nextId{ 1 };
        // object: nextId初期化用に既存IDを調べるprefab entry。
        for (const auto& object : source.document.at("objects"))
        {
            // objectId: 現在entryから読み取ったprefab ID。
            std::uint64_t objectId{};
            // valid IDの最大値から次回採番値を決めます。
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

        // before: operation適用前のprefab解析結果。
        const auto before = AnalyzePrefab(source);
        // operationsApplied: 適用したpatch operation数。
        std::size_t operationsApplied{};
        // operation: 各patchを順に適用して変更数を集計します。
        for (const auto& operation : operations)
        {
            operationsApplied += ApplyPatchOperation(
                source.document,
                operation,
                nextId);
        }
        // resultSource: operation適用後のprefab解析用source。
        PrefabSource resultSource = source;
        // after: operation適用後のprefab解析結果。
        const auto after = AnalyzePrefab(resultSource);
        // valid: patch後にerror診断が残らないか。
        const bool valid = after.at("problems").empty();

        // before/afterと書込状態を返すreport JSON。
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
        // errorが残るpatchはfileへ書き込みません。
        if (!valid)
        {
            report["error"] =
                "The patch would leave the prefab invalid; no file was written.";
        }
        // dry-runでなく変更がある場合に限り保存します。
        else if (!dryRun && operationsApplied != 0)
        {
            // output: 明示pathまたは元prefab fileを選択します。
            const auto output = outputPath.empty()
                ? source.prefabFile
                : ResolvePatchOutput(source.projectRoot, outputPath);
            // backup: 既存outputを上書き前に退避するpath。
            std::filesystem::path backup;
            // backup作成が必要な既存outputか確認します。
            if (std::filesystem::is_regular_file(output))
            {
                backup = output.wstring()
                    + L".bak-"
                    + std::to_wstring(GetTickCount64());
                // copyError: backup file作成時のOS error。
                std::error_code copyError;
                std::filesystem::copy_file(
                    output,
                    backup,
                    std::filesystem::copy_options::none,
                    copyError);
                // file copy中のOS errorを検査します。
                if (copyError)
                {
                    // backupを保全できない場合はprefab更新を中断します。
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
        // dry-runまたは変更なしではoutputをnullにします。
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
        // validation成否をcommand exit codeで返します。
        return valid ? 0 : 1;
    }

    // RunSceneTests(projectRoot: project root, scene: scene path, specificationPath: test JSON, reportPath: 任意の出力先): scene assertionを評価してreportを出力します。
    [[nodiscard]] int RunSceneTests(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& specificationPath,
        const std::filesystem::path& reportPath)
    {
        // source: test対象sceneとparsed document。
        const auto source = LoadSceneSource(projectRoot, scene);
        // specificationFile: project基準で解決したtest JSON path。
        const auto specificationFile = specificationPath.is_absolute()
            ? specificationPath
            : source.projectRoot / specificationPath;
        // specification: ReadJsonFileが返したtest定義JSON。
        const auto specification = ReadJsonFile(specificationFile);
        // tests: 直下またはtests fieldから得たassertion列。
        const auto tests = specification.is_array()
            ? specification
            : specification.value(
                "tests",
                nlohmann::json::array());
        // specificationにtests配列があるか確認します。
        if (!tests.is_array())
        {
            // 不正なtest specificationを入力errorとして拒否します。
            throw std::invalid_argument(
                "Test specification must be an array or contain a tests array.");
        }

        // analysis: assertion評価に使うscene診断report。
        const auto analysis = AnalyzeScene(source);
        // assertions: 各testの成否と診断を格納します。
        nlohmann::json assertions = nlohmann::json::array();
        // passedCount: 成功したassertion数。
        std::size_t passedCount{};
        // index: test specification内のassertion番号。
        for (std::size_t index{}; index < tests.size(); ++index)
        {
            // test: 現在評価するassertion JSON。
            const auto& test = tests.at(index);
            // name: reportに表示するtest名。
            const auto name = test.is_object()
                ? test.value(
                    "name",
                    "test-" + std::to_string(index + 1))
                : "test-" + std::to_string(index + 1);
            // kind: 評価するassertion種別。
            const auto kind = test.is_object()
                ? test.value("kind", std::string{})
                : std::string{};
            // passed: 現在のassertionが成功したか。
            bool passed{};
            // detail: assertion結果の説明またはfailure理由。
            std::string detail;
            // actual: assertionが実際に観測した値。
            nlohmann::json actual = nullptr;
            // 各assertionを独立に評価してfailureを記録します。
            try
            {
                // test entryがobjectで非空kindを持つか検査します。
                if (!test.is_object()
                    || kind.empty())
                {
                    detail = "Each test requires a kind.";
                }
                // scene-valid/no-problems assertionを評価します。
                else if (kind == "scene-valid"
                    || kind == "no-problems")
                {
                    passed = analysis.at("problems").empty();
                    actual = analysis.at("problems");
                    detail = passed
                        ? "The scene has no validation problems."
                        : "The scene contains validation problems.";
                }
                // object-exists assertionを評価します。
                else if (kind == "object-exists")
                {
                    // object selectorが指定されているか確認します。
                    if (!test.contains("target"))
                    {
                        detail = "object-exists requires target.";
                    }
                    // targetがあればscene objectとの一致を調べます。
                    else
                    {
                        // matches: target selectorに一致したobject index列。
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
                // component-exists assertionを評価します。
                else if (kind == "component-exists")
                {
                    // target selectorとstring typeが揃っているか検査します。
                    if (!test.contains("target")
                        || !test.contains("type")
                        || !test.at("type").is_string())
                    {
                        detail =
                            "component-exists requires target and type.";
                    }
                    // 有効なselectorから対象objectとcomponentを探します。
                    else
                    {
                        // matches: target selectorに一致したobject index列。
                        const auto matches = FindPatchTargets(
                            source.document,
                            test.at("target"));
                        // objectIndex: selector一致objectを順に調べます。
                        for (const auto objectIndex : matches)
                        {
                            // object: component assertionで調べるscene entry。
                            const auto& object = source.document
                                .at("objects")
                                .at(objectIndex);
                            // components: objectのcomponent array field。
                            const auto components = object.find("components");
                            // componentsが配列でなければ次objectへ進みます。
                            if (components == object.end()
                                || !components->is_array())
                            {
                                // 配列のないobjectにcomponentは存在しません。
                                continue;
                            }
                            // component: component配列内のentryをtype条件と照合します。
                            for (const auto& component : *components)
                            {
                                // component typeがspecificationのtypeと一致するか確認します。
                                if (component.is_object()
                                    && component.value(
                                        "type",
                                        std::string{})
                                        == test.at("type").get<std::string>())
                                {
                                    passed = true;
                                    // 一致componentを見つけたら検索を終えます。
                                    break;
                                }
                            }
                            // object間の探索も成功時点で終了します。
                            if (passed)
                            {
                                // 目的componentが見つかったため走査を終えます。
                                break;
                            }
                        }
                        actual = passed;
                        detail = passed
                            ? "The requested component exists."
                            : "The requested component does not exist.";
                    }
                }
                // object-count assertionを評価します。
                else if (kind == "object-count")
                {
                    // count: analysis reportにあるscene object数。
                    const auto count = analysis.at("objectCount").get<std::size_t>();
                    actual = count;
                    // exact count指定があるか確認します。
                    if (test.contains("expected")
                        && test.at("expected").is_number_integer())
                    {
                        // expected: specificationが要求する正確なobject数。
                        const auto expected = test.at("expected").get<std::size_t>();
                        passed = count == expected;
                        detail = "Expected object count "
                            + std::to_string(expected) + ".";
                    }
                    // exact countがなければmin/max範囲を使います。
                    else
                    {
                        // minimum: 許容object数の下限。
                        const auto minimum = test.value(
                            "min",
                            static_cast<std::size_t>(0));
                        // maximum: 許容object数の上限。
                        const auto maximum = test.value(
                            "max",
                            std::numeric_limits<std::size_t>::max());
                        passed = count >= minimum && count <= maximum;
                        detail = "Expected object count in range.";
                    }
                }
                // asset-exists assertionを評価します。
                else if (kind == "asset-exists")
                {
                    // 対象asset pathがstringで指定されたか確認します。
                    if (!test.contains("path")
                        || !test.at("path").is_string())
                    {
                        detail = "asset-exists requires path.";
                    }
                    // 有効なpathのasset存在状態を照合します。
                    else
                    {
                        // requested: specificationが要求するasset path。
                        const auto requested = test.at("path").get<std::string>();
                        // analysis report内のassetをrequested pathと比較します。
                        // asset: requested pathと比較するanalysis entry。
                        for (const auto& asset : analysis.at("assets"))
                        {
                            // requested assetが一致したか確認します。
                            if (asset.value("path", std::string{}) == requested)
                            {
                                passed = asset.value("exists", false);
                                actual = asset;
                                // 一致assetが分かったため走査を終えます。
                                break;
                            }
                        }
                        detail = passed
                            ? "The requested asset exists."
                            : "The requested asset is missing.";
                    }
                }
                // value-equals assertionを評価します。
                else if (kind == "value-equals")
                {
                    // target・path・valueの3項目を検証します。
                    if (!test.contains("target")
                        || !test.contains("path")
                        || !test.at("path").is_string()
                        || !test.contains("value"))
                    {
                        detail =
                            "value-equals requires target, path, and value.";
                    }
                    // 必須項目があればtarget fieldの値を探します。
                    else
                    {
                        // targetIndex: assertion targetで解決したobject位置。
                        const auto targetIndex = FindSinglePatchTarget(
                            source.document,
                            test.at("target"));
                        // actualValue: 指定pathでscene objectから得た値。
                        const auto actualValue = FindPatchValue(
                            source.document.at("objects").at(targetIndex),
                            test.at("path").get<std::string>());
                        // pathが解決した場合のみactual値と期待値を比較します。
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
                // 未対応assertion kindの診断を作ります。
                else
                {
                    detail = "Unknown test kind: " + kind;
                }
            }
            // assertion中の例外をfailure detailに記録します。
            catch (const std::exception& exception)
            {
                detail = exception.what();
                passed = false;
            }
            // 成功したassertion数を集計します。
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

        // allPassed: 全assertionが成功したか。
        const bool allPassed = passedCount == assertions.size();
        // report: command・成否・assertion結果を返すJSON。
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
        // reportPathがある場合はreport JSONを保存します。
        if (!reportPath.empty())
        {
            // output: project root内に解決したreport path。
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
        // 全assertionの成否をcommand exit codeで返します。
        return allPassed ? 0 : 1;
    }

}
