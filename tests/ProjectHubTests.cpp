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
    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    nlohmann::json LoadJson(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "Could not read generated JSON file.");
        nlohmann::json document;
        input >> document;
        return document;
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonProjectHubTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    void VerifyTemplate(
        const std::filesystem::path& parent,
        const wchar_t* folder,
        const std::string& name,
        const LamaPon::Hub::ProjectTemplate projectTemplate,
        const std::size_t expectedObjectCount,
        const bool expectsCamera)
    {
        const auto root = parent / folder;
        LamaPon::Hub::CreateProject(
            root,
            name,
            projectTemplate);

        Require(
            LamaPon::Hub::IsProject(root),
            "Generated folder was not recognized as a LamaPon project.");
        Require(
            LamaPon::Hub::ProjectName(root) == name,
            "Generated project name did not round-trip.");
        const auto settings = LamaPon::LoadProjectSettings(
            root / L".lamapon" / L"project.json");
        Require(
            settings.startupScene == L"scenes/Main.scene.json",
            "Generated startup scene path is incorrect.");

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

        const auto shaderRoot = root / L"assets" / L"shaders";
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

        // .gitignoreの中身も確かめます。プロジェクトはGitで共有する
        // 前提なので、`.lamapon/`へ書き出すものを足したときに除外
        // リストへ入れ忘れると、相手のエディターのレイアウトを
        // 上書きしたり、他人のPCのビルド生成物やパッケージの複製が
        // コミットに混ざります。存在確認だけでは気付けません
        // （2026-08-05にprofile.json・game-module-build.log・
        // package-backups/の3つが漏れていました）。
        {
            std::ifstream ignoreInput(root / L".gitignore");
            const std::string ignoreText(
                std::istreambuf_iterator<char>{
                    ignoreInput },
                std::istreambuf_iterator<char>{});
            for (const auto* entry : {
                    ".lamapon/editor-settings.json",
                    ".lamapon/imgui-layout.ini",
                    ".lamapon/LamaPonEditor.log",
                    ".lamapon/bin/",
                    ".lamapon/build/",
                    ".lamapon/profile.json",
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
            // project.jsonは共有する側なので、除外してはいけません。
            Require(
                ignoreText.find(".lamapon/project.json")
                    == std::string::npos,
                "project.json must stay tracked so the project opens elsewhere.");
        }

        bool rejectedOverwrite = false;
        try
        {
            LamaPon::Hub::CreateProject(
                root,
                name,
                projectTemplate);
        }
        catch (const std::runtime_error&)
        {
            rejectedOverwrite = true;
        }
        Require(
            rejectedOverwrite,
            "Creating over a non-empty project should be rejected.");
    }

    void VerifyEngineTreeGuard(
        const std::filesystem::path& parent)
    {
        const auto engine = parent / L"SyntheticEngine";
        const auto hubSource = engine / L"src" / L"LamaPon" / L"Hub"
            / L"ProjectHub.cpp";
        const auto cliSource = engine / L"tools" / L"LamaPonCli"
            / L"Main.cpp";
        std::filesystem::create_directories(hubSource.parent_path());
        std::filesystem::create_directories(cliSource.parent_path());
        for (const auto& marker : {
                engine / L"CMakeLists.txt",
                hubSource,
                cliSource })
        {
            std::ofstream output(marker, std::ios::binary);
            Require(
                static_cast<bool>(output),
                "Could not create a synthetic engine marker.");
        }

        const auto nestedProject = engine / L"games" / L"WrongPlace";
        Require(
            LamaPon::Hub::IsInsideEngineSourceTree(nestedProject),
            "A not-yet-created child of an engine tree must be detected.");
        bool rejected = false;
        try
        {
            LamaPon::Hub::CreateProject(
                nestedProject,
                "WrongPlace",
                LamaPon::Hub::ProjectTemplate::ThreeDimensional);
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        Require(
            rejected && !std::filesystem::exists(nestedProject),
            "Creating a game inside the engine repository must be rejected"
            " without leaving files behind.");

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

    void VerifyLearningTemplate(
        const std::filesystem::path& parent)
    {
        const auto root = parent / L"Learning";
        LamaPon::Hub::CreateProject(
            root,
            "はじめてのゲーム",
            LamaPon::Hub::ProjectTemplate::LearningThreeDimensional);

        const auto scene = LoadJson(
            root / L"assets" / L"scenes" / L"Main.scene.json");
        Require(
            scene.at("objects").size() == 8,
            "The learning scene must contain a playable sample.");
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

        const auto journey = LamaPon::Hub::LoadLearningJourney(root);
        auto status = LamaPon::Hub::GetLearningStatus(root);
        Require(
            journey.steps.size() == 8
                && journey.title == "ゲームエンジニアとして成長する"
                && journey.conceptText
                    == "C++を理解し、ゲームを作りながら、ゲームエンジニアとして成長できるゲームエンジン"
                && status.totalSteps == 8
                && status.completedSteps == 0
                && status.nextStep.has_value()
                && status.nextStep->id == "play-first"
                && status.selectedRole == "undecided",
            "Initial learning progress is incorrect.");

        LamaPon::Hub::CompleteLearningStep(root, "play-first");
        // 完了は冪等です。同じボタンを二度押しても件数を増やしません。
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

        bool invalidStepRejected = false;
        try
        {
            LamaPon::Hub::CompleteLearningStep(root, "not-a-step");
        }
        catch (const std::invalid_argument&)
        {
            invalidStepRejected = true;
        }
        Require(
            invalidStepRejected,
            "An unknown learning step must be rejected.");

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

    void VerifyLearningRetrofit(
        const std::filesystem::path& parent)
    {
        const auto root = parent / L"Retrofit";
        LamaPon::Hub::CreateProject(
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

        bool overwriteRejected = false;
        try
        {
            LamaPon::Hub::InitializeLearningJourney(root);
        }
        catch (const std::runtime_error&)
        {
            overwriteRejected = true;
        }
        Require(
            overwriteRejected,
            "Learning initialization must not overwrite existing materials.");
    }

    void VerifyLearningTwoDimensionalTemplate(
        const std::filesystem::path& parent)
    {
        const auto root = parent / L"Learning2D";
        LamaPon::Hub::CreateProject(
            root,
            "2D学習ゲーム",
            LamaPon::Hub::ProjectTemplate::LearningTwoDimensional);

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

// エンジン更新チェックの純ロジック（通信なし）を検証します。
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

    const auto current = ParseLatestRelease(
        R"({"tag_name":"v2026.7.31"})",
        "2026.7.31");
    Require(
        !current.updateAvailable,
        "The current release must not report an update.");

    const auto broken = ParseLatestRelease(
        "not-json",
        "2026.7.31");
    Require(
        !broken.updateAvailable,
        "Broken JSON must not report an update.");

    // 予期しないドメインのURLはリリース一覧ページへ置き換えます。
    const auto unsafeUrl = ParseLatestRelease(
        R"({"tag_name":"v9999.1.1","html_url":"https://evil.example/x"})",
        "2026.7.31");
    Require(
        unsafeUrl.updateAvailable
            && unsafeUrl.releaseUrl.rfind(
                "https://github.com/", 0) == 0,
        "Unexpected URLs must fall back to the releases page.");
}

int main()
{
    try
    {
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
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
