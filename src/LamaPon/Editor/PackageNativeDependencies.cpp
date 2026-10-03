#include "LamaPon/Editor/PackageNativeDependencies.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // nativeで許可するキー
    constexpr std::array<std::string_view, 4> NativeKeys{
        "includeDirectories",
        "libraries",
        "runtimeFiles",
        "defines"
    };

    // 上書きを禁止するDLL名
    constexpr std::array<std::wstring_view, 9> ReservedRuntimeNames{
        L"lamaponruntime.dll",
        L"lamapongamemodule.dll",
        L"xaudio2_9redist.dll",
        L"vcruntime140.dll",
        L"vcruntime140_1.dll",
        L"msvcp140.dll",
        L"msvcp140_1.dll",
        L"msvcp140_2.dll",
        L"lamaponeditor.dll"
    };


    // PackageManagerの命名規則と一致するか判定します(name: 検証するフォルダー名)。
    [[nodiscard]] bool IsPackageFolderName(
        const std::string_view name) noexcept
    {
        if (name.empty() || name.size() > 64u)
        {
            return false;
        }
        // 各文字がパッケージ名の許可文字か判定します(character: 検証する文字)。
        return std::ranges::all_of(
            name,
            [](const char character) noexcept
            {
                return (character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9')
                    || character == '-'
                    || character == '_';
            });
    }

    // ASCIIの英大文字だけを小文字へ変換します(value: 変換する文字列の所有先)。
    [[nodiscard]] std::wstring ToLowerAscii(std::wstring value)
    {
        // 検証または変換対象の文字
        for (auto& character : value)
        {
            if (character >= L'A' && character <= L'Z')
            {
                character = static_cast<wchar_t>(
                    character - L'A' + L'a');
            }
        }
        return value;
    }

    // 宣言項目を含むエラー表示用の名前を作ります(packageName: パッケージ名, field: nativeの項目名)。
    [[nodiscard]] std::string FieldLabel(
        const std::string_view packageName,
        const std::string_view field)
    {
        return "packages/" + std::string{ packageName }
            + "/package.json の native." + std::string{ field };
    }

    // 相対指定と字句正規化後の配下関係を検証します(packageDirectory: 基準の絶対パス, value: 宣言する相対パス, packageName: エラー表示用の名前, field: エラー表示用の項目名)。
    [[nodiscard]] std::filesystem::path ResolveRelativePath(
        const std::filesystem::path& packageDirectory,
        const std::string& value,
        const std::string_view packageName,
        const std::string_view field)
    {
        // エラー表示用の宣言項目名
        const auto label = FieldLabel(packageName, field);
        if (value.empty()
            || value.size() > LamaPon::PackageNativePathMaxBytes)
        {
            throw std::invalid_argument(
                label + " のパスは1〜256バイトで指定してください。");
        }
        // Windowsのドライブ指定・UNC・ワイルドカードを先に弾きます。
        if (value.find(':') != std::string::npos
            || value.find('*') != std::string::npos
            || value.find('?') != std::string::npos
            || value.front() == '/'
            || value.front() == '\\')
        {
            throw std::invalid_argument(
                label
                + " には、パッケージフォルダーからの相対パスだけを"
                  "指定してください: " + value);
        }
        // 検証または変換対象の文字
        for (const char character : value)
        {
            if (static_cast<unsigned char>(character) < 0x20u)
            {
                throw std::invalid_argument(
                    label + " に制御文字は使えません。");
            }
        }

        // 宣言された相対パス
        const auto relative = LamaPon::PathFromUtf8(value);
        if (relative.is_absolute() || relative.has_root_name())
        {
            throw std::invalid_argument(
                label + " に絶対パスは指定できません: " + value);
        }
        // 相対パスの構成要素
        for (const auto& part : relative)
        {
            if (part == L".." )
            {
                throw std::invalid_argument(
                    label
                    + " にパッケージフォルダーの外を指す \"..\" は"
                      "使えません: " + value);
            }
        }

        // 正規化してからもう一度、パッケージフォルダー配下かを見ます。
        // 字句正規化した依存パス
        const auto resolved =
            (packageDirectory / relative).lexically_normal();
        // 字句正規化した基準パス
        const auto root = packageDirectory.lexically_normal();
        // 基準パスの比較用文字列
        const auto rootText = root.wstring();
        // 依存パスの比較用文字列
        auto resolvedText = resolved.wstring();
        if (resolvedText.size() <= rootText.size()
            || resolvedText.compare(0, rootText.size(), rootText) != 0)
        {
            throw std::invalid_argument(
                label
                + " はパッケージフォルダーの中を指してください: "
                + value);
        }
        return resolved;
    }

    // ASCII大小文字を無視して拡張子を判定します(path: 判定対象のパス, expected: 小文字で指定する拡張子)。
    [[nodiscard]] bool HasExtension(
        const std::filesystem::path& path,
        const std::wstring_view expected)
    {
        return ToLowerAscii(path.extension().wstring())
            == expected;
    }

    // 名前と値の許可文字・長さを検証します(value: NAMEまたはNAME=VALUE)。
    [[nodiscard]] bool IsSafeDefine(
        const std::string_view value) noexcept
    {
        if (value.empty() || value.size() > 128u)
        {
            return false;
        }
        // マクロ名と値の区切り位置
        const auto separator = value.find('=');
        // マクロ名またはパッケージ名
        const auto name = value.substr(0, separator);
        if (name.empty() || name.size() > 64u)
        {
            return false;
        }
        if (!(std::isalpha(
                static_cast<unsigned char>(name.front()))
            || name.front() == '_'))
        {
            return false;
        }
        // 検証または変換対象の文字
        for (const char character : name)
        {
            if (!(std::isalnum(
                    static_cast<unsigned char>(character))
                || character == '_'))
            {
                return false;
            }
        }
        if (separator == std::string_view::npos)
        {
            return true;
        }
        // マクロに代入する値
        const auto assigned = value.substr(separator + 1u);
        if (assigned.size() > 64u)
        {
            return false;
        }
        // マクロ値の全ての文字を検証します(character: 検証する文字)。
        return std::ranges::all_of(
            assigned,
            [](const char character) noexcept
            {
                return std::isalnum(
                        static_cast<unsigned char>(character))
                    || character == '_'
                    || character == '.'
                    || character == '+'
                    || character == '-';
            });
    }

    // 指定された配列の型と件数を検証し、無ければnullptrを返します(native: native設定のJSON, key: 読み込む項目名, packageName: エラー表示用の名前)。
    [[nodiscard]] const nlohmann::json* FindArray(
        const nlohmann::json& native,
        const std::string_view key,
        const std::string_view packageName)
    {
        // 対象配列のJSON検索結果
        const auto found = native.find(key);
        if (found == native.end())
        {
            return nullptr;
        }
        if (!found->is_array())
        {
            throw std::invalid_argument(
                FieldLabel(packageName, key)
                + " はJSON配列で指定してください。");
        }
        if (found->size() > LamaPon::PackageNativeMaxEntries)
        {
            throw std::invalid_argument(
                FieldLabel(packageName, key)
                + " の件数が多すぎます（上限32件）。");
        }
        return &*found;
    }

    // 配列要素が文字列なら取り出し、それ以外はinvalid_argumentを投げます(element: 検証する配列要素, packageName: エラー表示用の名前, key: エラー表示用の項目名)。
    [[nodiscard]] std::string RequireString(
        const nlohmann::json& element,
        const std::string_view packageName,
        const std::string_view key)
    {
        if (!element.is_string())
        {
            throw std::invalid_argument(
                FieldLabel(packageName, key)
                + " の要素は文字列で指定してください。");
        }
        return element.get<std::string>();
    }

    // ファイル全体を読み、開けなければ空を返します(path: 読込対象のパス)。
    [[nodiscard]] std::string ReadFileText(
        const std::filesystem::path& path)
    {
        // manifestまたは既存設定の入力
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        return std::string(
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{});
    }

    // 引用符・バックスラッシュ・ドル記号をエスケープして引用します(value: CMakeへ渡す文字列)。
    [[nodiscard]] std::string QuoteForCMake(
        const std::string& value)
    {
        // CMake用の引用済み文字列
        std::string quoted;
        quoted.reserve(value.size() + 2u);
        quoted.push_back('"');
        // 検証または変換対象の文字
        for (const char character : value)
        {
            if (character == '"' || character == '\\'
                || character == '$')
            {
                quoted.push_back('\\');
            }
            quoted.push_back(character);
        }
        quoted.push_back('"');
        return quoted;
    }

    // 存在しない宣言ファイルとディレクトリの説明を集めます(package: 実在を調べる依存)。
    [[nodiscard]] std::string DescribeMissingNativeFiles(
        const LamaPon::PackageNativeDependency& package)
    {
        // 不足する依存ファイルの説明
        std::string missing;
        // 不足ファイルの説明を追加します(path: 不足するファイルのパス)。
        const auto report =
            [&missing, &package](
                const std::filesystem::path& path)
        {
            missing += "\n  - " + package.packageName
                + ": " + LamaPon::PathToUtf8(path);
        };
        // 宣言された依存ファイルのパス
        for (const auto& path : package.includeDirectories)
        {
            if (!std::filesystem::is_directory(path))
            {
                report(path);
            }
        }
        // 宣言された依存ファイルのパス
        for (const auto& path : package.libraries)
        {
            if (!std::filesystem::is_regular_file(path))
            {
                report(path);
            }
        }
        // 宣言された依存ファイルのパス
        for (const auto& path : package.runtimeFiles)
        {
            if (!std::filesystem::is_regular_file(path))
            {
                report(path);
            }
        }
        return missing;
    }
}

namespace LamaPon
{
    // nativeの型・件数・パス・拡張子・マクロを検証します。
    PackageNativeDependency ParsePackageNativeDependency(
        const std::string_view manifestJson,
        const std::filesystem::path& packageDirectory,
        const std::string_view packageName)
    {
        // 検証済みのネイティブ依存
        PackageNativeDependency dependency;
        dependency.packageName = packageName;
        dependency.packageDirectory = packageDirectory;

        // パッケージのmanifest文書
        nlohmann::json manifest;
        try
        {
            manifest = nlohmann::json::parse(manifestJson);
        }
        // JSONの解析失敗を宣言エラーへ変換します(error: 解析の失敗理由)。
        catch (const std::exception& error)
        {
            throw std::invalid_argument(
                "packages/" + std::string{ packageName }
                + "/package.json を読めません: " + error.what());
        }
        if (!manifest.is_object())
        {
            throw std::invalid_argument(
                "packages/" + std::string{ packageName }
                + "/package.json はJSONオブジェクトで"
                  "指定してください。");
        }
        // native設定のJSON検索結果
        const auto native = manifest.find("native");
        if (native == manifest.end())
        {
            return dependency;
        }
        if (!native->is_object())
        {
            throw std::invalid_argument(
                "packages/" + std::string{ packageName }
                + "/package.json の native はJSONオブジェクトで"
                  "指定してください。");
        }
        // 宣言項目またはフォルダー要素
        for (const auto& entry : native->items())
        {
            if (std::ranges::find(NativeKeys, entry.key())
                == NativeKeys.end())
            {
                throw std::invalid_argument(
                    "packages/" + std::string{ packageName }
                    + "/package.json の native に未知のキーが"
                      "あります: " + entry.key()
                    + "（使えるのは includeDirectories, libraries,"
                      " runtimeFiles, defines だけです）");
            }
        }

        // 指定されたnative配列への借用参照
        if (const auto* const values = FindArray(
            *native,
            "includeDirectories",
            packageName))
        {
            // 検証する宣言配列の要素
            for (const auto& element : *values)
            {
                dependency.includeDirectories.push_back(
                    ResolveRelativePath(
                        packageDirectory,
                        RequireString(
                            element,
                            packageName,
                            "includeDirectories"),
                        packageName,
                        "includeDirectories"));
            }
        }
        // 指定されたnative配列への借用参照
        if (const auto* const values = FindArray(
            *native,
            "libraries",
            packageName))
        {
            // 検証する宣言配列の要素
            for (const auto& element : *values)
            {
                // 宣言された依存ファイルのパス
                auto path = ResolveRelativePath(
                    packageDirectory,
                    RequireString(
                        element,
                        packageName,
                        "libraries"),
                    packageName,
                    "libraries");
                if (!HasExtension(path, L".lib"))
                {
                    throw std::invalid_argument(
                        FieldLabel(packageName, "libraries")
                        + " には .lib だけを指定してください: "
                        + PathToUtf8(path.filename()));
                }
                dependency.libraries.push_back(std::move(path));
            }
        }
        // 指定されたnative配列への借用参照
        if (const auto* const values = FindArray(
            *native,
            "runtimeFiles",
            packageName))
        {
            // 検証する宣言配列の要素
            for (const auto& element : *values)
            {
                // 宣言された依存ファイルのパス
                auto path = ResolveRelativePath(
                    packageDirectory,
                    RequireString(
                        element,
                        packageName,
                        "runtimeFiles"),
                    packageName,
                    "runtimeFiles");
                if (!HasExtension(path, L".dll"))
                {
                    throw std::invalid_argument(
                        FieldLabel(packageName, "runtimeFiles")
                        + " には .dll だけを指定してください: "
                        + PathToUtf8(path.filename()));
                }
                dependency.runtimeFiles.push_back(
                    std::move(path));
            }
        }
        // 指定されたnative配列への借用参照
        if (const auto* const values = FindArray(
            *native,
            "defines",
            packageName))
        {
            // 検証する宣言配列の要素
            for (const auto& element : *values)
            {
                // 検証または出力するマクロ
                auto define = RequireString(
                    element,
                    packageName,
                    "defines");
                if (!IsSafeDefine(define))
                {
                    throw std::invalid_argument(
                        FieldLabel(packageName, "defines")
                        + " は NAME または NAME=VALUE の形（英数字と"
                          "アンダースコア）で指定してください: "
                        + define);
                }
                dependency.defines.push_back(std::move(define));
            }
        }
        return dependency;
    }

    // 依存を名前順に収集し、不正な宣言を個別のエラーとして残します。
    PackageNativeScan ScanPackageNativeDependencies(
        const std::filesystem::path& assetRoot)
    {
        // 依存一覧と個別エラーの結果
        PackageNativeScan scan;
        // パッケージを走査する基準パス
        const auto packagesRoot = assetRoot / L"packages";
        // ファイル操作の失敗状態
        std::error_code error;
        if (!std::filesystem::is_directory(packagesRoot, error))
        {
            return scan;
        }

        // 依存または検索ディレクトリ一覧
        std::vector<std::filesystem::path> directories;
        // 宣言項目またはフォルダー要素
        for (const auto& entry :
            std::filesystem::directory_iterator(
                packagesRoot,
                error))
        {
            if (entry.is_directory(error))
            {
                directories.push_back(entry.path());
            }
        }
        // フォルダーの列挙順に依存しないよう、名前順で固定します。
        std::ranges::sort(directories);

        // 対象のパッケージか検索先パス
        for (const auto& directory : directories)
        {
            // マクロ名またはパッケージ名
            const auto name = PathToUtf8(directory.filename());
            if (!IsPackageFolderName(name))
            {
                continue;
            }
            // package.jsonのパス
            const auto manifestPath = directory / L"package.json";
            if (!std::filesystem::is_regular_file(
                manifestPath,
                error))
            {
                continue;
            }
            try
            {
                // 検証済みのネイティブ依存
                auto dependency = ParsePackageNativeDependency(
                    ReadFileText(manifestPath),
                    directory,
                    name);
                if (!dependency.Empty())
                {
                    scan.packages.push_back(
                        std::move(dependency));
                }
            }
            // 個別の宣言エラーを収集して走査を続けます(failure: 読込の失敗理由)。
            catch (const std::exception& failure)
            {
                scan.errors.emplace_back(failure.what());
            }
        }
        return scan;
    }

    // 宣言ファイルの不足をまとめて例外で通知します。
    void RequirePackageNativeFiles(
        const std::vector<PackageNativeDependency>& packages)
    {
        // 不足する依存ファイルの説明
        std::string missing;
        // 調査または出力する依存
        for (const auto& package : packages)
        {
            missing += DescribeMissingNativeFiles(package);
        }
        if (!missing.empty())
        {
            throw std::runtime_error(
                "パッケージが必要とするネイティブライブラリが"
                "見つかりません。パッケージのREADMEに従って"
                "配置してください:"
                + missing);
        }
    }

    // 宣言ファイルが揃った依存を使用対象へ移します。
    PackageNativeSelection SelectAvailablePackageNativeDependencies(
        std::vector<PackageNativeDependency> packages)
    {
        // 使用可能な依存と不足説明
        PackageNativeSelection selection;
        // 調査または出力する依存
        for (auto& package : packages)
        {
            // 不足する依存ファイルの説明
            const auto missing = DescribeMissingNativeFiles(package);
            if (missing.empty())
            {
                selection.available.push_back(std::move(package));
                continue;
            }
            selection.missing.push_back(
                "パッケージ " + package.packageName
                + " のネイティブライブラリが見つからないため、"
                  "SDKなしでビルドします（READMEに従って配置すると"
                  "有効になります）:"
                + missing);
        }
        return selection;
    }

    // 予約名と重複を拒否して同梱DLLの一覧を作ります。
    std::vector<PackageRuntimeFile> CollectPackageRuntimeFiles(
        const std::vector<PackageNativeDependency>& packages)
    {
        // 同梱するDLLの一覧
        std::vector<PackageRuntimeFile> files;
        // 重複検出用の登録済み名
        std::set<std::wstring> seen;
        // 調査または出力する依存
        for (const auto& package : packages)
        {
            // 宣言された依存ファイルのパス
            for (const auto& path : package.runtimeFiles)
            {
                // 同梱先のDLL名
                auto fileName = path.filename().wstring();
                // DLL名のASCII小文字表現
                const auto lowered = ToLowerAscii(fileName);
                if (std::ranges::find(
                        ReservedRuntimeNames,
                        std::wstring_view{ lowered })
                    != ReservedRuntimeNames.end())
                {
                    throw std::runtime_error(
                        "パッケージ " + package.packageName
                        + " は、エンジン自身のDLLと同じ名前を"
                          "同梱しようとしています: "
                        + PathToUtf8(path.filename()));
                }
                // DLL名を重複検査します(iterator: 登録された名の位置, inserted: 新規登録できたか)。
                if (const auto [iterator, inserted] =
                        seen.insert(lowered);
                    !inserted)
                {
                    throw std::runtime_error(
                        "複数のパッケージが同じ名前のDLLを"
                        "同梱しようとしています: "
                        + PathToUtf8(path.filename()));
                }
                files.push_back({
                    path,
                    std::move(fileName),
                    package.packageName });
            }
        }
        return files;
    }

    // 実行時DLLの親フォルダーをパス文字列の重複なく集めます。
    std::vector<std::filesystem::path>
        PackageNativeSearchDirectories(
            const std::vector<PackageNativeDependency>& packages)
    {
        // 依存または検索ディレクトリ一覧
        std::vector<std::filesystem::path> directories;
        // 重複検出用の登録済み名
        std::set<std::wstring> seen;
        // 調査または出力する依存
        for (const auto& package : packages)
        {
            // 宣言された実行時DLLのパス
            for (const auto& file : package.runtimeFiles)
            {
                // 対象のパッケージか検索先パス
                auto directory = file.parent_path();
                if (seen.insert(directory.wstring()).second)
                {
                    directories.push_back(std::move(directory));
                }
            }
        }
        return directories;
    }

    // 必要な場合だけCMake設定を書き出します。
    void WritePackageNativeCMakeFile(
        const std::filesystem::path& outputPath,
        const std::vector<PackageNativeDependency>& packages)
    {
        // 生成するCMake設定文書
        std::string text =
            "# このファイルはLamaPonが生成します。手で編集しても\n"
            "# 次のGame Moduleビルドで上書きされます。\n"
            "# 元データは assets/packages/<名前>/package.json の\n"
            "# native です。\n"
            "set(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)\n"
            "set(LAMAPON_PACKAGE_LIBRARIES)\n"
            "set(LAMAPON_PACKAGE_DEFINES)\n";
        // 調査または出力する依存
        for (const auto& package : packages)
        {
            text += "\n# " + package.packageName + "\n";
            // 宣言された依存ファイルのパス
            for (const auto& path : package.includeDirectories)
            {
                text += "list(APPEND LAMAPON_PACKAGE_INCLUDE_DIRECTORIES "
                    + QuoteForCMake(PathToUtf8(path)) + ")\n";
            }
            // 宣言された依存ファイルのパス
            for (const auto& path : package.libraries)
            {
                text += "list(APPEND LAMAPON_PACKAGE_LIBRARIES "
                    + QuoteForCMake(PathToUtf8(path)) + ")\n";
            }
            // 検証または出力するマクロ
            for (const auto& define : package.defines)
            {
                text += "list(APPEND LAMAPON_PACKAGE_DEFINES "
                    + QuoteForCMake(define) + ")\n";
            }
        }

        // 内容が同じ場合は更新時刻を保ち、CMakeの不要な再構成を避けます。
        if (ReadFileText(outputPath) == text)
        {
            return;
        }
        std::filesystem::create_directories(
            outputPath.parent_path());
        // CMake設定の出力ストリーム
        std::ofstream output(
            outputPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "パッケージのビルド設定を書き出せませんでした: "
                + PathToUtf8(outputPath));
        }
        // 生成するCMake設定文書
        output << text;
        output.close();
        if (!output)
        {
            throw std::runtime_error(
                "パッケージのビルド設定を書き出せませんでした: "
                + PathToUtf8(outputPath));
        }
    }
}
