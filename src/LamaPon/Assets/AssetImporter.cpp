#include "LamaPon/Assets/AssetImporter.h"

#include "LamaPon/Core/PathUtils.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <stdexcept>
#include <string_view>

namespace
{
    // 字句上の相対パスがルート外へ出ないか調べる(root: 基点の絶対パス, candidate: 検証する絶対パス)。
    bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        // 基点からの相対パス
        const auto relative = candidate.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
        {
            return candidate == root;
        }
        // 相対パスの構成要素
        for (const auto& part : relative)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // 文字列を小文字に変換する(value: 変換元の文字列)。
    std::wstring Lowercase(std::wstring value)
    {
        // 文字列の各文字を小文字にする(character: 変換する文字)。
        std::ranges::transform(
            value,
            value.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        return value;
    }

    // 拡張子の大小を区別せず.metaを判定する(path: 対象のパス)。
    bool IsMetadataFile(const std::filesystem::path& path)
    {
        return Lowercase(path.extension().wstring()) == L".meta";
    }

    // 拡張子を含む元パスに.metaを付ける(assetPath: アセットのパス)。
    std::filesystem::path MetadataPathFor(
        const std::filesystem::path& assetPath)
    {
        return std::filesystem::path{
            assetPath.wstring() + L".meta"
        };
    }

    // 本体と必要な.metaの衝突を調べる(destination: 保存先候補, directory: フォルダーの指定)。
    bool DestinationAvailable(
        const std::filesystem::path& destination,
        const bool directory)
    {
        return !std::filesystem::exists(destination)
            && (directory
                || !std::filesystem::exists(
                    MetadataPathFor(destination)));
    }

    // 既知の複合拡張子を保って末尾を取り出す(path: 元ファイルのパス)。
    std::wstring CompoundSuffix(
        const std::filesystem::path& path)
    {
        // 複合拡張子として保つ末尾
        constexpr std::array<std::wstring_view, 5> suffixes{
            L".scene.json",
            L".prefab.json",
            L".material.json",
            L".animation.json",
            L".animator.json"
        };
        // 拡張子を含むファイル名
        const std::wstring filename = path.filename().wstring();
        // 末尾判定用の小文字名
        const std::wstring lowercase = Lowercase(filename);
        // 保つ複合拡張子
        for (const auto suffix : suffixes)
        {
            if (lowercase.ends_with(suffix))
            {
                return filename.substr(
                    filename.size() - suffix.size());
            }
        }
        return path.extension().wstring();
    }

    // 既存データを上書きしない連番名を選ぶ(targetDirectory: 保存先フォルダー, source: 元のパス, directory: フォルダーの指定, renamed: 改名の有無の出力)。
    std::filesystem::path UniqueDestination(
        const std::filesystem::path& targetDirectory,
        const std::filesystem::path& source,
        const bool directory,
        bool& renamed)
    {
        // 元の名前での保存先候補
        const auto preferred =
            targetDirectory / source.filename();
        if (DestinationAvailable(preferred, directory))
        {
            renamed = false;
            return preferred;
        }

        // 拡張子を含むファイル名
        const std::wstring filename = source.filename().wstring();
        // 保つ複合拡張子
        const std::wstring suffix = directory
            ? std::wstring{}
            : CompoundSuffix(source);
        // 連番を付ける拡張子前の名前
        const std::wstring baseName = suffix.empty()
            ? filename
            : filename.substr(0, filename.size() - suffix.size());
        // 衝突回避用の連番
        for (std::size_t index = 1; index < 10000; ++index)
        {
            // 連番付きの保存先候補
            const auto candidate = targetDirectory
                / (baseName
                    + L" ("
                    + std::to_wstring(index)
                    + L")"
                    + suffix);
            if (DestinationAvailable(candidate, directory))
            {
                renamed = true;
                return candidate;
            }
        }
        throw std::runtime_error(
            "Could not create a unique imported asset name.");
    }

    // 親フォルダーを作り上書きせずにコピーする(source: 元ファイル, destination: コピー先)。
    void CopyFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination)
    {
        // コピー・走査の失敗情報
        std::error_code error;
        std::filesystem::create_directories(
            destination.parent_path(),
            error);
        if (error)
        {
            throw std::runtime_error(
                "Could not create the import destination: "
                + LamaPon::PathToUtf8(destination.parent_path()));
        }

        std::filesystem::copy_file(
            source,
            destination,
            std::filesystem::copy_options::none,
            error);
        if (error)
        {
            throw std::runtime_error(
                "Could not copy the asset: "
                + LamaPon::PathToUtf8(source)
                + ": "
                + error.message());
        }
    }

    // 自己コピーを拒否しリンクと.metaを除いて取り込む(source: 元フォルダー, destination: コピー先フォルダー, assetRoot: 相対記録の基点, renamed: 入力名の変更指定, result: 成功・除外結果の追記先)。
    void ImportDirectory(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::filesystem::path& assetRoot,
        const bool renamed,
        LamaPon::AssetImportResult& result)
    {
        // 正規化したコピー元
        const auto normalizedSource =
            std::filesystem::weakly_canonical(source);
        if (IsPathWithin(normalizedSource, destination))
        {
            throw std::runtime_error(
                "A folder cannot be imported into itself.");
        }

        // コピー・走査の失敗情報
        std::error_code error;
        std::filesystem::create_directories(destination, error);
        if (error)
        {
            throw std::runtime_error(
                "Could not create the imported folder: "
                + LamaPon::PathToUtf8(destination));
        }

        // 権限不足を除外する走査設定
        const auto options =
            std::filesystem::directory_options::skip_permission_denied;
        // 取り込むフォルダーの走査位置
        for (std::filesystem::recursive_directory_iterator iterator{
                source,
                options
            };
            iterator != std::filesystem::recursive_directory_iterator{};
            ++iterator)
        {
            // 走査中のファイル・フォルダー
            const auto& entry = *iterator;
            if (entry.is_symlink())
            {
                if (entry.is_directory(error))
                {
                    iterator.disable_recursion_pending();
                }
                error.clear();
                ++result.skippedLinkCount;
                continue;
            }

            // 基点からの相対パス
            const auto relative =
                entry.path().lexically_relative(source);
            // 相対構造を保つコピー先
            const auto importedPath = destination / relative;
            if (entry.is_directory(error) && !error)
            {
                std::filesystem::create_directories(
                    importedPath,
                    error);
                if (error)
                {
                    throw std::runtime_error(
                        "Could not create an imported subfolder: "
                        + LamaPon::PathToUtf8(importedPath));
                }
                continue;
            }
            error.clear();
            if (!entry.is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }
            if (IsMetadataFile(entry.path()))
            {
                ++result.skippedMetadataCount;
                continue;
            }

            CopyFile(entry.path(), importedPath);
            result.files.push_back(
                {
                    entry.path(),
                    importedPath.lexically_relative(assetRoot),
                    renamed
                });
        }
    }
}

namespace LamaPon
{
    AssetImportResult AssetImporter::Import(
        const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& targetDirectory)
    {
        // 成功・失敗・除外件数の結果
        AssetImportResult result;
        // 正規化したアセットルート
        const auto normalizedRoot =
            std::filesystem::weakly_canonical(assetRoot);
        if (!std::filesystem::is_directory(normalizedRoot))
        {
            throw std::invalid_argument(
                "The asset root does not exist.");
        }
        if (targetDirectory.is_absolute())
        {
            throw std::invalid_argument(
                "The asset import folder must be relative.");
        }

        // ルート内のコピー先フォルダー
        const auto destinationDirectory =
            std::filesystem::weakly_canonical(
                normalizedRoot / targetDirectory);
        if (!IsPathWithin(normalizedRoot, destinationDirectory)
            || !std::filesystem::is_directory(destinationDirectory)
            || std::filesystem::is_symlink(destinationDirectory))
        {
            throw std::invalid_argument(
                "The asset import destination is invalid.");
        }

        // 一件ずつ処理する入力パス
        for (const auto& requestedSource : sources)
        {
            // 失敗時に削除するコピー先
            std::filesystem::path createdDestination;
            // この入力を処理する前の成功数
            const std::size_t originalFileCount =
                result.files.size();
            try
            {
                // 正規化した入力の絶対パス
                const auto source =
                    std::filesystem::weakly_canonical(requestedSource);
                if (!std::filesystem::exists(source))
                {
                    throw std::runtime_error(
                        "The dropped file or folder was not found.");
                }
                if (std::filesystem::is_symlink(source))
                {
                    ++result.skippedLinkCount;
                    continue;
                }
                if (std::filesystem::is_regular_file(source)
                    && IsMetadataFile(source))
                {
                    ++result.skippedMetadataCount;
                    continue;
                }

                // 入力がフォルダーかどうか
                const bool directory =
                    std::filesystem::is_directory(source);
                if (!directory
                    && !std::filesystem::is_regular_file(source))
                {
                    throw std::runtime_error(
                        "Only files and folders can be imported.");
                }

                // 衝突回避による改名の有無
                bool renamed{};
                createdDestination = UniqueDestination(
                    destinationDirectory,
                    source,
                    directory,
                    renamed);
                if (directory)
                {
                    ImportDirectory(
                        source,
                        createdDestination,
                        normalizedRoot,
                        renamed,
                        result);
                    ++result.importedDirectoryCount;
                }
                else
                {
                    CopyFile(source, createdDestination);
                    result.files.push_back(
                        {
                            source,
                            createdDestination.lexically_relative(
                                normalizedRoot),
                            renamed
                        });
                }
                result.renamedSourceCount += renamed ? 1 : 0;
            }
            // 入力一件のコピーを取り消して失敗を記録する(exception: 取り込み失敗の原因)。
            catch (const std::exception& exception)
            {
                if (!createdDestination.empty())
                {
                    // 失敗したコピー先の削除結果
                    std::error_code rollbackError;
                    std::filesystem::remove_all(
                        createdDestination,
                        rollbackError);
                }
                result.files.resize(originalFileCount);
                result.failures.push_back(
                    {
                        requestedSource,
                        exception.what()
                    });
            }
        }
        return result;
    }
}
