#include "LamaPon/Editor/PackageManager.h"

#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace
{
    // Windows標準のtar.exe（bsdtar）でZipをフォルダーへ展開します。
    // bsdtarは既定で「..」を含むパスの展開を拒否するため、
    // アーカイブによるフォルダー外への書き込みを防げます。
    void ExtractZipWithSystemTar(
        const std::filesystem::path& zipPath,
        const std::filesystem::path& destination)
    {
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
            throw std::runtime_error(
                "tar.exe was not found (bundled with Windows 10 and later).");
        }

        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -xf \""
            + zipPath.wstring() + L"\" -C \""
            + destination.wstring() + L"\"";

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
            throw std::runtime_error(
                "Could not start tar.exe.");
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (exitCode != 0)
        {
            throw std::runtime_error(
                "Package archive extraction failed: "
                + LamaPon::PathToUtf8(zipPath));
        }
    }

    // フォルダーの「中身」をZipへ固めます（フォルダー自身は
    // 階層に含めないので、展開先へそのまま widened します）。
    void CreateZipFromDirectoryContents(
        const std::filesystem::path& directory,
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
            throw std::runtime_error(
                "tar.exe was not found (bundled with Windows 10 and later).");
        }

        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -a -c -f \""
            + zipPath.wstring() + L"\" -C \""
            + directory.wstring() + L"\" .";

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
            throw std::runtime_error(
                "Could not start tar.exe.");
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (exitCode != 0)
        {
            std::filesystem::remove(zipPath, removeError);
            throw std::runtime_error(
                "パッケージのZip作成に失敗しました: "
                + LamaPon::PathToUtf8(zipPath));
        }
    }

    // ステージング用の一時フォルダー名（エクスポーターと同じ流儀）。
    std::filesystem::path MakeStagingPath(
        const std::filesystem::path& parent,
        const std::string& name)
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());
        return parent
            / ("pkg-staging-" + name + "-" + suffix);
    }
}

namespace LamaPon
{
    bool IsPackageNameSafe(
        const std::string_view name) noexcept
    {
        if (name.empty() || name.size() > 64)
        {
            return false;
        }
        for (const char character : name)
        {
            const bool valid =
                (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '-'
                || character == '_';
            if (!valid)
            {
                return false;
            }
        }
        return true;
    }

    bool IsAllowedPackageUrl(
        const std::string_view url) noexcept
    {
        // 誤設定や一覧の改ざんに備えて、配布リポジトリ配下の
        // URLだけを許可します。
        return url.rfind(
                "https://raw.githubusercontent.com/Timiratz/"
                "LamaPon-Engine/",
                0) == 0
            || url.rfind(
                "https://github.com/Timiratz/"
                "LamaPon-Engine/",
                0) == 0;
    }

    bool SplitHttpsUrl(
        const std::string_view url,
        std::wstring& host,
        std::wstring& path)
    {
        constexpr std::string_view scheme = "https://";
        if (url.rfind(scheme, 0) != 0)
        {
            return false;
        }
        const auto rest = url.substr(scheme.size());
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos
            || slash == 0)
        {
            return false;
        }
        host = Utf8ToWide(rest.substr(0, slash));
        path = Utf8ToWide(rest.substr(slash));
        return !host.empty() && !path.empty();
    }

    std::vector<PackageInfo> ParsePackageIndex(
        const std::string_view indexJson)
    {
        const auto document =
            nlohmann::json::parse(indexJson);
        if (document.value("format", std::string{})
                != "LamaPonPackageIndex"
            || document.value("version", 0) != 1)
        {
            throw std::runtime_error(
                "Unsupported package index format.");
        }

        std::vector<PackageInfo> packages;
        for (const auto& entry : document.value(
            "packages",
            nlohmann::json::array()))
        {
            PackageInfo package;
            package.name =
                entry.value("name", std::string{});
            package.displayName = entry.value(
                "displayName",
                package.name);
            package.description =
                entry.value("description", std::string{});
            package.author =
                entry.value("author", std::string{});
            package.version =
                entry.value("version", std::string{});
            package.minimumEngineVersion = entry.value(
                "minimumEngineVersion",
                std::string{});
            package.downloadUrl =
                entry.value("downloadUrl", std::string{});
            package.sizeBytes =
                entry.value("sizeBytes", std::uint64_t{});

            // 不正なエントリは黙って除外します（他の正常な
            // パッケージまで巻き込まないため）。
            if (!IsPackageNameSafe(package.name)
                || package.version.empty()
                || !IsAllowedPackageUrl(
                    package.downloadUrl))
            {
                continue;
            }
            packages.push_back(std::move(package));
        }
        return packages;
    }

    std::filesystem::path PackageInstallDirectory(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        return assetRoot / L"packages"
            / PathFromUtf8(name);
    }

    std::string InstalledPackageVersion(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        if (!IsPackageNameSafe(name))
        {
            return {};
        }
        const auto manifestPath =
            PackageInstallDirectory(assetRoot, name)
            / L"package.json";
        if (!std::filesystem::is_regular_file(manifestPath))
        {
            return {};
        }
        try
        {
            std::ifstream input(
                manifestPath,
                std::ios::binary);
            nlohmann::json manifest;
            input >> manifest;
            return manifest.value(
                "version",
                std::string{});
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    void InstallPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::vector<std::uint8_t>& zipBytes)
    {
        if (!IsPackageNameSafe(package.name))
        {
            throw std::invalid_argument(
                "Package name is not safe to install: "
                + package.name);
        }
        if (zipBytes.empty())
        {
            throw std::runtime_error(
                "Package archive is empty.");
        }
        if (!std::filesystem::is_directory(assetRoot))
        {
            throw std::runtime_error(
                "Asset root does not exist: "
                + PathToUtf8(assetRoot));
        }

        const auto packagesRoot =
            assetRoot / L"packages";
        std::filesystem::create_directories(packagesRoot);
        const auto staging = MakeStagingPath(
            packagesRoot,
            package.name);
        const auto zipPath = staging.wstring() + L".zip";

        try
        {
            std::filesystem::create_directories(staging);
            {
                std::ofstream output(
                    std::filesystem::path(zipPath),
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error(
                        "Could not write the package archive.");
                }
                output.write(
                    reinterpret_cast<const char*>(
                        zipBytes.data()),
                    static_cast<std::streamsize>(
                        zipBytes.size()));
            }
            ExtractZipWithSystemTar(zipPath, staging);

            // マニフェストが無いZipには一覧の情報から生成します
            // （インストール済み判定と更新検出に使うため）。
            const auto manifestPath =
                staging / L"package.json";
            if (!std::filesystem::is_regular_file(
                manifestPath))
            {
                const nlohmann::json manifest{
                    { "name", package.name },
                    { "displayName", package.displayName },
                    { "version", package.version },
                    {
                        "minimumEngineVersion",
                        package.minimumEngineVersion
                    }
                };
                std::ofstream output(
                    manifestPath,
                    std::ios::binary | std::ios::trunc);
                output << manifest.dump(2) << '\n';
            }

            // 完成したステージングで既存を置き換えます。
            //
            // 「消してから入れる」（remove_all→rename）は使いません。
            // renameが失敗した時点で旧版が既に消えていて、パッケージ
            // ごと消滅するからです（ウイルス対策ソフトのロックや
            // 開きっぱなしのファイルで実際に起きます）。
            // 旧版はまず退避先へrenameし、新版が入らなければ戻します。
            // renameは同じボリューム内なら中身に触れないので、
            // どの時点で失敗しても旧版のファイルは無傷です。
            //
            // 退避先はassets/の外（.lamapon/package-backups/）です。
            // パッケージはC++ソースを含むことがあり、ゲーム
            // モジュールがassets/*.cppをまとめてコンパイルするため、
            // assets/内へ旧版のコピーが残ると同じシンボルが二重に
            // 定義されてビルドが壊れます。
            //
            // 成功したら退避を1世代だけ残します。更新でフォルダーが
            // 置き換わる仕様上、パッケージへ手を入れていた場合の
            // 逃げ道になります（次の更新で上書きされます）。
            const auto destination =
                PackageInstallDirectory(
                    assetRoot,
                    package.name);
            const auto backupRoot =
                assetRoot.parent_path()
                / L".lamapon"
                / L"package-backups";
            const auto backup =
                backupRoot / PathFromUtf8(package.name);

            const bool hadPrevious =
                std::filesystem::exists(destination);
            if (hadPrevious)
            {
                std::filesystem::create_directories(
                    backupRoot);
                // 前回の退避はここで消えます（1世代だけ）。
                std::filesystem::remove_all(backup);
                // ここが失敗したら何も動かしていないので、
                // そのまま投げて旧版を守ります。
                std::filesystem::rename(
                    destination,
                    backup);
            }
            try
            {
                std::filesystem::rename(
                    staging,
                    destination);
            }
            catch (...)
            {
                // 新版が入らなかったので旧版を戻します。
                if (hadPrevious)
                {
                    std::error_code restoreError;
                    std::filesystem::rename(
                        backup,
                        destination,
                        restoreError);
                    if (restoreError)
                    {
                        // 戻しも失敗。それでも旧版のファイルは
                        // 退避先に無傷で残っているので、場所を
                        // 伝えて手で戻せるようにします。
                        throw std::runtime_error(
                            "パッケージの入れ替えに失敗し、"
                            "自動復元もできませんでした。"
                            "旧版はここに残っています: "
                            + PathToUtf8(backup));
                    }
                }
                throw;
            }
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                staging,
                cleanupError);
            std::filesystem::remove(
                std::filesystem::path(zipPath),
                cleanupError);
            throw;
        }

        std::error_code cleanupError;
        std::filesystem::remove(
            std::filesystem::path(zipPath),
            cleanupError);
    }

    PackageInfo InstallPackageFromFile(
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& zipPath)
    {
        if (!std::filesystem::is_regular_file(zipPath))
        {
            throw std::runtime_error(
                "Zipファイルが見つかりません: "
                + PathToUtf8(zipPath));
        }

        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(
                std::filesystem::file_size(zipPath)));
        {
            std::ifstream input(zipPath, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error(
                    "Zipファイルを読み込めませんでした: "
                    + PathToUtf8(zipPath));
            }
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(
                    bytes.size()));
        }

        // 名前とバージョンはZip内のpackage.jsonが正。無い場合は
        // ファイル名（<name>-<version>.zip）から推測します。
        PackageInfo package;
        const auto staging = MakeStagingPath(
            std::filesystem::temp_directory_path(),
            "inspect");
        try
        {
            std::filesystem::create_directories(staging);
            const auto tempZip =
                staging.wstring() + L".zip";
            {
                std::ofstream output(
                    std::filesystem::path(tempZip),
                    std::ios::binary | std::ios::trunc);
                output.write(
                    reinterpret_cast<const char*>(
                        bytes.data()),
                    static_cast<std::streamsize>(
                        bytes.size()));
            }
            ExtractZipWithSystemTar(
                std::filesystem::path(tempZip),
                staging);
            const auto manifestPath =
                staging / L"package.json";
            if (std::filesystem::is_regular_file(
                manifestPath))
            {
                std::ifstream input(
                    manifestPath,
                    std::ios::binary);
                nlohmann::json manifest;
                input >> manifest;
                package.name = manifest.value(
                    "name",
                    std::string{});
                package.displayName = manifest.value(
                    "displayName",
                    package.name);
                package.description = manifest.value(
                    "description",
                    std::string{});
                package.author = manifest.value(
                    "author",
                    std::string{});
                package.version = manifest.value(
                    "version",
                    std::string{ "1.0" });
                package.minimumEngineVersion =
                    manifest.value(
                        "minimumEngineVersion",
                        std::string{});
            }
            std::error_code cleanupError;
            std::filesystem::remove_all(
                staging,
                cleanupError);
            std::filesystem::remove(
                std::filesystem::path(tempZip),
                cleanupError);
        }
        catch (...)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(
                staging,
                cleanupError);
            throw;
        }

        if (package.name.empty())
        {
            auto stem = PathToUtf8(zipPath.stem());
            if (const auto dash = stem.rfind('-');
                dash != std::string::npos)
            {
                if (package.version.empty())
                {
                    package.version =
                        stem.substr(dash + 1);
                }
                stem = stem.substr(0, dash);
            }
            std::ranges::transform(
                stem,
                stem.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(
                        std::tolower(character));
                });
            package.name = stem;
        }
        if (package.displayName.empty())
        {
            package.displayName = package.name;
        }
        if (package.version.empty())
        {
            package.version = "1.0";
        }
        if (!IsPackageNameSafe(package.name))
        {
            throw std::runtime_error(
                "パッケージ名が不正です（英小文字・数字・-・_ のみ）: "
                + package.name);
        }

        InstallPackage(assetRoot, package, bytes);
        return package;
    }

    PackageBuildResult BuildPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::filesystem::path& outputDirectory)
    {
        if (!IsPackageNameSafe(package.name))
        {
            throw std::invalid_argument(
                "パッケージ名は英小文字・数字・-・_ で"
                "1～64文字にしてください。");
        }
        if (package.version.empty())
        {
            throw std::invalid_argument(
                "バージョンを入力してください。");
        }

        const auto source =
            PackageInstallDirectory(assetRoot, package.name);
        if (!std::filesystem::is_directory(source))
        {
            throw std::runtime_error(
                "パッケージフォルダーがありません: "
                + PathToUtf8(source));
        }

        // フォルダー内のpackage.jsonを最新の内容で作り直します。
        const nlohmann::json manifest{
            { "name", package.name },
            {
                "displayName",
                package.displayName.empty()
                    ? package.name
                    : package.displayName
            },
            { "description", package.description },
            { "author", package.author },
            { "version", package.version },
            {
                "minimumEngineVersion",
                package.minimumEngineVersion
            }
        };
        const auto manifestPath =
            source / L"package.json";
        {
            std::ofstream output(
                manifestPath,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "package.jsonを書き出せませんでした: "
                    + PathToUtf8(manifestPath));
            }
            output << manifest.dump(2) << '\n';
        }

        std::filesystem::create_directories(
            outputDirectory);
        const auto zipPath = outputDirectory
            / PathFromUtf8(
                package.name + "-" + package.version
                + ".zip");
        CreateZipFromDirectoryContents(source, zipPath);

        PackageBuildResult result;
        result.zipPath = zipPath;
        result.manifestPath = manifestPath;
        result.sizeBytes = std::filesystem::file_size(
            zipPath);
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                source))
        {
            if (entry.is_regular_file())
            {
                ++result.fileCount;
            }
        }

        // 一覧へ載せる場合のひな形。配布先は作者が自分で決めるため
        // URLはプレースホルダーにしています。
        nlohmann::json indexEntry = manifest;
        indexEntry["downloadUrl"] =
            "https://example.com/packages/"
            + PathToUtf8(zipPath.filename());
        indexEntry["sizeBytes"] = result.sizeBytes;
        result.indexEntryJson = indexEntry.dump(2);
        return result;
    }

    void UninstallPackage(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        if (!IsPackageNameSafe(name))
        {
            throw std::invalid_argument(
                "Package name is not safe to uninstall.");
        }
        const auto destination =
            PackageInstallDirectory(assetRoot, name);
        if (!std::filesystem::exists(destination))
        {
            return;
        }
        std::filesystem::remove_all(destination);
    }
}
