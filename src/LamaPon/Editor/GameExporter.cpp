#include "LamaPon/Editor/GameExporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/AssetPacker.h"
#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectSettings.h"
#include "LamaPon/Editor/ExeIconTool.h"
#include "LamaPon/Editor/GameModuleBuilder.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    std::vector<std::uint8_t> ReadAllBytes(
        const std::filesystem::path& path)
    {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open the exported file: "
                + LamaPon::PathToUtf8(path));
        }
        // tellg()はstd::fposなので、intと三項演算子で混ぜられません
        // （C2445）。負値の判定を先に済ませます。
        const auto end = input.tellg();
        if (end < 0)
        {
            throw std::runtime_error(
                "Could not determine the size of the exported"
                " file: "
                + LamaPon::PathToUtf8(path));
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(end));
        input.seekg(0);
        if (!bytes.empty())
        {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!input)
            {
                throw std::runtime_error(
                    "Could not read the exported file: "
                    + LamaPon::PathToUtf8(path));
            }
        }
        return bytes;
    }

    void WriteAllBytes(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not rewrite the exported file: "
                + LamaPon::PathToUtf8(path));
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!output)
        {
            throw std::runtime_error(
                "Could not finish writing the exported file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // 書き出したLamaPonRuntime.dllの鍵スロットを、このゲームだけの
    // 鍵で上書きします（詳しい並びはCrypto.hを参照）。
    //
    // ゲームごとに鍵を変えるのが目的です。エンジン共通の鍵のままだと、
    // 1本のゲームから鍵を抜いた人が、他のすべてのゲームのassets.tpakを
    // そのまま開けてしまいます。しかもエンジンは公開予定なので、
    // 共通鍵ならソースを読むだけで済みます。
    //
    // 見つからなければ書き出しごと失敗させます。ここを黙って
    // 素通りさせると、既定の鍵（＝公開されている鍵）で暗号化された
    // ゲームが出荷されてしまいます。
    void EmbedArchiveKey(
        const std::filesystem::path& runtimeLibrary,
        const LamaPon::Crypto::AesKey& key)
    {
        auto bytes = ReadAllBytes(runtimeLibrary);
        const auto marker =
            LamaPon::Crypto::ExpectedKeySlotMarker();
        const auto begin = bytes.begin();
        const auto end = bytes.end();

        auto found = std::search(
            begin,
            end,
            marker.begin(),
            marker.end());
        if (found == end)
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll has no archive key slot. "
                "The engine installation is older than this "
                "editor; update it and export again.");
        }
        // 2つ以上あるときは、どちらが本物か決められません
        // （偶然一致した並びを書き換えるとDLLを壊します）。
        if (std::search(
                found + 1,
                end,
                marker.begin(),
                marker.end())
            != end)
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll has more than one archive "
                "key slot; refusing to patch it.");
        }

        const auto slot = LamaPon::Crypto::MakeKeySlot(key);
        if (static_cast<std::size_t>(std::distance(found, end))
            < slot.size())
        {
            throw std::runtime_error(
                "LamaPonRuntime.dll is truncated around its "
                "archive key slot.");
        }
        std::copy(slot.begin(), slot.end(), found);
        WriteAllBytes(runtimeLibrary, bytes);
    }

    // 配布物へ同梱する（アーカイブへ入らない）ファイルを、その場で
    // 暗号化します。事前コンパイル済みシェーダーが対象です——
    // HLSLソースを外しても、DXBCがそのまま置いてあれば逆アセンブルで
    // 中身は読めてしまいます。
    std::size_t SealFilesInDirectory(
        const std::filesystem::path& directory,
        const LamaPon::Crypto::AesKey& key)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            return 0;
        }
        std::size_t sealed{};
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                directory))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const auto bytes = ReadAllBytes(entry.path());
            if (LamaPon::Crypto::IsSealed(
                    bytes.data(),
                    bytes.size()))
            {
                continue;
            }
            WriteAllBytes(
                entry.path(),
                LamaPon::Crypto::Seal(
                    bytes.data(),
                    bytes.size(),
                    key));
            ++sealed;
        }
        return sealed;
    }

    bool IsRelativePathSafe(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return false;
        }

        for (const auto& part : path)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        const auto relative = candidate.lexically_relative(root);
        return !relative.empty()
            && IsRelativePathSafe(relative);
    }

    std::filesystem::path MakeSiblingWorkingPath(
        const std::filesystem::path& output,
        const std::wstring_view label)
    {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        return output.parent_path()
            / (output.filename().wstring()
                + L".lamapon-"
                + std::wstring(label)
                + L"-"
                + suffix);
    }

    bool RenameWithRetry(
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error)
    {
        constexpr int MaxAttempts = 6;
        for (int attempt = 0;
            attempt < MaxAttempts;
            ++attempt)
        {
            error.clear();
            std::filesystem::rename(from, to, error);
            if (!error)
            {
                return true;
            }

            // WebDAVはrenameを完了した後にERROR_NOT_SUPPORTEDを返す
            // 場合があります。移動先だけが存在するなら操作は完了済み
            // なので、重ねて失敗扱いにしません。
            const auto renameError = error;
            std::error_code sourceError;
            std::error_code destinationError;
            const bool sourceExists =
                std::filesystem::exists(from, sourceError);
            const bool destinationExists =
                std::filesystem::exists(to, destinationError);
            if (!sourceError
                && !destinationError
                && !sourceExists
                && destinationExists)
            {
                error.clear();
                return true;
            }
            error = renameError;

            // Windows DefenderやExplorerが直前に触ったフォルダーを
            // 一時的に保持することがあります。アクセス拒否だけは
            // 短く待って再試行し、それ以外のエラーはすぐ返します。
            if (error.value() != ERROR_ACCESS_DENIED
                && error != std::errc::permission_denied)
            {
                return false;
            }
            Sleep(25u * (1u << attempt));
        }
        return false;
    }

    std::runtime_error ExportError(
        const std::string_view message,
        const std::filesystem::path& path)
    {
        return std::runtime_error(
            std::string(message)
            + ": "
            + LamaPon::PathToUtf8(path));
    }

    // エクスポート先のゲームへ同梱するVC++ランタイム。エディター
    // （配布版エンジン）の隣に置かれたDLLをそのままコピーします。
    constexpr std::array<std::wstring_view, 5>
        RuntimeCrtLibraries{
            L"vcruntime140.dll",
            L"vcruntime140_1.dll",
            L"msvcp140.dll",
            L"msvcp140_1.dll",
            L"msvcp140_2.dll"
        };

    // JSONの中の"shaderKeywords"配列を、入れ子も含めて全部集めます。
    void CollectShaderKeywords(
        const nlohmann::json& node,
        std::vector<std::string>& keywords)
    {
        if (node.is_object())
        {
            for (const auto& [key, value] : node.items())
            {
                if (key == "shaderKeywords"
                    && value.is_array())
                {
                    for (const auto& keyword : value)
                    {
                        if (keyword.is_string())
                        {
                            keywords.push_back(
                                keyword.get<std::string>());
                        }
                    }
                    continue;
                }
                CollectShaderKeywords(value, keywords);
            }
            return;
        }
        if (node.is_array())
        {
            for (const auto& value : node)
            {
                CollectShaderKeywords(value, keywords);
            }
        }
    }

    void RunSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        std::error_code removeError;
        std::filesystem::remove(zipPath, removeError);

        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(
                systemDirectory,
                MAX_PATH) == 0)
        {
            throw std::runtime_error(
                "Could not locate the Windows system directory.");
        }
        const auto tarPath =
            std::filesystem::path(systemDirectory)
            / L"tar.exe";
        if (!std::filesystem::is_regular_file(tarPath))
        {
            throw ExportError(
                "tar.exe was not found (required for zip export, bundled with Windows 10 and later)",
                tarPath);
        }

        // -a: 拡張子からzip形式を推定 / -C: 親フォルダーへ移動して
        // フォルダー名だけをアーカイブへ入れます。
        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -a -c -f \""
            + zipPath.wstring() + L"\" -C \""
            + folder.parent_path().wstring() + L"\" \""
            + folder.filename().wstring() + L"\"";

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(
                tarPath.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process) == FALSE)
        {
            throw ExportError(
                "Could not start tar.exe",
                tarPath);
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (exitCode != 0)
        {
            std::filesystem::remove(zipPath, removeError);
            throw ExportError(
                "Zip archive creation failed",
                zipPath);
        }

        std::error_code sizeError;
        const auto zipSize =
            std::filesystem::file_size(zipPath, sizeError);
        // 22 bytesは、entryが1件もないZIPのEnd of Central Directory
        // だけを書いたサイズです。Windows tarはWebDAVの-Cを列挙できず、
        // exit code 0の空ZIPを返すことがあるため、終了コードだけでなく
        // 成果物も検査します。
        if (sizeError || zipSize <= 22)
        {
            std::filesystem::remove(zipPath, removeError);
            throw ExportError(
                "Zip archive was empty after creation",
                zipPath);
        }
    }

    void PublishZipArchive(
        const std::filesystem::path& sourceZip,
        const std::filesystem::path& destinationZip)
    {
        const auto stagingZip = MakeSiblingWorkingPath(
            destinationZip,
            L"staging");
        const auto backupZip = MakeSiblingWorkingPath(
            destinationZip,
            L"backup");

        std::error_code copyError;
        std::filesystem::copy_file(
            sourceZip,
            stagingZip,
            std::filesystem::copy_options::none,
            copyError);

        std::error_code sourceSizeError;
        std::error_code stagingSizeError;
        const auto sourceSize = std::filesystem::file_size(
            sourceZip,
            sourceSizeError);
        const auto stagingSize = std::filesystem::file_size(
            stagingZip,
            stagingSizeError);
        // WebDAVはcopy完了後に失敗コードを返すこともあるため、
        // 最終サイズが一致していれば成功として扱います。
        if (sourceSizeError
            || stagingSizeError
            || sourceSize != stagingSize)
        {
            std::error_code cleanupError;
            std::filesystem::remove(stagingZip, cleanupError);
            if (copyError)
            {
                throw std::filesystem::filesystem_error(
                    "Could not copy the completed zip archive",
                    sourceZip,
                    stagingZip,
                    copyError);
            }
            throw ExportError(
                "Copied zip archive did not match its source",
                stagingZip);
        }

        const bool hadPreviousZip =
            std::filesystem::exists(destinationZip);
        if (hadPreviousZip)
        {
            std::error_code renameError;
            if (!RenameWithRetry(
                    destinationZip,
                    backupZip,
                    renameError))
            {
                std::error_code cleanupError;
                std::filesystem::remove(stagingZip, cleanupError);
                throw std::filesystem::filesystem_error(
                    "Could not move the previous zip archive",
                    destinationZip,
                    backupZip,
                    renameError);
            }
        }

        std::error_code renameError;
        if (!RenameWithRetry(
                stagingZip,
                destinationZip,
                renameError))
        {
            std::error_code cleanupError;
            std::filesystem::remove(stagingZip, cleanupError);
            if (hadPreviousZip
                && !std::filesystem::exists(destinationZip))
            {
                RenameWithRetry(
                    backupZip,
                    destinationZip,
                    cleanupError);
            }
            throw std::filesystem::filesystem_error(
                "Could not publish the zip archive",
                stagingZip,
                destinationZip,
                renameError);
        }

        if (hadPreviousZip)
        {
            std::error_code cleanupError;
            std::filesystem::remove(backupZip, cleanupError);
        }
    }

    // Windows標準のtar.exe（bsdtar）でフォルダーを.zipへ固めます。
    // WebDAV上ではtarの-Cがexit 0の空ZIPを作るため、入力を一時的に
    // ローカルへ複製し、完成ZIPだけをtransactionalに配布先へ移します。
    void CreateZipWithSystemTar(
        const std::filesystem::path& folder,
        const std::filesystem::path& zipPath)
    {
        const auto temporaryRoot =
            std::filesystem::temp_directory_path()
            / (L"LamaPonExportZip-"
                + std::to_wstring(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()));
        std::error_code directoryError;
        if (!LamaPon::EnsureDirectoryExists(
                temporaryRoot,
                directoryError))
        {
            throw std::filesystem::filesystem_error(
                "Could not create the local zip workspace",
                temporaryRoot,
                directoryError);
        }

        const bool useLocalInput =
            LamaPon::ShouldUseLocalGameModuleBuildCache(folder);
        const auto archiveFolder = useLocalInput
            ? temporaryRoot / folder.filename()
            : folder;
        const auto localZip = temporaryRoot
            / (folder.filename().wstring() + L".zip");
        try
        {
            if (useLocalInput)
            {
                std::error_code copyError;
                std::filesystem::copy(
                    folder,
                    archiveFolder,
                    std::filesystem::copy_options::recursive,
                    copyError);
                if (copyError)
                {
                    throw std::filesystem::filesystem_error(
                        "Could not copy the export into the local zip workspace",
                        folder,
                        archiveFolder,
                        copyError);
                }
            }

            RunSystemTar(archiveFolder, localZip);
            PublishZipArchive(localZip, zipPath);
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                temporaryRoot,
                cleanupError);
            throw;
        }

        std::error_code cleanupError;
        std::filesystem::remove_all(
            temporaryRoot,
            cleanupError);
    }
}

namespace LamaPon
{
    std::wstring SanitizeGameFileName(
        const std::string& gameName)
    {
        constexpr std::wstring_view invalidCharacters =
            LR"(\/:*?"<>|)";
        std::wstring result;
        for (const wchar_t character : Utf8ToWide(gameName))
        {
            const bool invalid = character < 0x20
                || invalidCharacters.find(character)
                    != std::wstring_view::npos;
            result.push_back(invalid ? L'_' : character);
        }

        // 先頭・末尾の空白とドットはWindowsのファイル名で
        // 使えないため取り除きます。
        const auto first = result.find_first_not_of(L" .");
        const auto last = result.find_last_not_of(L" .");
        result = first == std::wstring::npos
            ? std::wstring{}
            : result.substr(first, last - first + 1);
        if (result.empty())
        {
            return L"LamaPonGame";
        }

        // CONやNULなどの予約デバイス名はそのまま使えないため
        // 先頭へ「_」を付けます。
        std::wstring upper = result;
        std::transform(
            upper.begin(),
            upper.end(),
            upper.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(
                    std::towupper(value));
            });
        constexpr std::array<std::wstring_view, 22>
            reservedNames{
                L"CON", L"PRN", L"AUX", L"NUL",
                L"COM1", L"COM2", L"COM3", L"COM4",
                L"COM5", L"COM6", L"COM7", L"COM8",
                L"COM9",
                L"LPT1", L"LPT2", L"LPT3", L"LPT4",
                L"LPT5", L"LPT6", L"LPT7", L"LPT8",
                L"LPT9"
            };
        if (std::ranges::find(reservedNames, upper)
            != reservedNames.end())
        {
            result.insert(result.begin(), L'_');
        }
        return result;
    }

    GameExportResult ExportGamePackage(
        const GameExportOptions& options)
    {
        const auto runtimeDirectory = std::filesystem::weakly_canonical(
            options.runtimeDirectory);
        const auto assetDirectory = std::filesystem::weakly_canonical(
            options.assetDirectory);
        const auto outputDirectory = std::filesystem::absolute(
            options.outputDirectory).lexically_normal();

        if (!std::filesystem::is_directory(runtimeDirectory))
        {
            throw ExportError(
                "Runtime directory was not found",
                runtimeDirectory);
        }
        if (!std::filesystem::is_directory(assetDirectory))
        {
            throw ExportError(
                "Asset directory was not found",
                assetDirectory);
        }
        if (outputDirectory.filename().empty())
        {
            throw ExportError(
                "Export directory requires a folder name",
                outputDirectory);
        }
        if (std::filesystem::exists(outputDirectory)
            && !std::filesystem::is_directory(outputDirectory))
        {
            throw ExportError(
                "Export destination is not a directory",
                outputDirectory);
        }
        if (outputDirectory == assetDirectory
            || IsPathWithin(assetDirectory, outputDirectory)
            || IsPathWithin(outputDirectory, assetDirectory))
        {
            throw ExportError(
                "Export directory cannot contain or be inside the asset directory",
                outputDirectory);
        }
        if (outputDirectory == runtimeDirectory
            || IsPathWithin(outputDirectory, runtimeDirectory))
        {
            throw ExportError(
                "Export directory cannot contain the runtime directory",
                outputDirectory);
        }
        ValidateProjectSettings(options.projectSettings);

        const auto gameExecutable =
            runtimeDirectory / L"LamaPonGame.exe";
        const auto runtimeLibrary =
            runtimeDirectory / L"LamaPonRuntime.dll";
        const auto audioRuntime =
            runtimeDirectory / L"xaudio2_9redist.dll";
        const auto gameModule = options.gameModulePath.empty()
            ? runtimeDirectory / L"LamaPonGameModule.dll"
            : std::filesystem::absolute(
                options.gameModulePath).lexically_normal();
        const auto startupScene =
            assetDirectory
            / options.projectSettings.startupScene;
        if (!std::filesystem::is_regular_file(gameExecutable))
        {
            throw ExportError(
                "LamaPonGame.exe was not found",
                gameExecutable);
        }
        if (!std::filesystem::is_regular_file(runtimeLibrary))
        {
            throw ExportError(
                "LamaPonRuntime.dll was not found",
                runtimeLibrary);
        }
        if (!std::filesystem::is_regular_file(audioRuntime))
        {
            throw ExportError(
                "xaudio2_9redist.dll was not found",
                audioRuntime);
        }
        if (!std::filesystem::is_regular_file(startupScene))
        {
            throw ExportError(
                "Startup scene was not found",
                startupScene);
        }

        // C++ Scriptを含むProjectで古い／無いGame Moduleをそのまま
        // 梱包すると、Sceneだけは読めるのにScriptが一つも動かず、空や
        // 背景色だけのゲームになります。配布先で初めて壊れるのではなく、
        // 書き出し時点で理由と直し方を返します。
        const auto moduleState = InspectGameModuleBuildState(
            assetDirectory.parent_path(),
            gameModule);
        if (moduleState.hasSources && !moduleState.outputExists)
        {
            throw ExportError(
                "C++ sources exist, but the Game Module is missing. Build the Game Module before exporting",
                gameModule);
        }
        if (moduleState.buildRequired)
        {
            throw ExportError(
                "The Game Module is older than a project C++ source. Build the Game Module before exporting",
                gameModule);
        }
        if (moduleState.outputExists)
        {
            std::error_code moduleTimeError;
            std::error_code runtimeTimeError;
            const auto moduleTime = std::filesystem::last_write_time(
                gameModule,
                moduleTimeError);
            const auto runtimeTime = std::filesystem::last_write_time(
                runtimeLibrary,
                runtimeTimeError);
            if (moduleTimeError || runtimeTimeError)
            {
                throw ExportError(
                    "Could not verify Game Module compatibility timestamps",
                    gameModule);
            }
            if (moduleTime < runtimeTime)
            {
                throw ExportError(
                    "The Game Module is older than LamaPonRuntime.dll and would be rejected at startup. Rebuild the Game Module before exporting",
                    gameModule);
            }
        }
        const auto gameIconSource =
            options.projectSettings.gameIcon.empty()
                ? std::filesystem::path{}
                : assetDirectory
                    / options.projectSettings.gameIcon;
        if (!gameIconSource.empty()
            && !std::filesystem::is_regular_file(
                gameIconSource))
        {
            throw ExportError(
                "Game icon was not found",
                gameIconSource);
        }

        const auto outputParent = outputDirectory.parent_path();
        std::error_code outputParentError;
        if (!LamaPon::EnsureDirectoryExists(
                outputParent,
                outputParentError))
        {
            throw std::filesystem::filesystem_error(
                "Could not create the export parent directory",
                outputParent,
                outputParentError);
        }
        const auto stagingDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"staging");
        const auto backupDirectory = MakeSiblingWorkingPath(
            outputDirectory,
            L"backup");

        // 実行ファイルはゲーム名を反映した名前で出力します。
        const std::wstring exportedExecutableName =
            SanitizeGameFileName(
                options.projectSettings.gameName)
            + L".exe";

        try
        {
            std::error_code stagingError;
            if (!LamaPon::EnsureDirectoryExists(
                    stagingDirectory,
                    stagingError))
            {
                throw std::filesystem::filesystem_error(
                    "Could not create the export staging directory",
                    stagingDirectory,
                    stagingError);
            }
            std::filesystem::copy_file(
                gameExecutable,
                stagingDirectory / exportedExecutableName);
            std::filesystem::copy_file(
                runtimeLibrary,
                stagingDirectory / runtimeLibrary.filename());
            std::filesystem::copy_file(
                audioRuntime,
                stagingDirectory / audioRuntime.filename());

            // このゲームだけのアーカイブ鍵を作り、書き出した
            // LamaPonRuntime.dllへ焼き込みます。先に済ませるのは、
            // 失敗したときにシェーダーの事前コンパイル（数秒）を
            // 無駄にしないためです。
            const auto archiveKey = LamaPon::Crypto::RandomKey();
            EmbedArchiveKey(
                stagingDirectory / runtimeLibrary.filename(),
                archiveKey);

            // ゲームアイコンを実行ファイルへ埋め込みます
            // （ExplorerのファイルアイコンとウィンドウのLamaPon標準
            // アイコンが差し替わります）。
            if (!gameIconSource.empty())
            {
                ReplaceExecutableIcon(
                    stagingDirectory / exportedExecutableName,
                    BuildIcoFromImageFile(gameIconSource));
            }

            // VC++ランタイムを同梱し、再頒布可能パッケージ未導入の
            // PCでもそのまま起動できるようにします（配布版エンジンの
            // 隣にあるDLLをコピー。無ければスキップ）。
            for (const auto crtLibrary : RuntimeCrtLibraries)
            {
                const auto crtSource =
                    runtimeDirectory / crtLibrary;
                if (std::filesystem::is_regular_file(
                        crtSource))
                {
                    std::filesystem::copy_file(
                        crtSource,
                        stagingDirectory
                            / crtSource.filename());
                }
            }
            if (std::filesystem::is_regular_file(
                    gameModule))
            {
                std::filesystem::copy_file(
                    gameModule,
                    stagingDirectory
                        / gameModule.filename());

                // Project game modules can depend on project-local runtime
                // libraries (middleware, native gameplay runtimes, and so
                // on). Keep those DLLs beside the exported executable so the
                // Windows loader can resolve them without machine-wide setup.
                for (const auto& entry :
                    std::filesystem::directory_iterator(
                        gameModule.parent_path()))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }
                    auto extension = entry.path().extension().wstring();
                    std::transform(
                        extension.begin(),
                        extension.end(),
                        extension.begin(),
                        [](const wchar_t value)
                        {
                            return static_cast<wchar_t>(
                                std::towlower(value));
                        });
                    if (extension != L".dll")
                    {
                        continue;
                    }

                    const auto destination =
                        stagingDirectory / entry.path().filename();
                    if (!std::filesystem::exists(destination))
                    {
                        std::filesystem::copy_file(
                            entry.path(),
                            destination);
                    }
                }
            }
            // HLSLソースを外す設定なら、アーカイブから除きます。
            // .hlsliも同じ（#include専用なので単体では使えませんが、
            // 中身は読めてしまうため）。
            const std::vector<std::wstring> skippedExtensions =
                options.projectSettings.stripShaderSourceOnExport
                    ? std::vector<std::wstring>{
                        L".hlsl",
                        L".hlsli" }
                    : std::vector<std::wstring>{};
            static_cast<void>(
                PackAssets(
                    assetDirectory,
                    stagingDirectory / L"assets.tpak",
                    archiveKey,
                    skippedExtensions));

            // シェーダーを先にコンパイルして同梱します。これが無いと、
            // プレイヤーの初回起動で全シェーダーのコンパイル（実測で
            // 53本・約3.5秒）をまるごと待たせることになります。
            //
            // 入口は総当たりです。VSOutlineのような「あれば使う」枠は
            // 持っていないシェーダーのほうが多く、その失敗も覚えないと
            // 実行時に毎回試し直されてしまいます。
            // AssetManagerはWIC／D2Dのファクトリーを作るのでCOMが要ります。
            // エディターからの書き出しでは既に初期化済みですが、
            // 書き出しをテストや別プロセスから呼ぶこともあるので、
            // ここで面倒を見ます（既に初期化済みなら何もしません）。
            const HRESULT comResult = CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED);
            const bool comInitialized = SUCCEEDED(comResult);
            try
            {
                AssetManager exportAssets{ nullptr, nullptr };
                exportAssets.SetAssetRoot(assetDirectory);
                const auto cacheDirectory =
                    stagingDirectory / L"shader-cache";

                // shader_featureのストリップ用に、プロジェクトの
                // どこかで実際に立てられているキーワードを集めます。
                // シーンやPrefabのJSONへ"shaderKeywords"として
                // 保存されているものが対象です。
                //
                // シェーダーごとに紐付けず、プロジェクト全体の和を
                // 取っているのは安全側だからです。別のシェーダーの
                // キーワードが紛れても「余分に焼く」だけで済み、
                // 必要なものを落とすことはありません。
                std::vector<std::string> usedKeywords;
                for (const auto& entry :
                    std::filesystem::recursive_directory_iterator(
                        assetDirectory))
                {
                    if (!entry.is_regular_file()
                        || entry.path().extension() != L".json")
                    {
                        continue;
                    }
                    try
                    {
                        std::ifstream input(
                            entry.path(),
                            std::ios::binary);
                        if (!input)
                        {
                            continue;
                        }
                        nlohmann::json document;
                        input >> document;
                        CollectShaderKeywords(
                            document,
                            usedKeywords);
                    }
                    catch (const std::exception&)
                    {
                        // 読めないJSONは飛ばします。ここで失敗しても
                        // 「絞れない＝全部焼く」になるだけです。
                    }
                }
                std::sort(
                    usedKeywords.begin(),
                    usedKeywords.end());
                usedKeywords.erase(
                    std::unique(
                        usedKeywords.begin(),
                        usedKeywords.end()),
                    usedKeywords.end());

                std::uint32_t shaderFiles{};
                for (const auto& entry :
                    std::filesystem::recursive_directory_iterator(
                        assetDirectory))
                {
                    if (!entry.is_regular_file())
                    {
                        continue;
                    }
                    auto extension =
                        entry.path().extension().wstring();
                    std::transform(
                        extension.begin(),
                        extension.end(),
                        extension.begin(),
                        [](const wchar_t value)
                        {
                            return static_cast<wchar_t>(
                                std::towlower(value));
                        });
                    // .hlsliは#include専用なので単体では通りません。
                    if (extension != L".hlsl")
                    {
                        continue;
                    }
                    // ソースを外すときは全バリアントを焼きます。
                    // ストリップと同時にやると、取りこぼした
                    // 組み合わせを実行時に作り直せず（ソースが
                    // 無いので）標準Litへ落ちてしまいます。
                    static_cast<void>(
                        PrecompileShader(
                            exportAssets,
                            entry.path(),
                            cacheDirectory,
                            {},
                            options.projectSettings
                                    .stripShaderSourceOnExport
                                ? nullptr
                                : &usedKeywords));
                    ++shaderFiles;
                }
                // ソースが無いときの引き先になる索引。
                WriteShaderCacheIndex(cacheDirectory);
                // 事前コンパイル済みのDXBCと索引を暗号化します。
                // アーカイブの外に置くファイルなので、ここで
                // 個別に包みます（実行時は中身を見て復号します）。
                const auto sealedShaderFiles =
                    SealFilesInDirectory(
                        cacheDirectory,
                        archiveKey);
                Logger::Instance().Info(
                    "Sealed precompiled shader cache: "
                    + std::to_string(sealedShaderFiles)
                    + " file(s).");
                Logger::Instance().Info(
                    "Precompiled shaders for export: "
                    + std::to_string(shaderFiles)
                    + " file(s)."
                    + (options.projectSettings
                            .stripShaderSourceOnExport
                        ? " HLSL sources were excluded from"
                          " the package."
                        : ""));
            }
            catch (const std::exception& exception)
            {
                // 事前コンパイルは速さのための工程です。失敗しても
                // 書き出し自体は成功させます（プレイヤーの初回起動が
                // 遅くなるだけで、ゲームは動きます）。
                Logger::Instance().Warning(
                    std::string(
                        "Shaders could not be precompiled for "
                        "export; the first launch will compile "
                        "them instead: ")
                    + exception.what());
            }
            if (comInitialized)
            {
                CoUninitialize();
            }

            const auto settingsPath =
                stagingDirectory / L"LamaPonGame.json";
            SaveProjectSettings(
                settingsPath,
                options.projectSettings,
                ProjectSettingsFileType::GamePackage);
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                stagingDirectory,
                cleanupError);
            throw;
        }

        const bool hadPreviousExport =
            std::filesystem::exists(outputDirectory);
        if (hadPreviousExport)
        {
            std::error_code renameError;
            if (!RenameWithRetry(
                    outputDirectory,
                    backupDirectory,
                    renameError))
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    stagingDirectory,
                    cleanupError);
                throw std::filesystem::filesystem_error(
                    "Could not move the previous game export",
                    outputDirectory,
                    backupDirectory,
                    renameError);
            }
        }

        std::error_code renameError;
        if (!RenameWithRetry(
                stagingDirectory,
                outputDirectory,
                renameError))
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                stagingDirectory,
                cleanupError);
            if (hadPreviousExport
                && !std::filesystem::exists(outputDirectory))
            {
                RenameWithRetry(
                    backupDirectory,
                    outputDirectory,
                    cleanupError);
            }
            throw std::filesystem::filesystem_error(
                "Could not publish the game export",
                stagingDirectory,
                outputDirectory,
                renameError);
        }

        if (hadPreviousExport)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                backupDirectory,
                cleanupError);
        }

        GameExportResult result;
        result.outputDirectory = outputDirectory;
        result.executablePath =
            outputDirectory / exportedExecutableName;
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                outputDirectory))
        {
            if (entry.is_regular_file())
            {
                ++result.fileCount;
                result.totalBytes += entry.file_size();
            }
        }

        // 配布用ZIPは完成した出力フォルダーの隣へ作成します。
        if (options.createZipArchive)
        {
            const auto zipPath =
                outputDirectory.parent_path()
                / (outputDirectory.filename().wstring()
                    + L".zip");
            CreateZipWithSystemTar(outputDirectory, zipPath);
            result.zipPath = zipPath;
        }
        return result;
    }
}
