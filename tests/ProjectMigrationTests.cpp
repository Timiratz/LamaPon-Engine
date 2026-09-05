#include "LamaPon/Core/ProjectMigration.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Physics/PhysicsSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
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
                "Could not create a test file.");
        }
        output << contents;
    }

    std::string ReadFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "project-migration";
        std::filesystem::remove_all(root);

        // エンジン側（最新）の組み込みシェーダー。
        const auto engineAssets = root / "engine" / "assets";
        WriteFile(
            engineAssets / "shaders" / "LamaPonLit.hlsl",
            "// lit v2");
        WriteFile(
            engineAssets / "shaders"
                / "LamaPonCustomMaterial.hlsl",
            "// custom v2");
        WriteFile(
            engineAssets / "shaders"
                / "LamaPonEnvironment.hlsl",
            "// environment v2");
        WriteFile(
            engineAssets / "textures"
                / "LamaPonEngineLogo.png",
            "fake logo");

        // 旧エンジンで作られたプロジェクト。
        // Lit=古い / Custom=利用者が改造 / Environment=欠落。
        const auto projectRoot = root / "project";
        WriteFile(
            projectRoot / ".lamapon" / "project.json",
            R"({"format":"LamaPonProject","version":1,)"
            R"("gameName":"旧プロジェクト"})");
        WriteFile(
            projectRoot / "assets" / "shaders"
                / "LamaPonLit.hlsl",
            "// lit v1");
        WriteFile(
            projectRoot / "assets" / "shaders"
                / "LamaPonCustomMaterial.hlsl",
            "// 改造済み");

        const auto first = LamaPon::MigrateProjectAssets(
            projectRoot,
            engineAssets,
            "2026.8.1");
        Require(
            first.changed,
            "an outdated project must report changes");
        Require(
            first.previousEngineVersion.empty(),
            "projects without a recorded version report empty");
        Require(
            first.updatedAssets.size() == 4,
            "outdated, modified, and missing assets all update");
        Require(
            ReadFile(
                projectRoot / "assets" / "shaders"
                    / "LamaPonLit.hlsl") == "// lit v2",
            "the outdated shader must be replaced");
        Require(
            ReadFile(
                projectRoot / "assets" / "shaders"
                    / "LamaPonEnvironment.hlsl")
                == "// environment v2",
            "a missing built-in shader must be restored");

        // 既存かつ内容が異なるファイルは、利用者の改造か古い公式版か
        // 区別できないため、どちらも一律で .bak へ退避されます
        // （欠落していたEnvironmentは新規作成なので退避されません）。
        Require(
            first.backedUpAssets.size() == 2,
            "every pre-existing, differing asset is backed up");
        Require(
            ReadFile(
                projectRoot / "assets" / "shaders"
                    / "LamaPonCustomMaterial.hlsl.bak")
                == "// 改造済み",
            "the user's edits must be preserved in .bak");
        Require(
            ReadFile(
                projectRoot / "assets" / "shaders"
                    / "LamaPonLit.hlsl.bak")
                == "// lit v1",
            "the outdated shader's prior content must be preserved in .bak");

        // エンジンバージョンが記録され、他の設定は保持されます。
        {
            std::ifstream input(
                projectRoot / ".lamapon" / "project.json",
                std::ios::binary);
            nlohmann::json document;
            input >> document;
            Require(
                document.value("engineVersion", std::string{})
                    == "2026.8.1",
                "the engine version must be recorded");
            Require(
                document.value("gameName", std::string{})
                    == "旧プロジェクト",
                "existing project settings must be preserved");
        }

        // 2回目は何も変わりません（冪等）。
        const auto second = LamaPon::MigrateProjectAssets(
            projectRoot,
            engineAssets,
            "2026.8.1");
        Require(
            !second.changed
                && second.updatedAssets.empty()
                && second.backedUpAssets.empty(),
            "an up-to-date project must not be touched");
        Require(
            second.previousEngineVersion == "2026.8.1",
            "the recorded version must round-trip");

        // 改行コードだけが異なる場合は、プロジェクトを開くたびに
        // ファイルが更新されないよう書き換えを省略します。
        WriteFile(
            engineAssets / "shaders" / "LamaPonLit.hlsl",
            "// lit v2\r\nline2\r\n");
        WriteFile(
            projectRoot / "assets" / "shaders"
                / "LamaPonLit.hlsl",
            "// lit v2\nline2\n");
        const auto newlineOnly = LamaPon::MigrateProjectAssets(
            projectRoot,
            engineAssets,
            "2026.8.1");
        Require(
            !newlineOnly.changed
                && newlineOnly.updatedAssets.empty(),
            "a newline-only difference must not rewrite the asset");
        Require(
            ReadFile(
                projectRoot / "assets" / "shaders"
                    / "LamaPonLit.hlsl")
                == "// lit v2\nline2\n",
            "the project's line endings must be preserved");

        // エンジンのassetsが無い場合（ソースビルド等）は無害。
        const auto missing = LamaPon::MigrateProjectAssets(
            projectRoot,
            root / "does-not-exist",
            "2026.8.1");
        Require(
            !missing.changed,
            "a missing engine asset root must be a no-op");

        // 版番号の比較。カレンダー版なので桁数が揃わないことが
        // あります（"2026.8" と "2026.8.1"）。
        const auto compare =
            [](const char* left, const char* right)
        {
            const auto result =
                LamaPon::CompareEngineVersions(left, right);
            Require(
                result.has_value(),
                "a numeric version must compare");
            return *result;
        };
        Require(
            compare("2026.8.5", "2026.8.5") == 0,
            "the same version must compare equal");
        Require(
            compare("2026.8.4", "2026.8.5") < 0,
            "an older patch must compare smaller");
        Require(
            compare("2026.9.1", "2026.8.5") > 0,
            "a newer minor must compare larger");
        Require(
            compare("2026.8", "2026.8.1") < 0,
            "a missing component counts as zero");
        Require(
            compare("2026.8.5", "2026.8.5.1") < 0,
            "a same-day re-release is newer");
        Require(
            compare("2026.10.1", "2026.9.1") > 0,
            "versions must compare numerically, not as text");
        Require(
            !LamaPon::CompareEngineVersions(
                "2026.8.x", "2026.8.5").has_value(),
            "a non-numeric version must not compare");
        Require(
            !LamaPon::CompareEngineVersions(
                "", "2026.8.5").has_value(),
            "an empty version must not compare");

        // プロジェクトの判定。project.jsonのengineVersionを見ます。
        const auto settingsPath =
            projectRoot / ".lamapon" / "project.json";
        const auto recordVersion =
            [&settingsPath](const char* version)
        {
            nlohmann::json document;
            document["engineVersion"] = version;
            WriteFile(settingsPath, document.dump(2));
        };

        recordVersion("2026.8.5");
        Require(
            LamaPon::InspectProjectVersion(
                projectRoot, "2026.8.5").status
                == LamaPon::ProjectVersionStatus::Match,
            "the same version must be reported as a match");
        recordVersion("2026.8.1");
        Require(
            LamaPon::InspectProjectVersion(
                projectRoot, "2026.8.5").status
                == LamaPon::ProjectVersionStatus::Older,
            "an older project must be reported as older");
        // 新しい形式のプロジェクトを古いエディターで開くと設定が失われるため、
        // エディターより新しい版は拒否します。
        recordVersion("2026.9.1");
        const auto newer = LamaPon::InspectProjectVersion(
            projectRoot, "2026.8.5");
        Require(
            newer.status
                == LamaPon::ProjectVersionStatus::Newer,
            "a newer project must be reported as newer");
        Require(
            newer.recordedVersion == "2026.9.1",
            "the recorded version must be reported back so"
            " the message can name it");

        // バージョンが無い、または読めない場合は移行対象として扱い、
        // 手動編集されたproject.jsonも開けるようにします。
        WriteFile(settingsPath, "{}");
        Require(
            LamaPon::InspectProjectVersion(
                projectRoot, "2026.8.5").status
                == LamaPon::ProjectVersionStatus::Unrecorded,
            "a project without a version must be"
            " unrecorded");
        recordVersion("bogus");
        Require(
            LamaPon::InspectProjectVersion(
                projectRoot, "2026.8.5").status
                == LamaPon::ProjectVersionStatus::Unrecorded,
            "an unreadable version must not lock the"
            " project out");

        // プロジェクト設定を保存しても、他の仕組みが書いたキーが
        // 残ること。
        //
        // 保存後もengineVersionと未知のキーが保持されることを確認します。
        {
            const auto settingsFile =
                projectRoot / ".lamapon" / "project.json";
            // 保存検証用に、読み取り可能な設定を用意します。
            nlohmann::json before;
            before["format"] = "LamaPonProject";
            before["version"] = 1;
            before["gameName"] = "VersionKeepTest";
            before["engineVersion"] = "2026.8.5";
            before["somethingElseEntirely"] = 42;
            WriteFile(settingsFile, before.dump(2));

            const auto loaded =
                LamaPon::LoadProjectSettings(settingsFile);
            LamaPon::SaveProjectSettings(
                settingsFile,
                loaded,
                LamaPon::ProjectSettingsFileType::Project);

            const auto after = nlohmann::json::parse(
                ReadFile(settingsFile));
            Require(
                after.value("engineVersion", std::string{})
                    == "2026.8.5",
                "saving project settings must keep the"
                " recorded engine version");
            Require(
                after.value("somethingElseEntirely", 0)
                    == 42,
                "saving project settings must keep keys it"
                " does not own");
        }

        // 物理の設定が保存・読み込みで往復すること。
        //
        // 物理設定はエディターと書き出したゲームの両方で必要なため、
        // ProjectとRuntimeの設定へ保存されることを確認します。
        {
            const auto settingsFile =
                projectRoot / ".lamapon" / "physics.json";
            LamaPon::ProjectSettings settings;
            settings.splashScreenEnabled = false;
            settings.viewport.navigationPreset =
                LamaPon::ViewportNavigationPreset::Orbit;
            settings.viewport.orbitSensitivity = 1.25f;
            settings.viewport.panSensitivity = 0.75f;
            settings.viewport.zoomSensitivity = 1.5f;
            settings.viewport.invertY = true;
            settings.physics.gravity = { 1.5f, -3.0f, 0.25f };
            settings.physics.fixedTimeStep = 1.0f / 120.0f;
            settings.physics.maximumCatchUpSteps = 4;
            settings.physics.solverIterations = 12;
            settings.physics.sleepLinearVelocity = 0.1f;
            settings.physics.sleepAngularVelocity = 0.2f;
            settings.physics.sleepDelay = 1.25f;
            // レイヤー名と衝突マトリクス（1と2を当たらなくする）。
            settings.physics.layerNames[1] = "Player";
            settings.physics.layerNames[2] = "Enemy";
            settings.physics.collisionMatrix[1] &=
                ~(1u << 2);
            settings.physics.collisionMatrix[2] &=
                ~(1u << 1);
            LamaPon::ValidateProjectSettings(settings);

            for (const auto fileType : {
                    LamaPon::ProjectSettingsFileType::Project,
                    LamaPon::ProjectSettingsFileType::
                        GamePackage })
            {
                LamaPon::SaveProjectSettings(
                    settingsFile,
                    settings,
                    fileType);
                const auto loaded =
                    LamaPon::LoadProjectSettings(
                        settingsFile);
                Require(
                    !loaded.splashScreenEnabled,
                    "the startup splash setting must survive the"
                    " round trip");
                if (fileType
                    == LamaPon::ProjectSettingsFileType::Project)
                {
                    Require(
                        loaded.viewport.navigationPreset
                            == LamaPon::ViewportNavigationPreset::Orbit
                            && std::abs(
                                loaded.viewport.orbitSensitivity
                                - 1.25f) < 1e-6f
                            && std::abs(
                                loaded.viewport.panSensitivity
                                - 0.75f) < 1e-6f
                            && std::abs(
                                loaded.viewport.zoomSensitivity
                                - 1.5f) < 1e-6f
                            && loaded.viewport.invertY,
                        "viewport settings must survive the project"
                        " round trip");
                }
                Require(
                    loaded.physics.gravity.x == 1.5f
                        && loaded.physics.gravity.y == -3.0f
                        && loaded.physics.gravity.z == 0.25f,
                    "gravity must survive the round trip");
                Require(
                    loaded.physics.maximumCatchUpSteps == 4
                        && loaded.physics.solverIterations
                            == 12,
                    "the step limits must survive the round"
                    " trip");
                Require(
                    loaded.physics.sleepDelay == 1.25f,
                    "the sleep delay must survive the round"
                    " trip");
                Require(
                    std::abs(
                        loaded.physics.fixedTimeStep
                        - 1.0f / 120.0f) < 1e-6f,
                    "the fixed time step must survive the"
                    " round trip");
                Require(
                    loaded.physics.layerNames[1] == "Player"
                        && loaded.physics.layerNames[2]
                            == "Enemy",
                    "layer names must survive the round"
                    " trip");
                Require(
                    (loaded.physics.collisionMatrix[1]
                        & (1u << 2)) == 0
                        && (loaded.physics
                            .collisionMatrix[2]
                            & (1u << 1)) == 0
                        // 触っていないペアは既定（当たる）のまま。
                        && (loaded.physics
                            .collisionMatrix[0]
                            & (1u << 1)) != 0,
                    "the collision matrix must survive the"
                    " round trip");
            }
            std::filesystem::remove(settingsFile);
        }

        // 物理更新が停止しないよう、刻み幅0などの無効値を拒否します。
        {
            LamaPon::ProjectSettings settings;
            settings.physics.fixedTimeStep = 0.0f;
            bool rejected = false;
            try
            {
                LamaPon::ValidateProjectSettings(settings);
            }
            catch (const std::exception&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "a zero fixed time step must be rejected");
        }

        // 設定画面を通らないJSONやスクリプトからの値も有効範囲へ丸めます。
        {
            LamaPon::PhysicsSettings broken;
            broken.fixedTimeStep = 0.0f;
            broken.solverIterations = 0;
            LamaPon::SetActivePhysicsSettings(broken);
            Require(
                LamaPon::ActivePhysicsSettings()
                        .fixedTimeStep > 0.0f
                    && LamaPon::ActivePhysicsSettings()
                        .solverIterations >= 1,
                "SetActivePhysicsSettings must clamp values"
                " that would stop the simulation");
            LamaPon::SetActivePhysicsSettings(
                LamaPon::PhysicsSettings{});
        }

        // 組み込みアセットのincludeの取りこぼし検査。
        //
        // 組み込みシェーダーへ.hlsliのincludeを追加した場合も、参照先が
        // 配布一覧へ含まれることを機械的に確認します。
        {
            const auto& builtIns =
                LamaPon::BuiltInProjectAssets();
            Require(
                !builtIns.empty(),
                "the built-in asset list must not be empty");

            const std::filesystem::path engineShaders{
                LAMAPON_ENGINE_ASSET_DIR };
            std::vector<std::string> shipped;
            for (const auto& relative : builtIns)
            {
                shipped.push_back(
                    relative.filename().string());
                const auto source =
                    engineShaders / relative;
                Require(
                    std::filesystem::is_regular_file(source),
                    ("a built-in asset must exist in the"
                        " engine assets: "
                        + relative.string()).c_str());
            }

            for (const auto& relative : builtIns)
            {
                const auto text =
                    ReadFile(engineShaders / relative);
                std::size_t cursor = 0;
                while (true)
                {
                    const auto found =
                        text.find("#include", cursor);
                    if (found == std::string::npos)
                    {
                        break;
                    }
                    const auto open =
                        text.find('"', found);
                    if (open == std::string::npos)
                    {
                        break;
                    }
                    const auto close =
                        text.find('"', open + 1);
                    if (close == std::string::npos)
                    {
                        break;
                    }
                    const auto included = text.substr(
                        open + 1,
                        close - open - 1);
                    cursor = close + 1;
                    // パス付きで書かれていてもファイル名で照合
                    // します（配布先は同じフォルダーです）。
                    const auto slash =
                        included.find_last_of("/\\");
                    const auto name =
                        slash == std::string::npos
                        ? included
                        : included.substr(slash + 1);
                    Require(
                        std::find(
                            shipped.begin(),
                            shipped.end(),
                            name) != shipped.end(),
                        ("a built-in shader includes a file"
                            " that is not shipped with"
                            " projects: "
                            + relative.string()
                            + " -> "
                            + included).c_str());
                }
            }
        }

        // エンジンが名前で読むシェーダーの取りこぼし検査。
        //
        // include検査では直接参照されるシェーダーを検出できないため、
        // ソース内のシェーダーパスも配布一覧と照合します。
        {
            const auto& builtIns =
                LamaPon::BuiltInProjectAssets();
            std::vector<std::string> shipped;
            for (const auto& relative : builtIns)
            {
                shipped.push_back(relative.filename().string());
            }

            constexpr std::string_view marker{ "\"shaders/" };
            for (const auto& file :
                std::filesystem::recursive_directory_iterator{
                    std::filesystem::path{
                        LAMAPON_ENGINE_SOURCE_DIR } })
            {
                if (!file.is_regular_file())
                {
                    continue;
                }
                const auto extension =
                    file.path().extension().string();
                if (extension != ".cpp" && extension != ".h")
                {
                    continue;
                }

                const auto text = ReadFile(file.path());
                std::size_t cursor = 0;
                while (true)
                {
                    const auto found = text.find(marker, cursor);
                    if (found == std::string::npos)
                    {
                        break;
                    }
                    const auto open = found + 1;
                    const auto close = text.find('"', open);
                    if (close == std::string::npos)
                    {
                        break;
                    }
                    const auto reference =
                        text.substr(open, close - open);
                    cursor = close + 1;
                    // 「shaders/」で終わるだけの連結用の断片は
                    // 対象外（拡張子で見分けます）。
                    if (!reference.ends_with(".hlsl")
                        && !reference.ends_with(".hlsli"))
                    {
                        continue;
                    }
                    const auto name = std::filesystem::path{
                        reference }.filename().string();
                    Require(
                        std::find(
                            shipped.begin(),
                            shipped.end(),
                            name) != shipped.end(),
                        ("the engine loads a shader that is not"
                            " shipped with projects: "
                            + reference
                            + " (in "
                            + file.path().filename().string()
                            + ")").c_str());
                }
            }
        }

        std::cout << "Project migration tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
