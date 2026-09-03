#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    struct ImportedAssetFile final
    {
        std::filesystem::path source;
        std::filesystem::path path;
        bool renamed{};
    };

    struct AssetImportFailure final
    {
        std::filesystem::path source;
        std::string message;
    };

    struct AssetImportResult final
    {
        std::vector<ImportedAssetFile> files;
        std::vector<AssetImportFailure> failures;
        std::size_t importedDirectoryCount{};
        std::size_t renamedSourceCount{};
        std::size_t skippedMetadataCount{};
        std::size_t skippedLinkCount{};
    };

    class AssetImporter final
    {
    public:
        [[nodiscard]] static AssetImportResult Import(
            const std::vector<std::filesystem::path>& sources,
            const std::filesystem::path& assetRoot,
            const std::filesystem::path& targetDirectory);
    };
}
