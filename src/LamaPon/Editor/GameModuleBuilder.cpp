#include "LamaPon/Editor/GameModuleBuilder.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Scripting/GameModule.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    using VersionComponents = std::vector<std::uint32_t>;

    [[nodiscard]] bool IsHostArm64() noexcept;

    [[nodiscard]] VersionComponents ParseVersionComponents(
        const std::wstring& text)
    {
        VersionComponents result;
        std::uint32_t value{};
        bool hasDigits = false;
        for (const wchar_t character : text)
        {
            if (std::iswdigit(character) != 0)
            {
                value = value * 10u
                    + static_cast<std::uint32_t>(character - L'0');
                hasDigits = true;
            }
            else if (character == L'.' && hasDigits)
            {
                result.push_back(value);
                value = 0;
                hasDigits = false;
            }
            else
            {
                return {};
            }
        }
        if (!hasDigits)
        {
            return {};
        }
        result.push_back(value);
        return result;
    }

    [[nodiscard]] std::filesystem::path VisualStudioEditionRoot(
        const std::filesystem::path& devCommand)
    {
        // <edition>/Common7/Tools/VsDevCmd.bat -> <edition>
        return devCommand.parent_path()
            .parent_path()
            .parent_path();
    }

    [[nodiscard]] std::filesystem::path FindLatestMsvcToolsetRoot(
        const std::filesystem::path& devCommand)
    {
        const auto toolsets = VisualStudioEditionRoot(devCommand)
            / "VC" / "Tools" / "MSVC";
        std::filesystem::path result;
        VersionComponents resultVersion;
        std::error_code scanError;
        if (!std::filesystem::is_directory(toolsets, scanError))
        {
            return {};
        }
        for (const auto& entry : std::filesystem::directory_iterator(
                toolsets,
                std::filesystem::directory_options::skip_permission_denied,
                scanError))
        {
            if (!entry.is_directory(scanError))
            {
                scanError.clear();
                continue;
            }
            const auto version = ParseVersionComponents(
                entry.path().filename().wstring());
            if (!version.empty()
                && (result.empty()
                    || version > resultVersion
                    || (version == resultVersion
                        && entry.path() > result)))
            {
                result = entry.path();
                resultVersion = version;
            }
            scanError.clear();
        }
        return result;
    }

    [[nodiscard]] std::filesystem::path FindMsvcCompiler(
        const std::filesystem::path& devCommand)
    {
        if (devCommand.empty())
        {
            return {};
        }
        const auto toolsetRoot = FindLatestMsvcToolsetRoot(devCommand);
        const auto hostDirectory = IsHostArm64()
            ? L"Hostarm64"
            : L"Hostx64";
        const auto targetDirectory = IsHostArm64()
            ? L"amd64"
            : L"x64";
        const auto compiler = toolsetRoot
            / L"bin"
            / hostDirectory
            / targetDirectory
            / L"cl.exe";
        std::error_code error;
        return std::filesystem::is_regular_file(compiler, error)
            ? compiler
            : std::filesystem::path{};
    }

    // VsDevCmd.bat（MSVCの環境変数を整えるバッチ）を探します。
    // 複数入っている場合は、インストール先の文字列順ではなく
    // 実際のMSVCツールセットの版数が新しいものを優先します。
    [[nodiscard]] std::filesystem::path
        FindVisualStudioDevCommand()
    {
        const std::filesystem::path visualStudioRoot{
            L"C:\\Program Files\\Microsoft Visual Studio"
        };
        std::vector<std::filesystem::path> candidates;
        std::error_code scanError;
        if (std::filesystem::is_directory(
            visualStudioRoot,
            scanError))
        {
            for (const auto& version :
                std::filesystem::directory_iterator(
                    visualStudioRoot,
                    std::filesystem::directory_options::
                        skip_permission_denied,
                    scanError))
            {
                if (!version.is_directory(scanError))
                {
                    scanError.clear();
                    continue;
                }
                for (const auto& edition :
                    std::filesystem::directory_iterator(
                        version.path(),
                        std::filesystem::directory_options::
                            skip_permission_denied,
                        scanError))
                {
                    if (edition.is_directory(scanError))
                    {
                        const auto candidate =
                            edition.path()
                            / "Common7"
                            / "Tools"
                            / "VsDevCmd.bat";
                        if (std::filesystem::is_regular_file(
                            candidate,
                            scanError))
                        {
                            candidates.push_back(candidate);
                        }
                    }
                    scanError.clear();
                }
                scanError.clear();
            }
        }
        std::ranges::sort(
            candidates,
            [](const auto& left, const auto& right)
            {
                const auto leftVersion = FindLatestMsvcToolsetRoot(left)
                    .filename().wstring();
                const auto rightVersion = FindLatestMsvcToolsetRoot(right)
                    .filename().wstring();
                const auto leftComponents = ParseVersionComponents(
                    leftVersion);
                const auto rightComponents = ParseVersionComponents(
                    rightVersion);
                if (leftComponents != rightComponents)
                {
                    return leftComponents > rightComponents;
                }
                return left > right;
            });
        return candidates.empty()
            ? std::filesystem::path{}
            : candidates.front();
    }

    // VS同梱のninja.exeを探します。
    //
    // NMake Makefilesはヘッダ依存の追跡が不完全で、エンジンの
    // ヘッダを更新しても古いobjをそのままリンクします。
    // エンジン本体のビルドは同じ理由で既にNinjaへ移してあり、
    // Game Module側だけが取り残されていました（2026-08-18に
    // GameModuleApiVersion を上げたとき、CarGameのモジュールが
    // 版数9のまま作り直されず、再生が背景だけになった）。
    //
    // 見つからなければ空を返し、従来どおりNMakeへ戻ります。
    [[nodiscard]] std::filesystem::path FindNinja()
    {
        const auto devCommand = FindVisualStudioDevCommand();
        if (devCommand.empty())
        {
            return {};
        }
        const auto editionRoot = VisualStudioEditionRoot(devCommand);
        const auto ninja =
            editionRoot
            / "Common7" / "IDE" / "CommonExtensions"
            / "Microsoft" / "CMake" / "Ninja" / "ninja.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(ninja, error))
        {
            return ninja;
        }
        return {};
    }

    // 既存のビルドディレクトリが別のジェネレーター、ソースツリー、
    // コンパイラーで作られていたら捨てます。CMakeはこれらの変更を
    // 同じキャッシュへ適用できないため、残したままだと再構成に失敗します。
    void DiscardStaleBuildDirectory(
        const std::filesystem::path& buildDirectory,
        const std::wstring& generator,
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& compiler) noexcept
    {
        std::error_code error;
        const auto cache = buildDirectory / L"CMakeCache.txt";
        if (!std::filesystem::is_regular_file(cache, error))
        {
            return;
        }
        std::ifstream input(cache);
        if (!input)
        {
            return;
        }
        const std::string expected =
            "CMAKE_GENERATOR:INTERNAL="
            + LamaPon::PathToUtf8(generator);
        const auto normalizeCachePath = [](std::string value)
        {
            std::ranges::replace(value, '\\', '/');
            std::ranges::transform(
                value,
                value.begin(),
                [](const char character)
                {
                    return static_cast<char>(
                        std::tolower(
                            static_cast<unsigned char>(character)));
                });
            return value;
        };
        const auto expectedCompiler = normalizeCachePath(
            LamaPon::PathToUtf8(compiler));
        const auto expectedSource = normalizeCachePath(
            LamaPon::PathToUtf8(sourceDirectory));
        bool discard = false;
        std::string line;
        while (std::getline(input, line))
        {
            if (line.starts_with("CMAKE_GENERATOR:INTERNAL="))
            {
                if (line != expected)
                {
                    discard = true;
                    break;
                }
                continue;
            }
            if (line.starts_with("CMAKE_HOME_DIRECTORY:"))
            {
                const auto separator = line.find('=');
                const auto actualSource = separator == std::string::npos
                    ? std::string{}
                    : normalizeCachePath(
                        line.substr(separator + 1));
                if (actualSource != expectedSource)
                {
                    discard = true;
                    break;
                }
                continue;
            }
            if (!compiler.empty()
                && line.starts_with("CMAKE_CXX_COMPILER:"))
            {
                const auto separator = line.find('=');
                const auto actualCompiler = separator == std::string::npos
                    ? std::string{}
                    : normalizeCachePath(
                        line.substr(separator + 1));
                if (actualCompiler != expectedCompiler)
                {
                    discard = true;
                    break;
                }
            }
        }
        if (discard)
        {
            input.close();
            error.clear();
            std::filesystem::remove_all(
                buildDirectory,
                error);
        }
    }

    // OSの実アーキテクチャがARM64かどうか。
    //
    // GetNativeSystemInfoはエミュレーション中のx64プロセスから呼ぶと
    // AMD64を返してしまう（見た目のアーキテクチャしか分からない）ので、
    // IsWow64Process2で**本当のマシン**を訊きます。
    [[nodiscard]] bool IsHostArm64() noexcept
    {
        USHORT processMachine{};
        USHORT nativeMachine{};
        if (IsWow64Process2(
                GetCurrentProcess(),
                &processMachine,
                &nativeMachine))
        {
            return nativeMachine
                == IMAGE_FILE_MACHINE_ARM64;
        }
        return false;
    }

    [[nodiscard]] std::filesystem::path EnvironmentPath(
        const wchar_t* name)
    {
        const DWORD required = GetEnvironmentVariableW(
            name,
            nullptr,
            0);
        if (required == 0)
        {
            return {};
        }
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            name,
            value.data(),
            required);
        if (written == 0 || written >= required)
        {
            return {};
        }
        value.resize(written);
        return value;
    }

    [[nodiscard]] bool IsUncPath(
        const std::filesystem::path& path) noexcept
    {
        auto value = path.native();
        std::ranges::replace(value, L'/', L'\\');
        if (value.starts_with(L"\\\\?\\UNC\\")
            || value.starts_with(L"\\\\?\\unc\\"))
        {
            return true;
        }
        // Extended-length local paths (\\?\C:\...) and device paths
        // (\\.\...) are not network paths merely because they begin with
        // two separators.
        if (value.starts_with(L"\\\\?\\")
            || value.starts_with(L"\\\\.\\"))
        {
            return false;
        }
        return value.starts_with(L"\\\\");
    }

    [[nodiscard]] bool UsesNetworkDrive(
        const std::filesystem::path& projectRoot) noexcept
    {
        try
        {
            const auto absolute = std::filesystem::absolute(projectRoot);
            if (IsUncPath(absolute))
            {
                return true;
            }
            const auto root = absolute.root_path();
            return !root.empty()
                && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] std::wstring ProjectCacheKey(
        const std::filesystem::path& projectRoot)
    {
        auto normalized = std::filesystem::weakly_canonical(
            std::filesystem::absolute(projectRoot)).native();
        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t character : normalized)
        {
            const auto folded = static_cast<std::uint64_t>(
                std::towlower(character));
            hash ^= folded;
            hash *= 1099511628211ull;
        }
        std::wostringstream stream;
        stream << std::hex << std::setw(16) << std::setfill(L'0')
            << hash;
        return stream.str();
    }

    // 実行中のLamaPonRuntime.dll自身のビルド時刻。GameModuleHostは
    // 「DLLがこれより古ければ読み込まない」安全弁を持つため、
    // 自動ビルドの判定も同じ基準を見る必要があります。
    // DLLをロードしていないプロセス（テスト等）では、実行ファイルの
    // 隣にあるDLLのファイル時刻で代用します。
    [[nodiscard]] bool TryGetRuntimeWriteTime(
        std::filesystem::file_time_type& writeTime) noexcept
    {
        std::filesystem::path runtimePath;
        if (const HMODULE runtime =
                GetModuleHandleW(L"LamaPonRuntime.dll");
            runtime != nullptr)
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD length = GetModuleFileNameW(
                runtime,
                path.data(),
                static_cast<DWORD>(path.size()));
            if (length != 0 && length < path.size())
            {
                path.resize(length);
                runtimePath = path;
            }
        }
        if (runtimePath.empty())
        {
            runtimePath = LamaPon::ExecutableDirectory()
                / L"LamaPonRuntime.dll";
        }
        std::error_code error;
        const auto time =
            std::filesystem::last_write_time(runtimePath, error);
        if (error)
        {
            return false;
        }
        writeTime = time;
        return true;
    }

    [[nodiscard]] std::filesystem::path LocalBuildCacheRoot()
    {
        // テストや管理環境では保存先を明示できます。未指定なら、
        // エンジンを更新しても残るユーザー単位のキャッシュです。
        if (const auto overrideRoot = EnvironmentPath(
                L"LAMAPON_GAME_MODULE_CACHE_ROOT");
            !overrideRoot.empty())
        {
            return overrideRoot;
        }
        if (const auto localAppData = EnvironmentPath(L"LOCALAPPDATA");
            !localAppData.empty())
        {
            return localAppData
                / L"LamaPon"
                / L"BuildCache";
        }
        return std::filesystem::temp_directory_path()
            / L"LamaPon"
            / L"BuildCache";
    }
}

namespace LamaPon
{
    GameModuleBuildState InspectGameModuleBuildState(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& requestedOutputModule) noexcept
    {
        GameModuleBuildState state;
        try
        {
            const auto assetRoot = projectRoot / L"assets";
            std::filesystem::file_time_type latestSource{};
            std::error_code error;
            if (std::filesystem::is_directory(assetRoot, error))
            {
                const auto options =
                    std::filesystem::directory_options::
                        skip_permission_denied;
                for (std::filesystem::recursive_directory_iterator
                        iterator{ assetRoot, options, error };
                    iterator
                        != std::filesystem::recursive_directory_iterator{};
                    iterator.increment(error))
                {
                    if (error)
                    {
                        error.clear();
                        continue;
                    }
                    if (!iterator->is_regular_file(error) || error)
                    {
                        error.clear();
                        continue;
                    }
                    auto extension = iterator->path().extension().wstring();
                    std::ranges::transform(
                        extension,
                        extension.begin(),
                        [](const wchar_t character)
                        {
                            return static_cast<wchar_t>(
                                std::towlower(character));
                        });
                    if (extension != L".cpp"
                        && extension != L".h"
                        && extension != L".hpp")
                    {
                        continue;
                    }
                    state.hasSources = true;
                    const auto writeTime =
                        iterator->last_write_time(error);
                    if (!error)
                    {
                        latestSource = std::max(
                            latestSource,
                            writeTime);
                    }
                    error.clear();
                }
            }

            const auto output = requestedOutputModule.empty()
                ? projectRoot
                    / L".lamapon"
                    / L"bin"
                    / L"LamaPonGameModule.dll"
                : requestedOutputModule;
            state.outputExists =
                std::filesystem::is_regular_file(output, error);
            error.clear();
            std::filesystem::file_time_type outputTime{};
            if (state.outputExists)
            {
                outputTime = std::filesystem::last_write_time(
                    output,
                    error);
                if (error)
                {
                    state.outputExists = false;
                    error.clear();
                }
            }
            // エンジンを建て直した直後はDLLがRuntimeより古くなり、
            // GameModuleHostが読み込みを拒否する（ユーザーには
            // 「再生しても何も始まらない」に見える）。ソースが
            // 変わっていなくても、この状態は再ビルドが必要。
            std::filesystem::file_time_type runtimeTime{};
            state.staleAgainstRuntime = state.outputExists
                && TryGetRuntimeWriteTime(runtimeTime)
                && outputTime < runtimeTime;
            state.buildRequired = state.hasSources
                && (!state.outputExists
                    || latestSource > outputTime
                    || state.staleAgainstRuntime);
        }
        catch (...)
        {
            // 自動判定に失敗してもEditor起動を妨げません。手動ビルドは
            // 従来どおり利用できます。
        }
        return state;
    }

    bool ShouldUseLocalGameModuleBuildCache(
        const std::filesystem::path& projectRoot) noexcept
    {
        return UsesNetworkDrive(projectRoot)
            || !EnvironmentPath(
                L"LAMAPON_GAME_MODULE_CACHE_ROOT").empty();
    }

    GameModuleBuildCommand MakeGameModuleBuildCommand(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineRoot,
        const std::filesystem::path& runtimeDirectory,
        const std::string& configuration)
    {
        if (configuration != "Debug"
            && configuration != "Release"
            && configuration != "RelWithDebInfo"
            && configuration != "MinSizeRel")
        {
            throw std::invalid_argument(
                "Game Module configuration must be Debug, Release,"
                " RelWithDebInfo, or MinSizeRel.");
        }
        const auto moduleSourceDirectory =
            engineRoot / L"tools" / L"ProjectGameModule";
        if (!std::filesystem::is_regular_file(
                moduleSourceDirectory / L"CMakeLists.txt"))
        {
            throw std::runtime_error(
                "Game Moduleビルドツールが見つかりません: "
                + PathToUtf8(moduleSourceDirectory));
        }
        if (!std::filesystem::is_regular_file(
                runtimeDirectory / L"LamaPonRuntime.lib"))
        {
            throw std::runtime_error(
                "LamaPonRuntime.libがEditorと同じフォルダーにありません");
        }

        const auto lamaponDirectory =
            projectRoot / L".lamapon";
        std::filesystem::create_directories(
            lamaponDirectory);

        GameModuleBuildCommand command;
        command.logPath =
            lamaponDirectory
            / L"game-module-build.log";
        command.outputModule =
            lamaponDirectory
            / L"bin"
            / L"LamaPonGameModule.dll";
        command.usesLocalBuildCache =
            ShouldUseLocalGameModuleBuildCache(projectRoot);
        command.buildDirectory = command.usesLocalBuildCache
            ? LocalBuildCacheRoot()
                / ProjectCacheKey(projectRoot)
                / Utf8ToWide(configuration)
                / L"game-module"
            : lamaponDirectory / L"build" / L"game-module";
        const auto workingOutputDirectory =
            command.usesLocalBuildCache
                ? command.buildDirectory.parent_path() / L"bin"
                : command.outputModule.parent_path();
        const auto workingLogPath = command.usesLocalBuildCache
            ? command.buildDirectory.parent_path()
                / L"game-module-build.log"
            : command.logPath;
        std::filesystem::create_directories(
            command.buildDirectory.parent_path());

        const auto quotedLogPath =
            L"\""
            + workingLogPath.wstring()
            + L"\"";
        command.parameters = command.usesLocalBuildCache
            ? L"/d /v:on /c \"("
            : L"/d /c";
        const auto devCommand = FindVisualStudioDevCommand();
        const auto compiler = FindMsvcCompiler(devCommand);
        if (!devCommand.empty())
        {
            // Game ModuleはLamaPonRuntime.dll（x64）とリンクするので
            // ターゲットは常にx64です。ホストがARM64（Windows on ARM）
            // のときはARM64ホストのクロスコンパイラを使います。
            // x64ホスト版をエミュレーションで回すより速く、環境に
            // よってはx64ホスト版が入っていないこともあるためです。
            command.parameters +=
                IsHostArm64()
                    ? L" call \""
                        + devCommand.wstring()
                        + L"\" -arch=amd64 -host_arch=arm64"
                          L" > nul 2>&1 &&"
                    : L" call \""
                        + devCommand.wstring()
                        + L"\" -arch=x64 -host_arch=x64"
                          L" > nul 2>&1 &&";
        }
        // CMakeはMSVCの `/showIncludes` 接頭辞を検出してNinjaへ渡します。
        // 日本語Windowsの既定コードページのままだと、その接頭辞が
        // rules.ninja内で文字化けしてdepsが0件になり、ヘッダ変更後に
        // 古いobjを再利用して構造体レイアウトが混在します。configureと
        // buildの両方をUTF-8コンソールで動かし、検出文字列とcl出力の
        // エンコーディングを一致させます。
        command.parameters += L" chcp 65001 > nul &&";
        // NMakeはヘッダ依存の追跡が不完全で、エンジンのヘッダを
        // 更新しても古いobjをリンクします。Ninjaがあればそちらを
        // 使います（エンジン本体のビルドも同じ理由でNinja）。
        const auto ninja = FindNinja();
        const std::wstring generator = ninja.empty()
            ? L"NMake Makefiles"
            : L"Ninja";
        DiscardStaleBuildDirectory(
            command.buildDirectory,
            generator,
            moduleSourceDirectory,
            compiler);
        command.parameters +=
            L" cmake -S \""
            + moduleSourceDirectory.wstring()
            + L"\" -B \""
            + command.buildDirectory.wstring()
            + L"\" -G \"" + generator + L"\""
            + L" -DCMAKE_BUILD_TYPE="
            + Utf8ToWide(configuration)
            + L" -DLAMAPON_ENGINE_ROOT:PATH=\""
            + engineRoot.wstring()
            + L"\" -DLAMAPON_PROJECT_ROOT:PATH=\""
            + projectRoot.wstring()
            + L"\" -DLAMAPON_RUNTIME_DIR:PATH=\""
            + runtimeDirectory.wstring()
            + L"\" -DLAMAPON_MODULE_OUTPUT_DIR:PATH=\""
            + workingOutputDirectory.wstring()
            + L"\"";
        if (!compiler.empty())
        {
            // CMakeCache.txtはコンパイラの絶対パスを保持します。
            // Visual Studioを更新／追加したときに古いcl.exeへ戻らない
            // よう、今回選んだツールセットを毎回明示します。
            command.parameters +=
                L" -DCMAKE_CXX_COMPILER:FILEPATH=\""
                + compiler.wstring()
                + L"\"";
        }
        if (!ninja.empty())
        {
            // VSのDev PromptでもninjaはPATHに無いことがあるので
            // 絶対パスで渡します。
            command.parameters +=
                L" -DCMAKE_MAKE_PROGRAM:FILEPATH=\""
                + ninja.wstring()
                + L"\"";
        }
        if (command.usesLocalBuildCache)
        {
            command.parameters +=
                L" -DLAMAPON_MODULE_DEPLOY_DIR:PATH=\""
                + command.outputModule.parent_path().wstring()
                + L"\"";
        }
        command.parameters +=
            L" > "
            + quotedLogPath
            + L" 2>&1 && cmake --build \""
            + command.buildDirectory.wstring()
            + L"\" --target LamaPonGameModule >> "
            + quotedLogPath
            + L" 2>&1";
        if (command.usesLocalBuildCache)
        {
            // `)`までの終了コードを保存し、成功・失敗どちらでもログを
            // projectへ1回だけコピーします。途中ログをWebDAVへ流し
            // 続けないためです。DLLの配置はCMakeのPOST_BUILDなので、
            // リンク失敗時に前回の正常DLLを壊しません。
            command.parameters +=
                L") & set \"lamapon_build_exit=!errorlevel!\""
                L" & if !lamapon_build_exit! equ 0 ("
                L"cmake -E make_directory \""
                + command.outputModule.parent_path().wstring()
                + L"\""
                L" & cmake -E copy_if_different \""
                + (workingOutputDirectory
                    / L"LamaPonGameModule.dll").wstring()
                + L"\" \""
                + command.outputModule.wstring()
                + L"\""
                L" & if errorlevel 1 set \"lamapon_build_exit=2\""
                L")"
                L" & copy /y \""
                + workingLogPath.wstring()
                + L"\" \""
                + command.logPath.wstring()
                + L"\" > nul 2>&1"
                L" & exit /b !lamapon_build_exit!\"";
        }
        return command;
    }

    int RefreshStaleGameModuleSources(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& buildDirectory) noexcept
    {
        // 内容が前回ビルドから変わったのに更新時刻が動いていない
        // ソースを検出し、時刻を現在へ進めます（宣言側のコメント参照）。
        // マニフェストは「FNV-1aハッシュ<タブ>相対パス」のテキスト。
        // JSONにしないのは、この関数を失敗させないため（依存最小）。
        try
        {
            const auto assetRoot = projectRoot / L"assets";
            std::error_code error;
            if (!std::filesystem::is_directory(assetRoot, error) || error)
            {
                return 0;
            }

            const auto hashFile = [](const std::filesystem::path& file,
                                     std::uint64_t& outHash)
            {
                std::ifstream input(file, std::ios::binary);
                if (!input)
                {
                    return false;
                }
                // FNV-1a 64bit。速く・依存なく・十分に衝突しにくい
                std::uint64_t hash = 1469598103934665603ull;
                char buffer[4096];
                while (input.read(buffer, sizeof(buffer))
                    || input.gcount() > 0)
                {
                    const auto count = input.gcount();
                    for (std::streamsize i = 0; i < count; ++i)
                    {
                        hash ^= static_cast<unsigned char>(buffer[i]);
                        hash *= 1099511628211ull;
                    }
                    if (!input)
                    {
                        break;
                    }
                }
                outHash = hash;
                return true;
            };

            // 前回のマニフェストを読む（キーはUTF-8の相対パス。
            // wstreamのロケール変換は非ASCIIファイル名を壊すので
            // ナローで読み書きする）
            const auto manifestPath =
                buildDirectory / L"lamapon-source-hashes.txt";
            std::map<std::string, std::uint64_t> previous;
            {
                std::ifstream input(manifestPath, std::ios::binary);
                std::string line;
                while (std::getline(input, line))
                {
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    const auto tab = line.find('\t');
                    if (tab == std::string::npos)
                    {
                        continue;
                    }
                    std::uint64_t value = 0;
                    for (const char character :
                        line.substr(0, tab))
                    {
                        if (character < '0' || character > '9')
                        {
                            value = 0;
                            break;
                        }
                        value = value * 10ull
                            + static_cast<std::uint64_t>(
                                character - '0');
                    }
                    if (value != 0)
                    {
                        previous[line.substr(tab + 1)] = value;
                    }
                }
            }

            int touched = 0;
            std::vector<std::pair<std::string, std::uint64_t>> current;
            const auto options = std::filesystem::directory_options::
                skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator
                    iterator{ assetRoot, options, error };
                iterator
                    != std::filesystem::recursive_directory_iterator{};
                iterator.increment(error))
            {
                if (error)
                {
                    error.clear();
                    continue;
                }
                if (!iterator->is_regular_file(error) || error)
                {
                    error.clear();
                    continue;
                }
                auto extension =
                    iterator->path().extension().wstring();
                std::ranges::transform(
                    extension,
                    extension.begin(),
                    [](const wchar_t character)
                    {
                        return static_cast<wchar_t>(
                            std::towlower(character));
                    });
                if (extension != L".cpp"
                    && extension != L".h"
                    && extension != L".hpp")
                {
                    continue;
                }
                std::uint64_t hash = 0;
                if (!hashFile(iterator->path(), hash))
                {
                    continue;
                }
                auto relative = std::filesystem::relative(
                    iterator->path(), assetRoot, error);
                if (error)
                {
                    error.clear();
                    continue;
                }
                const auto key = PathToUtf8(relative);
                current.emplace_back(key, hash);

                const auto found = previous.find(key);
                if (found == previous.end()
                    || found->second == hash)
                {
                    continue;
                }
                // 内容が変わっている。mtimeが動いていない可能性が
                // あるので、現在時刻へ進めてNMakeに確実に拾わせる
                // （変わったファイルはどのみち再コンパイル対象なので
                //  常に進めて害はない）
                const HANDLE handle = CreateFileW(
                    iterator->path().c_str(),
                    FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE
                        | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    continue;
                }
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                if (SetFileTime(handle, nullptr, nullptr, &now))
                {
                    ++touched;
                }
                CloseHandle(handle);
            }

            // 今回のハッシュを記録（ビルド失敗時も、次回に同じ判定が
            // できるよう書いておく。touch済みのmtimeは残るので
            // NMakeは変更を見失わない）
            std::error_code createError;
            std::filesystem::create_directories(
                buildDirectory, createError);
            std::ofstream output(
                manifestPath,
                std::ios::binary | std::ios::trunc);
            if (output)
            {
                for (const auto& [key, hash] : current)
                {
                    output << std::to_string(hash) << '\t'
                           << key << '\n';
                }
            }
            return touched;
        }
        catch (...)
        {
            return 0;
        }
    }
    std::optional<std::uint32_t> ReadGameModuleApiVersion(
        const std::filesystem::path& modulePath) noexcept
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(modulePath, error))
        {
            return std::nullopt;
        }
        // 隣のLamaPonRuntime.dllを拾えるよう、DLLのあるフォルダを
        // 検索パスへ加えて読みます。
        const HMODULE handle = LoadLibraryExW(
            modulePath.c_str(),
            nullptr,
            LOAD_WITH_ALTERED_SEARCH_PATH);
        if (handle == nullptr)
        {
            return std::nullopt;
        }
        // 呼ぶのはdescriptorを返すエクスポートだけです。
        // ここはGameModuleHostが版数を弾くときに通るのと
        // 同じ経路なので、版数違いでも安全に読めます。
        const auto getDescriptor =
            reinterpret_cast<GetGameModuleDescriptorFunction>(
                GetProcAddress(
                    handle,
                    "LamaPonGetGameModule"));
        std::optional<std::uint32_t> apiVersion;
        if (getDescriptor != nullptr)
        {
            if (const auto* descriptor = getDescriptor();
                descriptor != nullptr)
            {
                apiVersion = descriptor->apiVersion;
            }
        }
        FreeLibrary(handle);
        return apiVersion;
    }

}
