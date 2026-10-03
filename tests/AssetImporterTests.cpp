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
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const char* message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryDirectory final
    {
    public:
        // 検査用の一時領域を作り、このObjectが削除を担当する。
        TemporaryDirectory()
        {
            // 領域名の重複を避ける時計カウント
            const auto unique = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path()
                / (L"LamaPonAssetImporterTests-"
                    + std::to_wstring(unique));
            std::filesystem::create_directories(m_path);
        }

        // 保持した検査用領域を削除し、削除失敗は例外を出さず無視する。
        ~TemporaryDirectory()
        {
            // 破棄時に無視する削除エラー
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        // このObjectの寿命内で有効な検査用領域のパスを借用で返す。
        [[nodiscard]] const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        // 破棄時に削除する検査領域のパス
        std::filesystem::path m_path;
    };

    // 親領域を用意して検査入力を書き、失敗時は例外を出す(path: 作成する入力ファイル, contents: 書き込む入力内容)。
    void WriteFile(
        const std::filesystem::path& path,
        const std::string& contents)
    {
        std::filesystem::create_directories(path.parent_path());
        // バイナリで書く検査入力のStream
        std::ofstream output(path, std::ios::binary);
        output << contents;
        // 書き込みに失敗した入力生成を検査エラーにします。
        if (!output)
        {
            throw std::runtime_error("Could not write test input.");
        }
    }
}

// 同名入力の改名・複合拡張子・領域Import・メタ生成と自身へのImport拒否を検査する。
int main()
{
    // 検査失敗を捕捉して終了コードへ変換します。
    try
    {
        // Scope終了時に削除する検査領域
        TemporaryDirectory temporary;
        // Import後のAssetを管理するルート
        const auto assetRoot = temporary.Path() / L"assets";
        // Assetルート内のImport先領域
        const auto target = assetRoot / L"textures";
        // 同名入力の1組目の元領域
        const auto sourceA = temporary.Path() / L"source-a";
        // 同名入力の2組目の元領域
        const auto sourceB = temporary.Path() / L"source-b";
        // 入れ子とメタを含むImport元領域
        const auto sourcePack = temporary.Path() / L"pack";
        std::filesystem::create_directories(target);
        WriteFile(sourceA / L"same.png", "first");
        WriteFile(sourceB / L"same.png", "second");
        WriteFile(sourceA / L"Hero.material.json", "{}");
        WriteFile(sourceB / L"Hero.material.json", "{}");
        WriteFile(sourcePack / L"nested" / L"model.vbo", "model");
        WriteFile(sourcePack / L"nested" / L"model.vbo.meta", "metadata");

        // 同名入力と領域をImportした結果
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

        // ImportしたAssetを走査するDB
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        // 全件走査とメタ生成の結果
        const auto databaseResult = database.Refresh(true);
        Require(databaseResult.assetCount == 5,
            "Imported assets were not registered in the database.");
        Require(databaseResult.createdMetaCount == 5,
            "Imported assets did not receive fresh metadata.");
        Require(database.FindByPath(L"textures/same.png") != nullptr
                && database.FindByPath(
                    L"textures/Hero (1).material.json") != nullptr,
            "Imported assets could not be resolved after refresh.");

        // 自身への領域Importを試す結果
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
    // 検査失敗を標準エラーへ出す(exception: 捕捉した検査エラー)。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
