#include "LamaPon/Editor/PackageNativeDependencies.h"

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

    [[nodiscard]] std::string ReadFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // 指定したマニフェストが拒否されることを確かめます。
    [[nodiscard]] bool Rejects(
        const std::string& manifestJson,
        const std::filesystem::path& packageDirectory)
    {
        try
        {
            static_cast<void>(
                LamaPon::ParsePackageNativeDependency(
                    manifestJson,
                    packageDirectory,
                    "my-sdk"));
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
        return false;
    }

    // nativeを持たない既存パッケージは、そのまま何も足しません。
    void TestManifestWithoutNativeIsEmpty(
        const std::filesystem::path& root)
    {
        const auto directory = root / "easing-tween";
        const auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"name":"easing-tween","version":"1.0.0"})",
                directory,
                "easing-tween");
        Require(
            dependency.Empty()
                && dependency.packageName == "easing-tween",
            "a package without a native block must add nothing");
    }

    void TestValidManifestResolvesPaths(
        const std::filesystem::path& root)
    {
        const auto directory = root / "my-sdk";
        const auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"name":"my-sdk","version":"1.0.0","native":{)"
                R"("includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/my_sdk.lib"],)"
                R"("runtimeFiles":["sdk/bin/my_sdk.dll"],)"
                R"("defines":["MY_SDK_ENABLED","MY_SDK_VERSION=2.1"])"
                R"(}})",
                directory,
                "my-sdk");
        Require(
            dependency.includeDirectories.size() == 1
                && dependency.includeDirectories.front()
                    == directory / "sdk" / "include",
            "include directories must resolve inside the package");
        Require(
            dependency.libraries.size() == 1
                && dependency.libraries.front()
                    == directory / "sdk" / "lib" / "my_sdk.lib",
            "libraries must resolve inside the package");
        Require(
            dependency.runtimeFiles.size() == 1
                && dependency.runtimeFiles.front()
                    == directory / "sdk" / "bin" / "my_sdk.dll",
            "runtime files must resolve inside the package");
        Require(
            dependency.defines
                == std::vector<std::string>{
                    "MY_SDK_ENABLED",
                    "MY_SDK_VERSION=2.1" },
            "defines must survive unchanged");
        Require(
            !dependency.Empty(),
            "a package with native entries is not empty");
    }

    // パッケージフォルダーの外を指す指定はすべて拒否します。
    void TestPathEscapesAreRejected(
        const std::filesystem::path& root)
    {
        const auto directory = root / "my-sdk";
        const char* const escapes[]{
            R"({"native":{"libraries":["../other/evil.lib"]}})",
            R"({"native":{"libraries":["sdk/../../evil.lib"]}})",
            R"({"native":{"libraries":["C:/Windows/System32/evil.lib"]}})",
            R"({"native":{"libraries":["/usr/lib/evil.lib"]}})",
            R"({"native":{"libraries":["\\\\server\\share\\evil.lib"]}})",
            R"({"native":{"includeDirectories":[".."]}})",
            R"({"native":{"runtimeFiles":["sdk/*.dll"]}})",
            R"({"native":{"libraries":[""]}})"
        };
        for (const auto* const manifest : escapes)
        {
            Require(
                Rejects(manifest, directory),
                "a path outside the package folder must be"
                " rejected");
        }
    }

    void TestExtensionsAreEnforced(
        const std::filesystem::path& root)
    {
        const auto directory = root / "my-sdk";
        Require(
            Rejects(
                R"({"native":{"libraries":["sdk/my_sdk.dll"]}})",
                directory),
            "libraries must be .lib files");
        Require(
            Rejects(
                R"({"native":{"runtimeFiles":["sdk/my_sdk.lib"]}})",
                directory),
            "runtime files must be .dll files");
        // 大文字の拡張子は同じものとして扱います。
        const auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"native":{"libraries":["sdk/My_Sdk.LIB"],)"
                R"("runtimeFiles":["sdk/My_Sdk.DLL"]}})",
                directory,
                "my-sdk");
        Require(
            dependency.libraries.size() == 1
                && dependency.runtimeFiles.size() == 1,
            "extensions must be matched without case");
    }

    // ビルドコマンドを書き換える抜け道を作らないため、未知のキーは
    // 黙って無視せず拒否します。
    void TestUnknownAndMalformedKeysAreRejected(
        const std::filesystem::path& root)
    {
        const auto directory = root / "my-sdk";
        const char* const malformed[]{
            R"({"native":{"compileOptions":["/GL"]}})",
            R"({"native":{"linkOptions":["/NODEFAULTLIB"]}})",
            R"({"native":{"libraries":"sdk/my_sdk.lib"}})",
            R"({"native":{"libraries":[7]}})",
            R"({"native":true})",
            R"({"native":{"defines":["MY SDK"]}})",
            R"({"native":{"defines":["9SDK"]}})",
            R"J({"native":{"defines":["SDK=$(cmd)"]}})J",
            R"({"native":{"defines":[""]}})"
        };
        for (const auto* const manifest : malformed)
        {
            Require(
                Rejects(manifest, directory),
                "an unknown or malformed native entry must be"
                " rejected");
        }

        std::string tooMany = R"({"native":{"libraries":[)";
        for (int index = 0; index < 33; ++index)
        {
            tooMany += index == 0 ? "" : ",";
            tooMany += R"("sdk/lib)"
                + std::to_string(index)
                + R"(.lib")";
        }
        tooMany += "]}}";
        Require(
            Rejects(tooMany, directory),
            "more than 32 entries must be rejected");
    }

    void TestInvalidJsonIsReported(
        const std::filesystem::path& root)
    {
        const auto directory = root / "my-sdk";
        Require(
            Rejects("{ not json", directory),
            "invalid JSON must be rejected");
        Require(
            Rejects("[]", directory),
            "a non-object manifest must be rejected");
    }

    // 1件壊れていても、残りのパッケージは使えるようにします。
    void TestScanCollectsAndReports(
        const std::filesystem::path& root)
    {
        const auto assetRoot = root / "scan" / "assets";
        WriteFile(
            assetRoot / "packages" / "easing-tween"
                / "package.json",
            R"({"name":"easing-tween","version":"1.0.0"})");
        WriteFile(
            assetRoot / "packages" / "audio-sdk"
                / "package.json",
            R"({"name":"audio-sdk","native":{)"
            R"("libraries":["lib/audio.lib"]}})");
        WriteFile(
            assetRoot / "packages" / "my-sdk" / "package.json",
            R"({"name":"my-sdk","native":{)"
            R"("runtimeFiles":["bin/my_sdk.dll"]}})");
        WriteFile(
            assetRoot / "packages" / "broken-sdk"
                / "package.json",
            R"({"name":"broken-sdk","native":{)"
            R"("libraries":["../escape.lib"]}})");
        // 名前が安全でないフォルダーは見ません。
        WriteFile(
            assetRoot / "packages" / "Bad Name"
                / "package.json",
            R"({"name":"Bad Name","native":{)"
            R"("libraries":["lib/bad.lib"]}})");

        const auto scan =
            LamaPon::ScanPackageNativeDependencies(assetRoot);
        Require(
            scan.packages.size() == 2,
            "only packages that declare native entries are"
            " collected");
        Require(
            scan.packages[0].packageName == "audio-sdk"
                && scan.packages[1].packageName == "my-sdk",
            "packages must be collected in name order");
        Require(
            scan.errors.size() == 1
                && scan.errors.front().find("broken-sdk")
                    != std::string::npos,
            "a broken manifest must be reported without"
            " stopping the others");

        Require(
            LamaPon::ScanPackageNativeDependencies(
                root / "scan" / "missing")
                .packages.empty(),
            "a project without packages must scan cleanly");
    }

    // SDKの配置忘れを、リンカーのエラーより先に説明します。
    void TestMissingFilesAreExplained(
        const std::filesystem::path& root)
    {
        const auto directory = root / "place" / "my-sdk";
        auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"native":{"includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/my_sdk.lib"],)"
                R"("runtimeFiles":["sdk/bin/my_sdk.dll"]}})",
                directory,
                "my-sdk");

        std::string reported;
        try
        {
            LamaPon::RequirePackageNativeFiles({ dependency });
        }
        catch (const std::runtime_error& error)
        {
            reported = error.what();
        }
        Require(
            reported.find("my-sdk") != std::string::npos
                && reported.find("my_sdk.lib") != std::string::npos
                && reported.find("my_sdk.dll") != std::string::npos
                && reported.find("include") != std::string::npos,
            "every missing native file must be named");

        std::filesystem::create_directories(
            directory / "sdk" / "include");
        WriteFile(directory / "sdk" / "lib" / "my_sdk.lib", "lib");
        WriteFile(directory / "sdk" / "bin" / "my_sdk.dll", "dll");
        LamaPon::RequirePackageNativeFiles({ dependency });
    }

    // SDK本体が未配置のパッケージは、ビルドを止めずにnative設定ごと外します。
    void TestMissingPackagesAreSkipped(
        const std::filesystem::path& root)
    {
        const auto parse = [&root](const char* const name)
        {
            return LamaPon::ParsePackageNativeDependency(
                R"({"native":{"includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/vendor.lib"],)"
                R"("runtimeFiles":["sdk/bin/vendor.dll"],)"
                R"("defines":["VENDOR_SDK"]}})",
                root / "skip" / name,
                name);
        };
        const auto placedDirectory = root / "skip" / "placed-sdk";
        std::filesystem::create_directories(
            placedDirectory / "sdk" / "include");
        WriteFile(placedDirectory / "sdk" / "lib" / "vendor.lib", "lib");
        WriteFile(placedDirectory / "sdk" / "bin" / "vendor.dll", "dll");

        const auto selection =
            LamaPon::SelectAvailablePackageNativeDependencies(
                { parse("missing-sdk"), parse("placed-sdk") });
        Require(
            selection.available.size() == 1u
                && selection.available.front().packageName == "placed-sdk"
                && selection.available.front().defines.size() == 1u,
            "a package with every native file must stay available");
        Require(
            selection.missing.size() == 1u
                && selection.missing.front().find("missing-sdk")
                    != std::string::npos
                && selection.missing.front().find("vendor.lib")
                    != std::string::npos
                && selection.missing.front().find("vendor.dll")
                    != std::string::npos,
            "a package without its SDK must be skipped with every missing "
            "file named");

        // 外したパッケージのdefinesはCMakeへ渡しません。
        const auto cmakePath = root / "skip" / "package-native.cmake";
        LamaPon::WritePackageNativeCMakeFile(
            cmakePath,
            selection.available);
        std::ifstream input(cmakePath, std::ios::binary);
        const std::string cmake{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        Require(
            cmake.find("placed-sdk") != std::string::npos
                && cmake.find("missing-sdk") == std::string::npos,
            "only packages with their SDK may reach the Game Module build");
    }

    // エンジン自身のDLLを、パッケージに差し替えさせません。
    void TestRuntimeFileCollisionsAreRejected(
        const std::filesystem::path& root)
    {
        const auto make =
            [&root](
                const char* const name,
                const std::string& file)
        {
            return LamaPon::ParsePackageNativeDependency(
                R"({"native":{"runtimeFiles":[")" + file
                    + R"("]}})",
                root / name,
                name);
        };

        for (const auto* const reserved : {
            "bin/LamaPonRuntime.dll",
            "bin/lamaponruntime.dll",
            "bin/LamaPonGameModule.dll",
            "bin/msvcp140.dll" })
        {
            bool rejected = false;
            try
            {
                static_cast<void>(
                    LamaPon::CollectPackageRuntimeFiles(
                        { make("my-sdk", reserved) }));
            }
            catch (const std::runtime_error&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "a package must not ship a DLL named like the"
                " engine's own");
        }

        bool duplicateRejected = false;
        try
        {
            static_cast<void>(
                LamaPon::CollectPackageRuntimeFiles({
                    make("my-sdk", "bin/shared.dll"),
                    make("audio-sdk", "lib/Shared.dll") }));
        }
        catch (const std::runtime_error&)
        {
            duplicateRejected = true;
        }
        Require(
            duplicateRejected,
            "two packages must not ship the same DLL name");

        const auto files = LamaPon::CollectPackageRuntimeFiles({
            make("my-sdk", "bin/my_sdk.dll"),
            make("audio-sdk", "lib/audio.dll") });
        Require(
            files.size() == 2
                && files[0].fileName == L"my_sdk.dll"
                && files[0].packageName == "my-sdk"
                && files[1].fileName == L"audio.dll",
            "runtime files must keep their package and name");
    }

    void TestCMakeFileGeneration(
        const std::filesystem::path& root)
    {
        const auto directory = root / "cmake" / "my-sdk";
        const auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"native":{"includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/my_sdk.lib"],)"
                R"("defines":["MY_SDK_ENABLED"]}})",
                directory,
                "my-sdk");
        const auto outputPath =
            root / "cmake" / "out" / "package-native.cmake";

        LamaPon::WritePackageNativeCMakeFile(
            outputPath,
            { dependency });
        const auto text = ReadFile(outputPath);
        Require(
            text.find("LAMAPON_PACKAGE_INCLUDE_DIRECTORIES")
                != std::string::npos
            && text.find("sdk/include") != std::string::npos
            && text.find("my_sdk.lib") != std::string::npos
            && text.find("\"MY_SDK_ENABLED\"")
                != std::string::npos,
            "the generated CMake file must carry every entry");
        Require(
            text.find("# my-sdk") != std::string::npos,
            "the generated CMake file must say where each entry"
            " came from");

        // 内容が同じ再生成では書き込まず、CMakeのconfigureを
        // 無駄にやり直させません。
        const auto firstWrite =
            std::filesystem::last_write_time(outputPath);
        LamaPon::WritePackageNativeCMakeFile(
            outputPath,
            { dependency });
        Require(
            std::filesystem::last_write_time(outputPath)
                == firstWrite,
            "an unchanged regeneration must not touch the file");

        // ネイティブ依存が無くなったら、空の定義で上書きします。
        LamaPon::WritePackageNativeCMakeFile(outputPath, {});
        const auto cleared = ReadFile(outputPath);
        Require(
            cleared.find("my_sdk.lib") == std::string::npos
                && cleared.find(
                    "set(LAMAPON_PACKAGE_LIBRARIES)")
                    != std::string::npos,
            "removing a package must clear the generated file");
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "package-native";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);

        TestManifestWithoutNativeIsEmpty(root);
        TestValidManifestResolvesPaths(root);
        TestPathEscapesAreRejected(root);
        TestExtensionsAreEnforced(root);
        TestUnknownAndMalformedKeysAreRejected(root);
        TestInvalidJsonIsReported(root);
        TestScanCollectsAndReports(root);
        TestMissingFilesAreExplained(root);
        TestMissingPackagesAreSkipped(root);
        TestRuntimeFileCollisionsAreRejected(root);
        TestCMakeFileGeneration(root);
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Package native dependency tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "Package native dependency tests passed.\n";
    return 0;
}
