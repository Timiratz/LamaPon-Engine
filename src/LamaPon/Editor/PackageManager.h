#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // パッケージを利用可能にするタイミング。既存パッケージは
    // Immediateのままなので、古いmanifest/indexとも互換です。
    enum class PackageActivation : std::uint8_t
    {
        Immediate,
        Restart,
        RestartAndRebuild
    };

    [[nodiscard]] std::string_view PackageActivationName(
        PackageActivation activation) noexcept;
    [[nodiscard]] PackageActivation PackageActivationFromName(
        std::string_view name) noexcept;
    [[nodiscard]] bool PackageRequiresRestart(
        PackageActivation activation) noexcept;

    // 追加先。反映タイミング（activation）とは独立した分類です。
    enum class PackageTarget : std::uint8_t
    {
        Project,
        Engine
    };

    [[nodiscard]] std::string_view PackageTargetName(
        PackageTarget target) noexcept;
    [[nodiscard]] PackageTarget PackageTargetFromName(
        std::string_view name) noexcept;

    // 配布リポジトリのパッケージ一覧（index.json）に載る1件分。
    struct PackageInfo final
    {
        // フォルダー名になる識別子（英小文字・数字・-・_のみ）。
        std::string name;
        std::string displayName;
        std::string description;
        std::string author;
        std::string version;
        // このパッケージが必要とする最低エンジンバージョン。
        std::string minimumEngineVersion;
        std::string downloadUrl;
        std::uint64_t sizeBytes{};
        // Zip全体のSHA-256（英小文字16進64桁）。一覧から得た
        // パッケージでは必須で、インストール前に照合します。
        std::string sha256;
        PackageActivation activation{ PackageActivation::Immediate };
        PackageTarget target{ PackageTarget::Project };
    };

    // 一覧JSONの取得先（配布リポジトリのpackages/index.json）。
    inline constexpr wchar_t PackageIndexHost[] =
        L"raw.githubusercontent.com";
    inline constexpr wchar_t PackageIndexPath[] =
        L"/Timiratz/LamaPon-Engine/main/packages/index.json";

    // 一覧JSONを解釈します。形式不正は例外、パッケージ0件は空を
    // 返します。name不正・URL不許可・sha256の無いエントリは
    // 除外します。
    [[nodiscard]] std::vector<PackageInfo> ParsePackageIndex(
        std::string_view indexJson);

    // インストール先フォルダー名として安全か
    // （英小文字・数字・-・_のみ、1～64文字）。
    [[nodiscard]] bool IsPackageNameSafe(
        std::string_view name) noexcept;

    // 英小文字16進64桁のSHA-256表記か。
    [[nodiscard]] bool IsCanonicalPackageSha256(
        std::string_view value) noexcept;

    // ダウンロードURLとして許可するか（配布リポジトリ配下のみ）。
    [[nodiscard]] bool IsAllowedPackageUrl(
        std::string_view url) noexcept;

    // "https://host/path" をWinHTTP用のホストとパスへ分解します。
    [[nodiscard]] bool SplitHttpsUrl(
        std::string_view url,
        std::wstring& host,
        std::wstring& path);

    // パッケージのインストール先（assets/packages/<name>）。
    [[nodiscard]] std::filesystem::path
        PackageInstallDirectory(
            const std::filesystem::path& assetRoot,
            std::string_view name);

    // インストール済みバージョン（未インストールなら空文字）。
    // assets/packages/<name>/package.json の version を読みます。
    [[nodiscard]] std::string InstalledPackageVersion(
        const std::filesystem::path& assetRoot,
        std::string_view name);

    // manifestにactivationが無い既存パッケージはImmediateです。
    [[nodiscard]] PackageActivation InstalledPackageActivation(
        const std::filesystem::path& assetRoot,
        std::string_view name) noexcept;

    // 手元のZipファイルからインストールします（作者から直接
    // 受け取った自作パッケージ用）。中のpackage.jsonから名前と
    // バージョンを読み、無ければファイル名から推測します。
    // 失敗時は例外を投げます。
    [[nodiscard]] PackageInfo InstallPackageFromFile(
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& zipPath);

    // Zipバイト列を検証・展開してインストールします。
    // package.sha256とZipのSHA-256が一致しない場合は展開前に
    // 例外にします（sha256が空・不正な形式の場合も拒否します）。
    // 展開はステージングフォルダーで行い、成功時のみ既存を
    // 置き換えるため途中失敗で壊れたパッケージを残しません。
    void InstallPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::vector<std::uint8_t>& zipBytes);

    // インストール済みパッケージを削除します。
    void UninstallPackage(
        const std::filesystem::path& assetRoot,
        std::string_view name);

    // 自作パッケージの書き出し結果。
    struct PackageBuildResult final
    {
        std::filesystem::path zipPath;
        std::filesystem::path manifestPath;
        std::size_t fileCount{};
        std::uint64_t sizeBytes{};
        // 配布リポジトリのindex.jsonへ貼り付ける1件分のJSON。
        std::string indexEntryJson;
    };

    // assets/packages/<name> を配布用のZipへ書き出します。
    // package.json（名前・表示名・説明・作者・バージョン・
    // 対応エンジン）はフォルダー内へ生成／更新してから含めます。
    // 失敗時は例外を投げます。
    [[nodiscard]] PackageBuildResult BuildPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::filesystem::path& outputDirectory);
}
