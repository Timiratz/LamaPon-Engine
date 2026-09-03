#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Assets/AssetImporter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonAssetImporterTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << contents;
        if (!output)
        {
            throw std::runtime_error("Could not write test input.");
        }
    }
}

int main()
{
    try
    {
        TemporaryDirectory temporary;
        const auto assetRoot = temporary.Path() / L"assets";
        const auto target = assetRoot / L"textures";
        const auto sourceA = temporary.Path() / L"source-a";
        const auto sourceB = temporary.Path() / L"source-b";
        const auto sourcePack = temporary.Path() / L"pack";
        std::filesystem::create_directories(target);
        WriteFile(sourceA / L"same.png", "first");
        WriteFile(sourceB / L"same.png", "second");
        WriteFile(sourceA / L"Hero.material.json", "{}");
        WriteFile(sourceB / L"Hero.material.json", "{}");
        WriteFile(sourcePack / L"nested" / L"model.vbo", "model");
        WriteFile(sourcePack / L"nested" / L"model.vbo.meta", "metadata");

        const auto result = LamaPon::AssetImporter::Import(
            {
                sourceA / L"same.png",
                sourceB / L"same.png",
                sourceA / L"Hero.material.json",
                sourceB / L"Hero.material.json",
                sourcePack
            },
            assetRoot,
            L"textures");

        Require(result.failures.empty(),
            "Valid assets unexpectedly failed to import.");
        Require(result.files.size() == 5,
            "The imported file count is incorrect.");
        Require(result.importedDirectoryCount == 1,
            "The imported directory count is incorrect.");
        Require(result.renamedSourceCount == 2,
            "Duplicate sources should be counted as renamed.");
        Require(result.skippedMetadataCount == 1,
            "Metadata sidecars should be skipped.");
        Require(std::filesystem::is_regular_file(
                target / L"same.png")
            && std::filesystem::is_regular_file(
                target / L"same (1).png"),
            "A duplicate filename was not renamed.");
        Require(std::filesystem::is_regular_file(
                target / L"Hero.material.json")
            && std::filesystem::is_regular_file(
                target / L"Hero (1).material.json"),
            "A compound asset suffix was not preserved while renaming.");
        Require(std::filesystem::is_regular_file(
                target / L"pack" / L"nested" / L"model.vbo")
            && !std::filesystem::exists(
                target / L"pack" / L"nested" / L"model.vbo.meta"),
            "Folder import did not preserve the expected contents.");

        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        const auto databaseResult = database.Refresh(true);
        Require(databaseResult.assetCount == 5,
            "Imported assets were not registered in the database.");
        Require(databaseResult.createdMetaCount == 5,
            "Imported assets did not receive fresh metadata.");
        Require(database.FindByPath(L"textures/same.png") != nullptr
                && database.FindByPath(
                    L"textures/Hero (1).material.json") != nullptr,
            "Imported assets could not be resolved after refresh.");

        const auto recursiveResult = LamaPon::AssetImporter::Import(
            { target },
            assetRoot,
            L"textures");
        Require(recursiveResult.failures.size() == 1,
            "Importing a folder into itself should fail.");
        Require(!std::filesystem::exists(target / L"textures"),
            "A failed folder import was not rolled back.");

        std::cout << "Asset importer tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
