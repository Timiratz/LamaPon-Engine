#include "LamaPon/Hub/LearningJourney.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    constexpr std::string_view JourneyFormat =
        "LamaPonLearningJourney";
    constexpr std::string_view ProgressFormat =
        "LamaPonLearningProgress";
    constexpr int LearningFormatVersion = 1;

    [[nodiscard]] std::filesystem::path Normalize(
        const std::filesystem::path& path)
    {
        return std::filesystem::absolute(path).lexically_normal();
    }

    void RequireProject(const std::filesystem::path& root)
    {
        if (!std::filesystem::is_regular_file(
                root / L".lamapon" / L"project.json")
            || !std::filesystem::is_directory(root / L"assets"))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project: "
                + LamaPon::PathToUtf8(root));
        }
    }

    [[nodiscard]] Json ReadJson(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not read learning file: "
                + LamaPon::PathToUtf8(path));
        }
        try
        {
            Json document;
            input >> document;
            return document;
        }
        catch (const std::exception& exception)
        {
            throw std::runtime_error(
                "Learning JSON is invalid: "
                + LamaPon::PathToUtf8(path)
                + ": " + exception.what());
        }
    }

    void WriteJson(
        const std::filesystem::path& path,
        const Json& value)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create learning file: "
                + LamaPon::PathToUtf8(path));
        }
        output << value.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Could not write learning file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    void WriteText(
        const std::filesystem::path& path,
        const std::string_view value)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create learning file: "
                + LamaPon::PathToUtf8(path));
        }
        output.write(
            value.data(),
            static_cast<std::streamsize>(value.size()));
        if (!output)
        {
            throw std::runtime_error(
                "Could not write learning file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    [[nodiscard]] bool IsKnownPhase(const std::string_view phase)
    {
        return phase == "play"
            || phase == "question"
            || phase == "understand"
            || phase == "build"
            || phase == "modify"
            || phase == "choose";
    }

    [[nodiscard]] bool IsKnownRole(
        const std::string_view role,
        const bool allowExplorer)
    {
        return role == "undecided"
            || role == "engineer"
            || role == "planner"
            || role == "designer"
            || (allowExplorer && role == "explorer");
    }

    [[nodiscard]] bool IsSafeLearningPath(
        const std::string& value)
    {
        const auto path = LamaPon::PathFromUtf8(value);
        if (path.empty() || path.is_absolute())
        {
            return false;
        }
        const auto normalized = path.lexically_normal();
        return normalized != L".."
            && (normalized.empty()
                || *normalized.begin() != L"..");
    }

    [[nodiscard]] Json StepToJson(
        const LamaPon::Hub::LearningStep& step)
    {
        return {
            { "id", step.id },
            { "phase", step.phase },
            { "title", step.title },
            { "purpose", step.purpose },
            { "action", step.action },
            { "success", step.success },
            { "role", step.role },
            { "estimatedMinutes", step.estimatedMinutes },
            { "files", step.files }
        };
    }

    [[nodiscard]] LamaPon::Hub::LearningStep StepFromJson(
        const Json& value)
    {
        if (!value.is_object())
        {
            throw std::runtime_error(
                "Every learning step must be an object.");
        }
        LamaPon::Hub::LearningStep step;
        step.id = value.value("id", std::string{});
        step.phase = value.value("phase", std::string{});
        step.title = value.value("title", std::string{});
        step.purpose = value.value("purpose", std::string{});
        step.action = value.value("action", std::string{});
        step.success = value.value("success", std::string{});
        step.role = value.value("role", std::string{});
        step.estimatedMinutes = value.value(
            "estimatedMinutes",
            std::uint32_t{});
        if (value.contains("files"))
        {
            if (!value.at("files").is_array())
            {
                throw std::runtime_error(
                    "Learning step files must be an array.");
            }
            step.files = value.at("files")
                .get<std::vector<std::string>>();
        }
        if (step.id.empty()
            || step.id.size() > 64
            || step.title.empty()
            || step.purpose.empty()
            || step.action.empty()
            || step.success.empty()
            || !IsKnownPhase(step.phase)
            || !IsKnownRole(step.role, true)
            || step.estimatedMinutes == 0
            || step.estimatedMinutes > 480)
        {
            throw std::runtime_error(
                "Learning step has invalid required fields: "
                + step.id);
        }
        for (const auto& path : step.files)
        {
            if (!IsSafeLearningPath(path))
            {
                throw std::runtime_error(
                    "Learning step contains an unsafe file path: "
                    + path);
            }
        }
        return step;
    }

    [[nodiscard]] LamaPon::Hub::LearningJourney DefaultJourney(
        const std::string& startupScene,
        const bool sampleSceneIncluded)
    {
        using LamaPon::Hub::LearningStep;
        return {
            "ゲームエンジニアとして成長する",
            "C++を理解し、ゲームを作りながら、ゲームエンジニアとして成長できるゲームエンジン",
            {
                LearningStep{
                    "play-first", "understand", "C++とSceneの関係を読む",
                    "ゲームエンジニアとして、C++とエンジンの接点を最初に把握します。",
                    sampleSceneIncluded
                        ? "Main.scene.jsonとLearningPlayer.cppを開き、Player、Goal、Input Mover、NativeScriptがゲームのどこを担当するか確認します。"
                        : "起動SceneとLearningPlayer.cppを開き、GameObject、Component、C++ Scriptの関係を確認します。",
                    sampleSceneIncluded
                        ? "SceneのPlayerとC++ Scriptの役割を説明できた。"
                        : "起動SceneとC++ Scriptの役割を説明できた。",
                    "explorer", 5, { "assets/" + startupScene }
                },
                LearningStep{
                    "ask-why", "build", "C++をビルドして動かす",
                    "C++の変更をビルドし、ゲームの動作へ反映する一連の工程を通します。",
                    sampleSceneIncluded
                        ? "Editorで再生し、Input Moverの設定とLearningPlayer.cppのUpdateが動作へどう関わるか確認します。"
                        : "Editorで再生し、SceneのComponentとLearningPlayer.cppのUpdateが動作へどう関わるか確認します。",
                    sampleSceneIncluded
                        ? "C++ Game Moduleをビルドし、変更がゲームへ反映されることを確認できた。"
                        : "C++ Game Moduleをビルドし、変更がゲームへ反映されることを確認できた。",
                    "planner", 8, { "assets/" + startupScene }
                },
                LearningStep{
                    "understand-loop", "understand", "ゲームループを読む",
                    "C++のUpdateとdeltaTimeが毎フレームの動きを作ることを理解します。",
                    "LearningPlayer.cppを開き、Start、Update、deltaTime、Inputの4か所をコメントと一緒に読みます。",
                    "Updateがいつ呼ばれ、deltaTimeを掛ける理由を自分の言葉で説明できた。",
                    "engineer", 12,
                    { "assets/scripts/LearningPlayer.cpp" }
                },
                LearningStep{
                    "modify-code", "modify", "C++で手触りを変える",
                    "小さな変更を保存・ビルド・実行まで自分で通します。",
                    sampleSceneIncluded
                        ? "LearningPlayer.cppのSpinSpeedまたはBoostScaleを変更して保存し、再生中の回転やSpaceキーの反応を比べます。"
                        : "Cubeを1つ作り、LearningPlayer.cppを保存・ビルドしてそのCubeへ付けます。SpinSpeedを変更して回転を比べます。",
                    sampleSceneIncluded
                        ? "変更前後の違いを確認し、元へ戻す方法も分かった。"
                        : "自分で作ったCubeがC++のUpdateによって回転した。",
                    "engineer", 15,
                    { "assets/scripts/LearningPlayer.cpp" }
                },
                LearningStep{
                    "design-look", "modify", "見た目と伝わり方を変える",
                    "操作感だけでなく、色・光・カメラもゲーム体験を作ると知ります。",
                    sampleSceneIncluded
                        ? "Player、Goal、Directional Light、Main Cameraのうち2つ以上をInspectorで変更し、遊びやすさを比較します。"
                        : "色、Directional Light、Main Camera、配置のうち2つ以上をInspectorで変更し、見やすさを比較します。",
                    "自分の変更が見やすさや雰囲気へ与えた効果を説明できた。",
                    "designer", 12, { "assets/" + startupScene }
                },
                LearningStep{
                    "plan-rule", "modify", "ルールを一つ設計する",
                    "実装前に目的・制約・成功条件を言葉にするプランナーの仕事を体験します。",
                    "learning/design-note.mdへ30秒で遊べる追加ルールを書き、シーンへObstacleかGoalを1つ追加します。",
                    "誰が何をすると成功か、第三者にも読めるルールになった。",
                    "planner", 15,
                    { "learning/design-note.md", "assets/" + startupScene }
                },
                LearningStep{
                    "verify-share", "understand", "再現して確かめる",
                    "自分のPCだけで動く状態を避け、制作手順を再現可能にします。",
                    "LamaPonCliでlearn doctor、validate、buildを実行し、エラーがあればJSONレポートから原因を直します。",
                    "doctorの必須項目とScene検証が成功し、変更したC++もビルドできた。",
                    "engineer", 10,
                    { "learning/journey.json", "LEARNING.md" }
                },
                LearningStep{
                    "choose-path", "choose", "次に伸ばす力を決める",
                    "ゲームを作り続けながら、次に伸ばす力と取り組みを具体化します。",
                    "C++、ゲーム制作、ゲームエンジニアとしての実践、エンジンのサポートのうち、次に取り組むものを1つ決めてlearning/design-note.mdへ書きます。",
                    "次に作る機能や改善内容を1つ決めた。",
                    "explorer", 5,
                    { "learning/design-note.md" }
                }
            }
        };
    }

    [[nodiscard]] std::string MakeGuide(
        const std::string& projectName,
        const std::string& startupScene,
        const bool sampleSceneIncluded)
    {
        return "# " + projectName + " 学習ガイド\n\n"
            "このプロジェクトは、C++を理解し、実際にゲームを作りながら、ゲームエンジニアとして成長するための教材です。\n\n"
            "## LamaPonが目指す4つの軸\n\n"
            "- **ゲームエンジニアを育てる**\n"
            "- **C++を理解する**\n"
            "- **実際にゲームを作る**\n"
            "- **ゲームエンジンのサポートを充実させる**\n\n"
            "## 最初の5分\n\n"
            "1. LamaPon Editor上部の再生ボタンを押し、現在のゲームを動かします。\n"
            + (sampleSceneIncluded
                ? std::string{
                    "2. Game Viewをクリックし、WASD（またはゲームパッド左スティック）で青いPlayerを黄色いGoalへ動かします。\n"
                    "3. Space（またはゲームパッドA）を押し、Playerの大きさが変わることを確認します。\n\n"
                    "C++ Game Moduleの初回ビルド中でも、移動は組み込みのInput Moverで試せます。Spaceの反応がまだ無い場合はビルド完了を待つか、`LearningPlayer.cpp`を保存してください。\n\n" }
                : std::string{
                    "2. 起動Sceneと`LearningPlayer.cpp`を開き、GameObject、Component、C++ Scriptの関係を確認します。\n"
                    "3. C++を試す段階ではCubeへ`LearningPlayer.cpp`を付けます。保存するとGame Moduleが自動ビルドされます。\n\n" })
            + "## なぜ動くのか\n\n"
            "- `assets/" + startupScene + "`: GameObjectとComponentの組み合わせ。\n"
            + (sampleSceneIncluded
                ? std::string{
                    "- `Input Mover`: 名前付き入力を毎フレーム位置へ変換する組み込みComponent。\n" }
                : std::string{})
            + "- `assets/scripts/LearningPlayer.cpp`: `Update(deltaTime)`で回転とSpace入力を扱うプロジェクト側のC++。\n"
            "- LamaPon Runtime: ゲームループを回し、全ComponentのUpdate、描画、入力を同じ順序で呼ぶ部分。\n\n"
            "## 進め方\n\n"
            "進捗は`.lamapon/learning-progress.json`へPCごとに保存され、Gitには入りません。教材の`learning/journey.json`は共有できます。CLIで進捗を確認・更新できます。\n\n"
            "```powershell\n"
            "LamaPonCli.exe learn status --project \".\"\n"
            "LamaPonCli.exe learn complete --project \".\"\n"
            "LamaPonCli.exe learn doctor --project \".\"\n"
            "LamaPonCli.exe validate --project \".\"\n"
            "LamaPonCli.exe build --project \".\"\n"
            "```\n\n"
            "## 次に伸ばす力\n\n"
            "- **ゲームエンジニアを育てる**: C++、入力、ゲームルール、エラー修正を実践します。\n"
            "- **C++を理解する**: `Start`、`Update`、`deltaTime`、コンポーネントの関係を読み解きます。\n"
            "- **実際にゲームを作る**: ルール、見た目、操作感を変更し、動くゲームとして確かめます。\n"
            "- **ゲームエンジンのサポートを充実させる**: `validate`、`build`、`doctor`で再現性を確認し、必要な改善を記録します。\n";
    }

    constexpr std::string_view StarterScript = R"LAMAPON(#include "LamaPon/LamaPon.h"

// このファイルは「ゲームループ」を読むための最小サンプルです。
// Engine本体ではなく、このプロジェクトのassets内だけを改造します。
class LearningPlayer final : public LamaPon::Script
{
public:
    // StartはPlay開始後、最初のUpdate直前に1回だけ呼ばれます。
    void Start() override
    {
        GetTransform().scale = { 1.0f, 1.0f, 1.0f };
    }

    // Updateは1フレームに1回呼ばれます。
    // deltaTime（前のフレームからの秒数）を掛けることで、PCの速さが
    // 違っても1秒あたりの回転量を同じにできます。
    void Update(const float deltaTime) override
    {
        GetTransform().Rotate(
            { 0.0f, 1.0f, 0.0f },
            SpinSpeed * deltaTime);

        // Jumpは既定でSpace／ゲームパッドAです。
        const float scale = Graphics().Input().IsDown("Jump")
            ? BoostScale
            : 1.0f;
        GetTransform().scale = { scale, scale, scale };
    }

private:
    // 最初の改造ポイント: 値を変えて保存し、動きを比べてください。
    static constexpr float SpinSpeed = 1.2f;
    static constexpr float BoostScale = 1.35f;
};

// この1行がクラスをGame Moduleへ登録します。
LAMAPON_SCRIPT(LearningPlayer);
)LAMAPON";

    constexpr std::string_view DesignNote = R"LAMAPON(# ゲーム企画メモ

正解を書く欄ではありません。遊んだ人へ何を感じてほしいかを短く決めます。

## 30秒で伝えたい体験

- 遊ぶ人:
- やること:
- 成功条件:
- 失敗または制約:

## 変更して確かめること

- 予想:
- 実際に遊んだ結果:
- 次に変えるなら:
)LAMAPON";

    void EnsureProgressIgnored(const std::filesystem::path& root)
    {
        const auto ignorePath = root / L".gitignore";
        std::string contents;
        if (std::filesystem::is_regular_file(ignorePath))
        {
            std::ifstream input(ignorePath, std::ios::binary);
            contents.assign(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }
        constexpr std::string_view entry =
            ".lamapon/learning-progress.json";
        if (contents.find(entry) != std::string::npos)
        {
            return;
        }
        if (!contents.empty() && contents.back() != '\n')
        {
            contents.push_back('\n');
        }
        contents += "# 個人ごとの学習進捗（教材はlearning/へ共有）\n";
        contents += entry;
        contents.push_back('\n');
        WriteText(ignorePath, contents);
    }

    void SaveProgress(
        const std::filesystem::path& projectRoot,
        const LamaPon::Hub::LearningProgress& progress)
    {
        WriteJson(
            LamaPon::Hub::LearningProgressPath(projectRoot),
            Json{
                { "format", std::string(ProgressFormat) },
                { "version", LearningFormatVersion },
                { "completedSteps", progress.completedStepIds },
                { "selectedRole", progress.selectedRole }
            });
    }
}

namespace LamaPon::Hub
{
    std::filesystem::path LearningJourneyPath(
        const std::filesystem::path& projectRoot)
    {
        return Normalize(projectRoot)
            / L"learning"
            / L"journey.json";
    }

    std::filesystem::path LearningProgressPath(
        const std::filesystem::path& projectRoot)
    {
        return Normalize(projectRoot)
            / L".lamapon"
            / L"learning-progress.json";
    }

    bool HasLearningJourney(
        const std::filesystem::path& projectRoot) noexcept
    {
        try
        {
            return std::filesystem::is_regular_file(
                LearningJourneyPath(projectRoot));
        }
        catch (...)
        {
            return false;
        }
    }

    void InitializeLearningJourney(
        const std::filesystem::path& requestedProjectRoot,
        const bool sampleSceneIncluded)
    {
        const auto projectRoot = Normalize(requestedProjectRoot);
        RequireProject(projectRoot);
        const auto settings = LoadProjectSettings(
            projectRoot / L".lamapon" / L"project.json");
        const std::string startupScene = WideToUtf8(
            settings.startupScene.generic_wstring());
        const auto journey = DefaultJourney(
            startupScene,
            sampleSceneIncluded);
        const std::vector<std::filesystem::path> destinations{
            LearningJourneyPath(projectRoot),
            projectRoot / L"LEARNING.md",
            projectRoot / L"learning" / L"design-note.md",
            projectRoot / L"assets" / L"scripts"
                / L"LearningPlayer.cpp"
        };
        for (const auto& destination : destinations)
        {
            if (std::filesystem::exists(destination))
            {
                throw std::runtime_error(
                    "Learning material already exists; nothing was overwritten: "
                    + PathToUtf8(destination));
            }
        }

        Json steps = Json::array();
        for (const auto& step : journey.steps)
        {
            steps.push_back(StepToJson(step));
        }
        WriteText(
            projectRoot / L"assets" / L"scripts"
                / L"LearningPlayer.cpp",
            StarterScript);
        WriteText(
            projectRoot / L"learning" / L"design-note.md",
            DesignNote);
        WriteText(
            projectRoot / L"LEARNING.md",
            MakeGuide(
                settings.gameName,
                startupScene,
                sampleSceneIncluded));
        WriteJson(
            LearningJourneyPath(projectRoot),
            Json{
                { "format", std::string(JourneyFormat) },
                { "version", LearningFormatVersion },
                { "title", journey.title },
                { "concept", journey.conceptText },
                { "steps", std::move(steps) }
            });
        EnsureProgressIgnored(projectRoot);
    }

    LearningJourney LoadLearningJourney(
        const std::filesystem::path& projectRoot)
    {
        const auto path = LearningJourneyPath(projectRoot);
        const auto document = ReadJson(path);
        if (!document.is_object()
            || document.value("format", std::string{}) != JourneyFormat
            || document.value("version", 0) != LearningFormatVersion
            || !document.contains("steps")
            || !document.at("steps").is_array())
        {
            throw std::runtime_error(
                "Unsupported learning journey format: "
                + PathToUtf8(path));
        }

        LearningJourney journey;
        journey.title = document.value("title", std::string{});
        journey.conceptText = document.value(
            "concept",
            std::string{});
        if (journey.title.empty()
            || journey.conceptText.empty()
            || document.at("steps").empty())
        {
            throw std::runtime_error(
                "Learning journey title, concept, and steps are required.");
        }
        std::unordered_set<std::string> ids;
        for (const auto& serialized : document.at("steps"))
        {
            auto step = StepFromJson(serialized);
            if (!ids.insert(step.id).second)
            {
                throw std::runtime_error(
                    "Duplicate learning step id: " + step.id);
            }
            journey.steps.push_back(std::move(step));
        }
        return journey;
    }

    LearningProgress LoadLearningProgress(
        const std::filesystem::path& projectRoot)
    {
        const auto path = LearningProgressPath(projectRoot);
        if (!std::filesystem::is_regular_file(path))
        {
            return {};
        }
        const auto document = ReadJson(path);
        if (!document.is_object()
            || document.value("format", std::string{}) != ProgressFormat
            || document.value("version", 0) != LearningFormatVersion
            || !document.value("completedSteps", Json::array()).is_array())
        {
            throw std::runtime_error(
                "Unsupported learning progress format: "
                + PathToUtf8(path));
        }
        LearningProgress progress;
        progress.completedStepIds = document.value(
            "completedSteps",
            std::vector<std::string>{});
        progress.selectedRole = document.value(
            "selectedRole",
            std::string{ "undecided" });
        if (!IsKnownRole(progress.selectedRole, false))
        {
            throw std::runtime_error(
                "Unknown learning role: " + progress.selectedRole);
        }
        std::unordered_set<std::string> unique;
        std::erase_if(
            progress.completedStepIds,
            [&unique](const std::string& id)
            {
                return id.empty() || !unique.insert(id).second;
            });
        return progress;
    }

    LearningStatus GetLearningStatus(
        const std::filesystem::path& projectRoot)
    {
        const auto journey = LoadLearningJourney(projectRoot);
        const auto progress = LoadLearningProgress(projectRoot);
        const std::unordered_set<std::string> completed{
            progress.completedStepIds.begin(),
            progress.completedStepIds.end()
        };
        LearningStatus status;
        status.totalSteps = journey.steps.size();
        status.selectedRole = progress.selectedRole;
        for (const auto& step : journey.steps)
        {
            if (completed.contains(step.id))
            {
                ++status.completedSteps;
            }
            else if (!status.nextStep.has_value())
            {
                status.nextStep = step;
            }
        }
        return status;
    }

    void CompleteLearningStep(
        const std::filesystem::path& projectRoot,
        const std::string& stepId)
    {
        const auto journey = LoadLearningJourney(projectRoot);
        if (std::ranges::find(
                journey.steps,
                stepId,
                &LearningStep::id) == journey.steps.end())
        {
            throw std::invalid_argument(
                "Unknown learning step: " + stepId);
        }
        auto progress = LoadLearningProgress(projectRoot);
        if (std::ranges::find(
                progress.completedStepIds,
                stepId) == progress.completedStepIds.end())
        {
            progress.completedStepIds.push_back(stepId);
            SaveProgress(projectRoot, progress);
        }
    }

    void SetLearningRole(
        const std::filesystem::path& projectRoot,
        const std::string& role)
    {
        static_cast<void>(LoadLearningJourney(projectRoot));
        if (!IsKnownRole(role, false))
        {
            throw std::invalid_argument(
                "Learning role must be undecided, engineer, planner, or designer.");
        }
        auto progress = LoadLearningProgress(projectRoot);
        progress.selectedRole = role;
        SaveProgress(projectRoot, progress);
    }

    void ResetLearningProgress(
        const std::filesystem::path& projectRoot)
    {
        static_cast<void>(LoadLearningJourney(projectRoot));
        std::error_code error;
        std::filesystem::remove(
            LearningProgressPath(projectRoot),
            error);
        if (error)
        {
            throw std::runtime_error(
                "Could not reset learning progress: "
                + error.message());
        }
    }

    LearningDoctorReport DiagnoseLearningJourney(
        const std::filesystem::path& requestedProjectRoot) noexcept
    {
        LearningDoctorReport report;
        try
        {
            const auto projectRoot = Normalize(requestedProjectRoot);
            const bool projectOk =
                std::filesystem::is_regular_file(
                    projectRoot / L".lamapon" / L"project.json")
                && std::filesystem::is_directory(
                    projectRoot / L"assets");
            report.checks.push_back({
                "project", "LamaPonプロジェクト", projectOk, true,
                projectOk
                    ? "project.jsonとassetsを確認しました。"
                    : "Hubで作成したプロジェクトを指定してください。"
            });
            if (!projectOk)
            {
                report.ready = false;
                return report;
            }

            const bool guideOk = std::filesystem::is_regular_file(
                projectRoot / L"LEARNING.md");
            report.checks.push_back({
                "guide", "学習ガイド", guideOk, true,
                guideOk
                    ? "LEARNING.mdを確認しました。"
                    : "LEARNING.mdがありません。learn initで追加できます。"
            });

            std::optional<LearningJourney> journey;
            std::string journeyDetail;
            try
            {
                journey = LoadLearningJourney(projectRoot);
                journeyDetail = std::to_string(
                    journey->steps.size()) + "個のステップを確認しました。";
            }
            catch (const std::exception& exception)
            {
                journeyDetail = exception.what();
            }
            report.checks.push_back({
                "journey", "学習カリキュラム",
                journey.has_value(), true, std::move(journeyDetail)
            });

            bool filesOk = journey.has_value();
            std::vector<std::string> missing;
            if (journey.has_value())
            {
                std::unordered_set<std::string> visited;
                for (const auto& step : journey->steps)
                {
                    for (const auto& path : step.files)
                    {
                        if (visited.insert(path).second
                            && !std::filesystem::exists(
                                projectRoot / PathFromUtf8(path)))
                        {
                            missing.push_back(path);
                            filesOk = false;
                        }
                    }
                }
            }
            report.checks.push_back({
                "materials", "教材ファイル", filesOk, true,
                filesOk
                    ? "全ステップが参照するファイルを確認しました。"
                    : missing.empty()
                        ? "カリキュラムを読めないため確認できません。"
                        : "不足: " + missing.front()
                            + (missing.size() > 1
                                ? " ほか"
                                    + std::to_string(missing.size() - 1)
                                    + "件"
                                : std::string{})
            });

            bool progressOk = true;
            std::string progressDetail =
                "未開始です（最初の完了時に作成されます）。";
            if (std::filesystem::is_regular_file(
                    LearningProgressPath(projectRoot)))
            {
                try
                {
                    const auto progress = LoadLearningProgress(
                        projectRoot);
                    progressDetail = std::to_string(
                        progress.completedStepIds.size())
                        + "個の完了記録を確認しました。";
                }
                catch (const std::exception& exception)
                {
                    progressOk = false;
                    progressDetail = exception.what();
                }
            }
            report.checks.push_back({
                "progress", "個人の進捗", progressOk, false,
                std::move(progressDetail)
            });
        }
        catch (const std::exception& exception)
        {
            report.checks.push_back({
                "unexpected", "診断", false, true, exception.what()
            });
        }

        report.ready = std::ranges::all_of(
            report.checks,
            [](const LearningCheck& check)
            {
                return !check.required || check.ok;
            });
        return report;
    }

    std::string LearningPhaseDisplayName(const std::string& phase)
    {
        if (phase == "play") return "制作を始める";
        if (phase == "question") return "仕組みを確認する";
        if (phase == "understand") return "C++を理解する";
        if (phase == "build") return "ビルドして動かす";
        if (phase == "modify") return "ゲームを作る";
        if (phase == "choose") return "次の制作";
        return phase;
    }

    std::string LearningRoleDisplayName(const std::string& role)
    {
        if (role == "engineer") return "エンジニア";
        if (role == "planner") return "プランナー";
        if (role == "designer") return "デザイナー";
        if (role == "explorer") return "探索中";
        if (role == "undecided") return "まだ決めない";
        return role;
    }
}
