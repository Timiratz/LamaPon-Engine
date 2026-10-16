#include "LamaPon/Editor/WebExportJob.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
    struct Handle final
    {
        // 自動解放するWindows handle
        HANDLE value{};
        // 有効なWindows handleを解放する。
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };

    // 環境変数の値をパスとして取得し未設定なら空を返す(name: 読み取る環境変数名)。
    std::filesystem::path EnvironmentPath(const wchar_t* name)
    {
        // 環境変数を取得するワイド文字バッファ
        std::wstring value(GetEnvironmentVariableW(name, nullptr, 0), L'\0');
        if (value.empty()) return {};
        value.resize(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())));
        return value;
    }

    // LocalAppData内のWebツール設定パスを返し環境変数不在なら例外を出す。
    std::filesystem::path SettingsPath()
    {
        // ツール設定を置くLocalAppData
        const auto local = EnvironmentPath(L"LOCALAPPDATA");
        if (local.empty()) throw std::runtime_error("LOCALAPPDATAが見つかりません。");
        return local / L"LamaPon" / L"web-export-tools.json";
    }

    // 末尾のバックスラッシュを含めWindowsのargv規則で引用する(path: 引用するパス)。
    std::wstring Quote(const std::filesystem::path& path)
    {
        // 引用済みパスまたは終了結果JSON
        std::wstring result = L"\"";
        // 連続する未出力のバックスラッシュ数
        std::size_t slashes{};
        // 引用するパスの文字
        for (const wchar_t ch : path.wstring())
        {
            if (ch == L'\\') { ++slashes; continue; }
            result.append(ch == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
            result += ch;
            slashes = 0;
        }
        result.append(slashes * 2, L'\\');
        return result + L'\"';
    }

    bool IsWindowsAlias(const std::filesystem::path& path)
    {
        for (const auto& part : path)
        {
            auto name = part.wstring();
            std::transform(name.begin(), name.end(), name.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            if (name == L"windowsapps") return true;
        }
        return false;
    }

    // 実際に起動して版と実体パスを確認する。シェル・一時ファイルは使わない。
    std::filesystem::path ProbePython(const std::filesystem::path& executable,
        bool launcher = false)
    {
        std::error_code error;
        if (executable.empty() || IsWindowsAlias(executable)
            || !std::filesystem::is_regular_file(executable, error)) return {};
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read{}, write{};
        if (!CreatePipe(&read, &write, &security, 4096)) return {};
        Handle reader{read}, writer{write};
        if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) return {};
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (input.value == INVALID_HANDLE_VALUE) return {};
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = writer.value;
        startup.StartupInfo.hStdError = writer.value;
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return {};
        struct Guard final { LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~Guard() { DeleteProcThreadAttributeList(value); } } guard{startup.lpAttributeList};
        HANDLE inherited[]{writer.value, input.value};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) return {};
        auto command = Quote(executable) + (launcher ? L" -3" : L"")
            + L" -I -S -c \"import sys; sys.stdout.buffer.write(sys.executable.encode('utf-8')); "
              L"sys.exit(0 if sys.version_info >= (3,11) else 1)\"";
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                &startup.StartupInfo, &process)) return {};
        Handle child{process.hProcess}, thread{process.hThread};
        if (WaitForSingleObject(child.value, 3000) != WAIT_OBJECT_0)
        {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 1000);
            return {};
        }
        DWORD code{1};
        if (!GetExitCodeProcess(child.value, &code) || code != 0) return {};
        DWORD available{};
        if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)
            || available == 0 || available > 32768) return {};
        std::string output(available, '\0');
        DWORD received{};
        if (!ReadFile(reader.value, output.data(), available, &received, nullptr)) return {};
        output.resize(received);
        try
        {
            const auto resolved = LamaPon::PathFromUtf8(output);
            return !IsWindowsAlias(resolved) && std::filesystem::is_regular_file(resolved, error)
                ? resolved : std::filesystem::path{};
        }
        catch (...) { return {}; }
    }

    // PATH全体を列挙し、先頭のWindowsApps aliasで探索を打ち切らない。
    std::vector<std::filesystem::path> PathExecutables(const wchar_t* name)
    {
        std::vector<std::filesystem::path> results;
        const auto value = EnvironmentPath(L"PATH").wstring();
        std::size_t offset{};
        while (offset < value.size())
        {
            const auto end = value.find(L';', offset);
            auto directory = value.substr(offset, end == std::wstring::npos ? end : end-offset);
            if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"')
                directory = directory.substr(1, directory.size()-2);
            if (!directory.empty()) results.emplace_back(std::filesystem::path(directory) / name);
            if (end == std::wstring::npos) break;
            offset = end+1;
        }
        return results;
    }

    // 指定exe・SDK・PATH・py launcher・標準配置先を確認する。
    std::filesystem::path FindPython(const LamaPon::WebExportTools& tools)
    {
        if (!tools.python.empty())
        {
            if (auto found = ProbePython(tools.python); !found.empty()) return found;
            throw std::runtime_error("指定したPythonは起動できないか、3.11未満です。空欄にすると自動検出します。");
        }
        std::vector<std::filesystem::path> candidates;
        const auto addChildren = [&](const std::filesystem::path& directory)
        {
            if (directory.empty()) return;
            std::error_code error;
            std::vector<std::filesystem::path> children;
            for (std::filesystem::directory_iterator it(directory, error), end;
                 !error && it != end; it.increment(error)) children.push_back(it->path());
            std::sort(children.rbegin(), children.rend());
            for (const auto& child : children) candidates.push_back(child / L"python.exe");
        };
        auto sdk = tools.emsdk.empty() ? EnvironmentPath(L"EMSDK") : tools.emsdk;
        if (!sdk.empty()) addChildren(sdk / L"python");
        if (auto configured = EnvironmentPath(L"EMSDK_PYTHON"); !configured.empty()) candidates.push_back(configured);
        const auto onPath = PathExecutables(L"python.exe");
        candidates.insert(candidates.end(), onPath.begin(), onPath.end());
        for (const auto& candidate : candidates)
            if (auto found = ProbePython(candidate); !found.empty()) return found;
        auto launchers = PathExecutables(L"py.exe");
        const auto local = EnvironmentPath(L"LOCALAPPDATA");
        const auto windows = EnvironmentPath(L"WINDIR");
        if (!local.empty()) launchers.push_back(local / L"Programs/Python/Launcher/py.exe");
        if (!windows.empty()) launchers.push_back(windows / L"py.exe");
        for (const auto& launcher : launchers)
            if (auto found = ProbePython(launcher, true); !found.empty()) return found;
        candidates.clear();
        if (!local.empty()) addChildren(local / L"Programs/Python");
        for (const auto* variable : {L"ProgramFiles", L"ProgramFiles(x86)"})
        {
            const auto directory = EnvironmentPath(variable);
            if (directory.empty()) continue;
            std::error_code error;
            for (std::filesystem::directory_iterator it(directory, error), end;
                 !error && it != end; it.increment(error))
            {
                const auto name = it->path().filename().wstring();
                if (name.starts_with(L"Python")) candidates.push_back(it->path() / L"python.exe");
            }
        }
        for (const auto& candidate : candidates)
            if (auto found = ProbePython(candidate); !found.empty()) return found;
        throw std::runtime_error("Python 3.11以降を自動検出できませんでした。PythonまたはEmscripten SDKをインストールしてください。インストール済みの場合はWebビルド環境でPythonを指定できます。");
    }

    bool IsInside(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        const auto relative = path.lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) return false;
        for (const auto& part : relative)
            if (part == L"..") return false;
        return true;
    }
}

namespace LamaPon
{
    std::filesystem::path WebSupportRoot()
    { return EnvironmentPath(L"LOCALAPPDATA") / L"LamaPon/toolchains/web"; }

    bool WebSupportReady() noexcept
    {
        try
        {
            std::ifstream input(WebSupportRoot() / L"web-support.json");
            if (!input) return false;
            const auto state = nlohmann::json::parse(input);
            if (state.value("format", "") != "lamapon.web-support" || !state.value("ready", false)) return false;
            for (const auto* key : {"python", "cmake", "ninja"})
                if (!std::filesystem::is_regular_file(PathFromUtf8(state.value(key, "")))) return false;
            return std::filesystem::is_regular_file(PathFromUtf8(state.value("emsdk", "")) / L".emscripten");
        }
        catch (...) { return false; }
    }

    WebExportTools LoadWebExportTools()
    {
        // 保存設定と環境変数から得るツール
        WebExportTools tools;
        tools.emsdk = EnvironmentPath(L"EMSDK");
        // 保存済みビルド環境を読むstream
        std::ifstream input(SettingsPath());
        if (input)
        {
            // 保存されたWebビルド環境のJSON
            const auto settings = nlohmann::json::parse(input);
            tools.python = PathFromUtf8(settings.value("python", std::string{}));
            // 保存設定にあるSDKのUTF8パス
            const auto sdk = settings.value("emsdk", std::string{});
            if (!sdk.empty()) tools.emsdk = PathFromUtf8(sdk);
        }
        if (WebSupportReady())
        {
            std::ifstream managed(WebSupportRoot() / L"web-support.json");
            const auto state = nlohmann::json::parse(managed);
            if (tools.python.empty()) tools.python = PathFromUtf8(state.at("python").get<std::string>());
            if (tools.emsdk.empty()) tools.emsdk = PathFromUtf8(state.at("emsdk").get<std::string>());
        }
        return tools;
    }

    void SaveWebExportTools(const WebExportTools& tools)
    {
        // 保存するツール設定のパス
        const auto path = SettingsPath();
        std::filesystem::create_directories(path.parent_path());
        // 設定を置換前に書く作業パス
        const auto temporary = path.wstring() + L".tmp";
        {
            // 置換前のビルド設定を書き出すstream
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << nlohmann::json{{"python", PathToUtf8(tools.python)},
                {"emsdk", PathToUtf8(tools.emsdk)}}.dump(2) << '\n';
            output.close();
            if (!output) throw std::runtime_error("Webビルド環境を保存できませんでした。");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::runtime_error("Webビルド環境の保存に失敗しました。");
    }

    WebExportJob::~WebExportJob() { Close(); }

    void WebExportJob::Close() noexcept
    {
        if (m_job) CloseHandle(m_job);
        if (m_process) CloseHandle(m_process);
        m_job = nullptr;
        m_process = nullptr;
    }

    void WebExportJob::Start(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile,
        const std::filesystem::path& output, const WebExportTools& tools)
    {
        StartProcess(engineRoot, projectFile, output, tools, {});
    }

    void WebExportJob::StartWebSupport(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile)
    { StartProcess(engineRoot, projectFile, {}, {}, "web-support"); }

    void WebExportJob::StartNativeBuildProject(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile, const std::filesystem::path& output,
        const WebExportTools& tools, const std::string& platform)
    {
        if (platform != "linux" && platform != "android")
            throw std::invalid_argument("Native build project target must be linux or android");
        StartProcess(engineRoot, projectFile, output, tools, platform);
    }

    void WebExportJob::StartAndroidApk(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile, const std::filesystem::path& output,
        const WebExportTools& tools, const AndroidExportTools& android)
    {
        for (const auto* path : {&android.sdk, &android.javaHome, &android.gradleHome, &android.sdlSource})
            if (path->empty() || !std::filesystem::is_directory(*path))
                throw std::invalid_argument("既存のAndroid SDK・JDK・Gradle・SDL3のフォルダーを指定してください。");
        StartProcess(engineRoot, projectFile, output, tools, "android", &android);
    }

    void WebExportJob::StartLinuxBuild(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile, const std::filesystem::path& output,
        const WebExportTools& tools, const LinuxExportTools& linux)
    {
        if (linux.sdlSource.empty() || !std::filesystem::is_directory(linux.sdlSource))
            throw std::invalid_argument("既存のSDL3ソースフォルダーを指定してください。");
        StartProcess(engineRoot, projectFile, output, tools, "linux", nullptr, &linux);
    }

    void WebExportJob::StartProcess(const std::filesystem::path& engineRoot,
        const std::filesystem::path& projectFile,
        const std::filesystem::path& output, const WebExportTools& tools, const std::string& platform,
        const AndroidExportTools* android, const LinuxExportTools* linux)
    {
        if (Running()) throw std::logic_error("出力処理は既に実行中です。");
        m_succeeded = false;
        m_htmlPath.clear();
        m_buildProjectDirectory.clear();
        m_nativeArtifactPath.clear();
        m_androidApk = android != nullptr;
        m_linuxBuild = linux != nullptr;
        m_expectedApk = m_androidApk ? std::filesystem::absolute(output / L"build/app/outputs/apk/debug/app-debug.apk")
            : std::filesystem::path{};
        m_expectedLinuxOutput = m_linuxBuild ? std::filesystem::absolute(output) : std::filesystem::path{};
        m_nativePlatform = platform;
        m_logPath.clear();
        m_resultPath.clear();
        m_message.clear();
        // 検出・指定したPython実行パス
        const bool setup = platform == "web-support";
        auto resolvedTools = tools;
        if (!setup && resolvedTools.python.empty() && WebSupportReady())
        {
            std::ifstream managed(WebSupportRoot() / L"web-support.json");
            const auto state = nlohmann::json::parse(managed);
            resolvedTools.python = PathFromUtf8(state.at("python").get<std::string>());
            if (resolvedTools.emsdk.empty()) resolvedTools.emsdk = PathFromUtf8(state.at("emsdk").get<std::string>());
        }
        const auto python = setup ? EnvironmentPath(L"SystemRoot") / L"System32/WindowsPowerShell/v1.0/powershell.exe"
            : FindPython(resolvedTools);
        // エディター用Web出力スクリプト
        const auto script = engineRoot / L"tools" / (setup ? L"SetupWebSupport.ps1" : platform.empty() ? L"editor_web_export.py"
            : m_linuxBuild ? L"editor_linux_export.py" : L"editor_native_export.py");
        if (!std::filesystem::is_regular_file(python))
            throw std::runtime_error("指定したPython実行ファイルが見つかりません。");
        if (!std::filesystem::is_regular_file(script))
            throw std::runtime_error("出力ツールが見つかりません。LamaPon SDKを更新してください。");
        // 今回のログ・結果文書の保存先
        const auto directory = std::filesystem::absolute(projectFile).parent_path()
            / (setup ? L"web-support-jobs" : platform.empty() ? L"web-export-jobs" : L"native-export-jobs")
            / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        m_logPath = directory / L"build.log";
        m_resultPath = directory / L"result.json";
        // ログと標準入力のhandle継承設定
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        // 子processのログ出力handle
        Handle log{CreateFileW(m_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        // 子processのNUL入力handle
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        // 子孫ごと管理するjobのhandle
        Handle job{CreateJobObjectW(nullptr, nullptr)};
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE || !job.value)
            throw std::runtime_error("ゲーム出力のログまたはプロセスを準備できませんでした。");
        // jobを閉じると子孫を終了する設定
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("ゲーム出力のプロセス管理を設定できませんでした。");

        // 非表示起動と標準IOの設定
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = log.value;
        startup.StartupInfo.hStdError = log.value;
        startup.StartupInfo.hStdInput = input.value;
        // 起動属性リストに必要なバイト数
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        // 起動属性リストの所有バッファ
        std::vector<unsigned char> attributes(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
            throw std::runtime_error("ゲーム出力の起動属性を準備できませんでした。");
        // 起動属性リストを解放する番人
        struct AttributesGuard final
        {
            // 自動解放する起動属性リスト
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            // 初期化済みの起動属性リストを解放する。
            ~AttributesGuard() { DeleteProcThreadAttributeList(value); }
        } guard{startup.lpAttributeList};
        // 継承を許可するログと入力のhandle
        HANDLE inherited[]{log.value, input.value};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr))
            throw std::runtime_error("ゲーム出力のログ接続を準備できませんでした。");
        // Pythonへ直接渡す引用済み起動引数
        std::wstring command;
        if (setup)
            command = Quote(python) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " + Quote(script)
                + L" -Install -AllowDownloads -ResultPath " + Quote(m_resultPath);
        else command = Quote(python) + L" -B -X utf8 -u " + Quote(script)
            + L" --project " + Quote(std::filesystem::absolute(projectFile))
            + L" --output " + Quote(std::filesystem::absolute(output))
            + L" --result " + Quote(m_resultPath);
        if (!platform.empty() && !setup) command += L" --platform " + Quote(PathFromUtf8(platform));
        else if (!setup && !resolvedTools.emsdk.empty()) command += L" --emsdk " + Quote(resolvedTools.emsdk);
        if (android)
        {
            command += L" --build-apk --android-sdk " + Quote(std::filesystem::absolute(android->sdk))
                + L" --java-home " + Quote(std::filesystem::absolute(android->javaHome))
                + L" --gradle-home " + Quote(std::filesystem::absolute(android->gradleHome))
                + L" --sdl-source-directory " + Quote(std::filesystem::absolute(android->sdlSource));
            if (android->allowDependencyDownloads) command += L" --allow-downloads";
        }
        if (linux)
        {
            command += L" --engine-root " + Quote(std::filesystem::absolute(engineRoot))
                + L" --sdl-source-directory " + Quote(std::filesystem::absolute(linux->sdlSource));
            if (!linux->distribution.empty())
                command += L" --distribution " + Quote(PathFromUtf8(linux->distribution));
        }
        // 起動したPythonのhandle情報
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, engineRoot.c_str(), &startup.StartupInfo, &process))
            throw std::runtime_error("Pythonを起動できませんでした。Pythonの実行環境を確認してください。");
        // 起動完了後に解放するthread handle
        Handle thread{process.hThread};
        // 成功後に引き継ぐprocess handle
        Handle processHandle{process.hProcess};
        // Pythonを停止状態でjobへ所属させてから再開し、起動した子孫も終了時に回収する。
        if (!AssignProcessToJobObject(job.value, process.hProcess)
            || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            TerminateProcess(process.hProcess, 1);
            throw std::runtime_error("ゲーム出力を開始できませんでした。");
        }
        m_process = processHandle.value;
        processHandle.value = nullptr;
        m_job = job.value;
        job.value = nullptr;
        m_message = platform.empty()
            ? "Web互換性を検査し、HTMLをビルドしています。初回は数分かかる場合があります。"
            : "ネイティブ出力の互換性を検査し、ビルド設定を生成しています。";
        if (m_androidApk) m_message = "Android互換性を検査し、debug APKをビルドしています。ログで進行状況を確認できます。";
        if (m_linuxBuild) m_message = "WSL内でLinuxゲームをビルドし、実行形式と同梱依存を検査しています。ログで進行状況を確認できます。";
        if (setup) m_message = "Web出力サポートを準備しています。初回のダウンロードには数分かかる場合があります。";
    }

    bool WebExportJob::Poll()
    {
        if (!Running() || WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) return false;
        // 子processの終了コード
        DWORD code{1};
        GetExitCodeProcess(m_process, &code);
        Close();
        try
        {
            // 子processの終了結果を読むstream
            std::ifstream input(m_resultPath);
            if (!input) throw std::runtime_error("結果を取得できませんでした。Python 3.11以降が必要です。ログを確認してください。");
            // 引用済みパスまたは終了結果JSON
            const auto result = nlohmann::json::parse(input);
            m_succeeded = code == 0 && result.value("ok", false);
            m_message = result.value("message", std::string{"ゲーム出力に失敗しました。"});
            if (m_succeeded)
            {
                if (IsWebSupportSetup())
                {
                    if (!WebSupportReady()) throw std::runtime_error("Web出力サポートの確認に失敗しました。準備を再実行してください。");
                }
                else if (m_nativePlatform.empty())
                {
                    m_htmlPath = PathFromUtf8(result.at("htmlPath").get<std::string>());
                    if (!std::filesystem::is_regular_file(m_htmlPath))
                        throw std::runtime_error("出力されたHTMLが見つかりません。ログを確認してください。");
                }
                else
                {
                    if (result.value("platform", std::string{}) != m_nativePlatform)
                        throw std::runtime_error("出力の対象OSが一致しません。");
                    m_buildProjectDirectory = PathFromUtf8(result.at("buildProjectPath").get<std::string>());
                    if (!std::filesystem::is_regular_file(m_buildProjectDirectory / L"CMakeLists.txt")
                        || !std::filesystem::is_regular_file(m_buildProjectDirectory / L"native-inspection.json"))
                        throw std::runtime_error("生成したビルド設定が見つかりません。ログを確認してください。");
                    if (m_androidApk)
                    {
                        const auto artifact = PathFromUtf8(result.at("artifactPath").get<std::string>());
                        if (!result.value("built", false) || !result.value("artifactChecksPassed", false)
                            || std::filesystem::weakly_canonical(artifact) != std::filesystem::weakly_canonical(m_expectedApk)
                            || !std::filesystem::is_regular_file(artifact) || !std::filesystem::file_size(artifact))
                            throw std::runtime_error("APKのビルド結果または出力ファイルを確認できませんでした。");
                        m_nativeArtifactPath = artifact;
                    }
                    if (m_linuxBuild)
                    {
                        const auto artifact = PathFromUtf8(result.at("artifactPath").get<std::string>());
                        const auto absoluteArtifact = std::filesystem::weakly_canonical(artifact);
                        const auto absoluteOutput = std::filesystem::weakly_canonical(m_expectedLinuxOutput);
                        if (!result.value("built", false) || !result.value("artifactChecksPassed", false)
                            || std::filesystem::weakly_canonical(m_buildProjectDirectory) != absoluteOutput
                            || !IsInside(absoluteArtifact, absoluteOutput)
                            || !std::filesystem::is_regular_file(absoluteArtifact)
                            || std::filesystem::file_size(absoluteArtifact) < 4)
                            throw std::runtime_error("Linuxゲームのビルド結果または出力ファイルを確認できませんでした。");
                        std::ifstream artifactStream(absoluteArtifact, std::ios::binary);
                        std::array<char, 4> magic{};
                        if (!artifactStream.read(magic.data(), static_cast<std::streamsize>(magic.size()))
                            || magic != std::array<char, 4>{'\x7f', 'E', 'L', 'F'})
                            throw std::runtime_error("出力されたゲームはLinux ELF実行ファイルではありません。");
                        m_nativeArtifactPath = absoluteArtifact;
                    }
                }
            }
        }
        // 結果の取得・検証で通知された失敗
        catch (const std::exception& error)
        {
            m_succeeded = false;
            m_message = error.what();
        }
        return true;
    }
}
