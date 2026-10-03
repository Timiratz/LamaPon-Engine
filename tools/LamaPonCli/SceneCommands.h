#pragma once
#include <filesystem>

namespace LamaPon::Cli
{
    // Scene/Prefab文書を解析し、stdoutへ共通JSONを出力します。
    // 引数解析と例外のJSON変換はMainが行い、成功0・検証失敗1を返します。
    // 実行中ゲームの状態は変更しません。
    // sceneの概要JSONを出力します(projectRoot: root, scene: 入力path)
    [[nodiscard]] int RunInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene);
    // scene文書を検証します(projectRoot: root, scene: 入力path)
    [[nodiscard]] int RunValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene);
    // prefabの概要JSONを出力します(projectRoot: root, prefab: 入力path)
    [[nodiscard]] int RunPrefabInspect(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab);
    // prefab文書を検証します(projectRoot: root, prefab: 入力path)
    [[nodiscard]] int RunPrefabValidate(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab);
    // scene patchを適用またはdry-runします(projectRoot: root, scene: 入力path, operationsPath: 操作JSON, outputPath: 出力path, dryRun: 保存省略)
    [[nodiscard]] int RunPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun);
    // prefab patchを適用またはdry-runします(projectRoot: root, prefab: 入力path, operationsPath: 操作JSON, outputPath: 出力path, dryRun: 保存省略)
    [[nodiscard]] int RunPrefabPatch(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& prefab,
        const std::filesystem::path& operationsPath,
        const std::filesystem::path& outputPath,
        const bool dryRun);
    // scene test specificationを実行しreportを保存します(projectRoot: root, scene: 入力path, specificationPath: test定義, reportPath: 出力path)
    [[nodiscard]] int RunSceneTests(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& scene,
        const std::filesystem::path& specificationPath,
        const std::filesystem::path& reportPath);
}
