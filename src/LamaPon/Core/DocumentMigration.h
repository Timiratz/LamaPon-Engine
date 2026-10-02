#pragma once

#include "LamaPon/Core/Api.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    enum class SerializedDocumentKind
    {
        // シーンの保存文書
        Scene,
        // プレハブの保存文書
        Prefab,
        // プロジェクト設定の文書
        ProjectSettings,
        // プレイヤー設定の保存文書
        PlayerPrefs,
        // 保存スロットの文書
        SaveData
    };

    struct DocumentMigrationReport final
    {
        // 移行前の文書形式バージョン
        std::uint32_t sourceVersion{};
        // 移行後の文書形式バージョン
        std::uint32_t targetVersion{};
        // 文書を移行したか
        bool changed{};
    };

    // エンジンが保存する文書の版番号
    inline constexpr std::uint32_t
        CurrentSerializedDocumentVersion = 1;

    // 保存文書を現行形式へ移行します(document: 更新対象のJSON, kind: 文書種別)。
    // 不正な形式や未対応の版番号にはstd::runtime_errorを送出します。
    [[nodiscard]] LAMAPON_API DocumentMigrationReport
        MigrateSerializedDocument(
            nlohmann::json& document,
            SerializedDocumentKind kind);

    // 文書内のアセット参照先を収集します(document: 参照を調べるJSON)。
    // パスを正規化して大文字小文字を区別せず重複を除き、再帰探索は収集数4096以上で打ち切ります。
    [[nodiscard]] LAMAPON_API
        std::vector<std::filesystem::path>
        CollectSerializedAssetPaths(
            const nlohmann::json& document);

    // アセット一覧を現在の参照から再構築します(document: 更新対象のJSON)。
    LAMAPON_API void RefreshSerializedAssetManifest(
        nlohmann::json& document);
}
