#include "LamaPon/LamaPon.h"
#include "LamaPon/Editor/Editor.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <Windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Windowsの起動引数を文字列一覧で返します。
    std::vector<std::wstring> CommandLineArguments()
    {
        // Windowsから取得した引数の個数
        int argumentCount{};
        // Windowsが確保した引数配列
        auto** argumentValues = CommandLineToArgvW(
            GetCommandLineW(),
            &argumentCount);
        // OSから引数配列を取得できない場合は起動を中断します。
        if (argumentValues == nullptr)
        {
            throw std::runtime_error(
                "Could not read the editor command line.");
        }

        // コピー後に返す引数一覧
        std::vector<std::wstring> arguments;
        arguments.reserve(
            static_cast<std::size_t>(argumentCount));
        // 引数番号(index: 反復位置)
        for (int index = 0; index < argumentCount; ++index)
        {
            arguments.emplace_back(argumentValues[index]);
        }
        LocalFree(argumentValues);
        return arguments;
    }

    // 起動引数にflagがあるか調べます(flag: 照合するフラグ)。
    bool HasCommandLineFlag(const std::wstring_view flag)
    {
        // 現在のプロセス引数
        const auto arguments = CommandLineArguments();
        // CLI引数位置(index: 反復位置)
        for (std::size_t index = 1;
            index < arguments.size();
            ++index)
        {
            // 指定されたフラグとの一致
            if (arguments[index] == flag)
            {
                return true;
            }
        }
        return false;
    }

    // flagの次の引数を返し、無ければ空文字列を返します(flag: オプション名)。
    std::wstring CommandLineOptionValue(
        const std::wstring_view flag)
    {
        // 現在のプロセス引数
        const auto arguments = CommandLineArguments();
        // オプション位置(index: 反復位置)
        for (std::size_t index = 1;
            index + 1 < arguments.size();
            ++index)
        {
            // 一致したオプションの直後が値です。
            if (arguments[index] == flag)
            {
                return arguments[index + 1];
            }
        }
        return {};
    }

    // 起動引数と検証結果からプロジェクトのルートを返します。
    std::filesystem::path RequestedProjectRoot()
    {
        // 現在のプロセス引数
        const auto arguments = CommandLineArguments();
        // 既定のProject root
        std::filesystem::path projectRoot{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };

        // Project指定の位置(index: 反復位置)
        for (std::size_t index = 1;
            index < arguments.size();
            ++index)
        {
            // --projectの値をプロジェクトルートに設定します。
            if (arguments[index] == L"--project")
            {
                // オプションの値が無ければ起動引数を拒否します。
                if (index + 1 >= arguments.size())
                {
                    throw std::invalid_argument(
                        "--project requires a project folder path.");
                }
                projectRoot = arguments[++index];
                continue;
            }
            // 値付きオプションとその値を飛ばします。
            if (arguments[index] == L"--screenshot"
                || arguments[index] == L"--report"
                || arguments[index] == L"--show"
                || arguments[index] == L"--shot-frames"
                || arguments[index] == L"--remote")
            {
                ++index;
                continue;
            }
            // オプション以外の引数をプロジェクトルート候補にします。
            if (!arguments[index].empty()
                && arguments[index].front() != L'-')
            {
                projectRoot = arguments[index];
            }
        }

        // .lamapon/project.jsonが指定された場合は親のルートへ戻します。
        if (projectRoot.filename() == L"project.json"
            && projectRoot.parent_path().filename()
                == L".lamapon")
        {
            projectRoot = projectRoot.parent_path().parent_path();
        }
        // 有効なプロジェクトパスが無ければ起動を中断します。
        if (projectRoot.empty())
        {
            throw std::invalid_argument(
                "A LamaPon project folder was not specified.");
        }

        // settingsとAssetsの存在を検証するプロジェクト絶対パス
        projectRoot = std::filesystem::absolute(
            projectRoot).lexically_normal();
        // Project Settingsの保存先
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // Assetファイルのルート
        const auto assetRoot = projectRoot / L"assets";
        // SettingsかAssetsが無ければプロジェクトとして扱いません。
        if (!std::filesystem::is_regular_file(settingsPath)
            || !std::filesystem::is_directory(assetRoot))
        {
            throw std::runtime_error(
                "The selected folder is not a LamaPon project: "
                + LamaPon::PathToUtf8(projectRoot));
        }
        return std::filesystem::weakly_canonical(projectRoot);
    }
}

// Editorを起動します(instance: Win32アプリケーション識別子)。
int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    // 起動失敗時の初期ログ保存先
    std::filesystem::path logPath =
        LamaPon::ExecutableDirectory() / L"LamaPonEditor.log";
    LamaPon::CrashReporter::Install(
        LamaPon::ExecutableDirectory() / L"Crashes",
        "LamaPonEditor");
    // 起動処理全体の失敗を捕捉します。
    try
    {
        // 検証済みプロジェクトの絶対ルート
        const auto projectRoot = RequestedProjectRoot();
        logPath = projectRoot
            / L".lamapon"
            / L"LamaPonEditor.log";
        LamaPon::CrashReporter::Install(
            projectRoot / L".lamapon" / L"Crashes",
            "LamaPonEditor");

        // 同じプロジェクトの多重起動を防ぐロック
        LamaPon::ProjectInstanceLock projectInstance(projectRoot);
        // ロックを取れなければ同じプロジェクトを開きません。
        if (!projectInstance.Acquired())
        {
            MessageBoxW(
                nullptr,
                L"このプロジェクトは、すでにLamaPon Editorで開かれています。\n"
                L"同じプロジェクトを同時に複数開くことはできません。",
                L"LamaPon Editor",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            // 多重起動として終了します。
            return 2;
        }

        // 相対パスをプロジェクトルート基準にします。
        SetCurrentDirectoryW(projectRoot.c_str());

        // 前回起動が正常終了したか追跡します。
        LamaPon::CrashSentinel crashSentinel(projectRoot);
        // --safeでセーフモードを強制するか
        bool safeMode = HasCommandLineFlag(L"--safe");
        // 自動撮影先
        const std::wstring screenshotPath =
            CommandLineOptionValue(L"--screenshot");
        // 自動遠隔操作の接続先
        const std::wstring remotePath =
            CommandLineOptionValue(L"--remote");
        // ScreenshotまたはRemote起動か
        const bool unattended =
            !screenshotPath.empty()
            || !remotePath.empty();
        // 異常終了後の復旧選択は対話起動に限ります。
        if (crashSentinel.PreviousRunCrashed()
            && !safeMode
            && !unattended)
        {
            // 利用者が選択した復旧方法
            const int choice = MessageBoxW(
                nullptr,
                L"前回、LamaPon Editorは正常に終了しませんでした。\n"
                L"どのように開きますか？\n\n"
                L"[はい] 通常どおり開く\n"
                L"[いいえ] セーフモードで開く"
                L"（C++スクリプトを読み込みません）\n"
                L"[キャンセル] 開かない\n\n"
                L"クラッシュの記録は .lamapon\\Crashes に"
                L"保存されています。",
                L"LamaPon Editor",
                MB_YESNOCANCEL | MB_ICONWARNING
                    | MB_SETFOREGROUND);
            // CancelまたはMessageBox失敗時は起動しません。
            if (choice == IDCANCEL || choice == 0)
            {
                // 利用者が起動を取り消しました。
                return 0;
            }
            safeMode = choice == IDNO;
        }

        // VersionStringで版を照合し、旧Editorによる新版Project破損を防ぎます。
        // 検査したProjectの作成バージョン
        const auto versionInfo =
            LamaPon::InspectProjectVersion(
                projectRoot,
                LamaPon::VersionString);
        // 現在のEditorより新しい版で作られたProjectを拒否します。
        if (versionInfo.status
            == LamaPon::ProjectVersionStatus::Newer)
        {
            // Version不一致の案内文
            const auto message =
                L"このプロジェクトは、より新しいバージョンの"
                L"LamaPon Engineで作られています。\n\n"
                L"プロジェクト: v"
                + LamaPon::Utf8ToWide(
                    versionInfo.recordedVersion)
                + L"\nこのエディター: v"
                + LamaPon::Utf8ToWide(
                    std::string(LamaPon::VersionString))
                + L"\n\n古いエディターで開くと、新しい版で足された"
                L"設定が失われることがあります。\n"
                L"v"
                + LamaPon::Utf8ToWide(
                    versionInfo.recordedVersion)
                + L"以降のLamaPon Engineで開いてください。";
            // 無人起動ではダイアログを出さず終了します。
            if (!unattended)
            {
                MessageBoxW(
                    nullptr,
                    message.c_str(),
                    L"LamaPon Editor",
                    MB_OK | MB_ICONERROR
                        | MB_SETFOREGROUND);
            }
            // 旧Editorで新しいProjectを開いた場合の終了コード
            return 3;
        }

        // Engine版が古いか記録されていないか
        const bool needsUpgrade =
            versionInfo.status
                == LamaPon::ProjectVersionStatus::Older
            || versionInfo.status
                == LamaPon::ProjectVersionStatus::Unrecorded;
        // 古い／未記録版のProjectだけ更新確認を行います。
        if (needsUpgrade)
        {
            // 記録済みEngine版
            const std::wstring from =
                versionInfo.recordedVersion.empty()
                ? L"（記録なし）"
                : L"v" + LamaPon::Utf8ToWide(
                    versionInfo.recordedVersion);
            // 更新内容と旧版を示す確認メッセージ
            const auto message =
                L"このプロジェクトは、古いバージョンの"
                L"LamaPon Engineで作られています。\n\n"
                L"プロジェクト: " + from
                + L"\nこのエディター: v"
                + LamaPon::Utf8ToWide(
                    std::string(LamaPon::VersionString))
                + L"\n\n今のバージョンに合わせて更新してから"
                L"開きますか？\n"
                L"組み込みシェーダーが最新へ揃います"
                L"（自分で書き換えていたものは .bak へ残します）。\n\n"
                L"「いいえ」を選ぶと、開かずに終了します。";
            // 更新確認の選択結果
            const int choice =
                !unattended
                    ? MessageBoxW(
                        nullptr,
                        message.c_str(),
                        L"LamaPon Editor",
                        MB_YESNO | MB_ICONWARNING
                            | MB_SETFOREGROUND)
                    : IDYES;
            // 更新拒否またはDialog失敗時は終了します。
            if (choice != IDYES)
            {
                // Projectを変更せずに終了します。
                return 0;
            }
        }

        // 変更済みAssetを.bakへ退避して更新します。
        const auto migration = LamaPon::MigrateProjectAssets(
            projectRoot,
            LamaPon::ExecutableDirectory() / L"assets",
            LamaPon::VersionString);
        // Asset更新の利用者向け報告は対話起動に限ります。
        if (needsUpgrade
            && migration.changed
            && !unattended)
        {
            // 更新結果の件数と退避ファイルを示す報告
            std::wstring report =
                L"プロジェクトを v"
                + LamaPon::Utf8ToWide(
                    std::string(LamaPon::VersionString))
                + L" へ更新しました。\n\n更新した組み込みアセット: "
                + std::to_wstring(
                    migration.updatedAssets.size())
                + L"件";
            // 既存編集を退避したAssetがあれば列挙します。
            if (!migration.backedUpAssets.empty())
            {
                report += L"\n\n書き換えられていたため .bak へ"
                    L"退避したもの:";
                // 退避されたAssetのパスを追加します(backedUp: 退避パス)。
                for (const auto& backedUp :
                    migration.backedUpAssets)
                {
                    report += L"\n  "
                        + backedUp.wstring();
                }
            }
            MessageBoxW(
                nullptr,
                report.c_str(),
                L"LamaPon Editor",
                MB_OK | MB_ICONINFORMATION
                    | MB_SETFOREGROUND);
        }
        // 退避Assetをログへ記録します(backedUp: 退避パス)。
        for (const auto& backedUp : migration.backedUpAssets)
        {
            LamaPon::Logger::Instance().Warning(
                "編集されていた組み込みアセットを更新しました"
                "（元の内容は .bak へ保存しています）: "
                + LamaPon::PathToUtf8(backedUp));
        }
        // Assetが更新された場合だけ結果を記録します。
        if (!migration.updatedAssets.empty())
        {
            LamaPon::Logger::Instance().Info(
                "プロジェクトを現在のエンジン v"
                + std::string(LamaPon::VersionString)
                + " へ更新しました（"
                + std::to_string(
                    migration.updatedAssets.size())
                + "件の組み込みアセット）");
        }

        // Project Settingsの保存先
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // 保存済みProject Settings
        const auto projectSettings =
            LamaPon::LoadProjectSettings(settingsPath);
        // プロジェクトAssetのルート
        const auto assetRoot = projectRoot / L"assets";
        // 設定に記録された起動Sceneのパス
        const auto scenePath =
            assetRoot / projectSettings.startupScene;
        // 起動Sceneが欠落・破損してもEditorで修復できるよう開きます。
        // Scene問題の案内文
        std::wstring startupSceneProblem;
        // 起動Sceneを安全に読み込めなかったか
        bool startupSceneCorrupt = false;
        // 欠落時は保存先を保ち、破損時は空Pathで元ファイルを保護します。
        if (!std::filesystem::is_regular_file(scenePath))
        {
            startupSceneProblem =
                L"起動シーンのファイルが見つかりませんでした。\n\n"
                + scenePath.native()
                + L"\n\n空のシーンで開きます。プロジェクト設定で"
                L"起動シーンを選び直すか、このまま作って保存して"
                L"ください。";
        }

        // --warpでCPUラスタライザを使います。
        if (HasCommandLineFlag(L"--warp"))
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        }

        // --d3ddebugで選択中APIの検証レイヤーを有効にします。
        if (HasCommandLineFlag(L"--d3ddebug"))
        {
            LamaPon::GraphicsDevice::SetEnableDebugLayer(true);
        }

        // Build Label付きタイトル
        const auto windowTitle = LamaPon::Utf8ToWide(
            projectSettings.gameName + " - LamaPon Editor ("
            + LamaPon::FormatBuildLabel() + ")");
        // Editor Application
        LamaPon::Application application(
            windowTitle,
            1280,
            720,
            projectSettings.gameName);

        // Package Assetの解決先を設定します。
        LamaPon::SetGraphicsBackendPackageAssetRoot(assetRoot);
        // 選択APIで初期化し、D3D12失敗時はD3D11へ戻します。
        application.Initialize(
            instance,
            projectSettings.graphics.renderingApi);
        // Network設定を適用します。
        static_cast<void>(application.Network().Configure(projectSettings.network));
        // 対話起動かつ通常モードでOnline設定を有効化します。
        if (!safeMode
            && !unattended
            && projectSettings.online.enabled)
        {
            // Online Service接続設定
            LamaPon::OnlineServiceConfiguration online;
            online.serviceBaseUrl =
                projectSettings.online.serviceBaseUrl;
            online.allowInsecureLoopback =
                projectSettings.online.allowInsecureLoopback;
            online.gameId = projectSettings.online.gameId;
            online.environmentId =
                projectSettings.online.environmentId;
            online.openAuthorizationBrowser =
                projectSettings.online.openAuthorizationBrowser;
            application.Online().Configure(std::move(online));
        }
        // Discord Rich PresenceはOnlineログインと独立して有効化できます。
        if (!safeMode
            && !unattended
            && projectSettings.online.discordPresence.enabled)
        {
            // Discord Presence接続設定
            LamaPon::DiscordPresenceConfiguration presence;
            presence.enabled = true;
            presence.applicationId =
                projectSettings.online.discordPresence
                    .applicationId;
            presence.defaultLargeImageKey =
                projectSettings.online.discordPresence
                    .defaultLargeImageKey;
            presence.defaultLargeImageText =
                projectSettings.online.discordPresence
                    .defaultLargeImageText;
            application.Online().ConfigureDiscordPresence(
                std::move(presence));
        }
        // 描画品質・解像度などを反映します。
        application.Graphics().SetGraphicsSettings(
            projectSettings.graphics);
        // 保存済みInput Actionを適用します。
        application.Input().SetActions(
            projectSettings.inputActions);
        // Asset読込先をProjectへ設定します。
        application.Graphics().Assets().SetAssetRoot(assetRoot);
        // Safe ModeではShader失敗記録を初期化します。
        if (safeMode)
        {
            // 消去したShader失敗記録数
            const auto discarded =
                LamaPon::ClearShaderCacheFailures();
            LamaPon::Logger::Instance().Warning(
                std::string(
                    "セーフモードで起動しました。"
                    "C++ Game Moduleは読み込まれていません。"
                    "シェーダーの失敗の記録を")
                + std::to_string(discarded)
                + "件捨てました。");
        }
        // Safe Mode以外ではGame ModuleとNative SDKを読み込みます。
        else
        {
            // Package SDK DLLをGame Moduleの探索先へ加えます。
            application.GameModule().SetNativeSearchDirectories(
                LamaPon::PackageNativeSearchDirectories(
                    LamaPon::ScanPackageNativeDependencies(
                        assetRoot).packages));
            // Project固有のGame Moduleを読み込みます。
            static_cast<void>(application.GameModule().Load(
                projectRoot
                    / L".lamapon"
                    / L"bin"
                    / L"LamaPonGameModule.dll"));
        }
        // Sceneに問題が無ければファイルから開きます。
        if (startupSceneProblem.empty())
        {
            // Scene読込に失敗した途中状態を捕捉します。
            try
            {
                application.ActiveScene().LoadFromFile(
                    scenePath);
            }
            // Sceneの読込失敗理由(exception: 読込例外)。
            catch (const std::exception& exception)
            {
                // 半端なSceneを破棄し、元ファイルを保護します。
                application.ActiveScene().Clear();
                startupSceneCorrupt = true;
                startupSceneProblem =
                    L"起動シーンを読み込めませんでした。\n\n"
                    + scenePath.native()
                    + L"\n\n"
                    + LamaPon::Utf8ToWide(exception.what())
                    + L"\n\n空のシーンで開きます。元のファイルは"
                    L"そのまま残してあります。上書きしないよう、"
                    L"保存は「名前を付けて保存」になります。";
            }
        }
        // 問題のあるSceneを空のまま開く理由を表示します。
        if (!startupSceneProblem.empty())
        {
            LamaPon::Logger::Instance().Error(
                "起動シーンを開けませんでした: "
                + LamaPon::PathToUtf8(scenePath));
            MessageBoxW(
                nullptr,
                startupSceneProblem.c_str(),
                L"LamaPon Editor",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
        // ビルド設定既定のEngine root
        auto engineRoot = std::filesystem::path{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };
        // 実行中Editorの配置先
        const auto installedEngineRoot =
            LamaPon::ExecutableDirectory();
        // 配布Editorなら配置済みEngine rootを使います。
        if (std::filesystem::is_regular_file(
                installedEngineRoot
                    / L"tools"
                    / L"ProjectGameModule"
                    / L"CMakeLists.txt"))
        {
            engineRoot = installedEngineRoot;
        }
        // 無人撮影・Remote操作のオプション
        LamaPon::EditorScreenshotOptions screenshot;
        screenshot.imagePath = screenshotPath;
        screenshot.reportPath =
            CommandLineOptionValue(L"--report");
        screenshot.show = LamaPon::PathToUtf8(
            std::filesystem::path{
                CommandLineOptionValue(L"--show") });
        screenshot.remoteDirectory = remotePath;
        // 取得するスクリーンショットのフレーム番号
        if (const auto frames =
                CommandLineOptionValue(L"--shot-frames");
            !frames.empty())
        {
            // --shot-framesの値をcaptureFrameへ変換します。
            screenshot.captureFrame =
                static_cast<std::uint32_t>(
                    std::stoul(frames));
        }
        // 破損Sceneはパスを渡さずCtrl+Sで元ファイルを保護します。
        LamaPon::EnableEditor(
            application,
            startupSceneCorrupt
                ? std::filesystem::path{}
                : scenePath,
            std::move(engineRoot),
            LAMAPON_BUILD_CONFIGURATION,
            safeMode,
            unattended ? &screenshot : nullptr);

        // Editorの終了コード
        int exitCode{};
        // Editor実行中の例外を境界で変換します。
        try
        {
            exitCode = application.Run();
        }
        // Game Module解放前にDLL由来の例外情報を複製します(exception: 実行例外)。
        catch (const std::exception& exception)
        {
            throw std::runtime_error(exception.what());
        }
        // std::exception以外を共通例外へ変換します。
        catch (...)
        {
            throw std::runtime_error(
                "Unknown exception escaped from the editor runtime.");
        }

        // Safe Modeから通常起動へ切り替える要求を処理します。
        if (LamaPon::WasNormalModeRestartRequested())
        {
            // 正常終了を記録します。
            crashSentinel.MarkCleanExit();
            // 子Editor起動前にProject lockを解放します。
            projectInstance.Release();
            // 通常モード起動用のコマンドライン
            std::wstring commandLine = L"\""
                + (LamaPon::ExecutableDirectory()
                    / L"LamaPonEditor.exe").native()
                + L"\" --project \""
                + projectRoot.native()
                + L"\"";
            // Windows Process起動情報
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            // 子Editor Processの識別情報
            PROCESS_INFORMATION processInfo{};
            // Project rootを引き継いでEditorを再起動します。
            if (CreateProcessW(
                nullptr,
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                projectRoot.c_str(),
                &startupInfo,
                &processInfo))
            {
                CloseHandle(processInfo.hThread);
                CloseHandle(processInfo.hProcess);
            }
        }
        return exitCode;
    }
    // 起動例外を記録し、対話起動なら理由を表示します(exception: 起動例外)。
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        // ログ保存先の親Directoryを準備します。
        if (!logPath.parent_path().empty())
        {
            // Directory作成時のErrorCode
            std::error_code error;
            std::filesystem::create_directories(
                logPath.parent_path(),
                error);
        }
        // 例外の詳細を起動ログへ記録します。
        std::ofstream log(logPath, std::ios::trunc);
        log << exception.what() << '\n';

        // Screenshot起動ではDialogを出さずReportへ記録します。
        if (!CommandLineOptionValue(L"--screenshot").empty())
        {
            // 指定された失敗Reportの保存先
            if (const auto reportPath =
                    CommandLineOptionValue(L"--report");
                !reportPath.empty())
            {
                // Screenshot失敗のJSON本文
                const nlohmann::json failure{
                    { "ok", false },
                    { "error", exception.what() },
                };
                // 失敗JSONを書き出すReport
                std::ofstream report(
                    std::filesystem::path{ reportPath },
                    std::ios::trunc);
                // 不正UTF-8を置換してJSONを保ちます。
                report << failure.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::
                        replace);
            }
            // Screenshot起動の失敗コード
            return 1;
        }

        // Wide形式にした例外文
        const auto message = LamaPon::Utf8ToWide(exception.what());
        // 対話起動でエラー理由を表示します。
        MessageBoxW(
            nullptr,
            message.c_str(),
            L"LamaPon Editor - エラー",
            MB_OK | MB_ICONERROR);
        // 起動失敗の終了コード
        return 1;
    }
}
