#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 新規作成と更新で共有する組み込みアセットの相対パス一覧を返します。
    // シェーダーを追加するときは依存するインクルードも一覧へ追加します。
    [[nodiscard]] const std::vector<std::filesystem::path>&
        BuiltInProjectAssets();

    // プロジェクトに記録されたエンジンバージョンと、今動いているエンジンの関係です。
    enum class ProjectVersionStatus
    {
        // 現行エンジンと一致する版
        Match,
        // 現行エンジンより古く更新対象
        Older,
        // 現行エンジンより新しい版
        // 新しい設定を失わないよう、古いエンジンでは開きません。
        Newer,
        // 未記録か解釈不能で更新対象
        Unrecorded
    };

    struct ProjectVersionInfo final
    {
        // 現行エンジンとの版の関係
        ProjectVersionStatus status{
            ProjectVersionStatus::Unrecorded };
        // 記録済みエンジン版、なければ空
        std::string recordedVersion;
    };

    // 記録されたエンジン版を読み取り比較します(projectRoot: プロジェクトのルート, currentEngineVersion: 現行エンジンの版番号)。
    // 文書を変更せず、読み込み失敗や解釈不能な版は未記録として扱います。
    [[nodiscard]] ProjectVersionInfo InspectProjectVersion(
        const std::filesystem::path& projectRoot,
        std::string_view currentEngineVersion);

    // 設定文書へエンジン版を記録します(projectRoot: プロジェクトのルート, version: 記録する版番号)。
    // 既存の他のキーは保持し、文書を読み書きできない場合は記録を省略します。
    void RecordProjectEngineVersion(
        const std::filesystem::path& projectRoot,
        std::string_view version);

    // ドット区切りの版番号を比較します(left: 比較元の版番号, right: 比較先の版番号)。
    // 左が小さければ-1、同じなら0、大きければ1で、不足成分は0として扱います。
    // 空成分・数字以外・10億超の成分にはnulloptを返します。
    [[nodiscard]] std::optional<int> CompareEngineVersions(
        std::string_view left,
        std::string_view right);

    // 古いエンジンで作られたプロジェクトを、現在のエンジンで安全に開けるよう更新した結果です。
    struct ProjectMigrationResult final
    {
        // 資源更新か版の記録を試みたか
        bool changed{};
        // 記録済みの前回エンジン版
        std::string previousEngineVersion;
        // 更新できた資源の相対パス
        std::vector<std::filesystem::path> updatedAssets;
        // .bakへ退避できた資源のパス
        std::vector<std::filesystem::path> backedUpAssets;
    };

    // 組み込みアセットと記録版を更新します(projectRoot: プロジェクトのルート, engineAssetRoot: 現行エンジンのアセット領域, currentEngineVersion: 記録する版番号)。
    // 内容が違う既存資源は.bakへの退避を試みますが、退避失敗でも更新を進めます。
    // ルートが存在しなければ更新せず、途中の失敗ではそれまでの結果を返します。
    // changedだけでは全更新や版の記録の成功を保証しないため、資源別の結果も確認します。
    [[nodiscard]] ProjectMigrationResult MigrateProjectAssets(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineAssetRoot,
        std::string_view currentEngineVersion);
}
