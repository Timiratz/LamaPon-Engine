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
    [[nodiscard]] std::string ReadFile(
        const std::filesystem::path& path)
    {
        // 読み込むテストファイル
        std::ifstream input(path, std::ios::binary);
        // ファイルが開けない場合は空文字列を返す。
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // Rejects(manifestJson: 検査するJSON, packageDirectory: パッケージ位置): 不正な依存定義が拒否されるか調べる。
    [[nodiscard]] bool Rejects(
        const std::string& manifestJson,
        const std::filesystem::path& packageDirectory)
    {
        // 依存定義の解析を試す。
        try
        {
            static_cast<void>(
                LamaPon::ParsePackageNativeDependency(
                    manifestJson,
                    packageDirectory,
                    "my-sdk"));
        }
        // 形式不正の拒否を成功として扱う。
        catch (const std::invalid_argument&)
        {
            return true;
        }
        // その他の失敗は拒否確認として扱わない。
        catch (const std::exception&)
        {
            return false;
        }
        return false;
    }

    // TestManifestWithoutNativeIsEmpty(root: テストルート): native指定のないパッケージが空になることを確認する。
    void TestManifestWithoutNativeIsEmpty(
        const std::filesystem::path& root)
    {
        // native指定のないパッケージ位置
        const auto directory = root / "easing-tween";
        // パースした空の依存定義
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

    // TestValidManifestResolvesPaths(root: テストルート): 宣言された相対パスがパッケージ内へ解決されることを確認する。
    void TestValidManifestResolvesPaths(
        const std::filesystem::path& root)
    {
        // 検証するパッケージ位置
        const auto directory = root / "my-sdk";
        // パースしたネイティブ依存定義
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

    // TestPathEscapesAreRejected(root: テストルート): パッケージ外を指す指定が拒否されることを確認する。
    void TestPathEscapesAreRejected(
        const std::filesystem::path& root)
    {
        // 検証するパッケージ位置
        const auto directory = root / "my-sdk";
        // 拒否すべきパス指定
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
        // 各不正マニフェストを拒否する。
        for (const auto* const manifest : escapes)
        {
            Require(
                Rejects(manifest, directory),
                "a path outside the package folder must be"
                " rejected");
        }
    }

    // TestExtensionsAreEnforced(root: テストルート): ライブラリと実行ファイルの拡張子を検証する。
    void TestExtensionsAreEnforced(
        const std::filesystem::path& root)
    {
        // 検証するパッケージ位置
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
        // 拡張子の大文字小文字を区別せず解析する。
        // 大文字拡張子を含む有効な依存定義
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

    // TestUnknownAndMalformedKeysAreRejected(root: テストルート): 未知キーや不正値を拒否することを確認する。
    void TestUnknownAndMalformedKeysAreRejected(
        const std::filesystem::path& root)
    {
        // 検証するパッケージ位置
        const auto directory = root / "my-sdk";
        // 拒否すべきキーや値を含むJSON
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
        // 各不正マニフェストを拒否する。
        for (const auto* const manifest : malformed)
        {
            Require(
                Rejects(manifest, directory),
                "an unknown or malformed native entry must be"
                " rejected");
        }

        // 上限超過用のライブラリ配列JSON
        std::string tooMany = R"({"native":{"libraries":[)";
        // 上限を超える数のライブラリを追加する。
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

    // TestInvalidJsonIsReported(root: テストルート): 不正JSONと非オブジェクトを拒否することを確認する。
    void TestInvalidJsonIsReported(
        const std::filesystem::path& root)
    {
        // 検証するパッケージ位置
        const auto directory = root / "my-sdk";
        Require(
            Rejects("{ not json", directory),
            "invalid JSON must be rejected");
        Require(
            Rejects("[]", directory),
            "a non-object manifest must be rejected");
    }

    // TestScanCollectsAndReports(root: テストルート): 不正な1件を報告し、有効な依存を収集する。
    void TestScanCollectsAndReports(
        const std::filesystem::path& root)
    {
        // スキャン対象のアセットルート
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
        // 安全でない名前のパッケージをスキャン対象へ置く。
        WriteFile(
            assetRoot / "packages" / "Bad Name"
                / "package.json",
            R"({"name":"Bad Name","native":{)"
            R"("libraries":["lib/bad.lib"]}})");

        // 収集結果とエラー一覧
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

    // TestMissingFilesAreExplained(root: テストルート): SDK不足時に必要なパスを示すことを確認する。
    void TestMissingFilesAreExplained(
        const std::filesystem::path& root)
    {
        // 配置検査対象のパッケージ位置
        const auto directory = root / "place" / "my-sdk";
        // 必要なSDKパスを宣言する依存定義
        auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"native":{"includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/my_sdk.lib"],)"
                R"("runtimeFiles":["sdk/bin/my_sdk.dll"]}})",
                directory,
                "my-sdk");

        // 依存ファイル不足の診断
        std::string reported;
        // 不足ファイルの診断を取得する。
        try
        {
            LamaPon::RequirePackageNativeFiles({ dependency });
        }
        // error: SDK不足を示す診断例外
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

    // TestMissingPackagesAreSkipped(root: テストルート): SDK不足パッケージだけを選択結果から外す。
    void TestMissingPackagesAreSkipped(
        const std::filesystem::path& root)
    {
        // name: パッケージ名を受けて依存定義を作る。
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
        // 必須SDKファイルを配置するパッケージ位置
        const auto placedDirectory = root / "skip" / "placed-sdk";
        std::filesystem::create_directories(
            placedDirectory / "sdk" / "include");
        WriteFile(placedDirectory / "sdk" / "lib" / "vendor.lib", "lib");
        WriteFile(placedDirectory / "sdk" / "bin" / "vendor.dll", "dll");

        // 利用可能・不足依存の選択結果
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

        // 選択済みパッケージだけをCMakeへ書き出す。
        // 選択結果を出力するCMakeファイル
        const auto cmakePath = root / "skip" / "package-native.cmake";
        LamaPon::WritePackageNativeCMakeFile(
            cmakePath,
            selection.available);
        // 生成したCMake定義
        std::ifstream input(cmakePath, std::ios::binary);
        // 出力ファイルの全内容
        const std::string cmake{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        Require(
            cmake.find("placed-sdk") != std::string::npos
                && cmake.find("missing-sdk") == std::string::npos,
            "only packages with their SDK may reach the Game Module build");
    }

    // TestRuntimeFileCollisionsAreRejected(root: テストルート): エンジンDLL名や重複名を拒否する。
    void TestRuntimeFileCollisionsAreRejected(
        const std::filesystem::path& root)
    {
        // make(name: パッケージ名, file: DLL相対パス): 実行時ファイル依存を作る。
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

        // エンジン予約名やランタイム共通DLL名を確認する。
        for (const auto* const reserved : {
            "bin/LamaPonRuntime.dll",
            "bin/lamaponruntime.dll",
            "bin/LamaPonGameModule.dll",
            "bin/msvcp140.dll" })
        {
            // 予約名の拒否結果
            bool rejected = false;
            // 予約名の実行時ファイルを検査する。
            try
            {
                static_cast<void>(
                    LamaPon::CollectPackageRuntimeFiles(
                        { make("my-sdk", reserved) }));
            }
            // 予約名拒否を記録する。
            catch (const std::runtime_error&)
            {
                rejected = true;
            }
            Require(
                rejected,
                "a package must not ship a DLL named like the"
                " engine's own");
        }

        // 大文字小文字を無視した重複名の拒否結果
        bool duplicateRejected = false;
        // パッケージ間のDLL名重複を検査する。
        try
        {
            static_cast<void>(
                LamaPon::CollectPackageRuntimeFiles({
                    make("my-sdk", "bin/shared.dll"),
                    make("audio-sdk", "lib/Shared.dll") }));
        }
        // 重複名拒否を記録する。
        catch (const std::runtime_error&)
        {
            duplicateRejected = true;
        }
        Require(
            duplicateRejected,
            "two packages must not ship the same DLL name");

        // 受理された実行時ファイル一覧
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

    // TestCMakeFileGeneration(root: テストルート): ネイティブ依存のCMake定義生成を検証する。
    void TestCMakeFileGeneration(
        const std::filesystem::path& root)
    {
        // 生成対象パッケージ位置
        const auto directory = root / "cmake" / "my-sdk";
        // CMakeへ出力する依存定義
        const auto dependency =
            LamaPon::ParsePackageNativeDependency(
                R"({"native":{"includeDirectories":["sdk/include"],)"
                R"("libraries":["sdk/lib/my_sdk.lib"],)"
                R"("defines":["MY_SDK_ENABLED"]}})",
                directory,
                "my-sdk");
        // 生成先CMakeファイル
        const auto outputPath =
            root / "cmake" / "out" / "package-native.cmake";

        LamaPon::WritePackageNativeCMakeFile(
            outputPath,
            { dependency });
        // 最初に生成したCMake内容
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

        // 同じ内容の再生成では更新時刻を維持する。
        // 初回生成後の更新時刻
        const auto firstWrite =
            std::filesystem::last_write_time(outputPath);
        LamaPon::WritePackageNativeCMakeFile(
            outputPath,
            { dependency });
        Require(
            std::filesystem::last_write_time(outputPath)
                == firstWrite,
            "an unchanged regeneration must not touch the file");

        // 依存削除後は空の定義で上書きする。
        LamaPon::WritePackageNativeCMakeFile(outputPath, {});
        // 依存削除後のCMake内容
        const auto cleared = ReadFile(outputPath);
        Require(
            cleared.find("my_sdk.lib") == std::string::npos
                && cleared.find(
                    "set(LAMAPON_PACKAGE_LIBRARIES)")
                    != std::string::npos,
            "removing a package must clear the generated file");
    }
}

// main(): ネイティブ依存の解析、選択、出力テストを実行する。
int main()
{
    // テスト失敗を終了コードへ変換する。
    try
    {
        // テスト用ファイルの保存先
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
    // 例外(error: テスト失敗情報)を標準エラーへ出力する。
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
