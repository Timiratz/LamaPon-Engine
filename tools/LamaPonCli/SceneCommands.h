#pragma once
#include <filesystem>

namespace LamaPon::Cli
{
    // Scene/Prefab文書を対象にしたコマンド群です。描画・実行中ゲームの
    // 状態を持ちません。引数解析と例外からJSONへの変換はMainが担当します。
    // 成功は0、検証失敗は1。stdoutのJSON契約は既存CLIと共通です。
    [[nodiscard]] int RunInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene);
    [[nodiscard]] int RunValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene);
    [[nodiscard]] int RunPrefabInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab);
    [[nodiscard]] int RunPrefabValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab);
    [[nodiscard]] int RunPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun);
    [[nodiscard]] int RunPrefabPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun);
    [[nodiscard]] int RunSceneTests(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& specificationPath,
        const std::filesystem::path& reportPath);
}
