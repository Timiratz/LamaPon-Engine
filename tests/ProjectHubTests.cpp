#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Hub/LearningJourney.h"
#include "LamaPon/Hub/ProjectHub.h"
#include "LamaPon/Hub/UpdateChecker.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const std::string& message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // LoadJson(path: JSONファイル): ファイルをJSON文書として読む。
    nlohmann::json LoadJson(const std::filesystem::path& path)
    {
        // 読み込むJSONファイル
        std::ifstream input(path, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "Could not read generated JSON file.");
        // パースしたJSON文書
        nlohmann::json document;
        input >> document;
        return document;
    }

    class TemporaryDirectory final
    {
    public:
        // TemporaryDirectory(): 一意な一時ディレクトリを作成する。
        TemporaryDirectory()
        {
            // 衝突回避に使う単調増加時刻
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonProjectHubTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        // 一時ディレクトリと内容を削除する。
        ~TemporaryDirectory()
        {
            // 削除失敗を例外にしない受け皿
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // Path(): 作成した一時ディレクトリの場所を返す。
        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        // 作成した一時ディレクトリ
        std::filesystem::path m_path;
    };

    // CreateFixtureProject(root: 作成先, name: プロジェクト名, projectTemplate: 雛形): テスト用overrideで雛形を作る。
    // overrideはテスト生成だけに使い、通常の制限は別検証する。
    void CreateFixtureProject(const std::filesystem::path& root,
        const std::string& name, const LamaPon::Hub::ProjectTemplate projectTemplate)
    {
        LamaPon::Hub::CreateProject(root, name, projectTemplate, true);
    }

    // VerifyTemplate(parent: 親フォルダー, folder: 作成フォルダー, projectName: 名前, projectTemplate: 雛形, expectedObjectCount: オブジェクト数, expectsCamera: カメラ有無): 生成物と上書き拒否を検証する。
    void VerifyTemplate(
        const std::filesystem::path& parent,
        const wchar_t* folder,
        const std::string& projectName,
        const LamaPon::Hub::ProjectTemplate projectTemplate,
        const std::size_t expectedObjectCount,
        const bool expectsCamera)
    {
        // 作成するプロジェクトルート
        const auto root = parent / folder;
        CreateFixtureProject(
            root,
            projectName,
            projectTemplate);

        Require(
            LamaPon::Hub::IsProject(root),
            "Generated folder was not recognized as a LamaPon project.");
        Require(
            LamaPon::Hub::ProjectName(root) == projectName,
            "Generated project name did not round-trip.");
        // 読み込んだプロジェクト設定
        const auto settings = LamaPon::LoadProjectSettings(
            root / L".lamapon" / L"project.json");
        // 新規プロジェクトへ組込演出や専用シェーダーを含めない。
        Require(
            !LoadJson(root / L".lamapon" / L"project.json")
                    .contains("sceneTransition")
                && !std::filesystem::exists(
                    root / L"assets" / L"shaders"
                        / L"LamaPonSceneTransition.hlsl"),
            "New projects must not ship a built-in transition effect.");
        Require(
            settings.startupScene == L"scenes/Main.scene.json",
            "Generated startup scene path is incorrect.");

        // 新規作成されたメインシーン
        const auto scene = LoadJson(
            root / L"assets" / L"scenes" / L"Main.scene.json");
        Require(
            scene.value("format", std::string{}) == "LamaPonScene"
                && scene.value("version", 0) == 1,
            "Generated scene header is invalid.");
        Require(
            scene.at("objects").size() == expectedObjectCount,
            "Generated scene has an unexpected object count.");
        Require(
            expectsCamera
                ? scene.at("mainCamera") == 2
                : scene.at("mainCamera").is_null(),
            "Generated scene main camera is incorrect.");
        Require(
            !std::filesystem::exists(root / L"assets" / L"scripts")
                && std::filesystem::is_regular_file(
                    root / L"assets" / L"textures"
                        / L"LamaPonEngineLogo.png")
                && !std::filesystem::exists(
                    root / L"assets" / L"audio")
                && !std::filesystem::exists(
                    root / L"assets" / L"materials")
                && std::filesystem::is_regular_file(root / L".gitignore")
                && std::filesystem::is_regular_file(root / L"README.md"),
            "Generated project folders are incomplete.");

        // 作成したシェーダーの格納先
        const auto shaderRoot = root / L"assets" / L"shaders";
        // 必須の組み込みシェーダーを確認する。
        for (const auto* name : {
                L"LamaPonLit.hlsl",
                L"LamaPonCustomMaterial.hlsl",
                L"LamaPonEnvironment.hlsl",
                L"LamaPonLightCulling.hlsl",
                L"LamaPonScreenDepth.hlsli",
                L"LamaPonShaderError.hlsl",
                L"LamaPonSpriteError.hlsl",
                L"LamaPonSpriteLit.hlsl",
                L"LamaPonSpriteMask.hlsl" })
        {
            Require(
                std::filesystem::is_regular_file(shaderRoot / name),
                "A required built-in shader was not generated.");
        }
        // サンプル専用シェーダーが混入していないか調べる。
        for (const auto* name : {
                L"LamaPonToon.hlsl",
                L"LamaPonNoise.hlsli",
                L"LamaPonNoiseSample.hlsl",
                L"LamaPonRetro3D.hlsl",
                L"LamaPonWater.hlsl",
                L"LamaPonTessellatedTerrain.hlsl",
                L"LamaPonGeometryExplode.hlsl" })
        {
            Require(
                !std::filesystem::exists(shaderRoot / name),
                "A sample shader must not be copied into new projects: "
                    + std::filesystem::path(name).string());
        }

        // 生成状態を除外し、project.jsonを共有対象に保つ。
        {
            // 生成された除外設定
            std::ifstream ignoreInput(root / L".gitignore");
            // .gitignoreの全内容
            const std::string ignoreText(
                std::istreambuf_iterator<char>{
                    ignoreInput },
                std::istreambuf_iterator<char>{});
            // 追跡対象外にする生成パスを確認する。
            for (const auto* entry : {
                    ".lamapon/editor-settings.json",
                    ".lamapon/imgui-layout.ini",
                    ".lamapon/LamaPonEditor.log",
                    ".lamapon/bin/",
                    ".lamapon/build/",
                    ".lamapon/profile.json",
                    ".lamapon/profiles/",
                    ".lamapon/memory/",
                    ".lamapon/game-module-build.log",
                    ".lamapon/package-backups/",
                    ".lamapon/Crashes/",
                    ".lamapon/jobs/",
                    ".lamapon/runtime/",
                    ".lamapon/learning-progress.json",
                    "build/",
                    "dist/",
                    "captures/",
                    "tests/output/",
                    "__pycache__/",
                    ".pytest_cache/",
                    "*.py[cod]",
                    "*.bak" })
            {
                Require(
                    ignoreText.find(entry)
                        != std::string::npos,
                    "The generated .gitignore must cover every generated path.");
            }
            Require(
                ignoreText.find(".lamapon/project.json")
                    == std::string::npos,
                "project.json must stay tracked so the project opens elsewhere.");
        }

        // 非空プロジェクトの上書き拒否結果
        bool rejectedOverwrite = false;
        // 生成済みプロジェクトへの上書きを検査する。
        try
        {
            CreateFixtureProject(
                root,
                projectName,
                projectTemplate);
        }
        // 上書き拒否を記録する。
        catch (const std::runtime_error&)
        {
            rejectedOverwrite = true;
        }
        Require(
            rejectedOverwrite,
            "Creating over a non-empty project should be rejected.");
    }

    // VerifyEngineTreeGuard(parent: テスト親フォルダー): エンジン配下への通常作成を拒否する。
    void VerifyEngineTreeGuard(
        const std::filesystem::path& parent)
    {
        // 合成エンジンルート
        const auto engine = parent / L"SyntheticEngine";
        // Hub実装を示す識別ファイル
        const auto hubSource = engine / L"src" / L"LamaPon" / L"Hub"
            / L"ProjectHub.cpp";
        // CLI実装を示す識別ファイル
        const auto cliSource = engine / L"tools" / L"LamaPonCli"
            / L"Main.cpp";
        std::filesystem::create_directories(hubSource.parent_path());
        std::filesystem::create_directories(cliSource.parent_path());
        // エンジン判定に使う識別ファイルを作る。
        for (const auto& marker : {
                engine / L"CMakeLists.txt",
                hubSource,
                cliSource })
        {
            // 作成する識別ファイル
            std::ofstream output(marker, std::ios::binary);
            Require(
                static_cast<bool>(output),
                "Could not create a synthetic engine marker.");
        }

        // エンジン配下に置こうとするプロジェクト
        const auto nestedProject = engine / L"games" / L"WrongPlace";
        Require(
            LamaPon::Hub::IsInsideEngineSourceTree(nestedProject),
            "A not-yet-created child of an engine tree must be detected.");
        // エンジン配下の作成拒否結果
        bool rejected = false;
        // ソースツリー内のプロジェクト作成を検査する。
        try
        {
            LamaPon::Hub::CreateProject(
                nestedProject,
                "WrongPlace",
                LamaPon::Hub::ProjectTemplate::ThreeDimensional);
        }
        // エンジン配下の拒否を記録する。
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        Require(
            rejected && !std::filesystem::exists(nestedProject),
            "Creating a game inside the engine repository must be rejected"
            " without leaving files behind.");

        // 明示許可したサンプルプロジェクト
        const auto explicitSample = engine / L"samples" / L"AllowedGame";
        LamaPon::Hub::CreateProject(
            explicitSample,
            "AllowedGame",
            LamaPon::Hub::ProjectTemplate::ThreeDimensional,
            true);
        Require(
            LamaPon::Hub::IsProject(explicitSample),
            "The explicit engine-sample override must remain available.");
    }

    // VerifyLearningTemplate(parent: テスト親フォルダー): 3D学習雛形と進捗保存を検証する。
    void VerifyLearningTemplate(
        const std::filesystem::path& parent)
    {
        // 作成する学習プロジェクト
        const auto root = parent / L"Learning";
        CreateFixtureProject(
            root,
            "はじめてのゲーム",
            LamaPon::Hub::ProjectTemplate::LearningThreeDimensional);

        // 生成された学習シーン
        const auto scene = LoadJson(
            root / L"assets" / L"scenes" / L"Main.scene.json");
        Require(
            scene.at("objects").size() == 8,
            "The learning scene must contain a playable sample.");
        // 学習シーン内のプレイヤー
        const auto& player = scene.at("objects").at(3);
        Require(
            player.at("name") == "Player"
                && player.at("components").at(2).at("type")
                    == "InputMover"
                && player.at("components").at(3).at("script")
                    == "Game.LearningPlayer",
            "The learning Player must work before and after C++ build.");
        Require(
            std::filesystem::is_regular_file(root / L"LEARNING.md")
                && std::filesystem::is_regular_file(
                    root / L"learning" / L"journey.json")
                && std::filesystem::is_regular_file(
                    root / L"learning" / L"design-note.md")
                && std::filesystem::is_regular_file(
                    root / L"assets" / L"scripts"
                        / L"LearningPlayer.cpp"),
            "The learning template did not create its teaching materials.");

        // 初期学習コース情報
        const auto journey = LamaPon::Hub::LoadLearningJourney(root);
        // 初期学習進捗
        auto status = LamaPon::Hub::GetLearningStatus(root);
        Require(
            journey.steps.size() == 8
                && journey.title == "ゲーム制作でC++を学ぶ"
                && journey.conceptText
                    == "ゲームを作りながら、C++とエンジンの基本を学ぶコースです。",
            "Initial learning journey metadata is incorrect.");
        Require(
            status.totalSteps == 8
                && status.completedSteps == 0
                && status.nextStep.has_value()
                && status.nextStep->id == "play-first"
                && status.selectedRole == "undecided",
            "Initial learning progress is incorrect.");

        LamaPon::Hub::CompleteLearningStep(root, "play-first");
        // 同じ完了操作を繰り返しても件数は増えない。
        LamaPon::Hub::CompleteLearningStep(root, "play-first");
        LamaPon::Hub::SetLearningRole(root, "designer");
        status = LamaPon::Hub::GetLearningStatus(root);
        Require(
            status.completedSteps == 1
                && status.nextStep.has_value()
                && status.nextStep->id == "ask-why"
                && status.selectedRole == "designer"
                && std::filesystem::is_regular_file(
                    LamaPon::Hub::LearningProgressPath(root)),
            "Learning completion or role choice did not persist.");

        // 不明な学習ステップの拒否結果
        bool invalidStepRejected = false;
        // 存在しない学習ステップの完了を検査する。
        try
        {
            LamaPon::Hub::CompleteLearningStep(root, "not-a-step");
        }
        // 不明なステップの拒否を記録する。
        catch (const std::invalid_argument&)
        {
            invalidStepRejected = true;
        }
        Require(
            invalidStepRejected,
            "An unknown learning step must be rejected.");

        // 学習コースの診断結果
        const auto doctor =
            LamaPon::Hub::DiagnoseLearningJourney(root);
        Require(
            doctor.ready
                && !doctor.checks.empty(),
            "A newly generated learning project must pass learn doctor.");

        LamaPon::Hub::ResetLearningProgress(root);
        status = LamaPon::Hub::GetLearningStatus(root);
        Require(
            status.completedSteps == 0
                && status.selectedRole == "undecided"
                && !std::filesystem::exists(
                    LamaPon::Hub::LearningProgressPath(root)),
            "Reset must remove only the local learning progress.");
    }

    // VerifyLearningRetrofit(parent: テスト親フォルダー): 既存プロジェクトへの学習資料追加を検証する。
    void VerifyLearningRetrofit(
        const std::filesystem::path& parent)
    {
        // 既存形式から作るプロジェクト
        const auto root = parent / L"Retrofit";
        CreateFixtureProject(
            root,
            "既存ゲーム",
            LamaPon::Hub::ProjectTemplate::ThreeDimensional);
        Require(
            !LamaPon::Hub::HasLearningJourney(root),
            "Blank templates should remain blank until learning is enabled.");
        LamaPon::Hub::InitializeLearningJourney(root);
        Require(
            LamaPon::Hub::HasLearningJourney(root)
                && LamaPon::Hub::DiagnoseLearningJourney(root).ready,
            "Learning materials could not be added to an existing project.");

        // 既存学習資料の上書き拒否結果
        bool overwriteRejected = false;
        // 既存資料の再初期化を検査する。
        try
        {
            LamaPon::Hub::InitializeLearningJourney(root);
        }
        // 学習資料の上書き拒否を記録する。
        catch (const std::runtime_error&)
        {
            overwriteRejected = true;
        }
        Require(
            overwriteRejected,
            "Learning initialization must not overwrite existing materials.");
    }

    // VerifyLearningTwoDimensionalTemplate(parent: テスト親フォルダー): 2D学習雛形を検証する。
    void VerifyLearningTwoDimensionalTemplate(
        const std::filesystem::path& parent)
    {
        // 作成する2D学習プロジェクト
        const auto root = parent / L"Learning2D";
        CreateFixtureProject(
            root,
            "2D学習ゲーム",
            LamaPon::Hub::ProjectTemplate::LearningTwoDimensional);

        // 生成された2D学習シーン
        const auto scene = LoadJson(
            root / L"assets" / L"scenes" / L"Main.scene.json");
        Require(
            scene.at("objects").size() == 4
                && scene.at("objects").at(2).at("name") == "Player"
                && scene.at("objects").at(3).at("name") == "Goal",
            "The 2D learning scene must contain a Player and Goal.");
        Require(
            std::filesystem::is_regular_file(root / L"LEARNING.md")
                && std::filesystem::is_regular_file(
                    root / L"learning" / L"journey.json")
                && std::filesystem::is_regular_file(
                    root / L"assets" / L"scripts"
                        / L"LearningPlayer.cpp"),
            "The 2D learning template did not create its teaching materials.");
    }
}

// VerifyUpdateChecker(): 通信せずに更新判定とリリース解析を検証する。
void VerifyUpdateChecker()
{
    using LamaPon::Hub::IsNewerVersion;
    using LamaPon::Hub::ParseLatestRelease;
    using LamaPon::Hub::ParseVersionNumbers;

    Require(
        ParseVersionNumbers("v2026.7.31")
            == std::vector<std::uint32_t>{ 2026, 7, 31 },
        "Version numbers were not parsed.");
    Require(
        ParseVersionNumbers("abc").empty()
            && ParseVersionNumbers("").empty()
            && ParseVersionNumbers("1..2").empty(),
        "Invalid versions must parse to empty.");

    Require(
        IsNewerVersion("2026.7.31", "v2026.8.1"),
        "A newer month must be detected.");
    Require(
        IsNewerVersion("2026.7.31", "2026.7.31.1"),
        "A same-day re-release must be newer.");
    Require(
        !IsNewerVersion("2026.7.31", "2026.7.31"),
        "The same version is not newer.");
    Require(
        !IsNewerVersion("2026.8.1", "v2026.7.31"),
        "An older version must not be newer.");
    Require(
        !IsNewerVersion("2026.7.31", "garbage"),
        "Unparsable versions must not report updates.");

    // 新しいリリース情報
    const auto available = ParseLatestRelease(
        R"({"tag_name":"v2026.8.1",)"
        R"("html_url":"https://github.com/Timiratz/LamaPon-Engine/releases/tag/v2026.8.1"})",
        "2026.7.31");
    Require(
        available.updateAvailable
            && available.latestVersion == "2026.8.1"
            && available.releaseUrl.rfind(
                "https://github.com/", 0) == 0,
        "A newer release JSON must report an update.");

    // 現行バージョンのリリース情報
    const auto current = ParseLatestRelease(
        R"({"tag_name":"v2026.7.31"})",
        "2026.7.31");
    Require(
        !current.updateAvailable,
        "The current release must not report an update.");

    // JSON形式不正のリリース情報
    const auto broken = ParseLatestRelease(
        "not-json",
        "2026.7.31");
    Require(
        !broken.updateAvailable,
        "Broken JSON must not report an update.");

    // 許可されないURLを公式リリース一覧へ置き換える。
    // 安全でないURLを含むリリース情報
    const auto unsafeUrl = ParseLatestRelease(
        R"({"tag_name":"v9999.1.1","html_url":"https://evil.example/x"})",
        "2026.7.31");
    Require(
        unsafeUrl.updateAvailable
            && unsafeUrl.releaseUrl.rfind(
                "https://github.com/", 0) == 0,
        "Unexpected URLs must fall back to the releases page.");
}

// main(): Hub、学習雛形、更新判定の全テストを実行する。
int main()
{
    // 検査失敗を終了コードへ変換する。
    try
    {
        // テスト群で共有する一時領域
        TemporaryDirectory temporary;
        VerifyTemplate(
            temporary.Path(),
            L"TwoD",
            "日本語2Dゲーム",
            LamaPon::Hub::ProjectTemplate::TwoDimensional,
            3,
            true);
        VerifyTemplate(
            temporary.Path(),
            L"ThreeD",
            "日本語3Dゲーム",
            LamaPon::Hub::ProjectTemplate::ThreeDimensional,
            5,
            true);
        VerifyLearningTemplate(temporary.Path());
        VerifyLearningTwoDimensionalTemplate(temporary.Path());
        VerifyLearningRetrofit(temporary.Path());
        VerifyEngineTreeGuard(temporary.Path());
        VerifyUpdateChecker();
        std::cout << "Project Hub template tests passed.\n";
        return 0;
    }
    // 例外(exception: テスト失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
