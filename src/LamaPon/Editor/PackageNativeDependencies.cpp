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
    // package.jsonの"native"で受け付けるキーです。ここに無いキーは
    // 拒否します。綴り間違いを黙って無視しないためと、将来フラグを
    // 通す抜け道を作らないためです。
    constexpr std::array<std::string_view, 4> NativeKeys{
        "includeDirectories",
        "libraries",
        "runtimeFiles",
        "defines"
    };

    // 書き出したゲームで、パッケージのDLLに上書きさせない名前です。
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

    // assets/packages/ の下でパッケージとして扱うフォルダー名です。
    // 規則の正本は PackageManager.h の IsPackageNameSafe ですが、
    // ここで参照するとGame Moduleビルドの単体テストまでパッケージ
    // ダウンロード一式を引き込むため、同じ規則を持ちます。
    [[nodiscard]] bool IsPackageFolderName(
        const std::string_view name) noexcept
    {
        if (name.empty() || name.size() > 64u)
        {
            return false;
        }
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

    [[nodiscard]] std::wstring ToLowerAscii(std::wstring value)
    {
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

    [[nodiscard]] std::string FieldLabel(
        const std::string_view packageName,
        const std::string_view field)
    {
        return "packages/" + std::string{ packageName }
            + "/package.json の native." + std::string{ field };
    }

    // パッケージフォルダーの外へ出ない相対パスだけを通します。
    [[nodiscard]] std::filesystem::path ResolveRelativePath(
        const std::filesystem::path& packageDirectory,
        const std::string& value,
        const std::string_view packageName,
        const std::string_view field)
    {
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
        for (const char character : value)
        {
            if (static_cast<unsigned char>(character) < 0x20u)
            {
                throw std::invalid_argument(
                    label + " に制御文字は使えません。");
            }
        }

        const auto relative = LamaPon::PathFromUtf8(value);
        if (relative.is_absolute() || relative.has_root_name())
        {
            throw std::invalid_argument(
                label + " に絶対パスは指定できません: " + value);
        }
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
        const auto resolved =
            (packageDirectory / relative).lexically_normal();
        const auto root = packageDirectory.lexically_normal();
        const auto rootText = root.wstring();
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

    [[nodiscard]] bool HasExtension(
        const std::filesystem::path& path,
        const std::wstring_view expected)
    {
        return ToLowerAscii(path.extension().wstring())
            == expected;
    }

    // NAME または NAME=VALUE だけを通します。
    [[nodiscard]] bool IsSafeDefine(
        const std::string_view value) noexcept
    {
        if (value.empty() || value.size() > 128u)
        {
            return false;
        }
        const auto separator = value.find('=');
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
        const auto assigned = value.substr(separator + 1u);
        if (assigned.size() > 64u)
        {
            return false;
        }
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

    [[nodiscard]] const nlohmann::json* FindArray(
        const nlohmann::json& native,
        const std::string_view key,
        const std::string_view packageName)
    {
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

    [[nodiscard]] std::string ReadFileText(
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

    // CMakeの文字列リテラルとして安全に書き出します。
    [[nodiscard]] std::string QuoteForCMake(
        const std::string& value)
    {
        std::string quoted;
        quoted.reserve(value.size() + 2u);
        quoted.push_back('"');
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

    // 宣言したファイルのうち、置かれていないものを1行ずつ説明します。
    [[nodiscard]] std::string DescribeMissingNativeFiles(
        const LamaPon::PackageNativeDependency& package)
    {
        std::string missing;
        const auto report =
            [&missing, &package](
                const std::filesystem::path& path)
        {
            missing += "\n  - " + package.packageName
                + ": " + LamaPon::PathToUtf8(path);
        };
        for (const auto& path : package.includeDirectories)
        {
            if (!std::filesystem::is_directory(path))
            {
                report(path);
            }
        }
        for (const auto& path : package.libraries)
        {
            if (!std::filesystem::is_regular_file(path))
            {
                report(path);
            }
        }
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
    PackageNativeDependency ParsePackageNativeDependency(
        const std::string_view manifestJson,
        const std::filesystem::path& packageDirectory,
        const std::string_view packageName)
    {
        PackageNativeDependency dependency;
        dependency.packageName = packageName;
        dependency.packageDirectory = packageDirectory;

        nlohmann::json manifest;
        try
        {
            manifest = nlohmann::json::parse(manifestJson);
        }
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

        if (const auto* const values = FindArray(
            *native,
            "includeDirectories",
            packageName))
        {
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
        if (const auto* const values = FindArray(
            *native,
            "libraries",
            packageName))
        {
            for (const auto& element : *values)
            {
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
        if (const auto* const values = FindArray(
            *native,
            "runtimeFiles",
            packageName))
        {
            for (const auto& element : *values)
            {
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
        if (const auto* const values = FindArray(
            *native,
            "defines",
            packageName))
        {
            for (const auto& element : *values)
            {
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

    PackageNativeScan ScanPackageNativeDependencies(
        const std::filesystem::path& assetRoot)
    {
        PackageNativeScan scan;
        const auto packagesRoot = assetRoot / L"packages";
        std::error_code error;
        if (!std::filesystem::is_directory(packagesRoot, error))
        {
            return scan;
        }

        std::vector<std::filesystem::path> directories;
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

        for (const auto& directory : directories)
        {
            const auto name = PathToUtf8(directory.filename());
            if (!IsPackageFolderName(name))
            {
                continue;
            }
            const auto manifestPath = directory / L"package.json";
            if (!std::filesystem::is_regular_file(
                manifestPath,
                error))
            {
                continue;
            }
            try
            {
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
            catch (const std::exception& failure)
            {
                scan.errors.emplace_back(failure.what());
            }
        }
        return scan;
    }

    void RequirePackageNativeFiles(
        const std::vector<PackageNativeDependency>& packages)
    {
        std::string missing;
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

    PackageNativeSelection SelectAvailablePackageNativeDependencies(
        std::vector<PackageNativeDependency> packages)
    {
        PackageNativeSelection selection;
        for (auto& package : packages)
        {
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

    std::vector<PackageRuntimeFile> CollectPackageRuntimeFiles(
        const std::vector<PackageNativeDependency>& packages)
    {
        std::vector<PackageRuntimeFile> files;
        std::set<std::wstring> seen;
        for (const auto& package : packages)
        {
            for (const auto& path : package.runtimeFiles)
            {
                auto fileName = path.filename().wstring();
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

    std::vector<std::filesystem::path>
        PackageNativeSearchDirectories(
            const std::vector<PackageNativeDependency>& packages)
    {
        std::vector<std::filesystem::path> directories;
        std::set<std::wstring> seen;
        for (const auto& package : packages)
        {
            for (const auto& file : package.runtimeFiles)
            {
                auto directory = file.parent_path();
                if (seen.insert(directory.wstring()).second)
                {
                    directories.push_back(std::move(directory));
                }
            }
        }
        return directories;
    }

    void WritePackageNativeCMakeFile(
        const std::filesystem::path& outputPath,
        const std::vector<PackageNativeDependency>& packages)
    {
        std::string text =
            "# このファイルはLamaPonが生成します。手で編集しても\n"
            "# 次のGame Moduleビルドで上書きされます。\n"
            "# 元データは assets/packages/<名前>/package.json の\n"
            "# native です。\n"
            "set(LAMAPON_PACKAGE_INCLUDE_DIRECTORIES)\n"
            "set(LAMAPON_PACKAGE_LIBRARIES)\n"
            "set(LAMAPON_PACKAGE_DEFINES)\n";
        for (const auto& package : packages)
        {
            text += "\n# " + package.packageName + "\n";
            for (const auto& path : package.includeDirectories)
            {
                text += "list(APPEND LAMAPON_PACKAGE_INCLUDE_DIRECTORIES "
                    + QuoteForCMake(PathToUtf8(path)) + ")\n";
            }
            for (const auto& path : package.libraries)
            {
                text += "list(APPEND LAMAPON_PACKAGE_LIBRARIES "
                    + QuoteForCMake(PathToUtf8(path)) + ")\n";
            }
            for (const auto& define : package.defines)
            {
                text += "list(APPEND LAMAPON_PACKAGE_DEFINES "
                    + QuoteForCMake(define) + ")\n";
            }
        }

        // 内容が同じなら書き込みません。更新時刻だけが変わると
        // CMakeが毎回configureをやり直します。
        if (ReadFileText(outputPath) == text)
        {
            return;
        }
        std::filesystem::create_directories(
            outputPath.parent_path());
        std::ofstream output(
            outputPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "パッケージのビルド設定を書き出せませんでした: "
                + PathToUtf8(outputPath));
        }
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
