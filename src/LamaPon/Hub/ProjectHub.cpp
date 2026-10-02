#include "LamaPon/Hub/ProjectHub.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectMigration.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Hub/LearningJourney.h"

#include <Windows.h>
#include <ShlObj.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    // 最近のプロジェクトの上限
    constexpr std::size_t MaximumRecentProjects = 20;

    class ProjectCreationRollback final
    {
    public:
        // 生成失敗時の保存先削除を準備する(projectRoot: 保存先, restoreEmptyDirectory: 既存の空フォルダーを戻すか)。
        ProjectCreationRollback(
            std::filesystem::path projectRoot,
            const bool restoreEmptyDirectory)
            : m_projectRoot(std::move(projectRoot)),
              m_restoreEmptyDirectory(restoreEmptyDirectory)
        {
        }

        // 未確定なら保存先を削除し、元の空フォルダーの再作成を試みる。
        ~ProjectCreationRollback()
        {
            if (m_committed)
            {
                return;
            }

            // ファイル操作のエラー
            std::error_code error;
            std::filesystem::remove_all(m_projectRoot, error);
            if (m_restoreEmptyDirectory)
            {
                error.clear();
                std::filesystem::create_directories(
                    m_projectRoot,
                    error);
            }
        }

        // 複製を禁止する。
        ProjectCreationRollback(
            const ProjectCreationRollback&) = delete;
        // 複製代入を禁止する。
        ProjectCreationRollback& operator=(
            const ProjectCreationRollback&) = delete;

        // 保存先を確定し、破棄時の削除を止める。
        void Commit() noexcept
        {
            m_committed = true;
        }

    private:
        // 失敗時に削除する保存先
        std::filesystem::path m_projectRoot;
        // 元の空フォルダーを戻すか
        bool m_restoreEmptyDirectory{};
        // 生成が完了したか
        bool m_committed{};
    };

    // Windows既知フォルダーを取得し、未作成なら作成する(id: フォルダーID)。
    std::filesystem::path KnownFolder(const KNOWNFOLDERID& id)
    {
        // Windowsが確保したフォルダーパス
        PWSTR value{};
        // フォルダー取得結果
        const HRESULT result = SHGetKnownFolderPath(
            id,
            KF_FLAG_CREATE,
            nullptr,
            &value);
        if (FAILED(result) || value == nullptr)
        {
            if (value != nullptr)
            {
                CoTaskMemFree(value);
            }
            throw std::runtime_error(
                "Windows known folder could not be resolved.");
        }
        // 処理対象の保存パス
        const std::filesystem::path path{ value };
        CoTaskMemFree(value);
        return path;
    }

    // 絶対パスを正規化し、可能なら実体パスへ解決する(path: 対象パス)。
    std::filesystem::path Normalize(
        const std::filesystem::path& path)
    {
        // 字句正規化した絶対パス
        const auto absolute =
            std::filesystem::absolute(path).lexically_normal();
        // ファイル操作のエラー
        std::error_code error;
        // 実体へ解決したパス
        const auto canonical =
            std::filesystem::weakly_canonical(absolute, error);
        return error ? absolute : canonical;
    }

    // 比較用に正規化したパスを小文字化する(path: 対象パス)。
    std::wstring ComparisonKey(
        const std::filesystem::path& path)
    {
        // 大小文字を揃えた比較用パス
        auto key = Normalize(path).native();
        // パス文字を小文字へ変換する(value: パス内の文字)。
        std::transform(
            key.begin(),
            key.end(),
            key.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(std::towlower(value));
            });
        return key;
    }

    // JSONを上書き保存する(path: 保存先, value: 保存内容)。
    void WriteJson(
        const std::filesystem::path& path,
        const Json& value)
    {
        std::filesystem::create_directories(path.parent_path());
        // 保存先の出力
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create file: "
                + LamaPon::PathToUtf8(path));
        }
        output << value.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Could not write file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // テキストを上書き保存する(path: 保存先, value: 保存内容)。
    void WriteText(
        const std::filesystem::path& path,
        const std::string_view value)
    {
        std::filesystem::create_directories(path.parent_path());
        // 保存先の出力
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create file: "
                + LamaPon::PathToUtf8(path));
        }
        output.write(
            value.data(),
            static_cast<std::streamsize>(value.size()));
        if (!output)
        {
            throw std::runtime_error(
                "Could not write file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // 実行ファイル同梱アセットをプロジェクトへコピーする(projectRoot: プロジェクトルート, relativePath: assets内の相対パス)。
    void CopyBuiltInAsset(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& relativePath)
    {
        // 同梱アセットのコピー元
        const auto source = LamaPon::ExecutableDirectory()
            / L"assets"
            / relativePath;
        // アセットのコピー先
        const auto destination = projectRoot
            / L"assets"
            / relativePath;
        if (!std::filesystem::is_regular_file(source))
        {
            throw std::runtime_error(
                "Built-in project asset was not found: "
                + LamaPon::PathToUtf8(source));
        }

        std::filesystem::create_directories(destination.parent_path());
        // ファイル操作のエラー
        std::error_code error;
        std::filesystem::copy_file(
            source,
            destination,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error)
        {
            throw std::runtime_error(
                "Could not copy built-in project asset: "
                + LamaPon::PathToUtf8(destination)
                + ": "
                + error.message());
        }
    }

    // TransformのJSONを組み立てる(position: 位置, rotation: オイラー角, scale: 倍率)。
    Json Transform(
        Json position = Json::array({ 0.0, 0.0, 0.0 }),
        Json rotation = Json::array({ 0.0, 0.0, 0.0 }),
        Json scale = Json::array({ 1.0, 1.0, 1.0 }))
    {
        return {
            { "position", std::move(position) },
            { "rotation", std::move(rotation) },
            { "scale", std::move(scale) }
        };
    }

    // 有効なGameObjectのJSONを組み立てる(id: Object ID, name: 表示名, transform: Transform JSON, components: Component配列, parent: 親Object IDまたはnull)。
    Json Object(
        // 確認対象の親フォルダー
        const std::uint64_t id,
        std::string name,
        Json transform,
        Json components,
        Json parent = nullptr)
    {
        return {
            { "id", id },
            { "name", std::move(name) },
            { "enabled", true },
            { "parent", std::move(parent) },
            { "transform", std::move(transform) },
            { "components", std::move(components) }
        };
    }

    // 既定の透視CameraのJSONを返す。
    Json CameraComponent()
    {
        return {
            { "type", "Camera" },
            { "enabled", true },
            { "verticalFieldOfView", 1.0471976 },
            { "nearPlane", 0.1 },
            { "farPlane", 1000.0 }
        };
    }

    // 既定環境と空のObject配列を持つScene JSONを返す。
    Json BaseScene()
    {
        return {
            { "format", "LamaPonScene" },
            { "version", 1 },
            { "mainCamera", nullptr },
            {
                "environment",
                {
                    { "ambientColor", { 0.65, 0.72, 0.85 } },
                    { "ambientIntensity", 0.35 },
                    {
                        "sky",
                        {
                            { "enabled", true },
                            { "topColor", { 0.025, 0.09, 0.26 } },
                            { "horizonColor", { 0.38, 0.62, 0.86 } },
                            { "groundColor", { 0.025, 0.035, 0.06 } },
                            { "intensity", 1.0 }
                        }
                    },
                    {
                        "fog",
                        {
                            { "enabled", false },
                            { "color", { 0.38, 0.54, 0.7 } },
                            { "startDistance", 8.0 },
                            { "endDistance", 32.0 },
                            { "density", 0.018 }
                        }
                    },
                    {
                        "bloom",
                        {
                            { "enabled", true },
                            { "threshold", 0.8 },
                            { "intensity", 0.35 },
                            { "radius", 2.0 }
                        }
                    },
                    {
                        "screenSpaceLensFlare",
                        {
                            { "enabled", false },
                            { "threshold", 1.6 },
                            { "intensity", 0.28 },
                            { "ghostDispersal", 0.35 },
                            { "haloWidth", 0.35 },
                            { "chromaticAberration", 0.06 },
                            { "streakIntensity", 0.18 },
                            { "streakLength", 0.22 },
                            { "streakDirections", 1 },
                            { "streakAngleDegrees", 0.0 }
                        }
                    },
                    {
                        "depthOfField",
                        {
                            { "enabled", false },
                            { "focusDistance", 10.0 },
                            { "focusRange", 2.0 },
                            { "blurStrength", 1.0 },
                            { "maximumRadius", 10.0 }
                        }
                    },
                    {
                        "motionBlur",
                        {
                            { "enabled", false },
                            { "intensity", 0.5 },
                            { "maximumRadius", 16.0 }
                        }
                    },
                    {
                        "autoExposure",
                        {
                            { "enabled", false },
                            { "keyValue", 0.18 },
                            { "minimumLuminance", 0.02 },
                            { "maximumLuminance", 8.0 },
                            { "speedToBright", 3.0 },
                            { "speedToDark", 1.0 }
                        }
                    }
                }
            },
            { "physics", { { "broadPhaseCellSize", 4.0 } } },
            { "objects", Json::array() }
        };
    }

    // CameraとSpriteを持つ2Dテンプレートを返す。
    Json TwoDimensionalScene()
    {
        // 生成するScene JSON
        Json scene = BaseScene();
        scene["mainCamera"] = 2;
        scene["environment"]["sky"]["enabled"] = false;
        scene["environment"]["bloom"]["enabled"] = false;
        scene["objects"].push_back(Object(
            1,
            "World",
            Transform(),
            Json::array()));
        scene["objects"].push_back(Object(
            2,
            "Main Camera",
            Transform({ 0.0, 0.0, 10.0 }),
            Json::array({
                CameraComponent(),
                Json{
                    { "type", "AudioListener" },
                    { "enabled", true }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            3,
            "Sprite",
            Transform(),
            Json::array({
                Json{
                    { "type", "SpriteRenderer" },
                    { "enabled", true },
                    { "size", { 160.0, 160.0 } },
                    { "color", { 0.1, 0.65, 1.0, 1.0 } },
                    { "texture", "" }
                },
                Json{
                    { "type", "BoxCollider2D" },
                    { "enabled", true },
                    { "size", { 160.0, 160.0 } },
                    { "offset", { 0.0, 0.0 } },
                    { "trigger", false },
                    { "layer", 1 },
                    { "mask", 0xffffffffu },
                    { "friction", 0.5 },
                    { "restitution", 0.0 }
                }
            }),
            1));
        return scene;
    }

    // Camera・光源・Cube・地面を持つ3Dテンプレートを返す。
    Json ThreeDimensionalScene()
    {
        // 生成するScene JSON
        Json scene = BaseScene();
        scene["mainCamera"] = 2;
        scene["objects"].push_back(Object(
            1,
            "World",
            Transform(),
            Json::array()));
        scene["objects"].push_back(Object(
            2,
            "Main Camera",
            Transform(
                { 0.0, 2.0, 7.0 },
                { -0.18, 0.0, 0.0 }),
            Json::array({
                CameraComponent(),
                Json{
                    { "type", "AudioListener" },
                    { "enabled", true }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            3,
            "Directional Light",
            Transform(
                { 0.0, 3.0, 0.0 },
                { -0.7853982, -0.6108652, 0.0 }),
            Json::array({
                Json{
                    { "type", "DirectionalLight" },
                    { "enabled", true },
                    { "color", { 1.0, 0.96, 0.88 } },
                    { "intensity", 1.15 },
                    { "castsShadows", true },
                    { "shadowDistance", 40.0 },
                    { "shadowBias", 0.0015 },
                    { "shadowNormalBias", 0.0025 },
                    { "shadowStrength", 0.85 },
                    { "shadowCascadeCount", 4 },
                    { "shadowSplitLambda", 0.65 }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            4,
            "Cube",
            Transform({ 0.0, 0.5, 0.0 }),
            Json::array({
                Json{
                    { "type", "MeshRenderer" },
                    { "enabled", true },
                    { "shape", "Cube" },
                    { "color", { 0.1, 0.65, 1.0, 1.0 } },
                    { "roughness", 0.4 },
                    { "normalStrength", 1.0 }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            5,
            "Ground",
            Transform(
                { 0.0, -0.05, 0.0 },
                { 0.0, 0.0, 0.0 },
                { 10.0, 0.1, 10.0 }),
            Json::array({
                Json{
                    { "type", "MeshRenderer" },
                    { "enabled", true },
                    { "shape", "Cube" },
                    { "color", { 0.18, 0.21, 0.25, 1.0 } },
                    { "roughness", 0.72 },
                    { "normalStrength", 1.0 }
                },
                Json{
                    { "type", "BoxCollider3D" },
                    { "enabled", true },
                    { "size", { 1.0, 1.0, 1.0 } },
                    { "offset", { 0.0, 0.0, 0.0 } },
                    { "trigger", false },
                    { "layer", 1 },
                    { "mask", 0xffffffffu },
                    { "friction", 0.6 },
                    { "restitution", 0.0 }
                }
            }),
            1));
        return scene;
    }

    // 3DテンプレートへPlayer・Goal・障害物を追加する。
    Json LearningThreeDimensionalScene()
    {
        // 生成するScene JSON
        Json scene = ThreeDimensionalScene();
        scene["objects"].at(1)["transform"] = Transform(
            { 0.0, 6.2, 9.2 },
            { -0.52, 0.0, 0.0 });

        // 学習用PlayerのObject JSON
        auto& player = scene["objects"].at(3);
        player["name"] = "Player";
        player["transform"] = Transform({ -3.0, 0.55, 2.0 });
        player["components"].push_back(Json{
            { "type", "BoxCollider3D" },
            { "enabled", true },
            { "size", { 1.0, 1.0, 1.0 } },
            { "offset", { 0.0, 0.0, 0.0 } },
            { "trigger", false },
            { "layer", 1 },
            { "mask", 0xffffffffu },
            { "friction", 0.5 },
            { "restitution", 0.0 }
        });
        player["components"].push_back(Json{
            { "type", "InputMover" },
            { "enabled", true },
            { "horizontalAction", "MoveHorizontal" },
            { "verticalAction", "MoveVertical" },
            { "speed", 4.0 }
        });
        player["components"].push_back(Json{
            { "type", "NativeScript" },
            { "enabled", true },
            { "script", "Game.LearningPlayer" },
            { "properties", Json::object() }
        });

        scene["objects"].at(4)["transform"] = Transform(
            { 0.0, -0.05, 0.0 },
            { 0.0, 0.0, 0.0 },
            { 12.0, 0.1, 12.0 });
        scene["objects"].push_back(Object(
            6,
            "Goal",
            Transform(
                { 3.2, 0.12, -2.2 },
                { 0.0, 0.0, 0.0 },
                { 1.8, 0.22, 1.8 }),
            Json::array({
                Json{
                    { "type", "MeshRenderer" },
                    { "enabled", true },
                    { "shape", "Cube" },
                    { "color", { 1.0, 0.72, 0.08, 1.0 } },
                    { "roughness", 0.28 },
                    { "normalStrength", 1.0 }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            7,
            "Obstacle A",
            Transform(
                { -0.8, 0.75, 0.2 },
                { 0.0, 0.35, 0.0 },
                { 1.1, 1.5, 1.1 }),
            Json::array({
                Json{
                    { "type", "MeshRenderer" },
                    { "enabled", true },
                    { "shape", "Cube" },
                    { "color", { 0.95, 0.32, 0.16, 1.0 } },
                    { "roughness", 0.48 },
                    { "normalStrength", 1.0 }
                }
            }),
            1));
        scene["objects"].push_back(Object(
            8,
            "Obstacle B",
            Transform(
                { 1.5, 0.45, 1.3 },
                { 0.0, -0.25, 0.0 },
                { 1.8, 0.9, 0.8 }),
            Json::array({
                Json{
                    { "type", "MeshRenderer" },
                    { "enabled", true },
                    { "shape", "Cube" },
                    { "color", { 0.55, 0.22, 0.84, 1.0 } },
                    { "roughness", 0.42 },
                    { "normalStrength", 1.0 }
                }
            }),
            1));
        return scene;
    }

    // 2Dテンプレートへ学習用PlayerとGoalを配置する。
    Json LearningTwoDimensionalScene()
    {
        // 生成するScene JSON
        Json scene = TwoDimensionalScene();
        // 学習用PlayerのObject JSON
        auto& player = scene["objects"].at(2);
        player["name"] = "Player";
        player["transform"] = Transform({ -2.0, 0.0, 0.0 });
        player["components"].at(0)["color"] =
            { 0.1, 0.65, 1.0, 1.0 };
        scene["objects"].push_back(Object(
            4,
            "Goal",
            Transform({ 2.0, 0.0, 0.0 }),
            Json::array({
                Json{
                    { "type", "SpriteRenderer" },
                    { "enabled", true },
                    { "size", { 120.0, 120.0 } },
                    { "color", { 1.0, 0.72, 0.08, 1.0 } },
                    { "texture", "" }
                }
            }),
            1));
        return scene;
    }

    // 指定テンプレートのScene JSONを返す(projectTemplate: テンプレート種別)。
    Json SceneForTemplate(
        const LamaPon::Hub::ProjectTemplate projectTemplate)
    {
        switch (projectTemplate)
        {
        case LamaPon::Hub::ProjectTemplate::LearningTwoDimensional:
            return LearningTwoDimensionalScene();
        case LamaPon::Hub::ProjectTemplate::LearningThreeDimensional:
            return LearningThreeDimensionalScene();
        case LamaPon::Hub::ProjectTemplate::TwoDimensional:
            return TwoDimensionalScene();
        case LamaPon::Hub::ProjectTemplate::ThreeDimensional:
        default:
            return ThreeDimensionalScene();
        }
    }

    // Hub設定を読み、解析・形式の不一致は空の設定へ戻す。
    Json LoadSettingsDocument()
    {
        // 処理対象の保存パス
        const auto path = LamaPon::Hub::SettingsPath();
        if (std::filesystem::is_regular_file(path))
        {
            try
            {
                // Hub設定の入力
                std::ifstream input(path, std::ios::binary);
                // Hub設定のJSON
                Json document;
                // Hub設定のJSON
                input >> document;
                if (document.is_object()
                    && document.value("format", std::string{})
                        == "LamaPonHub"
                    && document.value("version", 0) == 1)
                {
                    return document;
                }
            }
            catch (const std::exception&)
            {
            }
        }
        return Json{
            { "format", "LamaPonHub" },
            { "version", 1 }
        };
    }

    // 他のHub設定キーを保って最近の一覧を保存する(paths: 登録順のルート一覧)。
    void SaveRecentPaths(
        const std::vector<std::filesystem::path>& paths)
    {
        // Hub設定のJSON
        auto document = LoadSettingsDocument();
        document["recentProjects"] = Json::array();
        // 処理対象の保存パス
        for (const auto& path : paths)
        {
            document["recentProjects"].push_back(
                LamaPon::PathToUtf8(path));
        }
        WriteJson(LamaPon::Hub::SettingsPath(), document);
    }

    // Hub設定から保存順のプロジェクトパスを読む。
    std::vector<std::filesystem::path> LoadRecentPaths()
    {
        // Hub設定のJSON
        const auto document = LoadSettingsDocument();
        // 保存順のプロジェクトパス
        std::vector<std::filesystem::path> result;
        // 保存済みのルート文字列
        for (const auto& value : document.value(
            "recentProjects",
            Json::array()))
        {
            if (value.is_string())
            {
                result.push_back(LamaPon::PathFromUtf8(
                    value.get_ref<const std::string&>()));
            }
        }
        return result;
    }
}

namespace LamaPon::Hub
{
    std::filesystem::path SettingsPath()
    {
        return KnownFolder(FOLDERID_LocalAppData)
            / L"LamaPon"
            / L"hub.json";
    }

    std::string LoadSkippedUpdateVersion()
    {
        return LoadSettingsDocument().value(
            "skippedUpdateVersion",
            std::string{});
    }

    void SaveSkippedUpdateVersion(const std::string& version)
    {
        // Hub設定のJSON
        auto document = LoadSettingsDocument();
        document["skippedUpdateVersion"] = version;
        WriteJson(SettingsPath(), document);
    }

    std::filesystem::path LoadLastProjectLocation()
    {
        return LamaPon::PathFromUtf8(
            LoadSettingsDocument().value(
                "lastProjectLocation",
                std::string{}));
    }

    void SaveLastProjectLocation(
        const std::filesystem::path& location)
    {
        // Hub設定のJSON
        auto document = LoadSettingsDocument();
        document["lastProjectLocation"] =
            LamaPon::PathToUtf8(location);
        WriteJson(SettingsPath(), document);
    }

    std::filesystem::path DefaultProjectsDirectory()
    {
        return KnownFolder(FOLDERID_Documents)
            / L"LamaPon Projects";
    }

    bool IsProject(const std::filesystem::path& projectRoot)
    {
        return std::filesystem::is_regular_file(
                projectRoot / L".lamapon" / L"project.json")
            && std::filesystem::is_directory(
                projectRoot / L"assets");
    }

    std::string ProjectName(
        const std::filesystem::path& projectRoot)
    {
        return LoadProjectSettings(
            projectRoot / L".lamapon" / L"project.json").gameName;
    }

    // 親を辿ってLamaPonソース配下か調べる(requestedPath: 確認するパス)。
    bool IsInsideEngineSourceTree(
        const std::filesystem::path& requestedPath)
    {
        // 親を辿る確認対象フォルダー
        auto candidate = Normalize(requestedPath);
        while (!candidate.empty())
        {
            // CMakeListsに加えLamaPon固有のソース2か所を確認する。
            if (std::filesystem::is_regular_file(
                    candidate / L"CMakeLists.txt")
                && std::filesystem::is_regular_file(
                    candidate / L"src" / L"LamaPon" / L"Hub"
                        / L"ProjectHub.cpp")
                && std::filesystem::is_regular_file(
                    candidate / L"tools" / L"LamaPonCli"
                        / L"Main.cpp"))
            {
                return true;
            }
            // 確認対象の親フォルダー
            const auto parent = candidate.parent_path();
            if (parent == candidate)
            {
                break;
            }
            candidate = parent;
        }
        return false;
    }

    // 空の保存先にプロジェクトを生成する(requestedProjectRoot: 保存先, projectName: ゲーム名, projectTemplate: 初期教材の種類, allowInsideEngineSource: エンジン配下への作成を許すか)。
    void CreateProject(
        const std::filesystem::path& requestedProjectRoot,
        const std::string& projectName,
        const ProjectTemplate projectTemplate,
        const bool allowInsideEngineSource)
    {
        if (projectName.empty())
        {
            throw std::invalid_argument(
                "Project name must not be empty.");
        }
        // 正規化した生成先
        const auto projectRoot = Normalize(requestedProjectRoot);
        if (!allowInsideEngineSource
            && IsInsideEngineSourceTree(projectRoot))
        {
            throw std::runtime_error(
                "A game project cannot be created inside the"
                " LamaPon source tree. Choose a folder beside the"
                " engine repository instead: "
                + PathToUtf8(projectRoot));
        }
        // 生成先が元から存在したか
        const bool projectRootExisted =
            std::filesystem::exists(projectRoot);
        if (projectRootExisted
            && !std::filesystem::is_empty(projectRoot))
        {
            throw std::runtime_error(
                "The project folder is not empty: "
                + PathToUtf8(projectRoot));
        }

        // 生成するプロジェクト設定
        ProjectSettings settings;
        settings.gameName = projectName;
        settings.startupScene = L"scenes/Main.scene.json";
        if (projectTemplate == ProjectTemplate::TwoDimensional
            || projectTemplate
                == ProjectTemplate::LearningTwoDimensional)
        {
            settings.graphics.shadowsEnabled = false;
            settings.graphics.bloomEnabled = false;
            settings.graphics.fogEnabled = false;
        }
        ValidateProjectSettings(settings);

        // 未完了時の保存先削除管理
        ProjectCreationRollback rollback{
            projectRoot,
            projectRootExisted
        };

        // 学習テンプレートの教材は共通アセットの生成後に追加する。
        std::filesystem::create_directories(
            projectRoot / L"assets" / L"scenes");

        // コピーする同梱アセットのパス
        for (const auto& relative :
            LamaPon::BuiltInProjectAssets())
        {
            CopyBuiltInAsset(projectRoot, relative);
        }

        SaveProjectSettings(
            projectRoot / L".lamapon" / L"project.json",
            settings,
            ProjectSettingsFileType::Project);
        // 初回起動で旧版と判定されないよう、生成時の版を記録する。
        RecordProjectEngineVersion(
            projectRoot,
            VersionString);
        WriteJson(
            projectRoot / L"assets" / L"scenes" / L"Main.scene.json",
            SceneForTemplate(projectTemplate));
        // project.jsonと教材を共有し、端末固有の進捗・ログ・生成物は除外する。
        WriteText(
            projectRoot / L".gitignore",
            ".lamapon/editor-settings.json\n"
            ".lamapon/imgui-layout.ini\n"
            ".lamapon/LamaPonEditor.log\n"
            ".lamapon/bin/\n"
            ".lamapon/build/\n"
            ".lamapon/profile.json\n"
            ".lamapon/profiles/\n"
            ".lamapon/memory/\n"
            ".lamapon/game-module-build.log\n"
            ".lamapon/package-backups/\n"
            ".lamapon/Crashes/\n"
            ".lamapon/jobs/\n"
            ".lamapon/runtime/\n"
            ".lamapon/learning-progress.json\n"
            "build/\n"
            "dist/\n"
            "captures/\n"
            "tests/output/\n"
            "__pycache__/\n"
            ".pytest_cache/\n"
            "*.py[cod]\n"
            "*.bak\n");
        if (projectTemplate == ProjectTemplate::LearningThreeDimensional
            || projectTemplate == ProjectTemplate::LearningTwoDimensional)
        {
            WriteText(
                projectRoot / L"README.md",
                "# " + projectName + "\n\n"
                "ゲーム制作を通してC++とエンジンの基本を学ぶ、"
                "LamaPonのサンプルプロジェクトです。\n\n"
                "まず[`LEARNING.md`](LEARNING.md)を開いてください。\n\n"
                "- `assets/`: 実際に動くSceneとC++ Script\n"
                "- `learning/journey.json`: 共有する学習手順\n"
                "- `learning/design-note.md`: ゲーム企画メモ\n"
                "- `.lamapon/project.json`: 共有するプロジェクト設定\n"
                "- `.lamapon/learning-progress.json`: 個人の進捗（Git除外）\n");
            InitializeLearningJourney(projectRoot, true);
        }
        else
        {
            WriteText(
                projectRoot / L"README.md",
                "# " + projectName + "\n\n"
                "LamaPonプロジェクトです。\n\n"
                "- `assets/`: シーン、スクリプト、画像、音声\n"
                "- `.lamapon/project.json`: プロジェクト設定\n");
        }
        rollback.Commit();
    }

    std::vector<RecentProject> LoadRecentProjects()
    {
        // 返す最近のプロジェクト一覧
        std::vector<RecentProject> projects;
        // 登録済みの比較用パス
        std::vector<std::wstring> keys;
        // Hub設定内の保存済みパス
        for (const auto& storedPath : LoadRecentPaths())
        {
            // 処理対象の保存パス
            const auto path = Normalize(storedPath);
            // 大小文字を揃えた比較用パス
            const auto key = ComparisonKey(path);
            if (!IsProject(path)
                || std::find(keys.begin(), keys.end(), key)
                    != keys.end())
            {
                continue;
            }
            try
            {
                projects.push_back({ path, ProjectName(path) });
                keys.push_back(key);
            }
            catch (const std::exception&)
            {
            }
            if (projects.size() >= MaximumRecentProjects)
            {
                break;
            }
        }
        return projects;
    }

    void AddRecentProject(
        const std::filesystem::path& projectRoot)
    {
        // 正規化した登録対象パス
        const auto normalized = Normalize(projectRoot);
        if (!IsProject(normalized))
        {
            throw std::runtime_error(
                "The selected folder is not a LamaPon project.");
        }
        // 登録・削除対象の比較用パス
        const auto selectedKey = ComparisonKey(normalized);
        // 保存する最近のルート一覧
        std::vector<std::filesystem::path> paths{ normalized };
        // 処理対象の保存パス
        for (const auto& path : LoadRecentPaths())
        {
            if (ComparisonKey(path) != selectedKey
                && IsProject(path))
            {
                paths.push_back(Normalize(path));
            }
            if (paths.size() >= MaximumRecentProjects)
            {
                break;
            }
        }
        SaveRecentPaths(paths);
    }

    void RemoveRecentProject(
        const std::filesystem::path& projectRoot)
    {
        // 登録・削除対象の比較用パス
        const auto selectedKey = ComparisonKey(projectRoot);
        // 保存する最近のルート一覧
        std::vector<std::filesystem::path> paths;
        // 処理対象の保存パス
        for (const auto& path : LoadRecentPaths())
        {
            if (ComparisonKey(path) != selectedKey)
            {
                paths.push_back(Normalize(path));
            }
        }
        SaveRecentPaths(paths);
    }
}
