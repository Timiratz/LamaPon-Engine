#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// データアセット作成とInspectorのlist追加で同じ既定値の規則を使う。
namespace LamaPon::EditorDetail
{
    struct SchemaStringOption final
    {
        // 文書へ保存する選択値
        std::string value;
        // Inspectorに表示する選択名
        std::string displayName;
    };

    // optionsの文字列またはvalue・displayNameから不正項目と保存値の重複を除く(field: 選択肢を含むフィールド定義)。
    [[nodiscard]] std::vector<SchemaStringOption> SchemaStringOptions(
        const nlohmann::json& field);

    // vec2なら2、vec3・color3なら3、それ以外は4成分を返す(type: 小文字の型名)。
    [[nodiscard]] std::size_t SchemaComponentCount(
        std::string_view type) noexcept;

    // object・list以外はdefaultを優先し、stringは最初の選択肢、color4のalphaは1、未知の型は空文字にする。
    // 型ごとの既定値を作りobjectは子を再帰初期化しlistは空にする(field: 初期化するフィールド定義)。
    [[nodiscard]] nlohmann::json SchemaDefaultValue(
        const nlohmann::json& field);

    // 不正なスキーマなら値を空にしてデータアセット文書を作る(typeName: 保存するデータアセットの型名, schemaJson: フィールド定義を含むJSON)。
    [[nodiscard]] nlohmann::json MakeDataAssetDocument(
        std::string_view typeName,
        std::string_view schemaJson);
}
