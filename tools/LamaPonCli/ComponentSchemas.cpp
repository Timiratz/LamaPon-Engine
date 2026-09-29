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

        [[nodiscard]] nlohmann::json BuildComponentSchemas()
        {
            auto field = ComponentField;
            auto vec2 = [](const double x, const double y)
            {
                return nlohmann::json::array({ x, y });
            };
            auto vec3 = [](const double x, const double y, const double z)
            {
                return nlohmann::json::array({ x, y, z });
            };
            auto color = [](const double r, const double g,
                const double b, const double a = 1.0)
            {
                return nlohmann::json::array({ r, g, b, a });
            };
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

            auto axis = field(
                "axis",
                "enum",
                "vertical");
            axis["values"] = { "horizontal", "vertical" };
            auto collisionDetection = field(
                "collisionDetection",
                "enum",
                "discrete");
            collisionDetection["values"] = { "discrete", "continuous" };
            auto controller = field(
                "controller",
                "asset",
                "");
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

        [[nodiscard]] const nlohmann::json* FindComponentSchema(
            const nlohmann::json& schemas,
            const std::string& type)
        {
            for (const auto& schema : schemas)
            {
                if (schema.value("type", std::string{}) == type)
                {
                    return &schema;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const nlohmann::json* FindComponentField(
            const nlohmann::json& schema,
            const std::string& path)
        {
            const auto fields = schema.find("fields");
            if (fields == schema.end() || !fields->is_array())
            {
                return nullptr;
            }
            for (const auto& field : *fields)
            {
                if (field.value("name", std::string{}) == path)
                {
                    return &field;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool JsonMatchesComponentType(
            const nlohmann::json& value,
            const std::string& type)
        {
            if (type == "bool") return value.is_boolean();
            if (type == "string" || type == "asset") return value.is_string();
            if (type == "enum") return value.is_string();
            if (type == "number") return value.is_number();
            if (type == "integer")
            {
                return value.is_number_integer()
                    || value.is_number_unsigned();
            }
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            if (type == "vec2") return value.is_array() && value.size() == 2
                && std::ranges::all_of(value, [](const auto& item)
                    { return item.is_number(); });
            if (type == "vec3") return value.is_array() && value.size() == 3
                && std::ranges::all_of(value, [](const auto& item)
                    { return item.is_number(); });
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

    void ValidateComponentValue(
        const std::string& componentType,
        const std::string& path,
        const nlohmann::json& value)
    {
        const auto& schemas = ComponentSchemas();
        const auto* schema = FindComponentSchema(
            schemas,
            componentType);
        if (schema == nullptr)
        {
            return;
        }
        const auto* field = FindComponentField(*schema, path);
        if (field == nullptr)
        {
            return;
        }
        const auto fieldType = field->value("type", std::string{});
        if (!JsonMatchesComponentType(value, fieldType))
        {
            throw std::invalid_argument(
                "Component field has the wrong type: "
                + componentType + "." + path);
        }
        if (fieldType == "enum")
        {
            const auto values = field->find("values");
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

    void ValidateComponentObject(
        const std::string& componentType,
        const nlohmann::json& component)
    {
        if (!component.is_object())
        {
            return;
        }
        for (const auto& [key, value] : component.items())
        {
            if (key == "type" || key == "enabled")
            {
                continue;
            }
            ValidateComponentValue(componentType, key, value);
        }
    }

    [[nodiscard]] int RunComponentCommand(
        const std::wstring& action,
        const std::string& type,
        const std::string& category)
    {
        const auto& schemas = ComponentSchemas();
        if (action == L"list")
        {
            nlohmann::json result = nlohmann::json::array();
            for (const auto& schema : schemas)
            {
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
        if (action == L"schema")
        {
            if (type.empty())
            {
                throw std::invalid_argument(
                    "component schema requires --type.");
            }
            const auto* schema = FindComponentSchema(schemas, type);
            if (schema == nullptr)
            {
                throw std::runtime_error(
                    "Unknown component schema: " + type);
            }
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
