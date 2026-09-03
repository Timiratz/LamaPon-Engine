#include "LamaPon/LamaPon.h"
#include "LamaPon/Editor/Editor.h"
// セーフモードで、覚えているシェーダーの失敗を捨てるため。
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
    std::vector<std::wstring> CommandLineArguments()
    {
        int argumentCount{};
        auto** argumentValues = CommandLineToArgvW(
            GetCommandLineW(),
            &argumentCount);
        if (argumentValues == nullptr)
        {
            throw std::runtime_error(
                "Could not read the editor command line.");
        }

        std::vector<std::wstring> arguments;
        arguments.reserve(
            static_cast<std::size_t>(argumentCount));
        for (int index = 0; index < argumentCount; ++index)
        {
            arguments.emplace_back(argumentValues[index]);
        }
        LocalFree(argumentValues);
        return arguments;
    }

    bool HasCommandLineFlag(const std::wstring_view flag)
    {
        const auto arguments = CommandLineArguments();
        for (std::size_t index = 1;
            index < arguments.size();
            ++index)
        {
            if (arguments[index] == flag)
            {
                return true;
            }
        }
        return false;
    }

    // 値付きオプション（--flag <値>）。無ければ空文字列です。
    std::wstring CommandLineOptionValue(
        const std::wstring_view flag)
    {
        const auto arguments = CommandLineArguments();
        for (std::size_t index = 1;
            index + 1 < arguments.size();
            ++index)
        {
            if (arguments[index] == flag)
            {
                return arguments[index + 1];
            }
        }
        return {};
    }

    std::filesystem::path RequestedProjectRoot()
    {
        const auto arguments = CommandLineArguments();
        std::filesystem::path projectRoot{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };

        for (std::size_t index = 1;
            index < arguments.size();
            ++index)
        {
            if (arguments[index] == L"--project")
            {
                if (index + 1 >= arguments.size())
                {
                    throw std::invalid_argument(
                        "--project requires a project folder path.");
                }
                projectRoot = arguments[++index];
                continue;
            }
            // 値付きオプションの**値**をプロジェクトパスと
            // 誤読しないように飛ばします。ここへ足し忘れると、
            // 値が「-で始まらない引数＝プロジェクト」の規則に
            // 拾われて「not a LamaPon project」で止まります
            // （--show project-settings:physics で実際に踏みました）。
            if (arguments[index] == L"--screenshot"
                || arguments[index] == L"--report"
                || arguments[index] == L"--show"
                || arguments[index] == L"--shot-frames"
                || arguments[index] == L"--remote")
            {
                ++index;
                continue;
            }
            if (!arguments[index].empty()
                && arguments[index].front() != L'-')
            {
                projectRoot = arguments[index];
            }
        }

        if (projectRoot.filename() == L"project.json"
            && projectRoot.parent_path().filename()
                == L".lamapon")
        {
            projectRoot = projectRoot.parent_path().parent_path();
        }
        if (projectRoot.empty())
        {
            throw std::invalid_argument(
                "A LamaPon project folder was not specified.");
        }

        projectRoot = std::filesystem::absolute(
            projectRoot).lexically_normal();
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        const auto assetRoot = projectRoot / L"assets";
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

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    std::filesystem::path logPath =
        LamaPon::ExecutableDirectory() / L"LamaPonEditor.log";
    LamaPon::CrashReporter::Install(
        LamaPon::ExecutableDirectory() / L"Crashes",
        "LamaPonEditor");
    try
    {
        const auto projectRoot = RequestedProjectRoot();
        logPath = projectRoot
            / L".lamapon"
            / L"LamaPonEditor.log";
        LamaPon::CrashReporter::Install(
            projectRoot / L".lamapon" / L"Crashes",
            "LamaPonEditor");

        LamaPon::ProjectInstanceLock projectInstance(projectRoot);
        if (!projectInstance.Acquired())
        {
            MessageBoxW(
                nullptr,
                L"このプロジェクトは、すでにLamaPon Editorで開かれています。\n"
                L"同じプロジェクトを同時に複数開くことはできません。",
                L"LamaPon Editor",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return 2;
        }

        SetCurrentDirectoryW(projectRoot.c_str());

        // 前回の実行がクラッシュや強制終了で終わっていたら、
        // 復旧方法を選べるようにします。C++スクリプトの不具合で
        // エディターが開けなくなる状態から抜け出すための導線です。
        LamaPon::CrashSentinel crashSentinel(projectRoot);
        bool safeMode = HasCommandLineFlag(L"--safe");
        // スクリーンショットモード（--screenshot）は無人実行なので、
        // ダイアログで止めずに通常どおり開きます。
        const std::wstring screenshotPath =
            CommandLineOptionValue(L"--screenshot");
        const std::wstring remotePath =
            CommandLineOptionValue(L"--remote");
        // 無人実行（スクリーンショット or リモート操作）。ダイアログを
        // 出すと誰も押せないボタンを待ち続けるので、全部抑止します。
        const bool unattended =
            !screenshotPath.empty()
            || !remotePath.empty();
        if (crashSentinel.PreviousRunCrashed()
            && !safeMode
            && !unattended)
        {
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
            if (choice == IDCANCEL || choice == 0)
            {
                return 0;
            }
            safeMode = choice == IDNO;
        }

        // 古いエンジンで作られたプロジェクトは、組み込みシェーダーが
        // 当時のままだと現在のエンジンと定数バッファのレイアウトが
        // 食い違い、描画異常やクラッシュの原因になります。開く前に
        // 最新へ揃えます（改造されていた分は .bak へ退避）。
        //
        // 揃える前に新旧を確かめます。**新しいエンジンで作られた
        // プロジェクトを開いてはいけません。** 古いエンジンで
        // 書き戻すと、新しい版が足した設定を落としたり、組み込み
        // シェーダーを巻き戻したりして、元のエンジンでも壊れた
        // 状態になります。
        const auto versionInfo =
            LamaPon::InspectProjectVersion(
                projectRoot,
                LamaPon::VersionString);
        if (versionInfo.status
            == LamaPon::ProjectVersionStatus::Newer)
        {
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
            if (!unattended)
            {
                MessageBoxW(
                    nullptr,
                    message.c_str(),
                    L"LamaPon Editor",
                    MB_OK | MB_ICONERROR
                        | MB_SETFOREGROUND);
            }
            return 3;
        }

        // 古い／記録が無いプロジェクトは、更新してよいか訊きます。
        // 黙って書き換えると、あとで「前のエディターで開けない」に
        // なったときに何が起きたのか分かりません。
        const bool needsUpgrade =
            versionInfo.status
                == LamaPon::ProjectVersionStatus::Older
            || versionInfo.status
                == LamaPon::ProjectVersionStatus::Unrecorded;
        if (needsUpgrade)
        {
            const std::wstring from =
                versionInfo.recordedVersion.empty()
                ? L"（記録なし）"
                : L"v" + LamaPon::Utf8ToWide(
                    versionInfo.recordedVersion);
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
            // 無人実行（スクリーンショットモード）では訊かずに
            // 「はい」相当で進めます。訊いても誰も押せません。
            const int choice =
                !unattended
                    ? MessageBoxW(
                        nullptr,
                        message.c_str(),
                        L"LamaPon Editor",
                        MB_YESNO | MB_ICONWARNING
                            | MB_SETFOREGROUND)
                    : IDYES;
            if (choice != IDYES)
            {
                return 0;
            }
        }

        const auto migration = LamaPon::MigrateProjectAssets(
            projectRoot,
            LamaPon::ExecutableDirectory() / L"assets",
            LamaPon::VersionString);
        if (needsUpgrade
            && migration.changed
            // 無人実行では報告ダイアログも出しません（ログには残る）。
            && !unattended)
        {
            // 何が変わったかを見せます。ログだけだと、絵が変わった
            // ときに「更新のせいなのか」が分かりません。
            std::wstring report =
                L"プロジェクトを v"
                + LamaPon::Utf8ToWide(
                    std::string(LamaPon::VersionString))
                + L" へ更新しました。\n\n更新した組み込みアセット: "
                + std::to_wstring(
                    migration.updatedAssets.size())
                + L"件";
            if (!migration.backedUpAssets.empty())
            {
                report += L"\n\n書き換えられていたため .bak へ"
                    L"退避したもの:";
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
        for (const auto& backedUp : migration.backedUpAssets)
        {
            LamaPon::Logger::Instance().Warning(
                "編集されていた組み込みアセットを更新しました"
                "（元の内容は .bak へ保存しています）: "
                + LamaPon::PathToUtf8(backedUp));
        }
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

        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        const auto projectSettings =
            LamaPon::LoadProjectSettings(settingsPath);
        const auto assetRoot = projectRoot / L"assets";
        const auto scenePath =
            assetRoot / projectSettings.startupScene;
        // 起動シーンが読めなくても、**開けなくはしません**。
        // 開けないということは、エディターで直す手段も無いという
        // ことです——シーンを選び直すのも、作り直すのも、他の
        // アセットを見るのも、全部できなくなります。
        //
        // ファイルが無いだけなら、そのパスのまま空のシーンで開きます
        // （保存すればそこへ作られます）。読めたのに壊れている場合は
        // **パスを渡しません**。渡すとCtrl+Sが「上書き保存」になり、
        // 手で直せたかもしれないファイルを空のシーンで潰します。
        std::wstring startupSceneProblem;
        bool startupSceneCorrupt = false;
        if (!std::filesystem::is_regular_file(scenePath))
        {
            startupSceneProblem =
                L"起動シーンのファイルが見つかりませんでした。\n\n"
                + scenePath.native()
                + L"\n\n空のシーンで開きます。プロジェクト設定で"
                L"起動シーンを選び直すか、このまま作って保存して"
                L"ください。";
        }

        // --warp: GPUを使わず、CPUラスタライザ（WARP）で描画します。
        // 仮想マシンやGPUが正しく動かない環境での動作確認用です。
        if (HasCommandLineFlag(L"--warp"))
        {
            LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        }

        // --d3ddebug: D3D11のデバッグレイヤーを有効にします。
        // 不正な描画は、これが無いと警告も出ずにドライバーへ渡り、
        // WARPでは**プロセスごと落ちます**。落ちる場所を突き止め
        // たいときに付けてください（普段は重いので既定は無効）。
        if (HasCommandLineFlag(L"--d3ddebug"))
        {
            LamaPon::GraphicsDevice::SetEnableDebugLayer(true);
        }

        const auto windowTitle = LamaPon::Utf8ToWide(
            projectSettings.gameName + " - LamaPon Editor");
        LamaPon::Application application(
            windowTitle,
            1280,
            720,
            projectSettings.gameName);

        application.Initialize(instance);
        application.Graphics().SetGraphicsSettings(
            projectSettings.graphics);
        application.Input().SetActions(
            projectSettings.inputActions);
        application.Graphics().Assets().SetAssetRoot(assetRoot);
        if (safeMode)
        {
            // 前回落ちた原因が分からない状態なので、起動を妨げ得る
            // ものは捨ててから開きます。覚えている「失敗」だけを
            // 捨てるので、コンパイル済みのバイトコードは残ります
            // （捨てると次の起動が全部コンパイルからになります）。
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
        else
        {
            static_cast<void>(application.GameModule().Load(
                projectRoot
                    / L".lamapon"
                    / L"bin"
                    / L"LamaPonGameModule.dll"));
        }
        if (startupSceneProblem.empty())
        {
            try
            {
                application.ActiveScene().LoadFromFile(
                    scenePath);
            }
            catch (const std::exception& exception)
            {
                // 途中まで読めている可能性があるので捨てます。
                // 半端に読めたシーンをそのまま見せると、消えている
                // ものが「元から無かった」ように見えます。
                application.ActiveScene().Clear();
                startupSceneCorrupt = true;
                startupSceneProblem =
                    L"起動シーンを読み込めませんでした。\n\n"
                    + scenePath.native()
                    + L"\n\n"
                    + LamaPon::Utf8ToWide(exception.what())
                    + L"\n\n空のシーンで開きます。**元のファイルは"
                    L"そのまま残してあります。**上書きしないよう、"
                    L"保存は「名前を付けて保存」になります。";
            }
        }
        if (!startupSceneProblem.empty())
        {
            LamaPon::Logger::Instance().Error(
                "起動シーンを開けませんでした: "
                + LamaPon::PathToUtf8(scenePath));
            // ログだけだと気付けません。空のシーンが出た理由が
            // 分からないと、作り直して上書きしてしまいます。
            MessageBoxW(
                nullptr,
                startupSceneProblem.c_str(),
                L"LamaPon Editor",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
        auto engineRoot = std::filesystem::path{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };
        const auto installedEngineRoot =
            LamaPon::ExecutableDirectory();
        if (std::filesystem::is_regular_file(
                installedEngineRoot
                    / L"tools"
                    / L"ProjectGameModule"
                    / L"CMakeLists.txt"))
        {
            engineRoot = installedEngineRoot;
        }
        // スクリーンショットモード: 指定があれば、撮影後に自動終了
        // します（生成AIがエディターUIを目視確認するための入口）。
        LamaPon::EditorScreenshotOptions screenshot;
        screenshot.imagePath = screenshotPath;
        screenshot.reportPath =
            CommandLineOptionValue(L"--report");
        screenshot.show = LamaPon::PathToUtf8(
            std::filesystem::path{
                CommandLineOptionValue(L"--show") });
        screenshot.remoteDirectory = remotePath;
        if (const auto frames =
                CommandLineOptionValue(L"--shot-frames");
            !frames.empty())
        {
            screenshot.captureFrame =
                static_cast<std::uint32_t>(
                    std::stoul(frames));
        }
        LamaPon::EnableEditor(
            application,
            // 壊れたファイルは「開いているシーン」にしません。
            // 空パスならCtrl+Sが「名前を付けて保存」になるので、
            // 手で直せたかもしれない中身を潰しません。
            startupSceneCorrupt
                ? std::filesystem::path{}
                : scenePath,
            std::move(engineRoot),
            LAMAPON_BUILD_CONFIGURATION,
            safeMode,
            unattended ? &screenshot : nullptr);

        int exitCode{};
        try
        {
            exitCode = application.Run();
        }
        catch (const std::exception& exception)
        {
            // Applicationが生きている間はGame Moduleも読み込まれて
            // います。ここでメッセージをエディタ側の例外へコピーし、
            // DLL解放後に無効な例外vtableを参照しないようにします。
            throw std::runtime_error(exception.what());
        }
        catch (...)
        {
            throw std::runtime_error(
                "Unknown exception escaped from the editor runtime.");
        }

        // セーフモードから「通常モードで開き直す」を選んだ場合は、
        // ここまででプロジェクトのロックとウィンドウが解放されて
        // いるため、安全に起動し直せます。
        if (LamaPon::WasNormalModeRestartRequested())
        {
            crashSentinel.MarkCleanExit();
            projectInstance.Release();
            std::wstring commandLine = L"\""
                + (LamaPon::ExecutableDirectory()
                    / L"LamaPonEditor.exe").native()
                + L"\" --project \""
                + projectRoot.native()
                + L"\"";
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
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
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        if (!logPath.parent_path().empty())
        {
            std::error_code error;
            std::filesystem::create_directories(
                logPath.parent_path(),
                error);
        }
        std::ofstream log(logPath, std::ios::trunc);
        log << exception.what() << '\n';

        // スクリーンショットモード（無人実行）ではダイアログを
        // 出しません。出すと誰も押せないOKボタンを待ち続けます。
        // 代わりに--reportのJSONへ理由を書いて終了します。
        if (!CommandLineOptionValue(L"--screenshot").empty())
        {
            if (const auto reportPath =
                    CommandLineOptionValue(L"--report");
                !reportPath.empty())
            {
                const nlohmann::json failure{
                    { "ok", false },
                    { "error", exception.what() },
                };
                std::ofstream report(
                    std::filesystem::path{ reportPath },
                    std::ios::trunc);
                // 不正なUTF-8（ANSI由来の例外文など）でも
                // JSONを壊さず置換します。
                report << failure.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::
                        replace);
            }
            return 1;
        }

        const auto message = LamaPon::Utf8ToWide(exception.what());
        MessageBoxW(
            nullptr,
            message.c_str(),
            L"LamaPon Editor - エラー",
            MB_OK | MB_ICONERROR);
        return 1;
    }
}
