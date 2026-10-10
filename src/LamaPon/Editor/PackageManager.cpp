#include "LamaPon/Editor/PackageManager.h"

#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace
{

    // Windowsのtarを非表示で実行して展開し、終了コードの失敗を例外にします(zipPath: 展開するZIPパス, destination: 展開先ディレクトリ)。
    void ExtractZipWithSystemTar(
        const std::filesystem::path& zipPath,
        const std::filesystem::path& destination)
    {
        // Windowsのシステムディレクトリ
        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(
                systemDirectory,
                MAX_PATH) == 0)
        {
            throw std::runtime_error(
                "Could not locate the Windows system directory.");
        }
        // システムのtar実行ファイル
        const auto tarPath =
            std::filesystem::path(systemDirectory)
            / L"tar.exe";
        if (!std::filesystem::is_regular_file(tarPath))
        {
            throw std::runtime_error(
                "tar.exe was not found (bundled with Windows 10 and later).");
        }

        // tarへ渡すコマンドライン
        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -xf \""
            + zipPath.wstring() + L"\" -C \""
            + destination.wstring() + L"\"";

        // 非表示プロセスの起動情報
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // 起動したtarのハンドル情報
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
        // tarの終了コード
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


    // フォルダー自体を階層に含めず、内容をZIPへ書き出します(directory: ZIPへ含める基準パス, zipPath: 出力するZIPパス)。
    void CreateZipFromDirectoryContents(
        const std::filesystem::path& directory,
        const std::filesystem::path& zipPath)
    {
        // ZIP削除の失敗状態
        std::error_code removeError;
        std::filesystem::remove(zipPath, removeError);

        // Windowsのシステムディレクトリ
        wchar_t systemDirectory[MAX_PATH]{};
        if (GetSystemDirectoryW(
                systemDirectory,
                MAX_PATH) == 0)
        {
            throw std::runtime_error(
                "Could not locate the Windows system directory.");
        }
        // システムのtar実行ファイル
        const auto tarPath =
            std::filesystem::path(systemDirectory)
            / L"tar.exe";
        if (!std::filesystem::is_regular_file(tarPath))
        {
            throw std::runtime_error(
                "tar.exe was not found (bundled with Windows 10 and later).");
        }

        // tarへ渡すコマンドライン
        std::wstring commandLine =
            L"\"" + tarPath.wstring() + L"\" -a -c -f \""
            + zipPath.wstring() + L"\" -C \""
            + directory.wstring() + L"\" .";

        // 非表示プロセスの起動情報
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // 起動したtarのハンドル情報
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
        // tarの終了コード
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


    // 時刻由来の展開用パスを作り、ディレクトリ自体は作成しません(parent: 保存先の親ディレクトリ, name: パスへ含める識別名)。
    std::filesystem::path MakeStagingPath(
        const std::filesystem::path& parent,
        const std::string& name)
    {
        // 展開フォルダー名の時刻識別子
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
    // 反映タイミングを保存用文字列へ変換します。
    std::string_view PackageActivationName(
        const PackageActivation activation) noexcept
    {
        switch (activation)
        {
        case PackageActivation::Restart:
            return "Restart";
        case PackageActivation::RestartAndRebuild:
            return "RestartAndRebuild";
        case PackageActivation::Immediate:
        default:
            return "Immediate";
        }
    }

    // 反映時期を解釈し、未知の値は即時反映にします。
    PackageActivation PackageActivationFromName(
        const std::string_view name) noexcept
    {
        if (name == "Restart")
        {
            return PackageActivation::Restart;
        }
        if (name == "RestartAndRebuild")
        {
            return PackageActivation::RestartAndRebuild;
        }
        return PackageActivation::Immediate;
    }

    // 即時反映以外に再起動を要求します。
    bool PackageRequiresRestart(
        const PackageActivation activation) noexcept
    {
        return activation != PackageActivation::Immediate;
    }

    // 追加先を保存用文字列へ変換します。
    std::string_view PackageTargetName(
        const PackageTarget target) noexcept
    {
        return target == PackageTarget::Engine
            ? "Engine" : "Project";
    }

    // 追加先を解釈し、未知の値はプロジェクト用にします。
    PackageTarget PackageTargetFromName(
        const std::string_view name) noexcept
    {
        return name == "Engine"
            ? PackageTarget::Engine : PackageTarget::Project;
    }

    // 保存先に許可する名前の長さと文字を検証します。
    bool IsPackageNameSafe(
        const std::string_view name) noexcept
    {
        if (name.empty() || name.size() > 64)
        {
            return false;
        }
        // 検証または変換対象の文字
        for (const char character : name)
        {
            // パッケージ名の許可文字か
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

    // SHA-256の小文字16進表記を検証します。
    bool IsCanonicalPackageSha256(
        const std::string_view value) noexcept
    {
        // 小文字16進64桁の全ての文字を検証します(character: 検証する文字)。
        return value.size() == 64
            && std::ranges::all_of(
                value,
                [](const char character)
                {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                });
    }

    // 配布先の許可URL接頭辞を検証します。
    bool IsAllowedPackageUrl(
        const std::string_view url) noexcept
    {
        // 誤設定や一覧の改ざんに備えて、配布リポジトリ配下のURLだけを許可します。
        return url.rfind(
                "https://raw.githubusercontent.com/Timiratz/"
                "LamaPon-Engine/",
                0) == 0
            || url.rfind(
                "https://github.com/Timiratz/"
                "LamaPon-Engine/",
                0) == 0;
    }

    // HTTPSスキームのホストとパスを分割します。
    bool SplitHttpsUrl(
        const std::string_view url,
        std::wstring& host,
        std::wstring& path)
    {
        // URLのHTTPSスキーム
        constexpr std::string_view scheme = "https://";
        if (url.rfind(scheme, 0) != 0)
        {
            return false;
        }
        // スキームを除いたURL
        const auto rest = url.substr(scheme.size());
        // ホストとパスの区切り位置
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

    // 配布一覧を検証して使用可能な項目を集めます。
    std::vector<PackageInfo> ParsePackageIndex(
        const std::string_view indexJson)
    {
        // 解析済み配布一覧のJSON
        const auto document =
            nlohmann::json::parse(indexJson);
        if (document.value("format", std::string{})
                != "LamaPonPackageIndex"
            || document.value("version", 0) != 1)
        {
            throw std::runtime_error(
                "Unsupported package index format.");
        }

        // インストール可能な一覧
        std::vector<PackageInfo> packages;
        // 配布一覧か出力元のファイル
        for (const auto& entry : document.value(
            "packages",
            nlohmann::json::array()))
        {
            // インストールまたは出力する情報
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
            package.sha256 =
                entry.value("sha256", std::string{});
            package.activation = PackageActivationFromName(
                entry.value("activation", std::string{}));
            package.target = PackageTargetFromName(
                entry.value("target", std::string{}));

            // 名前・バージョン・URL・SHA-256が不正な項目を除外します。
            if (!IsPackageNameSafe(package.name)
                || package.version.empty()
                || !IsAllowedPackageUrl(
                    package.downloadUrl)
                || !IsCanonicalPackageSha256(package.sha256))
            {
                continue;
            }
            packages.push_back(std::move(package));
        }
        return packages;
    }

    // パッケージの保存先パスを返します。
    std::filesystem::path PackageInstallDirectory(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        return assetRoot / L"packages"
            / PathFromUtf8(name);
    }

    // 配置済みのmanifestからバージョンを読みます。
    std::string InstalledPackageVersion(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        if (!IsPackageNameSafe(name))
        {
            return {};
        }
        // package.jsonのパス
        const auto manifestPath =
            PackageInstallDirectory(assetRoot, name)
            / L"package.json";
        if (!std::filesystem::is_regular_file(manifestPath))
        {
            return {};
        }
        try
        {
            // manifestの入力ストリーム
            std::ifstream input(
                manifestPath,
                std::ios::binary);
            // 読込または生成するmanifest
            nlohmann::json manifest;
            // 読込または生成するmanifest
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

    // 配置済みのmanifestから反映時期を読みます。
    PackageActivation InstalledPackageActivation(
        const std::filesystem::path& assetRoot,
        const std::string_view name) noexcept
    {
        if (!IsPackageNameSafe(name))
        {
            return PackageActivation::Immediate;
        }
        try
        {
            // manifestの入力ストリーム
            std::ifstream input(
                PackageInstallDirectory(assetRoot, name)
                    / L"package.json",
                std::ios::binary);
            // 読込または生成するmanifest
            nlohmann::json manifest;
            // 読込または生成するmanifest
            input >> manifest;
            return PackageActivationFromName(
                manifest.value("activation", std::string{}));
        }
        catch (const std::exception&)
        {
            return PackageActivation::Immediate;
        }
    }

    // ZIPとnative宣言の検証後に旧版を退避して配置します。
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
        // 展開前に一覧のSHA-256を検証します。
        if (!IsCanonicalPackageSha256(package.sha256))
        {
            throw std::runtime_error(
                "Package has no valid SHA-256 to verify: "
                + package.name);
        }
        if (Crypto::Sha256Hex(zipBytes.data(), zipBytes.size())
            != package.sha256)
        {
            throw std::runtime_error(
                "Package archive failed its SHA-256 check: "
                + package.name);
        }
        if (!std::filesystem::is_directory(assetRoot))
        {
            throw std::runtime_error(
                "Asset root does not exist: "
                + PathToUtf8(assetRoot));
        }

        // パッケージの保存先
        const auto packagesRoot =
            assetRoot / L"packages";
        std::filesystem::create_directories(packagesRoot);
        // 検証と展開用の作業パス
        const auto staging = MakeStagingPath(
            packagesRoot,
            package.name);
        // 展開用または出力先のZIPパス
        const auto zipPath = staging.wstring() + L".zip";

        try
        {
            std::filesystem::create_directories(staging);
            {
                // ZIPまたはmanifestの出力
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

            // package.jsonが無ければ一覧の情報で生成します。
            // package.jsonのパス
            const auto manifestPath =
                staging / L"package.json";
            if (!std::filesystem::is_regular_file(
                manifestPath))
            {
                // 読込または生成するmanifest
                const nlohmann::json manifest{
                    { "name", package.name },
                    { "displayName", package.displayName },
                    { "version", package.version },
                    {
                        "minimumEngineVersion",
                        package.minimumEngineVersion
                    },
                    {
                        "activation",
                        PackageActivationName(package.activation)
                    },
                    {
                        "target",
                        PackageTargetName(package.target)
                    }
                };
                // ZIPまたはmanifestの出力
                std::ofstream output(
                    manifestPath,
                    std::ios::binary | std::ios::trunc);
                output << manifest.dump(2) << '\n';
            }

            // 配置前にnative宣言を検証し、不正な設定を拒否します。
            {
                // 配置前に検証するmanifestの入力
                std::ifstream manifestInput(
                    manifestPath,
                    std::ios::binary);
                // 検証するmanifestの全文
                const std::string manifestText(
                    std::istreambuf_iterator<char>{
                        manifestInput },
                    std::istreambuf_iterator<char>{});
                static_cast<void>(
                    ParsePackageNativeDependency(
                        manifestText,
                        staging,
                        package.name));
            }


            // 旧版は重複コンパイルを防ぐためassetsの外へ一世代残し、新版の配置失敗時は復元します。
            // インストール済みパッケージのパス
            const auto destination =
                PackageInstallDirectory(
                    assetRoot,
                    package.name);
            // 旧版の退避先・assetsの外
            const auto backupRoot =
                assetRoot.parent_path()
                / L".lamapon"
                / L"package-backups";
            // 一世代だけ残す旧版の退避パス
            const auto backup =
                backupRoot / PathFromUtf8(package.name);

            // 既存のパッケージがあるか
            const bool hadPrevious =
                std::filesystem::exists(destination);
            if (hadPrevious)
            {
                std::filesystem::create_directories(
                    backupRoot);
                // 前回の退避はここで消えます（1世代だけ）。
                std::filesystem::remove_all(backup);
                // ここが失敗したら何も動かしていないので、そのまま投げて旧版を守ります。
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
                    // 旧版復元の失敗状態
                    std::error_code restoreError;
                    std::filesystem::rename(
                        backup,
                        destination,
                        restoreError);
                    if (restoreError)
                    {
                        // 復元に失敗した旧版は退避先へ残し、場所をエラーで通知します。
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
            // 展開物やZIPの片付け失敗状態
            std::error_code cleanupError;
            std::filesystem::remove_all(
                staging,
                cleanupError);
            std::filesystem::remove(
                std::filesystem::path(zipPath),
                cleanupError);
            throw;
        }

        // 展開物やZIPの片付け失敗状態
        std::error_code cleanupError;
        std::filesystem::remove(
            std::filesystem::path(zipPath),
            cleanupError);
    }

    // 手元のZIPを調べ、取得したバイト列のハッシュで検証して配置します。
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

        // 選択ZIP全体のバイト列
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(
                std::filesystem::file_size(zipPath)));
        {
            // 選択ZIPの入力ストリーム
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


        // インストールまたは出力する情報
        PackageInfo package;
        // 検証と展開用の作業パス
        const auto staging = MakeStagingPath(
            std::filesystem::temp_directory_path(),
            "inspect");
        try
        {
            std::filesystem::create_directories(staging);
            // 内容確認用のZIPパス
            const auto tempZip =
                staging.wstring() + L".zip";
            {
                // ZIPまたはmanifestの出力
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
            // package.jsonのパス
            const auto manifestPath =
                staging / L"package.json";
            if (std::filesystem::is_regular_file(
                manifestPath))
            {
                // manifestの入力ストリーム
                std::ifstream input(
                    manifestPath,
                    std::ios::binary);
                // 読込または生成するmanifest
                nlohmann::json manifest;
                // 読込または生成するmanifest
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
                package.activation = PackageActivationFromName(
                    manifest.value("activation", std::string{}));
                package.target = PackageTargetFromName(
                    manifest.value("target", std::string{}));
                if (manifest.contains("graphicsBackend"))
                {
                    package.target = PackageTarget::Engine;
                }
            }
            // 展開物やZIPの片付け失敗状態
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
            // 展開物やZIPの片付け失敗状態
            std::error_code cleanupError;
            std::filesystem::remove_all(
                staging,
                cleanupError);
            throw;
        }

        if (package.name.empty())
        {
            // 名前とバージョン推測用の元名
            auto stem = PathToUtf8(zipPath.stem());
            // 名前とバージョンの区切り位置
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
            // ファイル名から推測したパッケージ名を小文字へ変換します(character: 変換する文字)。
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

        // 手元のZIPは読み込んだバイト列のハッシュを照合値に使います。
        package.sha256 = Crypto::Sha256Hex(
            bytes.data(),
            bytes.size());
        InstallPackage(assetRoot, package, bytes);
        return package;
    }

    // 作者指定の設定を保持してmanifestを更新し、ZIPを書き出します。
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

        // ZIPへ含めるパッケージのパス
        const auto source =
            PackageInstallDirectory(assetRoot, package.name);
        if (!std::filesystem::is_directory(source))
        {
            throw std::runtime_error(
                "パッケージフォルダーがありません: "
                + PathToUtf8(source));
        }

        // 手で書いたOS別ネイティブ宣言は、作り直しても残します。
        // package.jsonのパス
        const auto manifestPath =
            source / L"package.json";
        // 保持するnative設定
        nlohmann::json nativeSection;
        // Portable出力用の対象OS／ABI別設定
        nlohmann::json nativeVariantsSection;
        // nativeVariantsが明示されているか（不正な値も黙って消さない）
        bool hasNativeVariants = false;
        // 保持する描画バックエンド設定
        nlohmann::json graphicsBackendSection;
        // 作者指定を優先する反映時期
        auto activation = package.activation;
        // 作者指定を優先する追加先
        auto target = package.target;
        if (std::filesystem::is_regular_file(manifestPath))
        {
            // manifestの入力ストリーム
            std::ifstream input(
                manifestPath,
                std::ios::binary);
            // 検証するmanifestの全文
            const std::string manifestText(
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{});
            // 不正なnativeを配布物へ載せないよう、ここで検証します。
            static_cast<void>(
                ParsePackageNativeDependency(
                    manifestText,
                    source,
                    package.name));
            try
            {
                // 既存manifestのJSON
                const auto previous =
                    nlohmann::json::parse(manifestText);
                if (previous.is_object()
                    && previous.contains("native"))
                {
                    nativeSection = previous.at("native");
                }
                if (previous.is_object()
                    && previous.contains("nativeVariants"))
                {
                    nativeVariantsSection = previous.at("nativeVariants");
                    hasNativeVariants = true;
                }
                if (previous.is_object()
                    && previous.contains("activation"))
                {
                    // 作成ダイアログに無い作者指定を保持します。
                    activation = PackageActivationFromName(
                        previous.value("activation", std::string{}));
                }
                if (previous.is_object()
                    && previous.contains("graphicsBackend"))
                {
                    graphicsBackendSection =
                        previous.at("graphicsBackend");
                    target = PackageTarget::Engine;
                }
                else if (previous.is_object()
                    && previous.contains("target"))
                {
                    target = PackageTargetFromName(
                        previous.value("target", std::string{}));
                }
            }
            catch (const std::exception&)
            {
                // native検証後の付随設定の読込失敗時は、取得できた設定で再生成します。
            }
        }

        // フォルダー内のpackage.jsonを最新の内容で作り直します。
        // 読込または生成するmanifest
        nlohmann::json manifest{
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
            },
            {
                "activation",
                PackageActivationName(activation)
            },
            {
                "target",
                PackageTargetName(target)
            }
        };
        if (!nativeSection.is_null())
        {
            manifest["native"] = std::move(nativeSection);
        }
        if (hasNativeVariants)
        {
            manifest["nativeVariants"] = std::move(nativeVariantsSection);
        }
        if (!graphicsBackendSection.is_null())
        {
            manifest["graphicsBackend"] =
                std::move(graphicsBackendSection);
        }
        {
            // manifestの出力ストリーム
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
        // 展開用または出力先のZIPパス
        const auto zipPath = outputDirectory
            / PathFromUtf8(
                package.name + "-" + package.version
                + ".zip");
        CreateZipFromDirectoryContents(source, zipPath);

        // 書き出したパッケージの情報
        PackageBuildResult result;
        result.zipPath = zipPath;
        result.manifestPath = manifestPath;
        result.sizeBytes = std::filesystem::file_size(
            zipPath);
        // 配布一覧か出力元のファイル
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(
                source))
        {
            if (entry.is_regular_file())
            {
                ++result.fileCount;
            }
        }


        // 一覧用の項目JSON
        nlohmann::json indexEntry = manifest;
        // ネイティブ依存と描画バックエンドの正本はZIP内のpackage.jsonとし、一覧から除外します。
        indexEntry.erase("native");
        indexEntry.erase("nativeVariants");
        indexEntry.erase("graphicsBackend");
        indexEntry["downloadUrl"] =
            "https://example.com/packages/"
            + PathToUtf8(zipPath.filename());
        indexEntry["sizeBytes"] = result.sizeBytes;
        {
            // ハッシュ照合用のZIP入力
            std::ifstream zipInput(zipPath, std::ios::binary);
            // ハッシュ照合用のZIPバイト列
            const std::vector<std::uint8_t> zipBytes(
                std::istreambuf_iterator<char>{ zipInput },
                std::istreambuf_iterator<char>{});
            if (zipBytes.size() != result.sizeBytes)
            {
                throw std::runtime_error(
                    "パッケージのZipを読み込めませんでした: "
                    + PathToUtf8(zipPath));
            }
            indexEntry["sha256"] = Crypto::Sha256Hex(
                zipBytes.data(),
                zipBytes.size());
        }
        result.indexEntryJson = indexEntry.dump(2);
        return result;
    }

    // 安全な名前で指定された配置先を削除します。
    void UninstallPackage(
        const std::filesystem::path& assetRoot,
        const std::string_view name)
    {
        if (!IsPackageNameSafe(name))
        {
            throw std::invalid_argument(
                "Package name is not safe to uninstall.");
        }
        // インストール済みパッケージのパス
        const auto destination =
            PackageInstallDirectory(assetRoot, name);
        if (!std::filesystem::exists(destination))
        {
            return;
        }
        std::filesystem::remove_all(destination);
    }
}
