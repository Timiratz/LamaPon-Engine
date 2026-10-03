#include "ComponentSchemas.h"
#include "LamaPon/Core/PathUtils.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace LamaPon::Cli
{
    namespace
    {
        // ComponentField(name: 項目名, type: 型, defaultValue: 初期値)
        [[nodiscard]] nlohmann::json ComponentField(
            const char* name,
            const char* type,
            nlohmann::json defaultValue)
        {
            return {
                { "name", name },
                { "type", type },
                { "default", std::move(defaultValue) },
                { "editable", true },
            };
        }

        // 全コンポーネントの編集項目定義を作ります。
        [[nodiscard]] nlohmann::json BuildComponentSchemas()
        {
            // 項目定義を作る共通関数
            auto field = ComponentField;
            // vec2(x: X座標, y: Y座標)
            auto vec2 = [](const double x, const double y)
            {
                return nlohmann::json::array({ x, y });
            };
            // vec3(x: X座標, y: Y座標, z: Z座標)
            auto vec3 = [](const double x, const double y, const double z)
            {
                return nlohmann::json::array({ x, y, z });
            };
            // color(r: 赤, g: 緑, b: 青, a: 透明度)
            auto color = [](const double r, const double g,
                const double b, const double a = 1.0)
            {
                return nlohmann::json::array({ r, g, b, a });
            };
            // schema(type: 識別名, category: 分類, fields: 項目群)
            auto schema = [](const char* type,
                const char* category,
                nlohmann::json fields)
            {
                return nlohmann::json{
                    { "type", type },
                    { "category", category },
                    { "fields", std::move(fields) },
                };
            };

            // 縦横を選ぶレイアウト軸
            auto axis = field(
                "axis",
                "enum",
                "vertical");
            axis["values"] = { "horizontal", "vertical" };
            // 物理演算の衝突判定方式
            auto collisionDetection = field(
                "collisionDetection",
                "enum",
                "discrete");
            collisionDetection["values"] = { "discrete", "continuous" };
            // 変形アニメーションの制御資産
            auto controller = field(
                "controller",
                "asset",
                "");
            // 再生するアニメーションクリップ
            auto clip = field(
                "clip",
                "asset",
                "");

            return nlohmann::json::array({
                schema(
                    "UICanvas",
                    "UI",
                    nlohmann::json::array({
                        field("referenceResolution", "vec2", vec2(1280, 720)),
                        field("matchWidthOrHeight", "number", 0.5),
                    })),
                schema(
                    "UIRectTransform",
                    "UI",
                    nlohmann::json::array({
                        field("anchorMin", "vec2", vec2(0.5, 0.5)),
                        field("anchorMax", "vec2", vec2(0.5, 0.5)),
                        field("pivot", "vec2", vec2(0.5, 0.5)),
                        field("anchoredPosition", "vec2", vec2(0, 0)),
                        field("sizeDelta", "vec2", vec2(220, 56)),
                    })),
                schema(
                    "UIButton",
                    "UI",
                    nlohmann::json::array({
                        field("label", "string", "ボタン"),
                        field("fontFamily", "string", "Yu Gothic UI"),
                        field("fontSize", "number", 24.0),
                        field("fallbackSize", "vec2", vec2(220, 56)),
                        field("normalColor", "color4", color(0.08, 0.28, 0.52, 0.96)),
                        field("hoveredColor", "color4", color(0.12, 0.42, 0.76)),
                        field("pressedColor", "color4", color(0.04, 0.20, 0.40)),
                        field("disabledColor", "color4", color(0.18, 0.20, 0.24, 0.65)),
                        field("textColor", "color4", color(1, 1, 1)),
                        field("interactable", "bool", true),
                        field("reloadCurrentScene", "bool", false),
                        field("loadTargetAdditive", "bool", false),
                        field("clickEvent", "string", ""),
                        field("sortOrder", "integer", 0),
                        field("texture", "asset", ""),
                        field("targetScene", "asset", ""),
                    })),
                schema(
                    "UIImage",
                    "UI",
                    nlohmann::json::array({
                        field("texture", "asset", ""),
                        field("color", "color4", color(1, 1, 1)),
                        field("border", "vec4", color(0, 0, 0, 0)),
                        field("fallbackSize", "vec2", vec2(100, 100)),
                        field("sortOrder", "integer", 0),
                        field("renderTexture", "string", ""),
                    })),
                schema(
                    "UIToggle",
                    "UI",
                    nlohmann::json::array({
                        field("label", "string", "トグル"),
                        field("isOn", "bool", false),
                        field("fontFamily", "string", "Yu Gothic UI"),
                        field("fontSize", "number", 24.0),
                        field("interactable", "bool", true),
                        field("boxColor", "color4", color(0.16, 0.20, 0.28, 0.96)),
                        field("checkColor", "color4", color(0.30, 0.75, 0.40)),
                        field("textColor", "color4", color(1, 1, 1)),
                        field("fallbackSize", "vec2", vec2(220, 40)),
                        field("sortOrder", "integer", 0),
                    })),
                schema(
                    "UISlider",
                    "UI",
                    nlohmann::json::array({
                        field("minValue", "number", 0.0),
                        field("maxValue", "number", 1.0),
                        field("value", "number", 0.5),
                        field("wholeNumbers", "bool", false),
                        field("interactable", "bool", true),
                        field("backgroundColor", "color4", color(0.14, 0.16, 0.22, 0.96)),
                        field("fillColor", "color4", color(0.12, 0.42, 0.76)),
                        field("handleColor", "color4", color(0.92, 0.94, 0.98)),
                        field("fallbackSize", "vec2", vec2(220, 24)),
                        field("sortOrder", "integer", 0),
                    })),
                schema(
                    "UIInputField",
                    "UI",
                    nlohmann::json::array({
                        field("text", "string", ""),
                        field("placeholder", "string", "テキストを入力..."),
                        field("fontFamily", "string", "Yu Gothic UI"),
                        field("fontSize", "number", 24.0),
                        field("maxLength", "integer", 256),
                        field("interactable", "bool", true),
                        field("backgroundColor", "color4", color(0.10, 0.12, 0.16, 0.96)),
                        field("focusedColor", "color4", color(0.14, 0.20, 0.30)),
                        field("textColor", "color4", color(1, 1, 1)),
                        field("placeholderColor", "color4", color(0.65, 0.68, 0.75, 0.8)),
                        field("fallbackSize", "vec2", vec2(280, 44)),
                        field("sortOrder", "integer", 0),
                    })),
                schema(
                    "UILayoutGroup",
                    "UI",
                    nlohmann::json::array({
                        axis,
                        field("spacing", "number", 8.0),
                        field("padding", "vec4", color(8, 8, 8, 8)),
                        field("childAlignment", "integer", 0),
                    })),
                schema(
                    "UIScrollView",
                    "UI",
                    nlohmann::json::array({
                        field("scrollSpeed", "number", 48.0),
                        field("interactable", "bool", true),
                        field("backgroundColor", "color4", color(0.08, 0.09, 0.12, 0.9)),
                        field("scrollbarColor", "color4", color(0.6, 0.65, 0.75, 0.9)),
                        field("sortOrder", "integer", 0),
                    })),
                schema(
                    "Rigidbody",
                    "Physics",
                    nlohmann::json::array({
                        field("velocity", "vec3", vec3(0, 0, 0)),
                        field("useGravity", "bool", true),
                        field("kinematic", "bool", false),
                        collisionDetection,
                        field("mass", "number", 1.0),
                        field("angularVelocity", "vec3", vec3(0, 0, 0)),
                        field("centerOfMass", "vec3", vec3(0, 0, 0)),
                        field("linearDrag", "number", 0.0),
                        field("angularDrag", "number", 0.05),
                        field("interpolate", "bool", true),
                        field("constraints.freezeRotationX", "bool", false),
                        field("constraints.freezeRotationY", "bool", false),
                        field("constraints.freezeRotationZ", "bool", false),
                        field("constraints.freezePositionX", "bool", false),
                        field("constraints.freezePositionY", "bool", false),
                        field("constraints.freezePositionZ", "bool", false),
                    })),
                schema(
                    "BoxCollider3D",
                    "Physics",
                    nlohmann::json::array({
                        field("size", "vec3", vec3(1, 1, 1)),
                        field("offset", "vec3", vec3(0, 0, 0)),
                        field("trigger", "bool", false),
                        field("layer", "integer", 0),
                        field("mask", "integer", 0xffffffffu),
                        field("friction", "number", 0.5),
                        field("restitution", "number", 0.0),
                        field("frictionCombine", "integer", 1),
                        field("restitutionCombine", "integer", 4),
                    })),
                schema(
                    "SphereCollider3D",
                    "Physics",
                    nlohmann::json::array({
                        field("radius", "number", 0.5),
                        field("offset", "vec3", vec3(0, 0, 0)),
                        field("trigger", "bool", false),
                        field("layer", "integer", 0),
                        field("mask", "integer", 0xffffffffu),
                        field("friction", "number", 0.5),
                        field("restitution", "number", 0.0),
                        field("frictionCombine", "integer", 1),
                        field("restitutionCombine", "integer", 4),
                    })),
                schema(
                    "CapsuleCollider3D",
                    "Physics",
                    nlohmann::json::array({
                        field("radius", "number", 0.5),
                        field("height", "number", 2.0),
                        field("offset", "vec3", vec3(0, 0, 0)),
                        field("trigger", "bool", false),
                        field("layer", "integer", 0),
                        field("mask", "integer", 0xffffffffu),
                        field("friction", "number", 0.5),
                        field("restitution", "number", 0.0),
                        field("frictionCombine", "integer", 1),
                        field("restitutionCombine", "integer", 4),
                    })),
                schema(
                    "CharacterController",
                    "Physics",
                    nlohmann::json::array({
                        field("radius", "number", 0.4),
                        field("height", "number", 1.8),
                        field("moveSpeed", "number", 4.0),
                        field("gravity", "number", 20.0),
                        field("jumpSpeed", "number", 7.0),
                        field("stepOffset", "number", 0.3),
                        field("skinWidth", "number", 0.03),
                        field("layer", "integer", 2),
                        field("collisionMask", "integer", 0xffffffffu),
                        field("useInput", "bool", true),
                        field("horizontalAction", "string", "MoveHorizontal"),
                        field("verticalAction", "string", "MoveVertical"),
                        field("jumpAction", "string", "Jump"),
                    })),
                schema(
                    "NavMeshAgent",
                    "Physics",
                    nlohmann::json::array({
                        field("speed", "number", 3.0),
                        field("stoppingDistance", "number", 0.1),
                        field("rotateToPath", "bool", true),
                        field("destination", "vec3", vec3(0, 0, 0)),
                        field("path", "array", nlohmann::json::array()),
                    })),
                schema(
                    "TransformAnimator",
                    "Animation",
                    nlohmann::json::array({
                        clip,
                        field("speed", "number", 1.0),
                        field("loop", "bool", true),
                        field("playOnStart", "bool", true),
                        controller,
                    })),
                schema(
                    "SpriteAnimator",
                    "Animation",
                    nlohmann::json::array({
                        field("columns", "integer", 1),
                        field("rows", "integer", 1),
                        field("speed", "number", 1.0),
                        field("playOnStart", "bool", true),
                        field("defaultClip", "string", ""),
                        field("clips", "array", nlohmann::json::array()),
                    })),
            });
        }

        // schemasからtypeに一致する定義を返します。
        // FindComponentSchema(schemas: 定義一覧, type: 識別名)
        [[nodiscard]] const nlohmann::json* FindComponentSchema(
            const nlohmann::json& schemas,
            const std::string& type)
        {
            // schema: 識別名が一致する定義を探す
            for (const auto& schema : schemas)
            {
                // 要求された型だけを返す
                if (schema.value("type", std::string{}) == type)
                {
                    return &schema;
                }
            }
            return nullptr;
        }

        // schema内からpathに一致する項目を返します。
        // FindComponentField(schema: 定義, path: 項目名)
        [[nodiscard]] const nlohmann::json* FindComponentField(
            const nlohmann::json& schema,
            const std::string& path)
        {
            // schemaの項目配列
            const auto fields = schema.find("fields");
            // 項目配列がなければ検索できない
            if (fields == schema.end() || !fields->is_array())
            {
                return nullptr;
            }
            // field: 指定pathの項目を探す
            for (const auto& field : *fields)
            {
                // 項目名が一致した定義を返す
                if (field.value("name", std::string{}) == path)
                {
                    return &field;
                }
            }
            return nullptr;
        }

        // valueがtypeで定義されたJSON型か判定します。
        // JsonMatchesComponentType(value: 値, type: 項目型)
        [[nodiscard]] bool JsonMatchesComponentType(
            const nlohmann::json& value,
            const std::string& type)
        {
            // boolはJSON真偽値に限定する
            if (type == "bool") return value.is_boolean();
            // 文字列と資産参照はJSON文字列で表す
            if (type == "string" || type == "asset") return value.is_string();
            // enum値は文字列で表す
            if (type == "enum") return value.is_string();
            // numberは整数と浮動小数を許可する
            if (type == "number") return value.is_number();
            // integerは符号付き・符号なし整数を許可する
            if (type == "integer")
            {
                return value.is_number_integer()
                    || value.is_number_unsigned();
            }
            // objectはJSONオブジェクトに限定する
            if (type == "object") return value.is_object();
            // arrayはJSON配列に限定する
            if (type == "array") return value.is_array();
            // vec2は数値2要素に限定する
            if (type == "vec2") return value.is_array() && value.size() == 2
                && std::ranges::all_of(value, [](const auto& item)
                    { return item.is_number(); });
            // vec3は数値3要素に限定する
            if (type == "vec3") return value.is_array() && value.size() == 3
                && std::ranges::all_of(value, [](const auto& item)
                    { return item.is_number(); });
            // vec4とcolor4は数値4要素に限定する
            if (type == "vec4" || type == "color4")
            {
                return value.is_array() && value.size() == 4
                    && std::ranges::all_of(value, [](const auto& item)
                        { return item.is_number(); });
            }
            return true;
        }

    }

    const nlohmann::json& ComponentSchemas()
    {
        // 一度だけ構築し、列挙とパッチ検証で同じ変更不能の定義を参照します。
        static const auto schemas = BuildComponentSchemas();
        return schemas;
    }

    // 定義済みの項目型とenum値を検証します。
    // ValidateComponentValue(componentType: 部品型, path: 項目名, value: 設定値)
    void ValidateComponentValue(
        const std::string& componentType,
        const std::string& path,
        const nlohmann::json& value)
    {
        // コンポーネント型一覧
        const auto& schemas = ComponentSchemas();
        // 対象コンポーネント定義
        const auto* schema = FindComponentSchema(
            schemas,
            componentType);
        // 定義のない型は追加制約なし
        if (schema == nullptr)
        {
            return;
        }
        // 対象項目定義
        const auto* field = FindComponentField(*schema, path);
        // 定義のない項目は追加制約なし
        if (field == nullptr)
        {
            return;
        }
        // 項目の宣言型
        const auto fieldType = field->value("type", std::string{});
        // 宣言型と値のJSON型が不一致
        if (!JsonMatchesComponentType(value, fieldType))
        {
            throw std::invalid_argument(
                "Component field has the wrong type: "
                + componentType + "." + path);
        }
        // enum項目は許可値も照合する
        if (fieldType == "enum")
        {
            // enumの許可値一覧
            const auto values = field->find("values");
            // 許可値一覧がある場合だけ値を照合する
            if (values != field->end()
                && std::ranges::none_of(
                    *values,
                    [&value](const auto& item)
                    {
                        return item == value;
                    }))
            {
                throw std::invalid_argument(
                    "Component field has an unsupported enum value: "
                    + componentType + "." + path);
            }
        }
    }

    // component内の既知項目を順に検証します。
    // ValidateComponentObject(componentType: 部品型, component: 部品データ)
    void ValidateComponentObject(
        const std::string& componentType,
        const nlohmann::json& component)
    {
        // object以外は項目検証の対象外
        if (!component.is_object())
        {
            return;
        }
        // key: 項目名, value: 設定値を検証
        for (const auto& [key, value] : component.items())
        {
            // 共通メタデータは型定義の対象外
            if (key == "type" || key == "enabled")
            {
                continue;
            }
            ValidateComponentValue(componentType, key, value);
        }
    }

    // listは一覧、schemaは単一の定義をJSON出力します。
    // RunComponentCommand(action: 操作, type: 部品型, category: 分類)
    [[nodiscard]] int RunComponentCommand(
        const std::wstring& action,
        const std::string& type,
        const std::string& category)
    {
        // コンポーネント型一覧
        const auto& schemas = ComponentSchemas();
        // listは型名と分類の一覧を返す
        if (action == L"list")
        {
            // 分類条件を適用した結果
            nlohmann::json result = nlohmann::json::array();
            // schema: 分類条件に合う定義を列挙
            for (const auto& schema : schemas)
            {
                // 指定分類以外を除外する
                if (!category.empty()
                    && schema.value("category", std::string{}) != category)
                {
                    continue;
                }
                result.push_back({
                    { "type", schema.at("type") },
                    { "category", schema.at("category") },
                    { "fieldCount", schema.at("fields").size() },
                });
            }
            // CLI応答JSON
            const nlohmann::json report{
                { "ok", true },
                { "command", "component list" },
                { "count", result.size() },
                { "components", std::move(result) },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }
        // schemaは指定型の詳細定義を返す
        if (action == L"schema")
        {
            // 型指定なしでは詳細を選べない
            if (type.empty())
            {
                throw std::invalid_argument(
                    "component schema requires --type.");
            }
            // 指定された型定義
            const auto* schema = FindComponentSchema(schemas, type);
            // 存在しない型はエラー
            if (schema == nullptr)
            {
                throw std::runtime_error(
                    "Unknown component schema: " + type);
            }
            // CLI応答JSON
            const nlohmann::json report{
                { "ok", true },
                { "command", "component schema" },
                { "schema", *schema },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }
        throw std::invalid_argument(
            "Unknown component action: "
            + LamaPon::PathToUtf8(std::filesystem::path(action)));
    }

}
