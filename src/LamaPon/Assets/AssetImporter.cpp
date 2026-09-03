#include "LamaPon/Assets/AssetImporter.h"

#include "LamaPon/Core/PathUtils.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <stdexcept>
#include <string_view>

namespace
{
    bool IsPathWithin(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        const auto relative = candidate.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
        {
            return candidate == root;
        }
        for (const auto& part : relative)
        {
            if (part == L"..")
            {
                return false;
            }
        }
        return true;
    }

    std::wstring Lowercase(std::wstring value)
    {
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

    bool IsMetadataFile(const std::filesystem::path& path)
    {
        return Lowercase(path.extension().wstring()) == L".meta";
    }

    std::filesystem::path MetadataPathFor(
        const std::filesystem::path& assetPath)
    {
        return std::filesystem::path{
            assetPath.wstring() + L".meta"
        };
    }

    bool DestinationAvailable(
        const std::filesystem::path& destination,
        const bool directory)
    {
        return !std::filesystem::exists(destination)
            && (directory
                || !std::filesystem::exists(
                    MetadataPathFor(destination)));
    }

    std::wstring CompoundSuffix(
        const std::filesystem::path& path)
    {
        constexpr std::array<std::wstring_view, 5> suffixes{
            L".scene.json",
            L".prefab.json",
            L".material.json",
            L".animation.json",
            L".animator.json"
        };
        const std::wstring filename = path.filename().wstring();
        const std::wstring lowercase = Lowercase(filename);
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

    std::filesystem::path UniqueDestination(
        const std::filesystem::path& targetDirectory,
        const std::filesystem::path& source,
        const bool directory,
        bool& renamed)
    {
        const auto preferred =
            targetDirectory / source.filename();
        if (DestinationAvailable(preferred, directory))
        {
            renamed = false;
            return preferred;
        }

        const std::wstring filename = source.filename().wstring();
        const std::wstring suffix = directory
            ? std::wstring{}
            : CompoundSuffix(source);
        const std::wstring baseName = suffix.empty()
            ? filename
            : filename.substr(0, filename.size() - suffix.size());
        for (std::size_t index = 1; index < 10000; ++index)
        {
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

    void CopyFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination)
    {
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

    void ImportDirectory(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        const std::filesystem::path& assetRoot,
        const bool renamed,
        LamaPon::AssetImportResult& result)
    {
        const auto normalizedSource =
            std::filesystem::weakly_canonical(source);
        if (IsPathWithin(normalizedSource, destination))
        {
            throw std::runtime_error(
                "A folder cannot be imported into itself.");
        }

        std::error_code error;
        std::filesystem::create_directories(destination, error);
        if (error)
        {
            throw std::runtime_error(
                "Could not create the imported folder: "
                + LamaPon::PathToUtf8(destination));
        }

        const auto options =
            std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator{
                source,
                options
            };
            iterator != std::filesystem::recursive_directory_iterator{};
            ++iterator)
        {
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

            const auto relative =
                entry.path().lexically_relative(source);
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
        AssetImportResult result;
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

        for (const auto& requestedSource : sources)
        {
            std::filesystem::path createdDestination;
            const std::size_t originalFileCount =
                result.files.size();
            try
            {
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

                const bool directory =
                    std::filesystem::is_directory(source);
                if (!directory
                    && !std::filesystem::is_regular_file(source))
                {
                    throw std::runtime_error(
                        "Only files and folders can be imported.");
                }

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
            catch (const std::exception& exception)
            {
                if (!createdDestination.empty())
                {
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
