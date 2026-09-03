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
        std::filesystem::path path;
        std::string name;
    };

    [[nodiscard]] std::filesystem::path SettingsPath();
    [[nodiscard]] std::filesystem::path DefaultProjectsDirectory();
    [[nodiscard]] bool IsProject(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] std::string ProjectName(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] bool IsInsideEngineSourceTree(
        const std::filesystem::path& path);

    void CreateProject(
        const std::filesystem::path& projectRoot,
        const std::string& projectName,
        ProjectTemplate projectTemplate,
        bool allowInsideEngineSource = false);

    [[nodiscard]] std::vector<RecentProject>
        LoadRecentProjects();
    void AddRecentProject(
        const std::filesystem::path& projectRoot);
    void RemoveRecentProject(
        const std::filesystem::path& projectRoot);

    // 「このバージョンをスキップ」したエンジン更新の記録（hub.json）。
    [[nodiscard]] std::string LoadSkippedUpdateVersion();
    void SaveSkippedUpdateVersion(const std::string& version);

    // 最後にプロジェクトを作成した保存場所の記録（hub.json）。
    // Hubを閉じても保存先の指定がリセットされないようにします。
    [[nodiscard]] std::filesystem::path LoadLastProjectLocation();
    void SaveLastProjectLocation(
        const std::filesystem::path& location);
}
