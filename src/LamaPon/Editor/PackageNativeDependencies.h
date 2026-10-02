#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // package.jsonのnativeにはパッケージ内の相対パスを宣言し、任意のビルドフラグは受け付けません。
    struct PackageNativeDependency final
    {
        // パッケージ名
        std::string packageName;
        // パッケージの基準ディレクトリ
        std::filesystem::path packageDirectory;
        // 追加するインクルード絶対パス
        std::vector<std::filesystem::path> includeDirectories;
        // リンクするlibの絶対パス
        std::vector<std::filesystem::path> libraries;
        // 同梱するDLLの絶対パス
        std::vector<std::filesystem::path> runtimeFiles;
        // コンパイル時に追加するマクロ
        std::vector<std::string> defines;

        // 全ての依存配列が空か返します。
        [[nodiscard]] bool Empty() const noexcept
        {
            return includeDirectories.empty()
                && libraries.empty()
                && runtimeFiles.empty()
                && defines.empty();
        }
    };

    // nativeの各配列の最大件数
    inline constexpr std::size_t PackageNativeMaxEntries = 32u;
    // 宣言パスの最大長・byte
    inline constexpr std::size_t PackageNativePathMaxBytes = 256u;


    // 不正な宣言はinvalid_argumentで拒否し、ファイルの実在確認はビルド・書き出し直前に行います。
    // native宣言を検証して読み込み、無ければ空の依存を返します(manifestJson: package.jsonの内容, packageDirectory: 基準の絶対パス, packageName: エラー表示用の名前)。
    [[nodiscard]] PackageNativeDependency
        ParsePackageNativeDependency(
            std::string_view manifestJson,
            const std::filesystem::path& packageDirectory,
            std::string_view packageName);

    struct PackageNativeScan final
    {
        // 検証済みネイティブ依存の一覧
        std::vector<PackageNativeDependency> packages;
        // 個別のmanifest読込失敗理由
        std::vector<std::string> errors;
    };


    // パッケージを名前順に走査して依存と個別エラーを集めます(assetRoot: assetsの基準ディレクトリ)。
    [[nodiscard]] PackageNativeScan ScanPackageNativeDependencies(
        const std::filesystem::path& assetRoot);


    // 宣言ファイルの実在を確認し、不足があればruntime_errorを投げます(packages: 検証する依存一覧)。
    void RequirePackageNativeFiles(
        const std::vector<PackageNativeDependency>& packages);

    struct PackageNativeSelection final
    {
        // 宣言ファイルが全て揃った依存
        std::vector<PackageNativeDependency> available;
        // 除外した依存の不足ファイル説明
        std::vector<std::string> missing;
    };


    // 一つでも宣言ファイルが不足する依存をマクロごと除外します(packages: 分類する依存一覧の所有先)。
    [[nodiscard]] PackageNativeSelection
        SelectAvailablePackageNativeDependencies(
            std::vector<PackageNativeDependency> packages);


    struct PackageRuntimeFile final
    {
        // 同梱するDLLのコピー元パス
        std::filesystem::path source;
        // コピー先のDLLファイル名
        std::wstring fileName;
        // DLLを持ち込むパッケージ名
        std::string packageName;
    };

    // 同梱DLL名を集め、エンジン予約名とASCII大小文字を無視した重複を拒否します(packages: 同梱対象の依存一覧)。
    [[nodiscard]] std::vector<PackageRuntimeFile>
        CollectPackageRuntimeFiles(
            const std::vector<PackageNativeDependency>& packages);


    // 実行時DLLの親フォルダーをパス文字列の重複なく返します(packages: 検索先を集める依存一覧)。
    [[nodiscard]] std::vector<std::filesystem::path>
        PackageNativeSearchDirectories(
            const std::vector<PackageNativeDependency>& packages);


    // 引用済みの依存設定を出力し、同じ内容なら書き換えません(outputPath: CMake設定の保存パス, packages: 空も許す依存一覧)。
    void WritePackageNativeCMakeFile(
        const std::filesystem::path& outputPath,
        const std::vector<PackageNativeDependency>& packages);
}
