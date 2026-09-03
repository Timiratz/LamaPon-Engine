#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <string>
#include <string_view>

// データアセット（`*.asset.json`）のスキーマを解釈する小さな関数です。
// 「新規作成時の初期値」と「Inspectorでlistへ要素を足したときの
// 初期値」は同じ規則である必要があるため、ここに1つだけ置きます。
namespace LamaPon::EditorDetail
{
    // vec2/vec3/vec4/color3/color4の成分数です。
    [[nodiscard]] std::size_t SchemaComponentCount(
        std::string_view type) noexcept;

    // スキーマの1フィールドに対応する既定値です。"default"があれば
    // それを、無ければ型ごとの0相当を返します。
    [[nodiscard]] nlohmann::json SchemaDefaultValue(
        const nlohmann::json& field);

    // 型名とスキーマから、既定値だけを詰めた`*.asset.json`の
    // ドキュメントを作ります。スキーマが壊れていても、値が空の
    // ドキュメントを返して作成自体は成功させます。
    [[nodiscard]] nlohmann::json MakeDataAssetDocument(
        std::string_view typeName,
        std::string_view schemaJson);
}
