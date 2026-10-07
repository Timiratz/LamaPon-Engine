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
        // 同時に実行するビルド数
        std::uint32_t parallelJobs{ 1 };
    };

    // CPUと空きメモリから1～2並列を選びます(logicalProcessors: 論理CPU数, availablePhysicalMemory: 空き物理メモリ・byte, availableCommitMemory: 空きコミット容量・byte)。
    [[nodiscard]] std::uint32_t SelectGameModuleBuildParallelJobs(
        std::uint32_t logicalProcessors,
        std::uint64_t availablePhysicalMemory,
        std::uint64_t availableCommitMemory) noexcept;

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


    // ビルドコマンドを構築します(projectRoot: プロジェクトルート, engineRoot: SDKルート, runtimeDirectory: Runtimeの配置先, configuration: ビルド構成, fastBuild: 編集用の高速ビルドか)。
    [[nodiscard]] GameModuleBuildCommand
        MakeGameModuleBuildCommand(
            const std::filesystem::path& projectRoot,
            const std::filesystem::path& engineRoot,
            const std::filesystem::path& runtimeDirectory,
            const std::string& configuration,
            bool fastBuild = false);


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
