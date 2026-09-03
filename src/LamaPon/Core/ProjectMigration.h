#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // プロジェクトが持つ「エンジンが正」の組み込みアセットです。
    //
    // **一覧はここにしかありません。** 新規作成（LamaPon Hub）も
    // 既存プロジェクトの更新（MigrateProjectAssets）も同じものを
    // 使います。以前は2箇所に別々の一覧があって中身が食い違い、
    // 「新しいシェーダーは入るのに、それがincludeするファイルが
    // 入らない」という壊れ方をしました。
    //
    // **includeされるファイル（.hlsli）も必ず入れてください。**
    // 入れ忘れると、更新した瞬間にシェーダーがコンパイルできず
    // プロジェクトが開けなくなります。テストがincludeの取りこぼしを
    // 検査します。
    [[nodiscard]] const std::vector<std::filesystem::path>&
        BuiltInProjectAssets();

    // プロジェクトに記録されたエンジンバージョンと、今動いている
    // エンジンの関係です。
    enum class ProjectVersionStatus
    {
        // 同じバージョン。何もしなくて開けます。
        Match,
        // プロジェクトの方が古い。更新すれば開けます。
        Older,
        // プロジェクトの方が新しい。**開いてはいけません。**
        // 古いエンジンで書き戻すと、新しいエンジンが足した設定を
        // 落としたり、組み込みシェーダーを巻き戻したりします。
        Newer,
        // バージョンの記録が無い（この仕組みより前のプロジェクト）。
        // 古いものとして扱います。
        Unrecorded
    };

    struct ProjectVersionInfo final
    {
        ProjectVersionStatus status{
            ProjectVersionStatus::Unrecorded };
        // プロジェクトに記録されていた文字列（無ければ空）。
        std::string recordedVersion;
    };

    // プロジェクトを開く前の判定です。ファイルは書き換えません。
    // 例外は投げません。
    [[nodiscard]] ProjectVersionInfo InspectProjectVersion(
        const std::filesystem::path& projectRoot,
        std::string_view currentEngineVersion);

    // プロジェクトへエンジンバージョンを記録します（他のキーは
    // 保持）。プロジェクトの**新規作成時に必ず呼ぶ**こと。
    // 記録が無いと、最初にエディターで開いたとき「古いプロジェクト
    // なので更新しますか？」と、作った直後なのに訊かれます。
    // 例外は投げません（記録できなくても作成は成立させる）。
    void RecordProjectEngineVersion(
        const std::filesystem::path& projectRoot,
        std::string_view version);

    // "2026.8.5" のような版番号を数値で比べます。区切りは'.'で、
    // 足りない桁は0として扱います（"2026.8" < "2026.8.1"）。
    // 数字として読めない部分があれば nullopt を返します。
    //
    // 戻り値は左が小さいとき負、等しいとき0、大きいとき正。
    [[nodiscard]] std::optional<int> CompareEngineVersions(
        std::string_view left,
        std::string_view right);

    // 古いエンジンで作られたプロジェクトを、現在のエンジンで安全に
    // 開けるよう更新した結果です。
    struct ProjectMigrationResult final
    {
        // 実際に何かを更新したか（falseなら最新のままでした）。
        bool changed{};
        // プロジェクトに記録されていた前回のエンジンバージョン
        // （記録が無い旧プロジェクトでは空）。
        std::string previousEngineVersion;
        // 更新した組み込みアセットの相対パス。
        std::vector<std::filesystem::path> updatedAssets;
        // 利用者が編集していたため退避（.bak）してから更新した
        // 組み込みアセット。
        std::vector<std::filesystem::path> backedUpAssets;
    };

    // プロジェクトの組み込みアセット（エンジンのシェーダー）を
    // 現在のエンジンのものへ揃えます。
    //
    // エンジンを更新するとライティング定数バッファのレイアウトなどが
    // 変わるため、プロジェクト側に古いシェーダーが残っていると
    // 描画がおかしくなったりクラッシュしたりします。内容が同じなら
    // 何もせず、利用者が改造していた場合は「<名前>.bak」へ退避して
    // から最新版へ置き換えます。
    //
    // engineAssetRootが存在しない（ソースビルドでの実行など）場合は
    // 何もしません。例外は投げません。
    [[nodiscard]] ProjectMigrationResult MigrateProjectAssets(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& engineAssetRoot,
        std::string_view currentEngineVersion);
}
