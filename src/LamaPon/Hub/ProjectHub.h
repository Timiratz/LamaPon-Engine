#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon::Hub
{
    enum class ProjectTemplate
    {
        ThreeDimensional,
        TwoDimensional,
        LearningThreeDimensional,
        LearningTwoDimensional
    };

    struct RecentProject final
    {
        // プロジェクトのルート
        std::filesystem::path path;
        // ゲームの表示名
        std::string name;
    };

    // 端末ごとのHub設定ファイルのパスを返す。
    [[nodiscard]] std::filesystem::path SettingsPath();
    // Documents内の既定プロジェクト保存先を返す。
    [[nodiscard]] std::filesystem::path DefaultProjectsDirectory();
    // 設定ファイルとassetsの存在を調べる(projectRoot: プロジェクトルート)。
    [[nodiscard]] bool IsProject(
        const std::filesystem::path& projectRoot);
    // 設定からゲーム名を読む(projectRoot: プロジェクトルート)。
    [[nodiscard]] std::string ProjectName(
        const std::filesystem::path& projectRoot);
    // 親を辿ってLamaPonソース配下か調べる(path: 確認するパス)。
    [[nodiscard]] bool IsInsideEngineSourceTree(
        const std::filesystem::path& path);

    // 空の保存先にプロジェクトを生成する(projectRoot: 保存先, projectName: ゲーム名, projectTemplate: 初期教材の種類, allowInsideEngineSource: エンジン配下への作成を許すか)。
    void CreateProject(
        const std::filesystem::path& projectRoot,
        const std::string& projectName,
        ProjectTemplate projectTemplate,
        bool allowInsideEngineSource = false);

    // 有効な最近のプロジェクトを重複なく最大20件返す。
    [[nodiscard]] std::vector<RecentProject>
        LoadRecentProjects();
    // プロジェクトを最近の一覧の先頭へ追加する(projectRoot: 登録するルート)。
    void AddRecentProject(
        const std::filesystem::path& projectRoot);
    // 最近の一覧からプロジェクトを除く(projectRoot: 除くルート)。
    void RemoveRecentProject(
        const std::filesystem::path& projectRoot);

    // Hub設定からスキップした更新版を読む。
    [[nodiscard]] std::string LoadSkippedUpdateVersion();
    // スキップする更新版を保存する(version: 更新版の識別文字列)。
    void SaveSkippedUpdateVersion(const std::string& version);

    // 最後に使ったプロジェクト保存先を読む。
    [[nodiscard]] std::filesystem::path LoadLastProjectLocation();
    // 次回使うプロジェクト保存先を保存する(location: 保存先)。
    void SaveLastProjectLocation(
        const std::filesystem::path& location);
}
