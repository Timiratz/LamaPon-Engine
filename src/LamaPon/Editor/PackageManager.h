#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // パッケージの反映タイミング・省略時はImmediate
    enum class PackageActivation : std::uint8_t
    {
        Immediate,
        Restart,
        RestartAndRebuild
    };

    // 反映時期の保存用文字列を返します(activation: 反映タイミング)。
    [[nodiscard]] std::string_view PackageActivationName(
        PackageActivation activation) noexcept;
    // 反映時期を読み、未知名や省略値はImmediateにします(name: 保存された反映時期)。
    [[nodiscard]] PackageActivation PackageActivationFromName(
        std::string_view name) noexcept;
    // 反映に再起動が必要か返します(activation: 反映タイミング)。
    [[nodiscard]] bool PackageRequiresRestart(
        PackageActivation activation) noexcept;

    // パッケージの追加先・反映タイミングとは独立
    enum class PackageTarget : std::uint8_t
    {
        Project,
        Engine
    };

    // 追加先の保存用文字列を返します(target: パッケージの追加先)。
    [[nodiscard]] std::string_view PackageTargetName(
        PackageTarget target) noexcept;
    // 追加先を読み、未知名や省略値はProjectにします(name: 保存された追加先)。
    [[nodiscard]] PackageTarget PackageTargetFromName(
        std::string_view name) noexcept;

    // 配布リポジトリのパッケージ一覧（index.json）に載る1件分。
    struct PackageInfo final
    {
        // 保存先名・小文字数字と-_のみ
        std::string name;
        // 一覧表示用の名前
        std::string displayName;
        // パッケージの説明
        std::string description;
        // パッケージの作者名
        std::string author;
        // パッケージのバージョン
        std::string version;
        // 必須の最低エンジンバージョン
        std::string minimumEngineVersion;
        // 配布ZIPの取得URL
        std::string downloadUrl;
        // 配布ZIPのサイズ・byte
        std::uint64_t sizeBytes{};
        // 照合用SHA-256・小文字16進64桁
        std::string sha256;
        // 反映するタイミング
        PackageActivation activation{ PackageActivation::Immediate };
        // パッケージの追加先
        PackageTarget target{ PackageTarget::Project };
    };

    // 配布一覧の取得先ホスト
    inline constexpr wchar_t PackageIndexHost[] =
        L"raw.githubusercontent.com";
    // 配布一覧の取得先パス
    inline constexpr wchar_t PackageIndexPath[] =
        L"/Timiratz/LamaPon-Engine/main/packages/index.json";


    // 一覧の形式を検証して読み、不正な項目を除外します(indexJson: 配布一覧のJSON全文)。
    [[nodiscard]] std::vector<PackageInfo> ParsePackageIndex(
        std::string_view indexJson);


    // 小文字・数字・ハイフン・下線の1から64文字か判定します(name: インストール先の名前)。
    [[nodiscard]] bool IsPackageNameSafe(
        std::string_view name) noexcept;


    // 小文字16進64桁のSHA-256か判定します(value: 検証するハッシュ表記)。
    [[nodiscard]] bool IsCanonicalPackageSha256(
        std::string_view value) noexcept;


    // 配布リポジトリ配下の許可URL接頭辞を持つか返します(url: 検証する取得先URL)。
    [[nodiscard]] bool IsAllowedPackageUrl(
        std::string_view url) noexcept;


    // HTTPSのURLをホストとパスへ分けます(url: 分割するURL, host: ホスト名の出力先, path: パスの出力先)。
    [[nodiscard]] bool SplitHttpsUrl(
        std::string_view url,
        std::wstring& host,
        std::wstring& path);


    // assets/packages配下の保存パスを作ります(assetRoot: assetsの基準ディレクトリ, name: 検証済みのパッケージ名)。
    [[nodiscard]] std::filesystem::path
        PackageInstallDirectory(
            const std::filesystem::path& assetRoot,
            std::string_view name);


    // インストール済みのバージョンを読み、名前不正・未配置・読込失敗なら空を返します(assetRoot: assetsの基準ディレクトリ, name: 調べるパッケージ名)。
    [[nodiscard]] std::string InstalledPackageVersion(
        const std::filesystem::path& assetRoot,
        std::string_view name);


    // 反映時期を読み、読込失敗や省略値はImmediateにします(assetRoot: assetsの基準ディレクトリ, name: 調べるパッケージ名)。
    [[nodiscard]] PackageActivation InstalledPackageActivation(
        const std::filesystem::path& assetRoot,
        std::string_view name) noexcept;


    // 手元のZIPのmanifestかファイル名から情報を取り出して配置します(assetRoot: assetsの基準ディレクトリ, zipPath: 利用者が選択したZIPパス)。
    [[nodiscard]] PackageInfo InstallPackageFromFile(
        const std::filesystem::path& assetRoot,
        const std::filesystem::path& zipPath);


    // ZIPのSHA-256とnative宣言を検証してから既存を置換します(assetRoot: assetsの基準ディレクトリ, package: 配置情報と期待するハッシュ, zipBytes: 配置するZIPのバイト列)。
    void InstallPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::vector<std::uint8_t>& zipBytes);


    // 検証済みのパッケージを削除し、未配置なら何もしません(assetRoot: assetsの基準ディレクトリ, name: 削除するパッケージ名)。
    void UninstallPackage(
        const std::filesystem::path& assetRoot,
        std::string_view name);

    // 自作パッケージの書き出し結果。
    struct PackageBuildResult final
    {
        // 書き出したZIPのパス
        std::filesystem::path zipPath;
        // 更新したpackage.jsonのパス
        std::filesystem::path manifestPath;
        // ZIP元の通常ファイル数
        std::size_t fileCount{};
        // 書き出したZIPのサイズ・byte
        std::uint64_t sizeBytes{};
        // 配布一覧用JSON・URLは仮値
        std::string indexEntryJson;
    };


    // manifestを更新してZIPと配布一覧用JSONを作ります(assetRoot: assetsの基準ディレクトリ, package: 名前と配布情報, outputDirectory: ZIPの保存先)。
    [[nodiscard]] PackageBuildResult BuildPackage(
        const std::filesystem::path& assetRoot,
        const PackageInfo& package,
        const std::filesystem::path& outputDirectory);
}
