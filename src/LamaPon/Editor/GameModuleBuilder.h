#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace LamaPon
{
    // エディターの非同期実行とCLIの同期実行で共有するGame Moduleのビルドコマンドです。
    struct GameModuleBuildCommand final
    {
        // cmd.exeへ渡す起動引数
        std::wstring parameters;
        // プロジェクト内のビルドログ
        std::filesystem::path logPath;
        // 配置するGame ModuleのDLLパス
        std::filesystem::path outputModule;
        // 中間生成物の保存先
        std::filesystem::path buildDirectory;
        // ローカルのビルドキャッシュか
        bool usesLocalBuildCache{};
    };

    struct GameModuleBuildState final
    {
        // 対象ソースがあるか
        bool hasSources{};
        // 出力DLLと更新時刻を取得できたか
        bool outputExists{};
        // DLLの再ビルドが必要か
        bool buildRequired{};
        // DLLがRuntimeより古いか
        bool staleAgainstRuntime{};
    };


    // ソースとDLLの更新時刻を比較して再ビルドの要否を調べます(projectRoot: プロジェクトの基準パス, outputModule: 判定対象DLL・空なら既定)。
    [[nodiscard]] GameModuleBuildState InspectGameModuleBuildState(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& outputModule = {}) noexcept;


    // ネットワーク保存先または環境変数指定時にローカルキャッシュを使います(projectRoot: プロジェクトの基準パス)。
    [[nodiscard]] bool ShouldUseLocalGameModuleBuildCache(
        const std::filesystem::path& projectRoot) noexcept;


    // 前提を検証し、依存設定を出力してビルド用のコマンドを構築します(projectRoot: プロジェクトの基準パス, engineRoot: エンジンのソース基準パス, runtimeDirectory: Runtime.libの保存先, configuration: CMakeのビルド構成名)。
    [[nodiscard]] GameModuleBuildCommand
        MakeGameModuleBuildCommand(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& engineRoot,
            const std::filesystem::path& runtimeDirectory,
            const std::string& configuration);


    // 例外時は0を返しますが、既に変更した更新時刻は元に戻しません。
    // 前回と内容が異なるソースの更新時刻を進め、変更数を返します(projectRoot: ソースを持つプロジェクト, buildDirectory: ハッシュmanifestの保存先)。
    int RefreshStaleGameModuleSources(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& buildDirectory) noexcept;


    // DLLをロードして公開されたAPI版数を読み、読めなければnulloptを返します(modulePath: 読込対象DLLのパス)。
    [[nodiscard]] std::optional<std::uint32_t>
        ReadGameModuleApiVersion(
            const std::filesystem::path& modulePath) noexcept;
}
