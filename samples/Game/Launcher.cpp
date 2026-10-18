#include <Windows.h>

#include <filesystem>
#include <string>

// エンジンDLLに依存せず、Dataフォルダー内のゲームへ引数と終了コードを引き継ぎます。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int showWindow)
{
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        MessageBoxW(nullptr, L"ゲームの保存先を取得できませんでした。",
            L"ゲームの起動エラー", MB_OK | MB_ICONERROR);
        return 1;
    }
    path.resize(length);
    const std::filesystem::path launcher(path);
    const auto data = launcher.parent_path()
        / (launcher.stem().wstring() + L"_Data");
    const auto game = data / L"LamaPonGame.exe";
    auto command = L"\"" + game.wstring() + L"\"";
    if (arguments != nullptr && *arguments != L'\0')
        command += L" " + std::wstring(arguments);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(showWindow);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr,
            FALSE, 0, nullptr, data.c_str(), &startup, &process))
    {
        const auto message = L"ゲームを起動できませんでした（エラー "
            + std::to_wstring(GetLastError()) + L"）。\n\n"
            + launcher.stem().wstring()
            + L"_Data フォルダーをexeと一緒に配置してください。";
        MessageBoxW(nullptr, message.c_str(), L"ゲームの起動エラー",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
