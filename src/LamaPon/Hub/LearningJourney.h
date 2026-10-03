#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon::Hub
{
    // カリキュラムはlearning/で共有し、個人進捗は各PCの.lamaponへ保存する。
    struct LearningStep final
    {
        // 項目の一意ID
        std::string id;
        // 学習段階ID
        std::string phase;
        // 項目の表示名
        std::string title;
        // 学習目的
        std::string purpose;
        // 実施する手順
        std::string action;
        // 完了の判断基準
        std::string success;
        // 推奨する学習者の役割
        std::string role;
        // 所要時間の目安（分）
        std::uint32_t estimatedMinutes{};
        // 教材内の参照パス
        std::vector<std::string> files;
    };

    struct LearningJourney final
    {
        // コースの表示名
        std::string title;
        // JSONのconceptに対応する説明
        std::string conceptText;
        // 実施順の学習項目
        std::vector<LearningStep> steps;
    };

    struct LearningProgress final
    {
        // 完了した項目ID
        std::vector<std::string> completedStepIds;
        // 学習者が選んだ役割
        std::string selectedRole{ "undecided" };
    };

    struct LearningStatus final
    {
        // 教材に存在する完了項目数
        std::size_t completedSteps{};
        // 教材の全項目数
        std::size_t totalSteps{};
        // 最初の未完了項目
        std::optional<LearningStep> nextStep;
        // 学習者が選んだ役割
        std::string selectedRole{ "undecided" };
    };

    struct LearningCheck final
    {
        // 診断項目ID
        std::string id;
        // 診断項目の表示名
        std::string label;
        // 確認に成功したか
        bool ok{};
        // 準備完了に必須か
        bool required{};
        // 確認結果の説明
        std::string detail;
    };

    struct LearningDoctorReport final
    {
        // 必須項目がすべて成功したか
        bool ready{};
        // 項目別の確認結果
        std::vector<LearningCheck> checks;
    };

    // 共有カリキュラムの保存先を返す(projectRoot: プロジェクトルート)。
    [[nodiscard]] std::filesystem::path LearningJourneyPath(
        const std::filesystem::path& projectRoot);
    // 個人進捗の保存先を返す(projectRoot: プロジェクトルート)。
    [[nodiscard]] std::filesystem::path LearningProgressPath(
        const std::filesystem::path& projectRoot);
    // カリキュラムの有無を調べ、確認失敗はfalseとする(projectRoot: プロジェクトルート)。
    [[nodiscard]] bool HasLearningJourney(
        const std::filesystem::path& projectRoot) noexcept;

    // 教材を追加し、既存教材があれば書き込み前に失敗する(projectRoot: プロジェクトルート, sampleSceneIncluded: サンプルSceneの同梱有無)。
    void InitializeLearningJourney(
        const std::filesystem::path& projectRoot,
        bool sampleSceneIncluded = false);

    // 共有カリキュラムを検証して読む(projectRoot: プロジェクトルート)。
    [[nodiscard]] LearningJourney LoadLearningJourney(
        const std::filesystem::path& projectRoot);
    // 個人進捗を読み、未作成なら初期値を返す(projectRoot: プロジェクトルート)。
    [[nodiscard]] LearningProgress LoadLearningProgress(
        const std::filesystem::path& projectRoot);
    // 有効な完了数と最初の未完了項目を返す(projectRoot: プロジェクトルート)。
    [[nodiscard]] LearningStatus GetLearningStatus(
        const std::filesystem::path& projectRoot);

    // 指定項目を完了として保存する(projectRoot: プロジェクトルート, stepId: 教材内の項目ID)。
    void CompleteLearningStep(
        const std::filesystem::path& projectRoot,
        const std::string& stepId);
    // 学習者の役割を保存する(projectRoot: プロジェクトルート, role: 学習者の役割ID)。
    void SetLearningRole(
        const std::filesystem::path& projectRoot,
        const std::string& role);
    // 個人進捗ファイルを削除する(projectRoot: プロジェクトルート)。
    void ResetLearningProgress(
        const std::filesystem::path& projectRoot);

    // 教材と進捗を診断し、必須項目の成否を返す(projectRoot: プロジェクトルート)。
    [[nodiscard]] LearningDoctorReport DiagnoseLearningJourney(
        const std::filesystem::path& projectRoot) noexcept;
    // 学習段階を表示名へ変換し、未知の値はそのまま返す(phase: 段階ID)。
    [[nodiscard]] std::string LearningPhaseDisplayName(
        const std::string& phase);
    // 役割を表示名へ変換し、未知の値はそのまま返す(role: 役割ID)。
    [[nodiscard]] std::string LearningRoleDisplayName(
        const std::string& role);
}
