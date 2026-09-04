#include "LamaPon/Editor/WebExportJob.h"
#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace
{
    struct Handle final
    {
        HANDLE value{};
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };

    std::filesystem::path EnvironmentPath(const wchar_t* name)
    {
        std::wstring value(GetEnvironmentVariableW(name, nullptr, 0), L'\0');
        if (value.empty()) return {};
        value.resize(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())));
        return value;
    }

    std::filesystem::path SettingsPath()
    {
        const auto local = EnvironmentPath(L"LOCALAPPDATA");
        if (local.empty()) throw std::runtime_error("LOCALAPPDATAが見つかりません。");
        return local / L"LamaPon" / L"web-export-tools.json";
    }

    // CreateProcessのargv規則に従い、末尾のバックスラッシュも保持します。
    // cmd.exeを介さないので、&や%を含むパスもコマンドとして解釈しません。
    std::wstring Quote(const std::filesystem::path& path)
    {
        std::wstring result = L"\"";
        std::size_t slashes{};
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

    std::filesystem::path FindPython(const LamaPon::WebExportTools& tools)
    {
        if (!tools.python.empty()) return tools.python;
        const auto sdkPython = tools.emsdk / L"python";
        std::error_code error;
        if (!tools.emsdk.empty() && std::filesystem::is_directory(sdkPython, error))
        {
            for (const auto& version : std::filesystem::directory_iterator(sdkPython))
            {
                const auto executable = version.path() / L"python.exe";
                if (std::filesystem::is_regular_file(executable)) return executable;
            }
        }
        std::array<wchar_t, 32768> found{};
        if (SearchPathW(nullptr, L"python.exe", nullptr,
            static_cast<DWORD>(found.size()), found.data(), nullptr))
        {
            const std::filesystem::path candidate(found.data());
            // ストア起動用のエイリアスは、ビルド用Pythonとして使いません。
            if (candidate.wstring().find(L"WindowsApps") == std::wstring::npos) return candidate;
        }
        throw std::runtime_error("Python 3.11以降が見つかりません。「Webビルド環境」でPythonまたはEmscripten SDKを指定してください。");
    }
}

namespace LamaPon
{
    WebExportTools LoadWebExportTools()
    {
        WebExportTools tools;
        tools.emsdk = EnvironmentPath(L"EMSDK");
        std::ifstream input(SettingsPath());
        if (input)
        {
            const auto settings = nlohmann::json::parse(input);
            tools.python = PathFromUtf8(settings.value("python", std::string{}));
            const auto sdk = settings.value("emsdk", std::string{});
            if (!sdk.empty()) tools.emsdk = PathFromUtf8(sdk);
        }
        return tools;
    }

    void SaveWebExportTools(const WebExportTools& tools)
    {
        const auto path = SettingsPath();
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.wstring() + L".tmp";
        {
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
        if (Running()) throw std::logic_error("Web出力は既に実行中です。");
        m_succeeded = false;
        m_htmlPath.clear();
        m_logPath.clear();
        m_resultPath.clear();
        m_message.clear();
        const auto python = FindPython(tools);
        const auto script = engineRoot / L"tools" / L"editor_web_export.py";
        if (!std::filesystem::is_regular_file(python))
            throw std::runtime_error("指定したPython実行ファイルが見つかりません。");
        if (!std::filesystem::is_regular_file(script))
            throw std::runtime_error("Web出力ツールが見つかりません。LamaPon SDKを更新してください。");
        const auto directory = projectFile.parent_path() / L"web-export-jobs"
            / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        m_logPath = directory / L"build.log";
        m_resultPath = directory / L"result.json";
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle log{CreateFileW(m_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle job{CreateJobObjectW(nullptr, nullptr)};
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE || !job.value)
            throw std::runtime_error("Webビルドのログまたはプロセスを準備できませんでした。");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            throw std::runtime_error("Webビルドのプロセス管理を設定できませんでした。");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdOutput = log.value;
        startup.StartupInfo.hStdError = log.value;
        startup.StartupInfo.hStdInput = input.value;
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> attributes(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
            throw std::runtime_error("Webビルドの起動属性を準備できませんでした。");
        struct AttributesGuard final
        {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributesGuard() { DeleteProcThreadAttributeList(value); }
        } guard{startup.lpAttributeList};
        HANDLE inherited[]{log.value, input.value};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr))
            throw std::runtime_error("Webビルドのログ接続を準備できませんでした。");
        std::wstring command = Quote(python) + L" -X utf8 -u " + Quote(script)
            + L" --project " + Quote(projectFile) + L" --output " + Quote(output)
            + L" --result " + Quote(m_resultPath);
        if (!tools.emsdk.empty()) command += L" --emsdk " + Quote(tools.emsdk);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, engineRoot.c_str(), &startup.StartupInfo, &process))
            throw std::runtime_error("Pythonを起動できませんでした。Webビルド環境を確認してください。");
        Handle thread{process.hThread};
        Handle processHandle{process.hProcess};
        if (!AssignProcessToJobObject(job.value, process.hProcess)
            || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            TerminateProcess(process.hProcess, 1);
            throw std::runtime_error("Webビルドを開始できませんでした。");
        }
        m_process = processHandle.value;
        processHandle.value = nullptr;
        m_job = job.value;
        job.value = nullptr;
        m_message = "Web互換性を検査し、HTMLをビルドしています。初回は数分かかる場合があります。";
    }

    bool WebExportJob::Poll()
    {
        if (!Running() || WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) return false;
        DWORD code{1};
        GetExitCodeProcess(m_process, &code);
        Close();
        try
        {
            std::ifstream input(m_resultPath);
            if (!input) throw std::runtime_error("結果を取得できませんでした。Python 3.11以降が必要です。ログを確認してください。");
            const auto result = nlohmann::json::parse(input);
            m_succeeded = code == 0 && result.value("ok", false);
            m_message = result.value("message", std::string{"Web出力に失敗しました。"});
            if (m_succeeded)
            {
                m_htmlPath = PathFromUtf8(result.at("htmlPath").get<std::string>());
                if (!std::filesystem::is_regular_file(m_htmlPath))
                    throw std::runtime_error("出力されたHTMLが見つかりません。ログを確認してください。");
            }
        }
        catch (const std::exception& error)
        {
            m_succeeded = false;
            m_message = error.what();
        }
        return true;
    }
}
