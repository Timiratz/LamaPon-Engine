#include "LamaPon/Editor/GameModuleBuilder.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"
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

    // OS本来のアーキテクチャがARM64か判定します。
    [[nodiscard]] bool IsHostArm64() noexcept;

    // ドット区切りの数字を版数成分へ分け、不正なら空を返します(text: 解析する版数表記)。
    [[nodiscard]] VersionComponents ParseVersionComponents(
        const std::wstring& text)
    {
        // 版数成分または最新ツールパス
        VersionComponents result;
        // 取得または解析中の値
        std::uint32_t value{};
        // 版数成分に数字があるか
        bool hasDigits = false;
        // 解析または変換する文字
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

    // VsDevCmdの既定配置からVSエディションの基準パスを返します(devCommand: VS環境バッチのパス)。
    [[nodiscard]] std::filesystem::path VisualStudioEditionRoot(
        const std::filesystem::path& devCommand)
    {
        // <edition>/Common7/Tools/VsDevCmd.bat -> <edition>
        return devCommand.parent_path()
            .parent_path()
            .parent_path();
    }

    // 版数成分の比較で最新のMSVCツールセットを選びます(devCommand: VS環境バッチのパス)。
    [[nodiscard]] std::filesystem::path FindLatestMsvcToolsetRoot(
        const std::filesystem::path& devCommand)
    {
        // MSVCツールセットの親パス
        const auto toolsets = VisualStudioEditionRoot(devCommand)
            / "VC" / "Tools" / "MSVC";
        // 版数成分または最新ツールパス
        std::filesystem::path result;
        // 現在の最新ツールセット版数
        VersionComponents resultVersion;
        // ツール列挙の失敗状態
        std::error_code scanError;
        if (!std::filesystem::is_directory(toolsets, scanError))
        {
            return {};
        }
        // 走査中のツールセット
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
            // ツール版数かVS年度フォルダー
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

    // OSのホスト種別に合う最新ツールセットのx64用clを探します(devCommand: VS環境バッチのパス)。
    [[nodiscard]] std::filesystem::path FindMsvcCompiler(
        const std::filesystem::path& devCommand)
    {
        if (devCommand.empty())
        {
            return {};
        }
        // 最新MSVCツールセットのパス
        const auto toolsetRoot = FindLatestMsvcToolsetRoot(devCommand);
        // コンパイラーのホスト種別
        const auto hostDirectory = IsHostArm64()
            ? L"Hostarm64"
            : L"Hostx64";
        // x64ターゲットのフォルダー名
        const auto targetDirectory = IsHostArm64()
            ? L"amd64"
            : L"x64";
        // 選択したcl実行ファイルのパス
        const auto compiler = toolsetRoot
            / L"bin"
            / hostDirectory
            / targetDirectory
            / L"cl.exe";
        // ファイル操作の失敗状態
        std::error_code error;
        return std::filesystem::is_regular_file(compiler, error)
            ? compiler
            : std::filesystem::path{};
    }


    // MSVCツールセットの版数が新しいVS環境バッチを選びます。
    [[nodiscard]] std::filesystem::path
        FindVisualStudioDevCommand()
    {
        // VSのインストール基準パス
        const std::filesystem::path visualStudioRoot{
            L"C:\\Program Files\\Microsoft Visual Studio"
        };
        // VS環境バッチの候補一覧
        std::vector<std::filesystem::path> candidates;
        // ツール列挙の失敗状態
        std::error_code scanError;
        if (std::filesystem::is_directory(
            visualStudioRoot,
            scanError))
        {
            // ツール版数かVS年度フォルダー
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
                // VSエディションのフォルダー
                for (const auto& edition :
                    std::filesystem::directory_iterator(
                        version.path(),
                        std::filesystem::directory_options::
                            skip_permission_denied,
                        scanError))
                {
                    if (edition.is_directory(scanError))
                    {
                        // VS環境バッチの候補パス
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
        // ツールセット版数の降順で候補を整列します(left: 左のVS環境バッチ, right: 右のVS環境バッチ)。
        std::ranges::sort(
            candidates,
            [](const auto& left, const auto& right)
            {
                // 左候補のツールセット版数表記
                const auto leftVersion = FindLatestMsvcToolsetRoot(left)
                    .filename().wstring();
                // 右候補のツールセット版数表記
                const auto rightVersion = FindLatestMsvcToolsetRoot(right)
                    .filename().wstring();
                // 左候補の版数成分
                const auto leftComponents = ParseVersionComponents(
                    leftVersion);
                // 右候補の版数成分
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


    // 選択したVSに同梱されたNinjaを探し、無ければ空を返します。
    [[nodiscard]] std::filesystem::path FindNinja()
    {
        // 選択したVS環境バッチのパス
        const auto devCommand = FindVisualStudioDevCommand();
        if (devCommand.empty())
        {
            return {};
        }
        // 選択したVSエディションのパス
        const auto editionRoot = VisualStudioEditionRoot(devCommand);
        // VS同梱Ninjaの実行ファイル
        const auto ninja =
            editionRoot
            / "Common7" / "IDE" / "CommonExtensions"
            / "Microsoft" / "CMake" / "Ninja" / "ninja.exe";
        // ファイル操作の失敗状態
        std::error_code error;
        if (std::filesystem::is_regular_file(ninja, error))
        {
            return ninja;
        }
        return {};
    }


    // ジェネレーター・ソース・clの不一致時に既存ビルドを削除します(buildDirectory: 比較と削除の対象, generator: 今回のジェネレーター名, sourceDirectory: 今回のCMakeソースパス, compiler: 今回のclパス・空なら不比較)。
    void DiscardStaleBuildDirectory(
        const std::filesystem::path& buildDirectory,
        const std::wstring& generator,
        const std::filesystem::path& sourceDirectory,
        const std::filesystem::path& compiler) noexcept
    {
        // ファイル操作の失敗状態
        std::error_code error;
        // 既存のCMakeキャッシュパス
        const auto cache = buildDirectory / L"CMakeCache.txt";
        if (!std::filesystem::is_regular_file(cache, error))
        {
            return;
        }
        // 既存CMakeキャッシュの入力
        std::ifstream input(cache);
        if (!input)
        {
            return;
        }
        // 今回要求するジェネレーター行
        const std::string expected =
            "CMAKE_GENERATOR:INTERNAL="
            + LamaPon::PathToUtf8(generator);
        // 区切りと大小文字を揃えてパスを比較します(value: 正規化するパス文字列)。
        const auto normalizeCachePath = [](std::string value)
        {
            std::ranges::replace(value, '\\', '/');
            // パスを小文字へ揃えます(character: 変換する文字)。
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
        // 正規化した今回のclパス
        const auto expectedCompiler = normalizeCachePath(
            LamaPon::PathToUtf8(compiler));
        // 正規化した今回のソースパス
        const auto expectedSource = normalizeCachePath(
            LamaPon::PathToUtf8(sourceDirectory));
        // 既存の構成が不一致か
        bool discard = false;
        // キャッシュまたはmanifestの行
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
                // キャッシュの値の区切り位置
                const auto separator = line.find('=');
                // 既存キャッシュのソースパス
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
                // キャッシュの値の区切り位置
                const auto separator = line.find('=');
                // 既存キャッシュのclパス
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


    // エミュレーション中もOS本来の種別でARM64か判定します。
    [[nodiscard]] bool IsHostArm64() noexcept
    {
        // プロセスのアーキテクチャ
        USHORT processMachine{};
        // OS本来のアーキテクチャ
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

    // 環境変数の文字列をパスとして読み、取得できなければ空を返します(name: 読み取る環境変数名)。
    [[nodiscard]] std::filesystem::path EnvironmentPath(
        const wchar_t* name)
    {
        // 環境変数読込に必要な文字数
        const DWORD required = GetEnvironmentVariableW(
            name,
            nullptr,
            0);
        if (required == 0)
        {
            return {};
        }
        // 環境変数の読込バッファ
        std::wstring value(required, L'\0');
        // 環境変数から取得した文字数
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

    // 拡張UNCも含めて判定し、ローカルの拡張長・デバイスパスを除外します(path: 調べるパス)。
    [[nodiscard]] bool IsUncPath(
        const std::filesystem::path& path) noexcept
    {
        // 取得または解析中の値
        auto value = path.native();
        std::ranges::replace(value, L'/', L'\\');
        if (value.starts_with(L"\\\\?\\UNC\\")
            || value.starts_with(L"\\\\?\\unc\\"))
        {
            return true;
        }
        // 拡張長のローカルパス（\\?\C:\...）とデバイスパス（\\.\...）は、先頭に区切り文字が2つあってもネットワークパスではありません。
        if (value.starts_with(L"\\\\?\\")
            || value.starts_with(L"\\\\.\\"))
        {
            return false;
        }
        return value.starts_with(L"\\\\");
    }

    // UNCまたはリモートドライブか調べ、判定失敗時はfalseを返します(projectRoot: 調べるプロジェクトのパス)。
    [[nodiscard]] bool UsesNetworkDrive(
        const std::filesystem::path& projectRoot) noexcept
    {
        try
        {
            // プロジェクトの絶対パス
            const auto absolute = std::filesystem::absolute(projectRoot);
            if (IsUncPath(absolute))
            {
                return true;
            }
            // プロジェクトのドライブ基準
            const auto root = absolute.root_path();
            return !root.empty()
                && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
        }
        catch (...)
        {
            return false;
        }
    }

    // 正規化したパスを小文字化してキャッシュ識別用にハッシュ化します(projectRoot: 識別するプロジェクトのパス)。
    [[nodiscard]] std::wstring ProjectCacheKey(
        const std::filesystem::path& projectRoot)
    {
        // 弱く正規化した絶対パス
        auto normalized = std::filesystem::weakly_canonical(
            std::filesystem::absolute(projectRoot)).native();
        // パスかソース内容の64bitハッシュ
        std::uint64_t hash = 14695981039346656037ull;
        // 解析または変換する文字
        for (const wchar_t character : normalized)
        {
            // 小文字化したパス文字の数値
            const auto folded = static_cast<std::uint64_t>(
                std::towlower(character));
            hash ^= folded;
            hash *= 1099511628211ull;
        }
        // キャッシュ識別子の16進出力
        std::wostringstream stream;
        stream << std::hex << std::setw(16) << std::setfill(L'0')
            << hash;
        return stream.str();
    }


    // 稼働中のRuntimeか実行ファイル隣のRuntimeの更新時刻を取得します(writeTime: 取得した時刻の出力先)。
    [[nodiscard]] bool TryGetRuntimeWriteTime(
        std::filesystem::file_time_type& writeTime) noexcept
    {
        // 稼働中または隣接するRuntime
        std::filesystem::path runtimePath;
        // 既存Runtimeの借用ハンドル
        if (const HMODULE runtime =
                GetModuleHandleW(L"LamaPonRuntime.dll");
            runtime != nullptr)
        {
            // Runtimeのファイル名バッファ
            std::wstring path(MAX_PATH, L'\0');
            // 取得したDLLパスの文字数
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
        // ファイル操作の失敗状態
        std::error_code error;
        // Runtimeの更新時刻
        const auto time =
            std::filesystem::last_write_time(runtimePath, error);
        if (error)
        {
            return false;
        }
        writeTime = time;
        return true;
    }

    // 環境変数指定を優先し、次にユーザー領域、最後にシステムの一時領域を選びます。
    [[nodiscard]] std::filesystem::path LocalBuildCacheRoot()
    {

        // 環境変数で指定したキャッシュ先
        if (const auto overrideRoot = EnvironmentPath(
                L"LAMAPON_GAME_MODULE_CACHE_ROOT");
            !overrideRoot.empty())
        {
            return overrideRoot;
        }
        // ユーザーのローカルデータ保存先
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
    std::uint32_t SelectGameModuleBuildParallelJobs(
        const std::uint32_t logicalProcessors,
        const std::uint64_t availablePhysicalMemory,
        const std::uint64_t availableCommitMemory) noexcept
    {
        // OSと他アプリに残す容量
        constexpr std::uint64_t reserveBytes = 2ull * 1024 * 1024 * 1024;
        // コンパイラー1個の想定容量
        constexpr std::uint64_t bytesPerJob = 2ull * 1024 * 1024 * 1024;
        // 使用可能容量の小さい方
        const auto availableBytes = std::min(
            availablePhysicalMemory, availableCommitMemory);
        // OS向けの余裕を除いた容量
        const auto budgetBytes = availableBytes > reserveBytes
            ? availableBytes - reserveBytes : 0;
        // 論理CPUの半分・最大2並列
        const auto cpuJobs = std::clamp(logicalProcessors / 2u, 1u, 2u);
        // メモリ予算で実行できる個数
        const auto memoryJobs = std::clamp(
            budgetBytes / bytesPerJob, std::uint64_t{ 1 }, std::uint64_t{ 2 });
        return std::min(cpuJobs, static_cast<std::uint32_t>(memoryJobs));
    }

    // ソースとDLL・Runtimeの更新時刻から再ビルドの要否を調べます(projectRoot: プロジェクトの基準パス, requestedOutputModule: 判定対象DLL・空なら既定)。
    GameModuleBuildState InspectGameModuleBuildState(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& requestedOutputModule) noexcept
    {
        // ソースとDLLのビルド判定状態
        GameModuleBuildState state;
        try
        {
            // 対象ソースを走査するassets
            const auto assetRoot = projectRoot / L"assets";
            // 最も新しいソースの更新時刻
            std::filesystem::file_time_type latestSource{};
            // ファイル操作の失敗状態
            std::error_code error;
            if (std::filesystem::is_directory(assetRoot, error))
            {
                // 権限エラーを飛ばす走査設定
                const auto options =
                    std::filesystem::directory_options::
                        skip_permission_denied;
                // ソース走査の位置
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
                    // 比較用に小文字化する拡張子
                    auto extension = iterator->path().extension().wstring();
                    // 拡張子を小文字へ揃えます(character: 変換する文字)。
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
                    // 走査したソースの更新時刻
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

            // 判定対象のDLLパス
            const auto output = requestedOutputModule.empty()
                ? projectRoot
                    / L".lamapon"
                    / L"bin"
                    / L"LamaPonGameModule.dll"
                : requestedOutputModule;
            state.outputExists =
                std::filesystem::is_regular_file(output, error);
            error.clear();
            // 出力DLLの更新時刻
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

            // Runtimeの更新時刻
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
            // 判定失敗時も起動を続け、取得できた状態を返します。
        }
        return state;
    }

    // ネットワーク保存先か明示された保存先指定を判定します。
    bool ShouldUseLocalGameModuleBuildCache(
        const std::filesystem::path& projectRoot) noexcept
    {
        return UsesNetworkDrive(projectRoot)
            || !EnvironmentPath(
                L"LAMAPON_GAME_MODULE_CACHE_ROOT").empty();
    }

    // 依存設定を出力し、構成・ビルド・配置・ログ転送のコマンドを組み立てます。
    GameModuleBuildCommand MakeGameModuleBuildCommand(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineRoot,
        const std::filesystem::path& runtimeDirectory,
        const std::string& configuration,
        const bool fastBuild)
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
        // Game ModuleのCMakeソースパス
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

        // プロジェクト内の管理パス
        const auto lamaponDirectory =
            projectRoot / L".lamapon";
        std::filesystem::create_directories(
            lamaponDirectory);

        // native宣言の不正は拒否し、SDK未配置の依存は警告してマクロごと除外します。
        // native宣言の走査結果
        const auto packageScan =
            ScanPackageNativeDependencies(
                projectRoot / L"assets");
        if (!packageScan.errors.empty())
        {
            // 宣言失敗をまとめた通知文
            std::string message =
                "パッケージのnative設定を読めません:";
            // 個別のnative宣言の失敗理由
            for (const auto& failure : packageScan.errors)
            {
                message += "\n  - " + failure;
            }
            throw std::runtime_error(message);
        }
        // ファイルが揃った依存と不足説明
        const auto nativeSelection =
            SelectAvailablePackageNativeDependencies(
                packageScan.packages);
        // 除外したnative依存の理由
        for (const auto& missing : nativeSelection.missing)
        {
            Logger::Instance().Warning(missing);
        }
        WritePackageNativeCMakeFile(
            lamaponDirectory / L"package-native.cmake",
            nativeSelection.available);

        // 構築するビルドコマンドとパス
        GameModuleBuildCommand command;
        // ビルド開始時のメモリ余裕
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
        {
            command.parallelJobs = SelectGameModuleBuildParallelJobs(
                GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
                memory.ullAvailPhys,
                memory.ullAvailPageFile);
        }
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
        // ビルド中のDLL出力先
        const auto workingOutputDirectory =
            command.usesLocalBuildCache
                ? command.buildDirectory.parent_path() / L"bin"
                : command.outputModule.parent_path();
        // ビルド中のログ出力先
        const auto workingLogPath = command.usesLocalBuildCache
            ? command.buildDirectory.parent_path()
                / L"game-module-build.log"
            : command.logPath;
        std::filesystem::create_directories(
            command.buildDirectory.parent_path());

        // 引用符で囲ったログパス
        const auto quotedLogPath =
            L"\""
            + workingLogPath.wstring()
            + L"\"";
        command.parameters = command.usesLocalBuildCache
            ? L"/d /v:on /c \"("
            : L"/d /c";
        // 選択したVS環境バッチのパス
        const auto devCommand = FindVisualStudioDevCommand();
        // 選択したcl実行ファイルのパス
        const auto compiler = FindMsvcCompiler(devCommand);
        if (!devCommand.empty())
        {
            // ターゲットは常にx64とし、ARM64ホストではクロスコンパイラーを使います。
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
        // ヘッダー依存の検出文字列とcl出力を一致させるため、構成とビルドの両方をUTF-8にします。
        command.parameters += L" chcp 65001 > nul &&";

        // VS同梱Ninjaの実行ファイル
        const auto ninja = FindNinja();
        // 使用するCMakeジェネレーター
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
            + L" -DLAMAPON_MODULE_FAST_BUILD:BOOL="
            + (fastBuild ? L"ON" : L"OFF")
            + L" -DLAMAPON_ENGINE_ROOT:PATH=\""
            + engineRoot.wstring()
            + L"\" -DLAMAPON_PROJECT_ROOT:PATH=\""
            + projectRoot.wstring()
            + L"\" -DLAMAPON_RUNTIME_DIR:PATH=\""
            + runtimeDirectory.wstring()
            + L"\" -DLAMAPON_MODULE_OUTPUT_DIR:PATH=\""
            + workingOutputDirectory.wstring()
            + L"\" -DLAMAPON_RUNTIME_API_VERSION:STRING="
            + std::to_wstring(GameModuleApiVersion);
        if (!compiler.empty())
        {
            // CMakeCache.txtに残る古いcl.exeを使わないよう、検出したツールセットを毎回明示します。
            command.parameters +=
                L" -DCMAKE_CXX_COMPILER:FILEPATH=\""
                + compiler.wstring()
                + L"\"";
        }
        if (!ninja.empty())
        {
            // VSのDev PromptでもninjaはPATHに無いことがあるので絶対パスで渡します。
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
            + L"\" --target LamaPonGameModule --parallel "
            + std::to_wstring(command.parallelJobs)
            + L" >> "
            + quotedLogPath
            + L" 2>&1";
        if (command.usesLocalBuildCache)
        {
            // 終了コードを保ち、DLLはビルド成功時だけ、ログは成否に関わらず一度だけプロジェクトへ戻します。
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

    // 内容ハッシュが変わったソースの時刻を更新し、今回のハッシュを保存します。
    int RefreshStaleGameModuleSources(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& buildDirectory) noexcept
    {
        // manifestは十進の内容ハッシュとUTF-8相対パスをタブで区切ります。
        try
        {
            // 対象ソースを走査するassets
            const auto assetRoot = projectRoot / L"assets";
            // ファイル操作の失敗状態
            std::error_code error;
            if (!std::filesystem::is_directory(assetRoot, error) || error)
            {
                return 0;
            }

            // ソース内容のハッシュを取得します(file: 読込対象のパス, outHash: ハッシュの出力先)。
            const auto hashFile = [](const std::filesystem::path& file,
                                     std::uint64_t& outHash)
            {
                // 内容ハッシュ用のソース入力
                std::ifstream input(file, std::ios::binary);
                if (!input)
                {
                    return false;
                }

                // パスかソース内容の64bitハッシュ
                std::uint64_t hash = 1469598103934665603ull;
                // 内容ハッシュ用の読込バッファ
                char buffer[4096];
                while (input.read(buffer, sizeof(buffer))
                    || input.gcount() > 0)
                {
                    // 読み取ったソースのバイト数
                    const auto count = input.gcount();
                    // ハッシュへ加えるバイトの添字
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


            // ソース内容ハッシュの保存パス
            const auto manifestPath =
                buildDirectory / L"lamapon-source-hashes.txt";
            // 前回の相対パス別ハッシュ
            std::map<std::string, std::uint64_t> previous;
            {
                // 前回の内容ハッシュmanifest入力
                std::ifstream input(manifestPath, std::ios::binary);
                // キャッシュまたはmanifestの行
                std::string line;
                while (std::getline(input, line))
                {
                    if (!line.empty() && line.back() == '\r')
                    {
                        line.pop_back();
                    }
                    // ハッシュと相対パスの区切り
                    const auto tab = line.find('\t');
                    if (tab == std::string::npos)
                    {
                        continue;
                    }
                    // 取得または解析中の値
                    std::uint64_t value = 0;
                    // 解析または変換する文字
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

            // 更新時刻を変更したファイル数
            int touched = 0;
            // 今回の相対パスとハッシュ一覧
            std::vector<std::pair<std::string, std::uint64_t>> current;
            // 権限エラーを飛ばす走査設定
            const auto options = std::filesystem::directory_options::
                skip_permission_denied;
            // ソース走査の位置
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
                // 比較用に小文字化する拡張子
                auto extension =
                    iterator->path().extension().wstring();
                // 拡張子を小文字へ揃えます(character: 変換する文字)。
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
                // パスかソース内容の64bitハッシュ
                std::uint64_t hash = 0;
                if (!hashFile(iterator->path(), hash))
                {
                    continue;
                }
                // assetsからの相対ソースパス
                auto relative = std::filesystem::relative(
                    iterator->path(), assetRoot, error);
                if (error)
                {
                    error.clear();
                    continue;
                }
                // ソースのUTF-8相対パス
                const auto key = PathToUtf8(relative);
                current.emplace_back(key, hash);

                // 前回のソースハッシュ検索結果
                const auto found = previous.find(key);
                if (found == previous.end()
                    || found->second == hash)
                {
                    continue;
                }
                // 更新時刻が動かないファイルも依存追跡に拾わせるため、内容変更時は更新時刻を進めます。
                // ファイルまたは読込DLLのハンドル
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
                // 設定する現在の更新時刻
                FILETIME now{};
                GetSystemTimeAsFileTime(&now);
                if (SetFileTime(handle, nullptr, nullptr, &now))
                {
                    ++touched;
                }
                CloseHandle(handle);
            }

            // ビルド成否に関わらず、更新時刻の判定に使った現在のハッシュを記録します。
            // manifest保存先作成の失敗状態
            std::error_code createError;
            std::filesystem::create_directories(
                buildDirectory, createError);
            // 現在の内容ハッシュmanifest出力
            std::ofstream output(
                manifestPath,
                std::ios::binary | std::ios::trunc);
            if (output)
            {
                // 相対パスと内容ハッシュの保存対象
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
    // descriptorからAPI版数を読み、ロードしたDLLを解放します。
    std::optional<std::uint32_t> ReadGameModuleApiVersion(
        const std::filesystem::path& modulePath) noexcept
    {
        // ファイル操作の失敗状態
        std::error_code error;
        if (!std::filesystem::is_regular_file(modulePath, error))
        {
            return std::nullopt;
        }
        // 隣のLamaPonRuntime.dllを拾えるよう、DLLのあるフォルダを検索パスへ加えて読みます。
        // ファイルまたは読込DLLのハンドル
        const HMODULE handle = LoadLibraryExW(
            modulePath.c_str(),
            nullptr,
            LOAD_WITH_ALTERED_SEARCH_PATH);
        if (handle == nullptr)
        {
            return std::nullopt;
        }
        // ゲーム起動用の関数を呼ばず、descriptorの共通先頭にある版数を読みます。
        // DLLのdescriptor取得関数
        const auto getDescriptor =
            reinterpret_cast<GetGameModuleDescriptorFunction>(
                GetProcAddress(
                    handle,
                    "LamaPonGetGameModule"));
        // 読み取れたAPI版数
        std::optional<std::uint32_t> apiVersion;
        if (getDescriptor != nullptr)
        {
            // DLL内のdescriptorへの借用参照
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
