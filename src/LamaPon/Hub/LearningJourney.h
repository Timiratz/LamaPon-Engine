#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon::Hub
{
    // プロジェクトに同梱する学習カリキュラムです。journey.jsonは
    // チームで共有し、LearningProgressだけを各PCの.lamaponへ保存します。
    struct LearningStep final
    {
        std::string id;
        std::string phase;
        std::string title;
        std::string purpose;
        std::string action;
        std::string success;
        std::string role;
        std::uint32_t estimatedMinutes{};
        std::vector<std::string> files;
    };

    struct LearningJourney final
    {
        std::string title;
        // conceptはC++20の予約語なので、JSON上の"concept"とは
        // 別の安全なメンバー名を使います。
        std::string conceptText;
        std::vector<LearningStep> steps;
    };

    struct LearningProgress final
    {
        std::vector<std::string> completedStepIds;
        std::string selectedRole{ "undecided" };
    };

    struct LearningStatus final
    {
        std::size_t completedSteps{};
        std::size_t totalSteps{};
        std::optional<LearningStep> nextStep;
        std::string selectedRole{ "undecided" };
    };

    struct LearningCheck final
    {
        std::string id;
        std::string label;
        bool ok{};
        bool required{};
        std::string detail;
    };

    struct LearningDoctorReport final
    {
        bool ready{};
        std::vector<LearningCheck> checks;
    };

    [[nodiscard]] std::filesystem::path LearningJourneyPath(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] std::filesystem::path LearningProgressPath(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] bool HasLearningJourney(
        const std::filesystem::path& projectRoot) noexcept;

    // 既存プロジェクトにも学習ガイドを追加できます。既存ファイルを
    // 黙って上書きしないため、同名教材がある場合は失敗します。
    void InitializeLearningJourney(
        const std::filesystem::path& projectRoot,
        bool sampleSceneIncluded = false);

    [[nodiscard]] LearningJourney LoadLearningJourney(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] LearningProgress LoadLearningProgress(
        const std::filesystem::path& projectRoot);
    [[nodiscard]] LearningStatus GetLearningStatus(
        const std::filesystem::path& projectRoot);

    void CompleteLearningStep(
        const std::filesystem::path& projectRoot,
        const std::string& stepId);
    void SetLearningRole(
        const std::filesystem::path& projectRoot,
        const std::string& role);
    void ResetLearningProgress(
        const std::filesystem::path& projectRoot);

    [[nodiscard]] LearningDoctorReport DiagnoseLearningJourney(
        const std::filesystem::path& projectRoot) noexcept;
    [[nodiscard]] std::string LearningPhaseDisplayName(
        const std::string& phase);
    [[nodiscard]] std::string LearningRoleDisplayName(
        const std::string& role);
}
