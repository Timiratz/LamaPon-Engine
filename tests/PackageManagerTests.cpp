#include "LamaPon/Editor/PackageManager.h"

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
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(
            path.parent_path());
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not create a test file.");
        }
        output << contents;
    }

    std::string ReadFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "test file must be readable");
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // Windows標準のtarでフォルダーをZipへ固めてバイト列を返します。
    std::vector<std::uint8_t> ZipDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& zipPath)
    {
        const std::wstring command =
            L"tar -a -cf \"" + zipPath.wstring()
            + L"\" -C \"" + directory.wstring() + L"\" .";
        Require(
            _wsystem(command.c_str()) == 0,
            "tar must create the test archive.");
        std::ifstream input(zipPath, std::ios::binary);
        Require(
            static_cast<bool>(input),
            "test archive must be readable");
        return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    void TestParsing()
    {
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
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/camera-follow-1.0.zip",
                        "sizeBytes": 2048
                    },
                    {
                        "name": "BAD NAME!",
                        "version": "1.0",
                        "downloadUrl": "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/main/packages/x.zip"
                    },
                    {
                        "name": "evil",
                        "version": "1.0",
                        "downloadUrl": "https://evil.example/x.zip"
                    }
                ]
            })");
        Require(
            packages.size() == 1,
            "invalid entries must be filtered out");
        Require(
            packages[0].name == "camera-follow"
                && packages[0].displayName == "カメラ追従"
                && packages[0].version == "1.0"
                && packages[0].minimumEngineVersion
                    == "2026.7.31"
                && packages[0].sizeBytes == 2048,
            "package fields must round-trip");

        bool rejected = false;
        try
        {
            static_cast<void>(
                LamaPon::ParsePackageIndex(
                    R"({"format":"Unknown","version":9})"));
        }
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

    void TestValidation()
    {
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

        std::wstring host;
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

    void TestInstallRoundTrip()
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "package-manager";
        std::filesystem::remove_all(root);
        const auto assetRoot = root / "assets";
        std::filesystem::create_directories(assetRoot);

        // パッケージの中身（スクリプトとデータ）を作ってZip化。
        const auto source = root / "source";
        WriteFile(
            source / "FollowCamera.cpp",
            "// script");
        WriteFile(
            source / "data" / "readme.txt",
            "hello package");
        const auto zipBytes = ZipDirectory(
            source,
            root / "package.zip");

        LamaPon::PackageInfo package;
        package.name = "camera-follow";
        package.displayName = "カメラ追従";
        package.version = "1.0";
        package.minimumEngineVersion = "2026.7.31";
        package.downloadUrl =
            "https://raw.githubusercontent.com/Timiratz/"
            "LamaPon-Engine/main/packages/a.zip";

        LamaPon::InstallPackage(
            assetRoot,
            package,
            zipBytes);
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

        // 更新: 新しい版で置き換え、古いファイルが残らないこと。
        std::filesystem::remove(
            source / "data" / "readme.txt");
        WriteFile(source / "NewFile.cpp", "// v2");
        auto manifest = std::string(
            R"({"name":"camera-follow","version":"1.1"})");
        WriteFile(source / "package.json", manifest);
        const auto zipBytes2 = ZipDirectory(
            source,
            root / "package2.zip");
        package.version = "1.1";
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

        // 更新の前に手を入れていたファイルは、退避（1世代）に
        // 残ること。更新はフォルダーごと置き換える仕様なので、
        // これが改造の唯一の逃げ道になります。
        // 退避はassets/の外にあること（ゲームモジュールが
        // assets/*.cppを丸ごとコンパイルするため、assets/内へ
        // 旧版のC++が残ると二重定義でビルドが壊れます）。
        const auto backup = root
            / ".lamapon"
            / "package-backups"
            / "camera-follow";
        WriteFile(
            installed / "NewFile.cpp",
            "// edited by user");
        auto manifest3 = std::string(
            R"({"name":"camera-follow","version":"1.2"})");
        WriteFile(source / "package.json", manifest3);
        const auto zipBytes3 = ZipDirectory(
            source,
            root / "package3.zip");
        package.version = "1.2";
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

        // 置き換えに失敗しても旧版が無傷なこと。旧版の中の
        // ファイルを開いたまま更新すると、フォルダーのrenameが
        // 失敗します（Windowsは開いているファイルを含む
        // フォルダーを動かせません）。以前の「消してから入れる」
        // 実装では、この状況で旧版が半分消えたまま残りました。
        {
            std::ifstream lock(
                installed / "NewFile.cpp",
                std::ios::binary);
            Require(
                static_cast<bool>(lock),
                "the lock file must open");
            bool failed = false;
            try
            {
                LamaPon::InstallPackage(
                    assetRoot,
                    package,
                    zipBytes3);
            }
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

        bool unsafeRejected = false;
        try
        {
            LamaPon::InstallPackage(
                assetRoot,
                LamaPon::PackageInfo{ "../escape" },
                zipBytes);
        }
        catch (const std::exception&)
        {
            unsafeRejected = true;
        }
        Require(
            unsafeRejected,
            "unsafe package names must be rejected");
    }
}

int main()
{
    try
    {
        TestParsing();
        TestValidation();
        TestInstallRoundTrip();
        std::cout << "Package manager tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
