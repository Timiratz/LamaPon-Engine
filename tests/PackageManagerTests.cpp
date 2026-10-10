#include "LamaPon/Core/Crypto.h"
#include "LamaPon/Editor/PackageManager.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"
#include "LamaPon/Graphics/GraphicsBackendPackage.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // WriteFile(path: 出力先, contents: 書込内容): 親ディレクトリを作ってバイナリ保存する。
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        // 作成するテストファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        // ファイル作成の失敗を検出する。
        if (!output)
        {
            throw std::runtime_error(
                "Could not create a test file.");
        }
        output << contents;
    }

    // ReadFile(path: 読込元): ファイル全体を文字列として読む。
    std::string ReadFile(
        const std::filesystem::path& path)
    {
        // 読み込むテストファイル
        std::ifstream input(path, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "test file must be readable");
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // ZipDirectory(directory: 圧縮元, zipPath: アーカイブ先): tarでフォルダーをZip化して読む。
    std::vector<std::uint8_t> ZipDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& zipPath)
    {
        // tarへ渡すアーカイブ作成コマンド
        const std::wstring command =
            L"tar -a -cf \"" + zipPath.wstring()
            + L"\" -C \"" + directory.wstring() + L"\" .";
        Require(
            _wsystem(command.c_str()) == 0,
            "tar must create the test archive.");
        // 生成したZipアーカイブ
        std::ifstream input(zipPath, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "test archive must be readable");
        return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // Sha256Of(bytes: ハッシュ対象): バイト列のSHA-256を16進文字列にする。
    std::string Sha256Of(const std::vector<std::uint8_t>& bytes)
    {
        return LamaPon::Crypto::Sha256Hex(bytes.data(), bytes.size());
    }

    // TestParsing(): パッケージインデックスの解析と互換性を検証する。
    void TestParsing()
    {
        // 有効な項目だけを含む解析結果
        const auto packages = LamaPon::ParsePackageIndex(
            R"({
                "format": "LamaPonPackageIndex",
                "version": 1,
                "packages": [
                    {
                        "name": "camera-follow",
                        "displayName": "カメラ追従",
                        "description": "追従カメラ",
                        "version": "1.0",
                        "minimumEngineVersion": "2026.7.31",
                        "activation": "Restart",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/camera-follow-1.0.zip",
                        "sizeBytes": 2048,
                        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
                    },
                    {
                        "name": "renderer",
                        "version": "1.0",
                        "target": "Engine",
                        "activation": "Restart",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/renderer-1.0.zip",
                        "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
                    },
                    {
                        "name": "no-hash",
                        "version": "1.0",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/no-hash-1.0.zip"
                    },
                    {
                        "name": "upper-hash",
                        "version": "1.0",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/upper-hash-1.0.zip",
                        "sha256": "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"
                    },
                    {
                        "name": "short-hash",
                        "version": "1.0",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/short-hash-1.0.zip",
                        "sha256": "e3b0c442"
                    },
                    {
                        "name": "BAD NAME!",
                        "version": "1.0",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/x.zip",
                        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
                    },
                    {
                        "name": "evil",
                        "version": "1.0",
                        "downloadUrl": "https://evil.example/x.zip",
                        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
                    }
                ]
            })");
        Require(
            packages.size() == 2,
            "invalid entries, including ones without a canonical"
            " sha256, must be filtered out");
        Require(
            packages[0].name == "camera-follow"
                && packages[0].displayName == "カメラ追従"
                && packages[0].version == "1.0"
                && packages[0].minimumEngineVersion
                    == "2026.7.31"
                && packages[0].activation
                    == LamaPon::PackageActivation::Restart
                && packages[0].target
                    == LamaPon::PackageTarget::Project
                && packages[0].sizeBytes == 2048
                && packages[0].sha256
                    == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "package fields must round-trip");
        Require(
            packages[1].target == LamaPon::PackageTarget::Engine
                && LamaPon::PackageTargetFromName("unknown")
                    == LamaPon::PackageTarget::Project
                && LamaPon::PackageTargetName(
                    LamaPon::PackageTarget::Engine) == "Engine",
            "package targets must preserve old index compatibility");

        Require(
            LamaPon::PackageActivationFromName("Immediate")
                    == LamaPon::PackageActivation::Immediate
                && LamaPon::PackageActivationFromName("Restart")
                    == LamaPon::PackageActivation::Restart
                && LamaPon::PackageActivationFromName(
                    "RestartAndRebuild")
                    == LamaPon::PackageActivation::RestartAndRebuild
                && LamaPon::PackageActivationFromName("unknown")
                    == LamaPon::PackageActivation::Immediate,
            "package activation names must be backward compatible");

        // 不明な形式の拒否結果
        bool rejected = false;
        // 不明なインデックス形式を検査する。
        try
        {
            static_cast<void>(
                LamaPon::ParsePackageIndex(
                    R"({"format":"Unknown","version":9})"));
        }
        // 形式拒否を記録する。
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(
            rejected,
            "unknown index formats must be rejected");

        Require(
            LamaPon::ParsePackageIndex(
                R"({"format":"LamaPonPackageIndex","version":1,"packages":[]})")
                .empty(),
            "an empty index must parse to an empty list");
    }

    // TestValidation(): パッケージ名、URL、SHA-256の検証規則を確認する。
    void TestValidation()
    {
        Require(
            LamaPon::Crypto::Sha256Hex(nullptr, 0)
                == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
                && LamaPon::IsCanonicalPackageSha256(
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
                && !LamaPon::IsCanonicalPackageSha256(
                    "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855")
                && !LamaPon::IsCanonicalPackageSha256("")
                && !LamaPon::IsCanonicalPackageSha256("e3b0c442"),
            "package SHA-256 format rules");

        Require(
            LamaPon::IsPackageNameSafe("camera-follow_2d")
                && !LamaPon::IsPackageNameSafe("Camera")
                && !LamaPon::IsPackageNameSafe("a b")
                && !LamaPon::IsPackageNameSafe("../up")
                && !LamaPon::IsPackageNameSafe(""),
            "package name safety rules");

        Require(
            LamaPon::IsAllowedPackageUrl(
                "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/a.zip")
                && LamaPon::IsAllowedPackageUrl(
                    "https://github.com/Timiratz/LamaPon-Engine/releases/download/x/a.zip")
                && !LamaPon::IsAllowedPackageUrl(
                    "https://evil.example/a.zip")
                && !LamaPon::IsAllowedPackageUrl(
                    "http://raw.githubusercontent.com/Timiratz/LamaPon-Engine/a.zip"),
            "package URL allowlist");

        // 分割されたHTTPSホスト名
        std::wstring host;
        // 分割されたHTTPSパス
        std::wstring path;
        Require(
            LamaPon::SplitHttpsUrl(
                "https://example.com/a/b.zip",
                host,
                path)
                && host == L"example.com"
                && path == L"/a/b.zip",
            "https URL splitting");
        Require(
            !LamaPon::SplitHttpsUrl(
                "ftp://example.com/a",
                host,
                path)
                && !LamaPon::SplitHttpsUrl(
                    "https://nohostpath",
                    host,
                    path),
            "invalid URLs must be rejected");
    }

    // TestInstallRoundTrip(): パッケージの導入、更新、退避、削除を検証する。
    void TestInstallRoundTrip()
    {
        // テストファイルの保存先
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "package-manager";
        std::filesystem::remove_all(root);
        // インストール対象のアセットルート
        const auto assetRoot = root / "assets";
        std::filesystem::create_directories(assetRoot);

        // 圧縮するパッケージファイル群
        const auto source = root / "source";
        WriteFile(
            source / "FollowCamera.cpp",
            "// script");
        WriteFile(
            source / "data" / "readme.txt",
            "hello package");
        // 圧縮した初回パッケージ
        const auto zipBytes = ZipDirectory(
            source,
            root / "package.zip");

        // 初回インストール対象のパッケージ情報
        LamaPon::PackageInfo package;
        package.name = "camera-follow";
        package.displayName = "カメラ追従";
        package.version = "1.0";
        package.minimumEngineVersion = "2026.7.31";
        package.activation = LamaPon::PackageActivation::Restart;
        package.downloadUrl =
            "https://raw.githubusercontent.com/Timiratz/"
            "LamaPon-Engine/main/packages/a.zip";

        // rejectsWithoutInstalling(sha256: 検査用ハッシュ): 不正ZIPが導入前に拒否されるか調べる。
        const auto rejectsWithoutInstalling =
            [&assetRoot, &package, &zipBytes](const std::string& sha256)
        {
            // 検査用の複製パッケージ情報
            auto candidate = package;
            candidate.sha256 = sha256;
            // インストール拒否結果
            bool rejected = false;
            // ハッシュ不一致のインストールを検査する。
            try
            {
                LamaPon::InstallPackage(
                    assetRoot,
                    candidate,
                    zipBytes);
            }
            // 不正ハッシュの拒否を記録する。
            catch (const std::exception&)
            {
                rejected = true;
            }
            return rejected
                && !std::filesystem::exists(
                    LamaPon::PackageInstallDirectory(
                        assetRoot,
                        package.name));
        };
        // Zipとの不一致を作った検査用ハッシュ
        auto tamperedHash = Sha256Of(zipBytes);
        tamperedHash[0] = tamperedHash[0] == '0' ? '1' : '0';
        Require(
            rejectsWithoutInstalling({})
                && rejectsWithoutInstalling(tamperedHash),
            "a package whose SHA-256 is missing or does not match"
            " must not install");

        package.sha256 = Sha256Of(zipBytes);
        LamaPon::InstallPackage(
            assetRoot,
            package,
            zipBytes);
        // 初回インストール先
        const auto installed =
            LamaPon::PackageInstallDirectory(
                assetRoot,
                package.name);
        Require(
            std::filesystem::is_regular_file(
                installed / "FollowCamera.cpp")
                && std::filesystem::is_regular_file(
                    installed / "data" / "readme.txt"),
            "package files must be installed");
        Require(
            std::filesystem::is_regular_file(
                installed / "package.json"),
            "a manifest must be synthesized when missing");
        Require(
            LamaPon::InstalledPackageVersion(
                assetRoot,
                package.name) == "1.0",
            "installed version must be readable");
        Require(
            LamaPon::InstalledPackageActivation(
                assetRoot,
                package.name) == LamaPon::PackageActivation::Restart,
            "a synthesized manifest must preserve activation");
        Require(
            nlohmann::json::parse(
                ReadFile(installed / "package.json"))
                    .value("target", std::string{}) == "Project",
            "a synthesized manifest must preserve the target");

        // 新版パッケージでファイルを置き換える。
        std::filesystem::remove(
            source / "data" / "readme.txt");
        WriteFile(source / "NewFile.cpp", "// v2");
        // 更新後のパッケージマニフェスト
        auto manifest = std::string(
            R"({"name":"camera-follow","version":"1.1"})");
        WriteFile(source / "package.json", manifest);
        // 2回目に圧縮したパッケージ
        const auto zipBytes2 = ZipDirectory(
            source,
            root / "package2.zip");
        package.version = "1.1";
        package.sha256 = Sha256Of(zipBytes2);
        LamaPon::InstallPackage(
            assetRoot,
            package,
            zipBytes2);
        Require(
            LamaPon::InstalledPackageVersion(
                assetRoot,
                package.name) == "1.1",
            "the zip's own manifest must win");
        Require(
            !std::filesystem::exists(
                installed / "data" / "readme.txt")
                && std::filesystem::is_regular_file(
                    installed / "NewFile.cpp"),
            "updates must fully replace the old install");

        // 手編集ファイルを1世代バックアップし、assets外へ退避する。
        const auto backup = root
            / ".lamapon"
            / "package-backups"
            / "camera-follow";
        WriteFile(
            installed / "NewFile.cpp",
            "// edited by user");
        // 3回目の更新に使うマニフェスト
        auto manifest3 = std::string(
            R"({"name":"camera-follow","version":"1.2"})");
        WriteFile(source / "package.json", manifest3);
        // 3回目に圧縮したパッケージ
        const auto zipBytes3 = ZipDirectory(
            source,
            root / "package3.zip");
        package.version = "1.2";
        package.sha256 = Sha256Of(zipBytes3);
        LamaPon::InstallPackage(
            assetRoot,
            package,
            zipBytes3);
        Require(
            LamaPon::InstalledPackageVersion(
                assetRoot,
                package.name) == "1.2",
            "the third install must land");
        Require(
            std::filesystem::is_regular_file(
                backup / "NewFile.cpp")
                && ReadFile(backup / "NewFile.cpp")
                    == "// edited by user",
            "the replaced version, edits included, must survive in the backup");

        // ロックした導入ファイルを使って置換失敗を作る。
        {
            // 移動を妨げるロック対象
            std::ifstream lock(
                installed / "NewFile.cpp",
                std::ios::binary);
            Require(
                static_cast<bool>(lock),
                "the lock file must open");
            // 置換失敗の検出結果
            bool failed = false;
            // ロック中パッケージの更新を試す。
            try
            {
                LamaPon::InstallPackage(
                    assetRoot,
                    package,
                    zipBytes3);
            }
            // 置換失敗を記録する。
            catch (const std::exception&)
            {
                failed = true;
            }
            Require(
                failed,
                "installing over a locked package must fail");
        }
        Require(
            LamaPon::InstalledPackageVersion(
                assetRoot,
                package.name) == "1.2"
                && std::filesystem::is_regular_file(
                    installed / "NewFile.cpp")
                && std::filesystem::is_regular_file(
                    installed / "package.json"),
            "a failed replacement must leave the old install untouched");

        LamaPon::UninstallPackage(
            assetRoot,
            package.name);
        Require(
            !std::filesystem::exists(installed),
            "uninstall must remove the package");
        Require(
            LamaPon::InstalledPackageVersion(
                assetRoot,
                package.name).empty(),
            "uninstalled packages report no version");

        // 危険な名前の拒否結果
        bool unsafeRejected = false;
        // インストール先のパストラバーサルを検査する。
        try
        {
            LamaPon::InstallPackage(
                assetRoot,
                LamaPon::PackageInfo{ "../escape" },
                zipBytes);
        }
        // 危険な名前の拒否を記録する。
        catch (const std::exception&)
        {
            unsafeRejected = true;
        }
        Require(
            unsafeRejected,
            "unsafe package names must be rejected");
    }

    // ネイティブ依存の宣言は、assets/へ入る前に検証します。
    void TestNativeManifestIsValidatedOnInstall()
    {
        // テストファイルの保存先
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "package-manager-native";
        std::filesystem::remove_all(root);
        // SDK検証用のアセットルート
        const auto assetRoot = root / "assets";
        std::filesystem::create_directories(assetRoot);

        // インストール対象のSDK情報
        LamaPon::PackageInfo package;
        package.name = "my-sdk";
        package.displayName = "My SDK";
        package.version = "1.0";
        package.minimumEngineVersion = "2026.7.31";

        // install(manifest: パッケージJSON, folder: 作業名): ZIPを作ってSDK依存を検証し導入する。
        const auto install =
            [&assetRoot, &package, &root](
                const std::string& manifest,
                const char* const folder)
        {
            // ZIP作成用の一時ソースディレクトリ
            const auto source = root / folder;
            std::filesystem::remove_all(source);
            WriteFile(source / "Adapter.cpp", "// adapter");
            WriteFile(source / "package.json", manifest);
            // 圧縮したSDKパッケージ
            const auto zipBytes = ZipDirectory(
                source,
                root / (std::string{ folder } + ".zip"));
            package.sha256 = Sha256Of(zipBytes);
            LamaPon::InstallPackage(
                assetRoot,
                package,
                zipBytes);
        };

        // パストラバーサル定義の拒否結果
        bool rejected = false;
        // パッケージ外を指すSDK定義を導入する。
        try
        {
            install(
                R"({"name":"my-sdk","version":"1.0","native":{)"
                R"("libraries":["../../escape.lib"]}})",
                "escaping");
        }
        // 不正SDK定義の拒否を記録する。
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(
            rejected,
            "a package that points outside its own folder must"
            " not install");
        Require(
            !std::filesystem::exists(
                LamaPon::PackageInstallDirectory(
                    assetRoot,
                    package.name)),
            "a rejected package must leave nothing behind");

        install(
            R"({"name":"my-sdk","version":"1.0","native":{)"
            R"("includeDirectories":["sdk/include"],)"
            R"("libraries":["sdk/lib/my_sdk.lib"],)"
            R"("runtimeFiles":["sdk/bin/my_sdk.dll"],)"
            R"("defines":["MY_SDK_ENABLED"]},)"
            R"("nativeVariants":{"linux-x86_64":{"sources":["linux/MySdkAdapter.cpp"],)"
            R"("libraries":["linux/lib/libmy_sdk.so"],)"
            R"("licenseFiles":["linux/LICENSE.txt"]}}})",
            "valid");
        // 有効なSDKパッケージの導入先
        const auto installed =
            LamaPon::PackageInstallDirectory(
                assetRoot,
                package.name);
        Require(
            std::filesystem::is_regular_file(
                installed / "package.json"),
            "a valid native package must install");

        // 作者が宣言したnative設定を保って再構築する。
        // 再構築した配布パッケージ
        const auto built = LamaPon::BuildPackage(
            assetRoot,
            package,
            root / "dist");
        {
            // ZIP内容を読む入力ストリーム
            std::ifstream builtInput(built.zipPath, std::ios::binary);
            // 再構築ZIPの全バイト
            const std::vector<std::uint8_t> builtBytes(
                std::istreambuf_iterator<char>{ builtInput },
                std::istreambuf_iterator<char>{});
            Require(
                nlohmann::json::parse(built.indexEntryJson)
                        .value("sha256", std::string{})
                    == Sha256Of(builtBytes),
                "a built package's index entry must carry the"
                " zip's SHA-256");
        }
        // インストール済みパッケージマニフェスト
        const auto manifest = nlohmann::json::parse(
            ReadFile(installed / "package.json"));
        Require(
            manifest.contains("native")
                && manifest.at("native").at("libraries")
                    .at(0).get<std::string>()
                    == "sdk/lib/my_sdk.lib"
                && manifest.at("nativeVariants").at("linux-x86_64")
                    .at("sources").at(0).get<std::string>()
                    == "linux/MySdkAdapter.cpp"
                && manifest.at("version").get<std::string>()
                    == package.version,
            "rebuilding a package must keep its native and target variants");
        Require(
            !nlohmann::json::parse(built.indexEntryJson)
                .contains("nativeVariants"),
            "a package index entry must not duplicate build-specific variants");

        // 導入済みnative依存の走査結果
        const auto scan =
            LamaPon::ScanPackageNativeDependencies(assetRoot);
        Require(
            scan.errors.empty()
                && scan.packages.size() == 1
                && scan.packages.front().defines
                    == std::vector<std::string>{
                        "MY_SDK_ENABLED" }
                && scan.packages.front().runtimeFiles.front()
                    == (installed / "sdk" / "bin" / "my_sdk.dll")
                        .lexically_normal(),
            "an installed native package must be discoverable");
    }

    // TestGraphicsBackendPackageInspection(): 描画バックエンドの検査、再構築、読込を確認する。
    void TestGraphicsBackendPackageInspection()
    {
        // テストファイルの保存先
        const auto root = std::filesystem::current_path()
            / "test-output"
            / "graphics-backend-package";
        std::filesystem::remove_all(root);
        // 描画バックエンド検査用アセットルート
        const auto assetRoot = root / "assets";
        // DirectX 12パッケージの場所
        const auto packageRoot = assetRoot / "packages"
            / LamaPon::DirectX12BackendPackageName;

        // 組み込みDirectX 11の検査結果
        const auto builtIn = LamaPon::InspectGraphicsBackendPackage(
            assetRoot,
            LamaPon::RenderingApi::DirectX11,
            "1.0.0");
        Require(
            builtIn.state
                == LamaPon::GraphicsBackendPackageState::BuiltIn
                && builtIn.IsReady(),
            "DirectX 11 must remain built in");

        Require(
            LamaPon::InspectGraphicsBackendPackage(
                assetRoot,
                LamaPon::RenderingApi::DirectX12Experimental,
                "1.0.0").state
                == LamaPon::GraphicsBackendPackageState::Missing,
            "a missing D3D12 package must be reported safely");

        WriteFile(
            packageRoot / "package.json",
            R"({"name":42})");
        Require(
            LamaPon::InspectGraphicsBackendPackage(
                assetRoot,
                LamaPon::RenderingApi::DirectX12Experimental,
                "1.0.0").state
                == LamaPon::GraphicsBackendPackageState::InvalidManifest,
            "wrong manifest value types must not throw");

        WriteFile(
            packageRoot / "package.json",
            R"({
                "name":"directx12-renderer",
                "version":"1.0.0",
                "minimumEngineVersion":"1.0.0",
                "activation":"Restart",
                "graphicsBackend":{
                    "api":"DirectX12Experimental",
                    "abiVersion":1,
                    "runtimeLibrary":"runtime/LamaPonGraphicsD3D12.dll"
                }
            })");
        Require(
            LamaPon::InspectGraphicsBackendPackage(
                assetRoot,
                LamaPon::RenderingApi::DirectX12Experimental,
                "1.0.0").state
                == LamaPon::GraphicsBackendPackageState::RuntimeMissing,
            "a missing backend DLL must not be treated as ready");

        WriteFile(
            packageRoot / "runtime" / "LamaPonGraphicsD3D12.dll",
            "test fixture");
        // DLL配置後のバックエンド検査結果
        const auto ready = LamaPon::InspectGraphicsBackendPackage(
            assetRoot,
            LamaPon::RenderingApi::DirectX12Experimental,
            "1.0.0");
        Require(
            ready.state == LamaPon::GraphicsBackendPackageState::Ready
                && ready.IsReady()
                && ready.descriptor.abiVersion
                    == LamaPon::GraphicsBackendPackageAbiVersion,
            "a compatible backend package must be ready");

        // 再構築するDirectX 12パッケージ情報
        LamaPon::PackageInfo buildInfo;
        buildInfo.name = LamaPon::DirectX12BackendPackageName;
        buildInfo.displayName = "DirectX 12 Renderer";
        buildInfo.version = "1.0.1";
        // 再構築した描画バックエンドパッケージ
        const auto built = LamaPon::BuildPackage(
            assetRoot,
            buildInfo,
            root / "dist");
        // 再構築後のマニフェスト
        const auto rebuiltManifest = nlohmann::json::parse(
            ReadFile(packageRoot / "package.json"));
        Require(
            rebuiltManifest.value("activation", std::string{})
                    == "Restart"
                && rebuiltManifest.value("target", std::string{})
                    == "Engine"
                && rebuiltManifest.contains("graphicsBackend"),
            "rebuilding must preserve backend activation and metadata");
        Require(
            nlohmann::json::parse(built.indexEntryJson)
                .value("target", std::string{}) == "Engine",
            "a backend index entry must use the engine category");

        WriteFile(
            packageRoot / "package.json",
            R"({
                "name":"directx12-renderer",
                "version":"1.0.0",
                "activation":"Restart",
                "graphicsBackend":{
                    "api":"DirectX12Experimental",
                    "abiVersion":999,
                    "runtimeLibrary":"../escape.dll"
                }
            })");
        Require(
            LamaPon::InspectGraphicsBackendPackage(
                assetRoot,
                LamaPon::RenderingApi::DirectX12Experimental,
                "1.0.0").state
                == LamaPon::GraphicsBackendPackageState::IncompatibleAbi,
            "an incompatible backend ABI must be rejected");

        // ABI配置テスト用アセットルート
        const auto activationRoot = root / "activation" / "assets";
        // ABI配置テスト用パッケージの場所
        const auto activationPackage = activationRoot / "packages"
            / LamaPon::DirectX12BackendPackageName;
        WriteFile(
            activationPackage / "package.json",
            R"({
                "name":"directx12-renderer",
                "version":"1.0.0",
                "minimumEngineVersion":"0.1.0",
                "activation":"Restart",
                "graphicsBackend":{
                    "api":"DirectX12Experimental",
                    "abiVersion":1,
                    "runtimeLibrary":"runtime/LamaPonGraphicsD3D12.dll"
                }
            })");
        std::filesystem::create_directories(
            activationPackage / "runtime");
        std::filesystem::copy_file(
            std::filesystem::path{ LAMAPON_D3D12_PROVIDER_PATH },
            activationPackage / "runtime" / "LamaPonGraphicsD3D12.dll",
            std::filesystem::copy_options::overwrite_existing);
        LamaPon::SetGraphicsBackendPackageAssetRoot(activationRoot);
        // 読み込んだバックエンドの有効化結果
        const auto activated = LamaPon::ActivateGraphicsBackendPackage(
            LamaPon::RenderingApi::DirectX12Experimental,
            "1.0.0");
        Require(
            activated.state
                == LamaPon::GraphicsBackendPackageState::Ready,
            "the packaged D3D12 ABI provider must load successfully");
    }
}

// main(): パッケージ管理とnative依存テストを実行する。
int main()
{
    // テスト失敗を終了コードへ変換する。
    try
    {
        TestParsing();
        TestValidation();
        TestInstallRoundTrip();
        TestNativeManifestIsValidatedOnInstall();
        TestGraphicsBackendPackageInspection();
        std::cout << "Package manager tests passed.\n";
        return 0;
    }
    // 例外(exception: テスト失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
