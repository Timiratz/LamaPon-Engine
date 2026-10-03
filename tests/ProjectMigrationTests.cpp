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
#include <utility>
#include <vector>

namespace
{
    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // WriteFile(path: 出力先, contents: ファイル内容): 親フォルダーを作ってバイナリ保存する。
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // 作成するテストファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        // ファイル作成の失敗を検出する。
        if (!output)
        {
            throw std::runtime_error(
                "Could not create a test file.");
        }
        output << contents;
    }

    // ReadFile(path: 入力元): ファイル全体を文字列として読む。
    std::string ReadFile(
        const std::filesystem::path& path)
    {
        // 読み込むプロジェクトファイル
        std::ifstream input(path, std::ios::binary);
        // 入力ファイルを開けない場合は空文字列を返す。
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }
}

// main(): 移行、版判定、設定保存の互換性を検証する。
int main()
{
    // 検査失敗を終了コードへ変換する。
    try
    {
        // テスト成果物の保存先
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "project-migration";
        std::filesystem::remove_all(root);

        // 最新エンジンの配布アセット
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

        // 古い設定と一部変更済みアセットを持つプロジェクト
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

        // 最初のアセット移行結果
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

        // 既存かつ内容が異なるファイルは、利用者の改造か古い公式版か区別できないため、どちらも一律で .bak へ退避されます（欠落していたEnvironmentは新規作成なので退避されません）。
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

        // 版番号の更新後も既存プロジェクト設定を保持する。
        {
            // 移行後のプロジェクト設定ファイル
            std::ifstream input(
                projectRoot / ".lamapon" / "project.json",
                std::ios::binary);
            // 読み込んだプロジェクト設定JSON
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

        // 2回目の移行では何も変更されない。
        // 最新状態へ適用した再移行結果
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

        // 改行差だけではアセットを書き換えない。
        WriteFile(
            engineAssets / "shaders" / "LamaPonLit.hlsl",
            "// lit v2\r\nline2\r\n");
        WriteFile(
            projectRoot / "assets" / "shaders"
                / "LamaPonLit.hlsl",
            "// lit v2\nline2\n");
        // 改行だけ異なる再移行結果
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

        // エンジンアセットのない環境では移行を行わない。
        // 存在しないエンジンアセットルートの結果
        const auto missing = LamaPon::MigrateProjectAssets(
            projectRoot,
            root / "does-not-exist",
            "2026.8.1");
        Require(
            !missing.changed,
            "a missing engine asset root must be a no-op");

        // compare(left: 比較元版, right: 比較先版): 数値版の比較結果を返す。
        const auto compare =
            [](const char* left, const char* right)
        {
            // バージョン比較の結果
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

        // project.jsonのengineVersionによる版判定を確認する。
        // 読み書きするproject.json
        const auto settingsPath =
            projectRoot / ".lamapon" / "project.json";
        // recordVersion(version: 保存する版): project.jsonへエンジン版を記録する。
        const auto recordVersion =
            [&settingsPath](const char* version)
        {
            // 更新するプロジェクト設定JSON
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
        // 新しい形式のプロジェクトを古いエディターで開くと設定が失われるため、エディターより新しい版は拒否します。
        recordVersion("2026.9.1");
        // エンジンより新しいプロジェクトの判定結果
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

        // バージョンが無い、または読めない場合は移行対象として扱い、手動編集されたproject.jsonも開けるようにします。
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

        // project.jsonの保存後も他機能のキーを保持する。
        {
            // 読み書きするプロジェクト設定ファイル
            const auto settingsFile =
                projectRoot / ".lamapon" / "project.json";
            // 保存検証用の基準設定
            nlohmann::json before;
            before["format"] = "LamaPonProject";
            before["version"] = 1;
            before["gameName"] = "VersionKeepTest";
            before["engineVersion"] = "2026.8.5";
            before["somethingElseEntirely"] = 42;
            // renderingApi未設定時はDirectX 11へ既定化する。
            before["graphics"] = {
                { "preset", "High" }
            };
            WriteFile(settingsFile, before.dump(2));

            // JSONから読み込んだ設定
            const auto loaded =
                LamaPon::LoadProjectSettings(settingsFile);
            Require(
                loaded.graphics.renderingApi
                    == LamaPon::RenderingApi::DirectX11,
                "project settings without a rendering API must"
                " default to DirectX 11");
            LamaPon::SaveProjectSettings(
                settingsFile,
                loaded,
                LamaPon::ProjectSettingsFileType::Project);

            // 保存後のproject.json
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

        // 描画APIのJSON名と保存往復。
        // 起動中のBackend状態とは分離し、設定値として安全に保存・再読み込みできる必要があります。
        {
            Require(
                LamaPon::RenderingApiName(
                    LamaPon::RenderingApi::Auto) == "Auto"
                    && LamaPon::RenderingApiName(
                        LamaPon::RenderingApi::DirectX11)
                        == "DirectX11"
                    && LamaPon::RenderingApiName(
                        LamaPon::RenderingApi::
                            DirectX12Experimental)
                        == "DirectX12Experimental",
                "rendering API names must match their JSON values");
            Require(
                LamaPon::RenderingApiFromName("Auto")
                        == LamaPon::RenderingApi::Auto
                    && LamaPon::RenderingApiFromName("DirectX11")
                        == LamaPon::RenderingApi::DirectX11
                    && LamaPon::RenderingApiFromName(
                        "DirectX12Experimental")
                        == LamaPon::RenderingApi::
                            DirectX12Experimental,
                "rendering API JSON values must parse");
            Require(
                LamaPon::RenderingApiFromName("FutureApi")
                    == LamaPon::RenderingApi::DirectX11,
                "an unknown rendering API name must fall back to"
                " DirectX 11");
            Require(
                LamaPon::RenderingApiName(
                    static_cast<LamaPon::RenderingApi>(-1))
                    == "DirectX11",
                "an invalid rendering API value must have a safe"
                " JSON name");

            // 無効な描画API値を持つ設定
            LamaPon::GraphicsSettings invalidGraphics;
            invalidGraphics.renderingApi =
                static_cast<LamaPon::RenderingApi>(-1);
            Require(
                LamaPon::ClampGraphicsSettings(invalidGraphics)
                        .renderingApi
                    == LamaPon::RenderingApi::DirectX11,
                "an invalid rendering API value must clamp to"
                " DirectX 11");

            struct RenderingApiCase final
            {
                // 保存往復を検査する描画API
                LamaPon::RenderingApi api;
                // JSONへ保存するAPI名
                const char* name;
            };
            // 保存往復を確認する描画API一覧
            constexpr RenderingApiCase cases[]{
                { LamaPon::RenderingApi::DirectX11, "DirectX11" },
                { LamaPon::RenderingApi::Auto, "Auto" },
                {
                    LamaPon::RenderingApi::DirectX12Experimental,
                    "DirectX12Experimental"
                }
            };
            // API設定を書き出すテストファイル
            const auto settingsFile =
                projectRoot / ".lamapon" / "rendering-api.json";
            // Project形式とGamePackage形式を比較する。
            for (const auto fileType : {
                    LamaPon::ProjectSettingsFileType::Project,
                    LamaPon::ProjectSettingsFileType::GamePackage })
            {
                // 各描画API値を保存・再読込する。
                for (const auto& testCase : cases)
                {
                    // 現在の描画API設定
                    LamaPon::ProjectSettings settings;
                    settings.graphics.renderingApi = testCase.api;
                    LamaPon::SaveProjectSettings(
                        settingsFile,
                        settings,
                        fileType);

                    // 保存された描画API設定JSON
                    const auto saved = nlohmann::json::parse(
                        ReadFile(settingsFile));
                    Require(
                        saved.at("graphics")
                                .at("renderingApi")
                                .get<std::string>()
                            == testCase.name,
                        "the rendering API JSON value was not saved");
                    Require(
                        LamaPon::LoadProjectSettings(settingsFile)
                                .graphics.renderingApi
                            == testCase.api,
                        "the rendering API did not survive the"
                        " project settings round trip");
                }
            }

            // 未知の描画API名を含む設定JSON
            nlohmann::json unknown;
            unknown["format"] = "LamaPonProject";
            unknown["version"] = 1;
            unknown["graphics"] = {
                { "renderingApi", "FutureApi" }
            };
            WriteFile(settingsFile, unknown.dump(2));
            Require(
                LamaPon::LoadProjectSettings(settingsFile)
                        .graphics.renderingApi
                    == LamaPon::RenderingApi::DirectX11,
                "an unknown project rendering API must load as"
                " DirectX 11");
            std::filesystem::remove(settingsFile);
        }

        // Project/GamePackageで公開online設定だけを往復し、資格情報を除外する。
        {
            // オンライン設定を書き出すテストファイル
            const auto settingsFile =
                projectRoot / ".lamapon" / "online.json";
            // 保存・再読込するオンライン設定
            LamaPon::ProjectSettings settings;
            settings.online.enabled = true;
            settings.online.serviceBaseUrl =
                "https://online.example.test/api";
            settings.online.gameId = "com.example.online-game";
            settings.online.environmentId = "staging_2";
            settings.online.openAuthorizationBrowser = false;
            settings.online.discordPresence.enabled = true;
            settings.online.discordPresence.applicationId =
                "123456789012345678";
            settings.online.discordPresence
                .defaultLargeImageKey = "game_icon";
            settings.online.discordPresence
                .defaultLargeImageText = "My Awesome Game";

            // ProjectとGamePackageの保存形式を確認する。
            for (const auto fileType : {
                    LamaPon::ProjectSettingsFileType::Project,
                    LamaPon::ProjectSettingsFileType::
                        GamePackage })
            {
                // 手編集で秘密鍵が混入したJSONを用意する。
                WriteFile(
                    settingsFile,
                    R"({"online":{"client_secret":"leak",)"
                    R"("accessToken":"leak","refreshToken":"leak"}})");
                LamaPon::SaveProjectSettings(
                    settingsFile,
                    settings,
                    fileType);
                // 保存後に復元したオンライン設定
                const auto loaded =
                    LamaPon::LoadProjectSettings(settingsFile);
                Require(
                    loaded.online.enabled
                        && loaded.online.serviceBaseUrl
                            == settings.online.serviceBaseUrl
                        && loaded.online.gameId
                            == settings.online.gameId
                        && loaded.online.environmentId
                            == settings.online.environmentId
                        && !loaded.online.allowInsecureLoopback
                        && !loaded.online
                            .openAuthorizationBrowser,
                    "online project settings must survive both"
                    " project and game-package round trips");
                Require(
                    loaded.online.discordPresence.enabled
                        && loaded.online.discordPresence
                                .applicationId
                            == settings.online.discordPresence
                                .applicationId
                        && loaded.online.discordPresence
                                .defaultLargeImageKey
                            == settings.online.discordPresence
                                .defaultLargeImageKey
                        && loaded.online.discordPresence
                                .defaultLargeImageText
                            == settings.online.discordPresence
                                .defaultLargeImageText,
                    "Discord rich presence settings must"
                    " survive project and game-package round"
                    " trips");

                // 保存されたオンライン設定JSON
                const auto document = nlohmann::json::parse(
                    ReadFile(settingsFile));
                // 保存されたonlineオブジェクト
                const auto& online = document.at("online");
                Require(
                    online.size() == 7
                        && !online.contains("client_secret")
                        && !online.contains("clientSecret")
                        && !online.contains("accessToken")
                        && !online.contains("refreshToken")
                        && !online.contains("token"),
                    "project settings must never retain Discord"
                    " secrets or session tokens");
                // 保存されたDiscord presence公開設定
                const auto& presence =
                    online.at("discordPresence");
                Require(
                    presence.size() == 4
                        && presence.contains("enabled")
                        && presence.contains("applicationId")
                        && presence.contains(
                            "defaultLargeImageKey")
                        && presence.contains(
                            "defaultLargeImageText"),
                    "Discord rich presence must store only its"
                    " four public settings");
            }

            // 明示したloopback HTTPはProjectだけ許可し、GamePackageでは拒否する。
            // ローカル開発用loopback設定
            auto development = settings;
            development.online.serviceBaseUrl =
                "http://127.0.0.1:8080";
            development.online.allowInsecureLoopback = true;
            LamaPon::ValidateProjectSettings(
                development,
                LamaPon::ProjectSettingsFileType::Project);
            LamaPon::SaveProjectSettings(
                settingsFile,
                development,
                LamaPon::ProjectSettingsFileType::Project);
            Require(
                LamaPon::LoadProjectSettings(settingsFile)
                    .online.allowInsecureLoopback,
                "an explicitly enabled loopback URL must be"
                " available to local project development");

            // GamePackage保存時の拒否結果
            bool rejected = false;
            // 配布設定へのloopback URL保存を検査する。
            try
            {
                LamaPon::SaveProjectSettings(
                    settingsFile,
                    development,
                    LamaPon::ProjectSettingsFileType::
                        GamePackage);
            }
            // 安全制約の拒否を記録する。
            catch (const std::exception&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "an enabled game package must reject the insecure"
                " loopback development switch");

            // formatを書き換えたGamePackageも読込時に拒否する。
            // 起動側で保存時検査を迂回させない。
            // 手編集した配布形式の設定JSON
            auto packaged = nlohmann::json::parse(
                ReadFile(settingsFile));
            packaged["format"] = "LamaPonGame";
            WriteFile(settingsFile, packaged.dump(2));
            rejected = false;
            // 読み込み制約を再検査する。
            try
            {
                static_cast<void>(
                    LamaPon::LoadProjectSettings(settingsFile));
            }
            // 配布形式読込の拒否を記録する。
            catch (const std::exception&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "loading a packaged game must reject the insecure"
                " loopback development switch");

            // 欠落値や危険なnamespaceを持つ設定は拒否する。
            // 不完全または危険な値を持つonline設定を拒否する。
            for (auto invalid : {
                    LamaPon::OnlineProjectSettings{
                        true,
                        {},
                        "game",
                        "production",
                        false,
                        true },
                    LamaPon::OnlineProjectSettings{
                        true,
                        "http://example.test",
                        "game",
                        "production",
                        true,
                        true },
                    LamaPon::OnlineProjectSettings{
                        true,
                        "https://online.example.test",
                        "bad/game",
                        "production",
                        false,
                        true },
                    LamaPon::OnlineProjectSettings{
                        true,
                        "https://online.example.test",
                        "game",
                        "bad environment",
                        false,
                        true } })
            {
                // 検査する無効値を適用した設定
                auto invalidSettings = settings;
                invalidSettings.online = std::move(invalid);
                rejected = false;
                // 無効なonline設定の検証を試す。
                try
                {
                    LamaPon::ValidateProjectSettings(
                        invalidSettings);
                }
                // 無効設定の拒否を記録する。
                catch (const std::exception&)
                {
                    rejected = true;
                }
                Require(
                    rejected,
                    "an incomplete or unsafe online project setting"
                    " was accepted");
            }

            // 手編集JSONの6項目は型変換せず、不正型を拒否する。
            // boolの暗黙変換で開発用通信が有効になるのを防ぐ。
            // 各online項目へ与える不正型
            const std::vector<std::pair<
                std::string,
                nlohmann::json>> invalidTypes{
                { "enabled", 1 },
                { "serviceBaseUrl", false },
                { "gameId", 7 },
                {
                    "environmentId",
                    nlohmann::json::array()
                },
                { "allowInsecureLoopback", "true" },
                { "openAuthorizationBrowser", nullptr }
            };
            // field: 対象キー、invalidValue: 不正型を検査する。
            for (const auto& [field, invalidValue] : invalidTypes)
            {
                // 有効値を持つ基準onlineオブジェクト
                nlohmann::json online{
                    { "enabled", true },
                    {
                        "serviceBaseUrl",
                        "https://online.example.test"
                    },
                    { "gameId", "com.example.type-test" },
                    { "environmentId", "production" },
                    { "allowInsecureLoopback", false },
                    { "openAuthorizationBrowser", true }
                };
                online[field] = invalidValue;
                // 不正型を設定したオンラインJSON
                WriteFile(
                    settingsFile,
                    nlohmann::json{
                        { "format", "LamaPonProject" },
                        { "online", std::move(online) }
                    }.dump(2));
                rejected = false;
                // 型不一致の読込を検査する。
                try
                {
                    static_cast<void>(
                        LamaPon::LoadProjectSettings(settingsFile));
                }
                // 型不一致の拒否を記録する。
                catch (const std::exception&)
                {
                    rejected = true;
                }
                Require(
                    rejected,
                    ("an online JSON field accepted the wrong type: "
                        + field).c_str());
            }
            std::filesystem::remove(settingsFile);
        }

        // ProjectとGamePackageで物理・表示設定が往復する。
        {
            // 物理設定を書き出すテストファイル
            const auto settingsFile =
                projectRoot / ".lamapon" / "physics.json";
            // 保存・再読込するプロジェクト設定
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
            // PlayerとEnemyの衝突を無効にする。
            settings.physics.layerNames[1] = "Player";
            settings.physics.layerNames[2] = "Enemy";
            settings.physics.collisionMatrix[1] &=
                ~(1u << 2);
            settings.physics.collisionMatrix[2] &=
                ~(1u << 1);
            settings.loadingScreen.message = "移動中...";
            settings.loadingScreen.showSpinner = true;
            LamaPon::ValidateProjectSettings(settings);

            // ProjectとGamePackageの往復を確認する。
            for (const auto fileType : {
                    LamaPon::ProjectSettingsFileType::Project,
                    LamaPon::ProjectSettingsFileType::
                        GamePackage })
            {
                LamaPon::SaveProjectSettings(
                    settingsFile,
                    settings,
                    fileType);
                // 保存後に読み込んだプロジェクト設定
                const auto loaded =
                    LamaPon::LoadProjectSettings(
                        settingsFile);
                Require(
                    !loaded.splashScreenEnabled,
                    "the startup splash setting must survive the"
                    " round trip");
                // 読み込み画面は書き出したゲームでも使うため、Project／GamePackageの両方で往復します。
                Require(
                    loaded.loadingScreen.message == "移動中..."
                        && loaded.loadingScreen.showSpinner,
                    "loading screen settings must survive the"
                    " round trip");
                // Project形式にだけ保存するエディター視点設定
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
                // 変更していない衝突ペアは既定値を維持する。
                Require(
                    (loaded.physics.collisionMatrix[1]
                        & (1u << 2)) == 0
                        && (loaded.physics
                            .collisionMatrix[2]
                            & (1u << 1)) == 0
                        && (loaded.physics
                            .collisionMatrix[0]
                            & (1u << 1)) != 0,
                    "the collision matrix must survive the"
                    " round trip");
            }

            // 廃止済みsceneTransitionを読み飛ばし、保存時に削除する。
            {
                // 古い形式を追加するJSON文書
                nlohmann::json legacy;
                {
                    // 現在の設定JSONを読む入力
                    std::ifstream input(settingsFile);
                    input >> legacy;
                }
                legacy["sceneTransition"] = {
                    { "effect", "iris" },
                    { "coverDuration", 0.55 }
                };
                {
                    // 古いtransition設定を書き込む出力
                    std::ofstream output(settingsFile);
                    output << legacy.dump(2);
                }
                // 旧形式を読み込んだ設定
                const auto loaded =
                    LamaPon::LoadProjectSettings(settingsFile);
                LamaPon::SaveProjectSettings(
                    settingsFile,
                    loaded,
                    LamaPon::ProjectSettingsFileType::Project);
                // 保存後の互換JSON
                nlohmann::json saved;
                {
                    // 保存結果を確認する入力
                    std::ifstream input(settingsFile);
                    input >> saved;
                }
                Require(
                    loaded.loadingScreen.message == "移動中..."
                        && !saved.contains("sceneTransition"),
                    "a legacy scene transition setting must be dropped");
            }
            std::filesystem::remove(settingsFile);
        }

        // 物理更新を停止させる無効な刻み幅を拒否する。
        {
            // 検証対象の物理設定
            LamaPon::ProjectSettings settings;
            settings.physics.fixedTimeStep = 0.0f;
            // 無効値の拒否結果
            bool rejected = false;
            // 無効な物理設定を検査する。
            try
            {
                LamaPon::ValidateProjectSettings(settings);
            }
            // 無効値の拒否を記録する。
            catch (const std::exception&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "a zero fixed time step must be rejected");
        }

        // 設定画面を通らない無効物理値も有効範囲へ丸める。
        {
            // 無効値を持つ物理設定
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

        // 組込shaderのinclude依存が配布一覧へ含まれることを検証する。
        // 配布先は同じフォルダーなのでファイル名で照合する。
        {
            // プロジェクトへ配布する組込アセット
            const auto& builtIns =
                LamaPon::BuiltInProjectAssets();
            Require(
                !builtIns.empty(),
                "the built-in asset list must not be empty");

            // エンジン側shader素材の場所
            const std::filesystem::path engineShaders{
                LAMAPON_ENGINE_ASSET_DIR };
            // 配布アセットのファイル名一覧
            std::vector<std::string> shipped;
            // 全配布アセットを確認する。
            for (const auto& relative : builtIns)
            {
                shipped.push_back(
                    relative.filename().string());
                // エンジン側にある配布元ファイル
                const auto source =
                    engineShaders / relative;
                Require(
                    std::filesystem::is_regular_file(source),
                    ("a built-in asset must exist in the"
                        " engine assets: "
                        + relative.string()).c_str());
            }

            // 各shaderのinclude先を配布一覧と照合する。
            for (const auto& relative : builtIns)
            {
                // 読み込んだshaderのソース
                const auto text =
                    ReadFile(engineShaders / relative);
                // include検索を続ける位置
                std::size_t cursor = 0;
                // 次のincludeディレクティブを探す。
                while (true)
                {
                    // 次に見つかったinclude位置
                    const auto found =
                        text.find("#include", cursor);
                    // 残りのincludeがなければ走査を終える。
                    if (found == std::string::npos)
                    {
                        // include走査を終了する。
                        break;
                    }
                    // includeパスの開始引用符
                    const auto open =
                        text.find('"', found);
                    // 引用符のない不正行以降は調べない。
                    if (open == std::string::npos)
                    {
                        // 以降のinclude走査を終了する。
                        break;
                    }
                    // includeパスの終了引用符
                    const auto close =
                        text.find('"', open + 1);
                    // 閉じ引用符がなければ走査を終える。
                    if (close == std::string::npos)
                    {
                        // 以降のinclude走査を終了する。
                        break;
                    }
                    // 抽出したincludeパス
                    const auto included = text.substr(
                        open + 1,
                        close - open - 1);
                    cursor = close + 1;
                    // 配布先の同じフォルダーにあるファイル名を得る。
                    // includeパス内の最後の区切り位置
                    const auto slash =
                        included.find_last_of("/\\");
                    // 配布一覧と照合するincludeファイル名
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

        // include以外でエンジンが直接読むshaderも配布一覧と照合する。
        // ソース中の文字列パス参照を調べる。
        {
            // プロジェクトへ配布する組込アセット
            const auto& builtIns =
                LamaPon::BuiltInProjectAssets();
            // 配布アセットのファイル名一覧
            std::vector<std::string> shipped;
            // 配布アセットの名前を収集する。
            for (const auto& relative : builtIns)
            {
                shipped.push_back(relative.filename().string());
            }

            // ソース中のshaderパスを示す接頭辞
            constexpr std::string_view marker{ "\"shaders/" };
            // エンジンソース内のファイルを調べる。
            for (const auto& file :
                std::filesystem::recursive_directory_iterator{
                    std::filesystem::path{
                        LAMAPON_ENGINE_SOURCE_DIR } })
            {
                // 配布一覧との比較対象になるソース拡張子
                if (!file.is_regular_file())
                {
                    // 通常ファイル以外を飛ばす。
                    continue;
                }
                // 現在のファイル拡張子
                const auto extension =
                    file.path().extension().string();
                // C++ソースとヘッダー以外を飛ばす。
                if (extension != ".cpp" && extension != ".h")
                {
                    // 対象外ファイルを飛ばす。
                    continue;
                }

                // 読み込んだソースファイル
                const auto text = ReadFile(file.path());
                // shader参照検索を続ける位置
                std::size_t cursor = 0;
                // 次のshaderパス参照を探す。
                while (true)
                {
                    // 次に見つかった参照位置
                    const auto found = text.find(marker, cursor);
                    // 参照が残っていなければ走査を終える。
                    if (found == std::string::npos)
                    {
                        // 参照走査を終了する。
                        break;
                    }
                    // shaderパスの先頭位置
                    const auto open = found + 1;
                    // shaderパスの終端引用符
                    const auto close = text.find('"', open);
                    // 閉じ引用符がなければ走査を終える。
                    if (close == std::string::npos)
                    {
                        // 参照走査を終了する。
                        break;
                    }
                    // 抽出したshader参照
                    const auto reference =
                        text.substr(open, close - open);
                    cursor = close + 1;
                    // HLSL拡張子でない文字列断片を飛ばす。
                    if (!reference.ends_with(".hlsl")
                        && !reference.ends_with(".hlsli"))
                    {
                        // shaderファイルでない断片を飛ばす。
                        continue;
                    }
                    // 配布一覧と照合するshaderファイル名
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
    // 例外(exception: 検査失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
