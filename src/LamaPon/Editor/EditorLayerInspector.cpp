// EditorLayerのInspector描画（全コンポーネント）とAdd Componentメニュー、マテリアルインスペクタをまとめた翻訳単位です。
#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Editor/EditorLayerShared.h"
#include "LamaPon/Editor/DataAssetSchema.h"

#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/CharacterControllerComponent.h"
#include "LamaPon/Components/CircleCollider2DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/Light2DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/InputMoverComponent.h"
#include "LamaPon/Components/JointComponent.h"
#include "LamaPon/Components/LODGroupComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Components/NavMeshAgentComponent.h"
#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/SpriteParticles2DComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/BillboardComponent.h"
#include "LamaPon/Components/RotatorComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/SpriteAnimatorComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/SpriteMaskComponent.h"
#include "LamaPon/Components/ParallaxLayerComponent.h"
#include "LamaPon/Components/TextRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Physics/PhysicsSettings.h"
#include "LamaPon/Scripting/GameModuleHost.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace LamaPon::EditorDetail;

namespace
{
    constexpr float InspectorWidth = 360.0f;

    // レイヤーの表示名（"番号: 名前"、無名なら番号だけ）。
    [[nodiscard]] std::string CollisionLayerLabel(
        const std::array<
            std::string,
            LamaPon::CollisionLayerCount>& names,
        const std::uint32_t layerIndex)
    {
        std::string label = std::to_string(layerIndex);
        if (!names[layerIndex & 31u].empty())
        {
            label += ": ";
            label += names[layerIndex & 31u];
        }
        return label;
    }

    // コライダーの「レイヤー」をドロップダウンで選びます。
    // プロジェクト設定で付けた名前が出ます（無名の番号も選べます——
    // 名前を付ける前に組んだシーンを壊さないため）。
    [[nodiscard]] bool DrawCollisionLayerCombo(
        const char* label,
        const std::array<
            std::string,
            LamaPon::CollisionLayerCount>& names,
        std::uint32_t& layer)
    {
        bool changed = false;
        const std::string preview =
            CollisionLayerLabel(names, layer & 31u);
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            for (std::uint32_t index = 0;
                index < LamaPon::CollisionLayerCount;
                ++index)
            {
                const bool selected = index == layer;
                if (ImGui::Selectable(
                        CollisionLayerLabel(
                            names,
                            index).c_str(),
                        selected))
                {
                    layer = index;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // コライダーの「衝突マスク」をチェックリストで編集します。
    // 一覧に出すのは名前の付いたレイヤーだけです（既定のマスクは
    // 全ビットONなので、無名の32個を全部並べると升目で埋まるため）。
    // 無名レイヤーのビットはここでは触らず、そのまま残ります。
    [[nodiscard]] bool DrawCollisionMaskCombo(
        const char* label,
        const std::array<
            std::string,
            LamaPon::CollisionLayerCount>& names,
        std::uint32_t& mask)
    {
        bool changed = false;
        std::string preview;
        if (mask == 0xFFFFFFFFu)
        {
            preview = "すべて";
        }
        else if (mask == 0u)
        {
            preview = "なし";
        }
        else
        {
            int enabled = 0;
            for (std::uint32_t index = 0;
                index < LamaPon::CollisionLayerCount;
                ++index)
            {
                enabled +=
                    (mask >> index) & 1u;
            }
            preview = std::to_string(enabled) + "個";
        }
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable(
                "すべて",
                false,
                ImGuiSelectableFlags_NoAutoClosePopups))
            {
                mask = 0xFFFFFFFFu;
                changed = true;
            }
            if (ImGui::Selectable(
                "なし",
                false,
                ImGuiSelectableFlags_NoAutoClosePopups))
            {
                mask = 0u;
                changed = true;
            }
            ImGui::Separator();
            for (std::uint32_t index = 0;
                index < LamaPon::CollisionLayerCount;
                ++index)
            {
                if (index != 0
                    && names[index].empty())
                {
                    continue;
                }
                bool on = ((mask >> index) & 1u) != 0;
                if (ImGui::Checkbox(
                    CollisionLayerLabel(
                        names,
                        index).c_str(),
                    &on))
                {
                    if (on)
                    {
                        mask |= (1u << index);
                    }
                    else
                    {
                        mask &= ~(1u << index);
                    }
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    const char* ShapeName(const LamaPon::PrimitiveShape shape) noexcept
    {
        switch (shape)
        {
        case LamaPon::PrimitiveShape::Cube:
            return "立方体";
        case LamaPon::PrimitiveShape::Sphere:
            return "球";
        case LamaPon::PrimitiveShape::Cylinder:
            return "円柱";
        case LamaPon::PrimitiveShape::Plane:
            return "平面";
        default:
            return "不明";
        }
    }

    std::string TruncatePrefabValue(
        std::string value)
    {
        constexpr std::size_t MaximumLength = 120;
        if (value.size() > MaximumLength)
        {
            value.resize(MaximumLength);
            value += "...";
        }
        return value;
    }

    struct NativePropertyEditResult final
    {
        bool hasSchema{};
        bool changed{};
        bool committed{};
        std::string error;
    };

    // 値の置き場所（objectのキー、またはarrayの添字）を1つに
    // まとめた参照です。同じ描画コードを、公開プロパティ本体と
    // listの要素の両方へ使えるようにするためのものです。
    struct SchemaValueSlot final
    {
        nlohmann::json* container{};
        std::string key;
        std::size_t index{};
        bool isArrayElement{};

        [[nodiscard]] bool Has() const
        {
            if (container == nullptr)
            {
                return false;
            }
            return isArrayElement
                ? container->is_array()
                    && index < container->size()
                : container->is_object()
                    && container->contains(key);
        }

        [[nodiscard]] const nlohmann::json& Get() const
        {
            return isArrayElement
                ? container->at(index)
                : container->at(key);
        }

        [[nodiscard]] nlohmann::json& GetOrCreate(
            nlohmann::json fallback) const
        {
            if (!Has())
            {
                Set(std::move(fallback));
            }
            return isArrayElement
                ? container->at(index)
                : container->at(key);
        }

        void Set(nlohmann::json value) const
        {
            if (container == nullptr)
            {
                return;
            }
            if (isArrayElement)
            {
                if (!container->is_array())
                {
                    *container = nlohmann::json::array();
                }
                while (container->size() <= index)
                {
                    container->push_back(nlohmann::json{});
                }
                container->at(index) = std::move(value);
                return;
            }
            (*container)[key] = std::move(value);
        }
    };

    // アセット参照欄（type: "asset"）の描画はアセット一覧を持つ
    // EditorLayer側へ委ねます。スキーマの解釈はここで完結させ、
    // 一覧とドラッグ＆ドロップだけを外から差し込む形です。
    using AssetFieldDrawer = std::function<bool(
        const char* controlId,
        const nlohmann::json& field,
        std::string& value)>;

    struct SchemaFieldEdit final
    {
        bool changed{};
        bool committed{};
    };

    SchemaFieldEdit DrawSchemaFields(
        nlohmann::json& properties,
        const nlohmann::json& fields,
        const std::string& idPrefix,
        const AssetFieldDrawer& drawAssetField);

    // 1フィールドぶんの入力欄です。値の読み書きはslot経由で行うため、
    // listの要素にも同じコードをそのまま使えます。
    SchemaFieldEdit DrawSchemaValue(
        const SchemaValueSlot& slot,
        const nlohmann::json& field,
        const std::string& controlId,
        const std::string& type,
        const AssetFieldDrawer& drawAssetField)
    {
        SchemaFieldEdit result;
        bool itemChanged{};
        bool immediateCommit{};

        if (type == "bool")
        {
            bool value = field.value("default", false);
            if (slot.Has() && slot.Get().is_boolean())
            {
                value = slot.Get().get<bool>();
            }
            itemChanged = ImGui::Checkbox(
                controlId.c_str(),
                &value);
            if (itemChanged)
            {
                slot.Set(value);
                immediateCommit = true;
            }
        }
        else if (type == "int")
        {
            int value = field.value("default", 0);
            if (slot.Has()
                && slot.Get().is_number_integer())
            {
                value = slot.Get().get<int>();
            }
            if (field.contains("min")
                && field.contains("max"))
            {
                const int minimum = field.at("min").get<int>();
                const int maximum = field.at("max").get<int>();
                itemChanged = ImGui::SliderInt(
                    controlId.c_str(),
                    &value,
                    minimum,
                    maximum);
            }
            else
            {
                const float step = field.value("step", 1.0f);
                itemChanged = ImGui::DragInt(
                    controlId.c_str(),
                    &value,
                    step);
            }
            if (itemChanged)
            {
                slot.Set(value);
            }
        }
        else if (type == "float")
        {
            float value = field.value("default", 0.0f);
            if (slot.Has() && slot.Get().is_number())
            {
                value = slot.Get().get<float>();
            }
            if (field.contains("min")
                && field.contains("max"))
            {
                const float minimum =
                    field.at("min").get<float>();
                const float maximum =
                    field.at("max").get<float>();
                itemChanged = ImGui::SliderFloat(
                    controlId.c_str(),
                    &value,
                    minimum,
                    maximum);
            }
            else
            {
                const float step = field.value("step", 0.1f);
                itemChanged = ImGui::DragFloat(
                    controlId.c_str(),
                    &value,
                    step);
            }
            if (itemChanged)
            {
                slot.Set(value);
            }
        }
        else if (type == "string")
        {
            std::string value = field.value(
                "default",
                std::string{});
            if (slot.Has() && slot.Get().is_string())
            {
                value = slot.Get().get<std::string>();
            }
            std::array<char, 512> buffer{};
            strncpy_s(
                buffer.data(),
                buffer.size(),
                value.c_str(),
                _TRUNCATE);
            itemChanged = ImGui::InputText(
                controlId.c_str(),
                buffer.data(),
                buffer.size());
            if (itemChanged)
            {
                slot.Set(std::string{ buffer.data() });
            }
        }
        else if (type == "asset")
        {
            std::string value = field.value(
                "default",
                std::string{});
            if (slot.Has() && slot.Get().is_string())
            {
                value = slot.Get().get<std::string>();
            }
            if (drawAssetField)
            {
                itemChanged = drawAssetField(
                    controlId.c_str(),
                    field,
                    value);
                if (itemChanged)
                {
                    slot.Set(value);
                    immediateCommit = true;
                }
            }
            else
            {
                ImGui::TextDisabled(
                    "%s",
                    value.empty()
                        ? "なし"
                        : value.c_str());
            }
        }
        else if (type == "vec2"
            || type == "vec3"
            || type == "vec4"
            || type == "color3"
            || type == "color4")
        {
            const std::size_t count =
                SchemaComponentCount(type);
            std::array<float, 4> value{
                0.0f,
                0.0f,
                0.0f,
                type.starts_with("color") ? 1.0f : 0.0f
            };
            const auto readArray =
                [&value, count](const nlohmann::json& array)
                {
                    if (!array.is_array())
                    {
                        return;
                    }
                    for (std::size_t index = 0;
                        index < count && index < array.size();
                        ++index)
                    {
                        if (array.at(index).is_number())
                        {
                            value[index] =
                                array.at(index).get<float>();
                        }
                    }
                };
            if (field.contains("default"))
            {
                readArray(field.at("default"));
            }
            if (slot.Has())
            {
                readArray(slot.Get());
            }

            if (type == "color3")
            {
                itemChanged = ImGui::ColorEdit3(
                    controlId.c_str(),
                    value.data());
            }
            else if (type == "color4")
            {
                itemChanged = ImGui::ColorEdit4(
                    controlId.c_str(),
                    value.data());
            }
            else if (count == 2)
            {
                itemChanged = ImGui::InputFloat2(
                    controlId.c_str(),
                    value.data());
            }
            else if (count == 3)
            {
                itemChanged = ImGui::InputFloat3(
                    controlId.c_str(),
                    value.data());
            }
            else
            {
                itemChanged = ImGui::InputFloat4(
                    controlId.c_str(),
                    value.data());
            }
            if (itemChanged)
            {
                auto array = nlohmann::json::array();
                for (std::size_t index = 0;
                    index < count;
                    ++index)
                {
                    array.push_back(value[index]);
                }
                slot.Set(std::move(array));
            }
        }
        else if (type == "object")
        {
            auto& child = slot.GetOrCreate(
                SchemaDefaultValue(field));
            if (!child.is_object())
            {
                child = nlohmann::json::object();
            }
            if (field.contains("fields")
                && field.at("fields").is_array())
            {
                ImGui::Indent();
                const auto edit = DrawSchemaFields(
                    child,
                    field.at("fields"),
                    controlId,
                    drawAssetField);
                ImGui::Unindent();
                itemChanged = edit.changed;
                immediateCommit = edit.committed;
            }
        }
        else
        {
            ImGui::TextDisabled(
                "未対応の型「%s」",
                type.c_str());
        }

        result.changed = itemChanged;
        result.committed = immediateCommit
            || ImGui::IsItemDeactivatedAfterEdit();
        return result;
    }

    // 可変長の並び（type: "list"）です。要素の型は"item"で指定し、
    // 数値・文字列だけでなく"object"（フィールドの集まり）も
    // 入れられます。
    SchemaFieldEdit DrawSchemaList(
        nlohmann::json& properties,
        const std::string& name,
        const nlohmann::json& field,
        const std::string& controlId,
        const AssetFieldDrawer& drawAssetField)
    {
        SchemaFieldEdit result;
        static const nlohmann::json defaultItem =
            nlohmann::json{ { "type", "float" } };
        const auto& item = field.contains("item")
                && field.at("item").is_object()
            ? field.at("item")
            : defaultItem;
        const std::string itemType = item.contains("type")
                && item.at("type").is_string()
            ? Lowercase(item.at("type").get<std::string>())
            : std::string{ "float" };

        if (!properties.contains(name)
            || !properties.at(name).is_array())
        {
            properties[name] = nlohmann::json::array();
        }
        auto& values = properties.at(name);

        if (ImGui::SmallButton(
                (std::string{ "＋ 追加##" } + controlId)
                    .c_str()))
        {
            values.push_back(SchemaDefaultValue(item));
            result.changed = true;
            result.committed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%zu件",
            values.size());

        constexpr std::size_t NoRemoval =
            static_cast<std::size_t>(-1);
        std::size_t removeIndex = NoRemoval;
        for (std::size_t index = 0;
            index < values.size();
            ++index)
        {
            const std::string elementId = controlId
                + "_"
                + std::to_string(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::SmallButton("－"))
            {
                removeIndex = index;
            }
            ImGui::SameLine();
            ImGui::Text("%zu", index);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);

            const SchemaValueSlot slot{
                &values,
                {},
                index,
                true
            };
            SchemaFieldEdit edit;
            if (itemType == "object")
            {
                ImGui::NewLine();
                auto& element = slot.GetOrCreate(
                    SchemaDefaultValue(item));
                if (!element.is_object())
                {
                    element = nlohmann::json::object();
                }
                if (item.contains("fields")
                    && item.at("fields").is_array())
                {
                    ImGui::Indent();
                    edit = DrawSchemaFields(
                        element,
                        item.at("fields"),
                        elementId,
                        drawAssetField);
                    ImGui::Unindent();
                }
            }
            else
            {
                edit = DrawSchemaValue(
                    slot,
                    item,
                    "##" + elementId,
                    itemType,
                    drawAssetField);
            }
            ImGui::PopID();
            result.changed = result.changed || edit.changed;
            result.committed =
                result.committed || edit.committed;
        }

        if (removeIndex != NoRemoval
            && removeIndex < values.size())
        {
            values.erase(
                values.begin()
                + static_cast<std::ptrdiff_t>(removeIndex));
            result.changed = true;
            result.committed = true;
        }
        return result;
    }

    SchemaFieldEdit DrawSchemaFields(
        nlohmann::json& properties,
        const nlohmann::json& fields,
        const std::string& idPrefix,
        const AssetFieldDrawer& drawAssetField)
    {
        SchemaFieldEdit result;
        std::size_t fieldIndex{};
        for (const auto& field : fields)
        {
            if (!field.is_object()
                || !field.contains("name")
                || !field.at("name").is_string()
                || !field.contains("type")
                || !field.at("type").is_string())
            {
                ++fieldIndex;
                continue;
            }

            const std::string name =
                field.at("name").get<std::string>();
            if (name.empty())
            {
                ++fieldIndex;
                continue;
            }
            const std::string type = Lowercase(
                field.at("type").get<std::string>());
            const std::string displayName = field.value(
                "displayName",
                name);
            const std::string controlId = idPrefix
                + "_"
                + std::to_string(fieldIndex)
                + "_"
                + name;

            ImGui::TextUnformatted(displayName.c_str());
            ImGui::SetNextItemWidth(-1.0f);

            SchemaFieldEdit edit;
            if (type == "list")
            {
                edit = DrawSchemaList(
                    properties,
                    name,
                    field,
                    controlId,
                    drawAssetField);
            }
            else
            {
                const SchemaValueSlot slot{
                    &properties,
                    name,
                    0,
                    false
                };
                edit = DrawSchemaValue(
                    slot,
                    field,
                    "##" + controlId,
                    type,
                    drawAssetField);
            }

            if (field.contains("tooltip")
                && field.at("tooltip").is_string()
                && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "%s",
                    field.at("tooltip")
                        .get_ref<const std::string&>()
                        .c_str());
            }
            result.changed = result.changed || edit.changed;
            result.committed =
                result.committed || edit.committed;
            ++fieldIndex;
        }
        return result;
    }

    NativePropertyEditResult DrawNativeScriptProperties(
        nlohmann::json& properties,
        const std::string_view schemaJson,
        const AssetFieldDrawer& drawAssetField = {})
    {
        NativePropertyEditResult result;
        if (schemaJson.empty())
        {
            return result;
        }

        try
        {
            const auto schema = nlohmann::json::parse(schemaJson);
            if (!schema.is_object()
                || !schema.contains("fields")
                || !schema.at("fields").is_array())
            {
                result.error =
                    "Inspectorスキーマのfields配列が見つかりません";
                return result;
            }

            result.hasSchema = true;
            const auto edit = DrawSchemaFields(
                properties,
                schema.at("fields"),
                "NativeProperty",
                drawAssetField);
            result.changed = edit.changed;
            result.committed = edit.committed;
        }
        catch (const std::exception& exception)
        {
            result.hasSchema = false;
            result.error = exception.what();
        }
        return result;
    }

    const char* ComponentDisplayName(const std::string_view typeName) noexcept
    {
        if (typeName == "Camera")
        {
            return "Camera";
        }
        if (typeName == "DirectionalLight")
        {
            return "Directional Light";
        }
        if (typeName == "PointLight")
        {
            return "Point Light";
        }
        if (typeName == "SpotLight")
        {
            return "Spot Light";
        }
        if (typeName == "Light2D")
        {
            return "Light 2D";
        }
        if (typeName == "RenderCulling")
        {
            return "描画カリング";
        }
        if (typeName == "ReflectionProbe")
        {
            return "リフレクションプローブ";
        }
        if (typeName == "BoxCollider2D")
        {
            return "Box Collider 2D";
        }
        if (typeName == "CircleCollider2D")
        {
            return "Circle Collider 2D";
        }
        if (typeName == "PolygonCollider2D")
        {
            return "Polygon Collider 2D";
        }
        if (typeName == "BoxCollider3D")
        {
            return "Box Collider 3D";
        }
        if (typeName == "CapsuleCollider3D")
        {
            return "Capsule Collider 3D";
        }
        if (typeName == "SphereCollider3D")
        {
            return "Sphere Collider 3D";
        }
        if (typeName == "CharacterController")
        {
            return "Character Controller";
        }
        if (typeName == "MeshRenderer")
        {
            return "Mesh Renderer";
        }
        if (typeName == "ModelRenderer")
        {
            return "Model Renderer";
        }
        if (typeName == "SpriteRenderer")
        {
            return "Sprite Renderer";
        }
        if (typeName == "SpriteMask")
        {
            return "Sprite Mask";
        }
        if (typeName == "SpriteAnimator")
        {
            return "Sprite Animator";
        }
        if (typeName == "NavMesh")
        {
            return "NavMesh Surface";
        }
        if (typeName == "NavMeshAgent")
        {
            return "NavMesh Agent";
        }
        if (typeName == "ParticleSystem")
        {
            return "Particle System";
        }
        if (typeName == "SpriteParticles2D")
        {
            return "2D Sprite Particles";
        }
        if (typeName == "ConvexHullCollider3D")
        {
            return "Convex Hull Collider";
        }
        if (typeName == "MeshCollider3D")
        {
            return "Mesh Collider";
        }
        if (typeName == "UICanvas")
        {
            return "UI Canvas";
        }
        if (typeName == "UIRectTransform")
        {
            return "UI Rect Transform";
        }
        if (typeName == "UIButton")
        {
            return "UI Button";
        }
        if (typeName == "UIImage")
        {
            return "UI Image";
        }
        if (typeName == "UIToggle")
        {
            return "UI Toggle";
        }
        if (typeName == "UISlider")
        {
            return "UI Slider";
        }
        if (typeName == "UIInputField")
        {
            return "UI Input Field";
        }
        if (typeName == "UILayoutGroup")
        {
            return "UI Layout Group";
        }
        if (typeName == "UIScrollView")
        {
            return "UI Scroll View";
        }
        if (typeName == "Tilemap")
        {
            return "Tilemap";
        }
        if (typeName == "ParallaxLayer")
        {
            return "Parallax Layer";
        }
        if (typeName == "TextRenderer")
        {
            return "Text Renderer";
        }
        if (typeName == "AudioSource")
        {
            return "Audio Source";
        }
        if (typeName == "AudioListener")
        {
            return "Audio Listener";
        }
        if (typeName == "Rotator")
        {
            return "Rotator";
        }
        if (typeName == "TransformAnimator")
        {
            return "Transform Animator";
        }
        if (typeName == "Rigidbody")
        {
            return "Rigidbody";
        }
        if (typeName == "Joint")
        {
            return "Joint";
        }
        if (typeName == "LODGroup")
        {
            return "LOD Group";
        }
        if (typeName == "InputMover")
        {
            return "Input Mover";
        }
        return typeName.data();
    }
}

namespace LamaPon
{
    std::string EditorLayer::InspectorFloatFormat() const
    {
        const auto decimals = std::min(
            m_projectSettings.inspectorDecimals,
            6u);
        return "%." + std::to_string(decimals) + "f";
    }

    void EditorLayer::LoadMaterialInspectorDraft()
    {
        m_materialInspectorAsset = m_selectedAsset;
        m_materialInspectorLoaded = false;
        m_materialInspectorDirty = false;
        m_materialInspectorError.clear();

        if (!IsMaterialAsset(m_materialInspectorAsset))
        {
            return;
        }

        try
        {
            m_materialInspectorDraft = LoadLitMaterialAsset(
                m_graphics.Assets().ResolvePath(
                    m_materialInspectorAsset),
                &m_graphics.Assets().Database(),
                &m_graphics.Assets());
            m_materialInspectorLoaded = true;
        }
        catch (const std::exception& exception)
        {
            m_materialInspectorError = exception.what();
        }
    }

    void EditorLayer::LoadModelInspectorDraft()
    {
        m_modelInspectorAsset = m_selectedAsset;
        m_modelInspectorLoaded = false;
        m_modelInspectorDirty = false;
        m_modelInspectorError.clear();
        m_modelImportScaleDraft = 1.0f;

        if (!IsFbxAsset(m_modelInspectorAsset))
        {
            return;
        }

        try
        {
            const std::filesystem::path metaPath(
                m_graphics.Assets()
                    .ResolvePath(m_modelInspectorAsset)
                    .wstring()
                + L".meta");
            if (std::filesystem::is_regular_file(metaPath))
            {
                std::ifstream input(metaPath, std::ios::binary);
                if (input)
                {
                    nlohmann::json document;
                    input >> document;
                    m_modelImportScaleDraft =
                        static_cast<float>(
                            document.value(
                                "importScale",
                                1.0));
                }
            }
            m_modelInspectorLoaded = true;
        }
        catch (const std::exception& exception)
        {
            m_modelInspectorError = exception.what();
        }
    }

    void EditorLayer::DrawModelAssetInspector()
    {
        if (m_modelInspectorAsset != m_selectedAsset)
        {
            LoadModelInspectorDraft();
        }

        const std::string assetName = PathToUtf8(
            m_selectedAsset.filename());
        ImGui::SeparatorText("Model Asset (FBX)");
        ImGui::TextWrapped("%s", assetName.c_str());
        ImGui::TextDisabled(
            "%s",
            PathToUtf8(m_selectedAsset).c_str());

        if (!m_modelInspectorError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_modelInspectorError.c_str());
            if (ImGui::Button("再読込"))
            {
                LoadModelInspectorDraft();
            }
            return;
        }
        if (!m_modelInspectorLoaded)
        {
            LoadModelInspectorDraft();
            if (!m_modelInspectorLoaded)
            {
                return;
            }
        }

        if (m_modelInspectorDirty)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.75f, 0.25f, 1.0f },
                "未保存の変更があります");
        }

        ImGui::SeparatorText("インポートスケール");
        ImGui::TextWrapped(
            "FBXファイル側の単位設定が実際のモデルサイズと"
            "食い違っている場合に、インポート時のスケールを"
            "補正できます（例: メートル単位で作られたモデルが"
            "誤ってセンチメートルとして書き出され、実寸の"
            "1/100でインポートされてしまう場合など）。"
            "ボーン・アニメーション付きモデルにも適用されます。");

        if (ImGui::DragFloat(
                "スケール",
                &m_modelImportScaleDraft,
                0.01f,
                0.0001f,
                10000.0f,
                "%.4f"))
        {
            m_modelImportScaleDraft = std::max(
                m_modelImportScaleDraft,
                0.0001f);
            m_modelInspectorDirty = true;
        }
        if (ImGui::Button(
                "100倍",
                ImVec2{ 88.0f, 0.0f }))
        {
            m_modelImportScaleDraft = 100.0f;
            m_modelInspectorDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(
                "既定(1.0)",
                ImVec2{ 88.0f, 0.0f }))
        {
            m_modelImportScaleDraft = 1.0f;
            m_modelInspectorDirty = true;
        }
        ImGui::TextDisabled(
            "cm→mの取り違えなら「100倍」を押してください。");

        const auto saveModelScale = [this]()
        {
            try
            {
                const std::filesystem::path metaPath(
                    m_graphics.Assets()
                        .ResolvePath(m_selectedAsset)
                        .wstring()
                    + L".meta");
                nlohmann::json document;
                {
                    std::ifstream input(
                        metaPath,
                        std::ios::binary);
                    if (input)
                    {
                        input >> document;
                    }
                }
                if (!document.is_object())
                {
                    document = nlohmann::json::object();
                }
                document["importScale"] =
                    m_modelImportScaleDraft;

                std::ofstream output(
                    metaPath,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    throw std::runtime_error(
                        "モデルのメタデータを書き込めませんでした: "
                        + PathToUtf8(m_selectedAsset));
                }
                output << document.dump(2) << '\n';
                output.close();
                if (!output)
                {
                    throw std::runtime_error(
                        "モデルのメタデータを書き込めませんでした: "
                        + PathToUtf8(m_selectedAsset));
                }

                ReloadSharedModel(m_selectedAsset);
                m_modelInspectorDirty = false;
                m_modelInspectorError.clear();
                SetStatus(
                    "インポートスケールを保存しました: "
                    + PathToUtf8(m_selectedAsset));
                return true;
            }
            catch (const std::exception& exception)
            {
                m_modelInspectorError = exception.what();
                SetStatus(exception.what(), true);
                return false;
            }
        };

        ImGui::Separator();
        if (ImGui::Button(
                "保存",
                ImVec2{ 96.0f, 0.0f }))
        {
            static_cast<void>(saveModelScale());
        }
        ImGui::SameLine();
        if (ImGui::Button("変更を戻す"))
        {
            LoadModelInspectorDraft();
            SetStatus("インポートスケールの変更を戻しました");
        }
    }

    bool EditorLayer::DrawShaderAssetSelector(
        const char* label,
        std::filesystem::path& shaderPath)
    {
        std::vector<std::filesystem::path> shaders;
        for (const auto& asset : m_assetFiles)
        {
            if (!IsShaderAsset(asset))
            {
                continue;
            }

            // 選択肢から外すのは「マテリアル／スプライトの
            // Shaderとして使えないもの」です。判定基準は
            // 「VSMain/PSMainを持たないか、エンジンが専用に
            // 差し替えて使うか」。エンジンへShaderを足したときは
            // ここも見てください（名前で外しているので自動では
            // 増えません）。
            //   LamaPonLit         … エンジンが標準として直接使う
            //                        （「LamaPon Lit (Default)」が
            //                        その入口なので二重に出さない）
            //   LamaPonEnvironment … 空・Bloom・トーンマップなどの
            //                        全画面パス専用
            //   LamaPonLightCulling… Compute Shader専用（CSMainだけ)。
            //                        選ぶとVSMain/PSMainが無くて
            //                        コンパイルに失敗する
            //   LamaPonShaderError … コンパイルに失敗したときの
            //   LamaPonSpriteError   代役（3D／2D）。自分で選べて
            //                        しまうと「壊れている印」が
            //                        意味を失う
            const std::string filename = Lowercase(
                LamaPon::PathToUtf8(asset.filename()));
            if (filename == "lamaponlit.hlsl"
                || filename == "lamaponenvironment.hlsl"
                || filename == "lamaponlightculling.hlsl"
                || IsShaderErrorPlaceholder(asset))
            {
                continue;
            }
            shaders.push_back(asset);
        }
        std::ranges::sort(
            shaders,
            [](const auto& left, const auto& right)
            {
                return Lowercase(PathToUtf8(left))
                    < Lowercase(PathToUtf8(right));
            });

        const std::string preview = shaderPath.empty()
            ? "LamaPon Lit (Default)"
            : PathToUtf8(shaderPath);
        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            const bool standardSelected = shaderPath.empty();
            if (ImGui::Selectable(
                    "LamaPon Lit (Default)",
                    standardSelected))
            {
                shaderPath.clear();
                changed = true;
            }
            if (standardSelected)
            {
                ImGui::SetItemDefaultFocus();
            }

            for (const auto& shader : shaders)
            {
                const std::string shaderLabel =
                    PathToUtf8(shader);
                const bool selected = IsSameAssetReference(
                    shaderPath,
                    shader);
                if (ImGui::Selectable(
                        shaderLabel.c_str(),
                        selected))
                {
                    shaderPath = shader;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool EditorLayer::DrawTextureAssetSelector(
        const char* label,
        std::filesystem::path& texturePath)
    {
        std::vector<std::filesystem::path> textures;
        for (const auto& asset : m_assetFiles)
        {
            if (IsTextureAsset(asset))
            {
                textures.push_back(asset);
            }
        }
        std::ranges::sort(
            textures,
            [](const auto& left, const auto& right)
            {
                return Lowercase(PathToUtf8(left))
                    < Lowercase(PathToUtf8(right));
            });

        const std::string preview = texturePath.empty()
            ? "None"
            : PathToUtf8(texturePath);
        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            const bool noneSelected = texturePath.empty();
            if (ImGui::Selectable("None", noneSelected))
            {
                texturePath.clear();
                changed = true;
            }
            if (noneSelected)
            {
                ImGui::SetItemDefaultFocus();
            }

            for (const auto& texture : textures)
            {
                const std::string textureLabel =
                    PathToUtf8(texture);
                const bool selected = IsSameAssetReference(
                    texturePath,
                    texture);
                if (ImGui::Selectable(
                        textureLabel.c_str(),
                        selected))
                {
                    texturePath = texture;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool EditorLayer::DrawCubemapAssetSelector(
        const char* label,
        std::filesystem::path& cubemapPath)
    {
        const std::string preview = cubemapPath.empty()
            ? "なし"
            : PathToUtf8(cubemapPath);
        bool changed = false;
        if (!ImGui::BeginCombo(label, preview.c_str()))
        {
            return false;
        }

        const bool noneSelected = cubemapPath.empty();
        if (ImGui::Selectable("なし", noneSelected))
        {
            cubemapPath.clear();
            changed = true;
        }
        if (noneSelected)
        {
            ImGui::SetItemDefaultFocus();
        }

        for (const auto& asset : m_assetFiles)
        {
            if (Lowercase(
                    PathToUtf8(asset.extension()))
                != ".dds")
            {
                continue;
            }

            // DDSでも2DテクスチャはSkyboxへ使えないため、候補から
            // 除外します。LoadTextureのキャッシュを利用するので、
            // コンボを開いたときだけ検証しても負荷は増えません。
            try
            {
                const auto texture =
                    m_graphics.Assets().LoadTexture(asset);
                if (texture == nullptr || !texture->isCube)
                {
                    continue;
                }
            }
            catch (const std::exception&)
            {
                continue;
            }

            const std::string assetLabel = PathToUtf8(asset);
            const bool selected = IsSameAssetReference(
                cubemapPath,
                asset);
            if (ImGui::Selectable(
                    assetLabel.c_str(),
                    selected))
            {
                cubemapPath = asset;
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
        return changed;
    }

    void EditorLayer::DrawMaterialAssetInspector()
    {
        if (m_materialInspectorAsset != m_selectedAsset)
        {
            LoadMaterialInspectorDraft();
        }

        const std::string assetName = PathToUtf8(
            m_selectedAsset.filename());
        ImGui::SeparatorText("Material Asset");
        ImGui::TextWrapped("%s", assetName.c_str());
        ImGui::TextDisabled(
            "%s",
            PathToUtf8(m_selectedAsset).c_str());

        if (!m_materialInspectorError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_materialInspectorError.c_str());
            if (ImGui::Button("再読込"))
            {
                LoadMaterialInspectorDraft();
            }
            return;
        }
        if (!m_materialInspectorLoaded)
        {
            LoadMaterialInspectorDraft();
            if (!m_materialInspectorLoaded)
            {
                return;
            }
        }

        if (m_materialInspectorDirty)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.75f, 0.25f, 1.0f },
                "未保存の変更があります");
        }

        ImGui::BeginDisabled(m_playing);

        ImGui::SeparatorText("Surface");
        auto baseColor =
            m_materialInspectorDraft.BaseColor();
        if (ImGui::ColorEdit4(
                "Base Color",
                &baseColor.x))
        {
            m_materialInspectorDraft.SetBaseColor(baseColor);
            m_materialInspectorDirty = true;
        }

        float roughness =
            m_materialInspectorDraft.Roughness();
        if (ImGui::SliderFloat(
                "Roughness",
                &roughness,
                0.02f,
                1.0f,
                "%.2f"))
        {
            m_materialInspectorDraft.SetRoughness(roughness);
            m_materialInspectorDirty = true;
        }

        float metallic =
            m_materialInspectorDraft.Metallic();
        if (ImGui::SliderFloat(
                "Metallic",
                &metallic,
                0.0f,
                1.0f,
                "%.2f"))
        {
            m_materialInspectorDraft.SetMetallic(metallic);
            m_materialInspectorDirty = true;
        }

        float normalStrength =
            m_materialInspectorDraft.NormalStrength();
        if (ImGui::SliderFloat(
                "Normal Strength",
                &normalStrength,
                0.0f,
                2.0f,
                "%.2f"))
        {
            m_materialInspectorDraft.SetNormalStrength(
                normalStrength);
            m_materialInspectorDirty = true;
        }

        auto albedoTexture =
            m_materialInspectorDraft.AlbedoTexture();
        if (DrawTextureAssetSelector(
                "Albedo Texture",
                albedoTexture))
        {
            m_materialInspectorDraft.SetAlbedoTexture(
                albedoTexture);
            m_materialInspectorDirty = true;
        }

        auto normalTexture =
            m_materialInspectorDraft.NormalTexture();
        if (DrawTextureAssetSelector(
                "Normal Map",
                normalTexture))
        {
            m_materialInspectorDraft.SetNormalTexture(
                normalTexture);
            m_materialInspectorDirty = true;
        }

        ImGui::SeparatorText("Shader");
        auto shaderPath =
            m_materialInspectorDraft.Shader();
        if (DrawShaderAssetSelector(
                "Shader##MaterialAsset",
                shaderPath))
        {
            m_materialInspectorDraft.SetShader(shaderPath);
            m_materialInspectorDirty = true;
        }
        ImGui::TextDisabled(
            "標準Litまたは作成済みのMaterial Shaderを選択します。");

        if (!m_materialInspectorDraft.Shader().empty())
        {
            if (ImGui::Button("Shaderを開く"))
            {
                OpenCodeAsset(
                    m_materialInspectorDraft.Shader());
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("新規Shader..."))
        {
            OpenCreateShaderDialog(
                m_selectedAsset.parent_path());
        }

        if (!m_materialInspectorDraft.Shader().empty()
            && ImGui::TreeNodeEx(
                "Custom Parameters",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (DrawCustomShaderParameters(
                m_materialInspectorDraft.Shader(),
                "MaterialAssetParameters",
                [this](const std::size_t index)
                {
                    return m_materialInspectorDraft
                        .CustomParameter(index);
                },
                [this](
                    const std::size_t index,
                    const DirectX::XMFLOAT4& value)
                {
                    m_materialInspectorDraft
                        .SetCustomParameter(index, value);
                },
                [this](const std::size_t index)
                {
                    return m_materialInspectorDraft
                        .CustomTexture(index);
                },
                [this](
                    const std::size_t index,
                    std::filesystem::path path)
                {
                    m_materialInspectorDraft
                        .SetCustomTexture(
                            index,
                            std::move(path));
                }))
            {
                m_materialInspectorDirty = true;
            }
            ImGui::TreePop();
        }

        const auto saveMaterial = [this]()
        {
            try
            {
                SaveLitMaterialAsset(
                    m_graphics.Assets().ResolvePath(
                        m_selectedAsset),
                    m_materialInspectorDraft,
                    &m_graphics.Assets().Database());
                ReloadSharedMaterial(m_selectedAsset);
                m_materialInspectorDirty = false;
                m_materialInspectorError.clear();
                SetStatus(
                    "Materialを保存しました: "
                    + PathToUtf8(m_selectedAsset));
                return true;
            }
            catch (const std::exception& exception)
            {
                m_materialInspectorError = exception.what();
                SetStatus(exception.what(), true);
                return false;
            }
        };

        ImGui::Separator();
        if (ImGui::Button(
                "保存",
                ImVec2{ 96.0f, 0.0f }))
        {
            static_cast<void>(saveMaterial());
        }
        ImGui::SameLine();
        if (ImGui::Button("変更を戻す"))
        {
            LoadMaterialInspectorDraft();
            SetStatus("Materialの変更を戻しました");
        }

        auto* selectedObject =
            m_scene.FindGameObject(m_selectedObjectId);
        const bool canApply = selectedObject != nullptr
            && (selectedObject->GetComponent<
                    MeshRendererComponent>() != nullptr
                || selectedObject->GetComponent<
                    ModelRendererComponent>() != nullptr);
        ImGui::BeginDisabled(!canApply);
        if (ImGui::Button(
                m_materialInspectorDirty
                    ? "保存して選択オブジェクトへ適用"
                    : "選択オブジェクトへ適用",
                ImVec2{ -1.0f, 0.0f }))
        {
            if ((!m_materialInspectorDirty
                    || saveMaterial()))
            {
                AssignSelectedMaterial();
            }
        }
        ImGui::EndDisabled();
        if (!canApply)
        {
            ImGui::TextDisabled(
                "Mesh RendererまたはModel Rendererを持つオブジェクトを選択すると適用できます。");
        }

        ImGui::EndDisabled();
    }

    const std::string* EditorLayer::FindDataAssetSchema(
        const std::string_view typeName) const noexcept
    {
        const auto* host = GameModuleHost::Current();
        if (host == nullptr)
        {
            return nullptr;
        }
        const auto* type = host->FindDataAssetType(typeName);
        return type != nullptr
            ? &type->schemaJson
            : nullptr;
    }

    std::string EditorLayer::DataAssetTypeOfFile(
        const std::filesystem::path& path) const
    {
        const auto found = m_dataAssetTypeByPath.find(
            Lowercase(PathToUtf8(path)));
        return found != m_dataAssetTypeByPath.end()
            ? found->second
            : std::string{};
    }

    bool EditorLayer::DrawAssetReferenceField(
        const char* controlId,
        const nlohmann::json& field,
        std::string& value)
    {
        const std::string assetType =
            field.contains("assetType")
                && field.at("assetType").is_string()
            ? Lowercase(
                field.at("assetType").get<std::string>())
            : std::string{ "any" };
        const std::string dataType = field.value(
            "dataType",
            std::string{});

        const auto matches =
            [this, &assetType, &dataType](
                const std::filesystem::path& path)
            {
                if (assetType == "texture")
                {
                    return IsTextureAsset(path);
                }
                if (assetType == "prefab")
                {
                    return IsPrefabAsset(path);
                }
                if (assetType == "material")
                {
                    return IsMaterialAsset(path);
                }
                if (assetType == "scene")
                {
                    return IsSceneAsset(path);
                }
                if (assetType == "audio")
                {
                    return IsAudioAsset(path);
                }
                if (assetType == "animation")
                {
                    return IsAnimationAsset(path);
                }
                if (assetType == "animator")
                {
                    return IsAnimatorControllerAsset(path);
                }
                if (assetType == "model")
                {
                    return IsModelAsset(path);
                }
                if (assetType == "shader")
                {
                    return IsAssignableShaderAsset(path);
                }
                if (assetType == "data")
                {
                    return IsDataAsset(path)
                        && (dataType.empty()
                            || DataAssetTypeOfFile(path)
                                == dataType);
                }
                return true;
            };

        bool changed = false;
        const std::string preview = value.empty()
            ? std::string{ "なし" }
            : value;
        if (ImGui::BeginCombo(controlId, preview.c_str()))
        {
            if (ImGui::Selectable("なし", value.empty()))
            {
                value.clear();
                changed = true;
            }
            for (const auto& asset : m_assetFiles)
            {
                if (!matches(asset))
                {
                    continue;
                }
                const auto label = PathToUtf8(asset);
                const bool selected = Lowercase(label)
                    == Lowercase(value);
                if (ImGui::Selectable(
                        label.c_str(),
                        selected))
                {
                    value = label;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        // アセットウィンドウからのドラッグ＆ドロップでも差せます。
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto dropped = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (matches(dropped))
                {
                    value = PathToUtf8(dropped);
                    changed = true;
                }
                else
                {
                    SetStatus(
                        "この欄には割り当てられない種類の"
                        "アセットです",
                        true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        return changed;
    }

    void EditorLayer::LoadDataAssetInspectorDraft()
    {
        m_dataAssetInspectorAsset = m_selectedAsset;
        m_dataAssetInspectorDraft.reset();
        RefreshDataAssetJsonBuffer();
        m_dataAssetInspectorDirty = false;
        m_dataAssetInspectorError.clear();
        m_dataAssetInspectorTypeName.clear();

        if (!IsDataAsset(m_dataAssetInspectorAsset))
        {
            return;
        }

        try
        {
            const auto bytes =
                m_graphics.Assets().ReadFileBytes(
                    m_dataAssetInspectorAsset);
            auto document = nlohmann::json::object();
            if (!bytes.empty())
            {
                const auto* first =
                    reinterpret_cast<const char*>(
                        bytes.data());
                document = nlohmann::json::parse(
                    first,
                    first + bytes.size());
            }
            if (!document.is_object())
            {
                document = nlohmann::json::object();
            }
            if (!document.contains("values")
                || !document.at("values").is_object())
            {
                document["values"] =
                    nlohmann::json::object();
            }
            m_dataAssetInspectorTypeName = document.value(
                "type",
                std::string{});
            m_dataAssetInspectorDraft =
                std::make_unique<nlohmann::json>(
                    std::move(document));
            RefreshDataAssetJsonBuffer();
        }
        catch (const std::exception& exception)
        {
            m_dataAssetInspectorError = exception.what();
        }
    }

    void EditorLayer::RefreshDataAssetJsonBuffer()
    {
        m_dataAssetJsonBuffer.fill('\0');
        if (m_dataAssetInspectorDraft == nullptr)
        {
            return;
        }
        strncpy_s(
            m_dataAssetJsonBuffer.data(),
            m_dataAssetJsonBuffer.size(),
            m_dataAssetInspectorDraft->dump(2).c_str(),
            _TRUNCATE);
    }

    bool EditorLayer::SaveDataAssetInspectorDraft()
    {
        if (m_dataAssetInspectorDraft == nullptr)
        {
            return false;
        }
        try
        {
            (*m_dataAssetInspectorDraft)["format"] =
                "LamaPonDataAsset";
            (*m_dataAssetInspectorDraft)["version"] = 1;
            (*m_dataAssetInspectorDraft)["type"] =
                m_dataAssetInspectorTypeName;

            const auto destination =
                m_graphics.Assets().ResolvePath(
                    m_dataAssetInspectorAsset);
            std::ofstream output(
                destination,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "データアセットのファイルを開けませんでした");
            }
            output << m_dataAssetInspectorDraft->dump(2)
                   << '\n';
            output.close();
            if (!output)
            {
                throw std::runtime_error(
                    "データアセットを書き込めませんでした");
            }

            // 再生中のゲームが読み直せるよう、キャッシュを捨てます。
            m_graphics.Assets().Invalidate(
                m_dataAssetInspectorAsset);
            m_dataAssetTypeByPath.insert_or_assign(
                Lowercase(
                    PathToUtf8(m_dataAssetInspectorAsset)),
                m_dataAssetInspectorTypeName);
            m_dataAssetInspectorDirty = false;
            m_dataAssetInspectorError.clear();
            RefreshDataAssetJsonBuffer();
            SetStatus(
                "データアセットを保存しました: "
                + PathToUtf8(m_dataAssetInspectorAsset));
            return true;
        }
        catch (const std::exception& exception)
        {
            m_dataAssetInspectorError = exception.what();
            SetStatus(exception.what(), true);
            return false;
        }
    }

    void EditorLayer::DrawDataAssetInspector()
    {
        if (m_dataAssetInspectorAsset != m_selectedAsset
            || (m_dataAssetInspectorDraft == nullptr
                && m_dataAssetInspectorError.empty()))
        {
            LoadDataAssetInspectorDraft();
        }

        ImGui::SeparatorText("データアセット");
        ImGui::TextWrapped(
            "%s",
            PathToUtf8(m_selectedAsset.filename()).c_str());
        ImGui::TextDisabled(
            "%s",
            PathToUtf8(m_selectedAsset).c_str());

        if (!m_dataAssetInspectorError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_dataAssetInspectorError.c_str());
            if (ImGui::Button("再読込"))
            {
                LoadDataAssetInspectorDraft();
            }
            return;
        }
        if (m_dataAssetInspectorDraft == nullptr)
        {
            return;
        }

        if (m_dataAssetInspectorDirty)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.75f, 0.25f, 1.0f },
                "未保存の変更があります");
        }

        ImGui::BeginDisabled(m_playing);

        // 型。Game Moduleを直して型名を変えたときのために、
        // ここから付け替えられるようにしています。
        const auto* host = GameModuleHost::Current();
        const std::string typePreview =
            m_dataAssetInspectorTypeName.empty()
                ? std::string{ "（未設定）" }
                : m_dataAssetInspectorTypeName;
        if (ImGui::BeginCombo("型", typePreview.c_str()))
        {
            if (host != nullptr)
            {
                for (const auto& type :
                    host->RegisteredDataAssets())
                {
                    const bool selected = type.typeName
                        == m_dataAssetInspectorTypeName;
                    const std::string label =
                        type.displayName
                        + "  ("
                        + type.typeName
                        + ")";
                    if (ImGui::Selectable(
                            label.c_str(),
                            selected))
                    {
                        m_dataAssetInspectorTypeName =
                            type.typeName;
                        m_dataAssetInspectorDirty = true;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::EndCombo();
        }

        const auto* schema = FindDataAssetSchema(
            m_dataAssetInspectorTypeName);
        bool hasTypedSchema{};
        if (schema == nullptr)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.55f, 0.25f, 1.0f },
                "型「%s」の宣言が見つかりません",
                m_dataAssetInspectorTypeName.c_str());
            ImGui::TextWrapped(
                "Game Moduleがまだビルドされていないか、"
                "LAMAPON_DATA_ASSETの宣言が消えています。"
                "下のJSONは直接編集できます。");
        }
        else
        {
            ImGui::SeparatorText("値");
            auto& values =
                (*m_dataAssetInspectorDraft)["values"];
            const auto edit = DrawNativeScriptProperties(
                values,
                *schema,
                [this](
                    const char* controlId,
                    const nlohmann::json& field,
                    std::string& value)
                {
                    return DrawAssetReferenceField(
                        controlId,
                        field,
                        value);
                });
            hasTypedSchema = edit.hasSchema;
            if (!edit.error.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.55f, 0.25f, 1.0f },
                    "Inspectorスキーマ: %s",
                    edit.error.c_str());
            }
            if (edit.changed)
            {
                m_dataAssetInspectorDirty = true;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("保存", ImVec2{ 96.0f, 0.0f }))
        {
            static_cast<void>(SaveDataAssetInspectorDraft());
        }
        ImGui::SameLine();
        if (ImGui::Button("変更を戻す"))
        {
            LoadDataAssetInspectorDraft();
            SetStatus("データアセットの変更を戻しました");
        }

        const bool showRawJson = !hasTypedSchema
            || ImGui::CollapsingHeader("詳細設定 (JSON)");
        if (showRawJson)
        {
            if (ImGui::InputTextMultiline(
                    "##DataAssetJson",
                    m_dataAssetJsonBuffer.data(),
                    m_dataAssetJsonBuffer.size(),
                    ImVec2{ -1.0f, 160.0f }))
            {
                try
                {
                    auto document = nlohmann::json::parse(
                        m_dataAssetJsonBuffer.data());
                    if (document.is_object())
                    {
                        if (!document.contains("values")
                            || !document.at("values")
                                .is_object())
                        {
                            document["values"] =
                                nlohmann::json::object();
                        }
                        m_dataAssetInspectorTypeName =
                            document.value(
                                "type",
                                m_dataAssetInspectorTypeName);
                        *m_dataAssetInspectorDraft =
                            std::move(document);
                        m_dataAssetInspectorDirty = true;
                        m_dataAssetInspectorError.clear();
                    }
                }
                catch (const std::exception&)
                {
                    // 入力途中は壊れたJSONになるため、ここでは
                    // 何もしません（保存すると直前の正しい内容が
                    // 書かれます）。
                }
            }
        }

        ImGui::EndDisabled();
    }

    const ShaderProperties& EditorLayer::ShaderPropertiesFor(
        const std::filesystem::path& shaderPath)
    {
        static const ShaderProperties empty;
        if (shaderPath.empty())
        {
            return empty;
        }

        const auto resolved =
            m_graphics.Assets().ResolvePath(shaderPath);
        std::error_code error;
        const auto writeTime =
            std::filesystem::last_write_time(resolved, error);
        if (error)
        {
            return empty;
        }

        const auto key = resolved.wstring();
        const auto entry = m_shaderPropertiesCache.find(key);
        if (entry != m_shaderPropertiesCache.end()
            && entry->second.writeTime == writeTime)
        {
            return entry->second.properties;
        }

        // 保存し直されたら読み直します（Shaderのホットリロードと
        // 同じ感覚で、宣言の変更もすぐ反映されます）。
        CachedShaderProperties cached;
        cached.properties = LoadShaderProperties(resolved);
        cached.writeTime = writeTime;
        auto& stored = m_shaderPropertiesCache[key];
        stored = std::move(cached);
        return stored.properties;
    }

    bool EditorLayer::DrawShaderKeywordToggles(
        const std::filesystem::path& shaderPath,
        const char* identifier,
        const ShaderKeywordSet& current,
        const std::function<void(ShaderKeywordSet)>& setter)
    {
        if (shaderPath.empty() || !setter)
        {
            return false;
        }
        const auto& declaration =
            m_graphics.ShaderVariantsFor(shaderPath);
        if (!declaration.error.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "バリアント宣言の誤り: %s",
                declaration.error.c_str());
            return false;
        }
        if (declaration.Empty())
        {
            return false;
        }

        bool changed = false;
        ImGui::PushID(identifier);
        ImGui::SeparatorText("バリアント");
        auto next = current;
        for (const auto& group : declaration.groups)
        {
            for (const auto& keyword : group.keywords)
            {
                if (keyword.empty())
                {
                    // 「どれも立てない」枠はUIに出しません
                    // （全部のチェックを外した状態がそれです）。
                    continue;
                }
                bool enabled = next.IsEnabled(keyword);
                if (ImGui::Checkbox(
                        keyword.c_str(),
                        &enabled))
                {
                    if (enabled)
                    {
                        // 同じグループの他のキーワードは外します。
                        // 2つ以上立つとどちらの#defineも渡って
                        // しまい、シェーダー側の想定が崩れます。
                        for (const auto& other :
                            group.keywords)
                        {
                            if (!other.empty())
                            {
                                next.Disable(other);
                            }
                        }
                        next.Enable(keyword);
                    }
                    else
                    {
                        next.Disable(keyword);
                    }
                    changed = true;
                }
            }
        }
        if (changed)
        {
            setter(next);
        }
        ImGui::TextDisabled(
            "組み合わせごとに別々にコンパイルされます"
            "（現在%zu通り）",
            declaration.VariantCount());
        ImGui::PopID();
        return changed;
    }

    void EditorLayer::ApplyShaderPropertyDefaults(
        const std::filesystem::path& shaderPath,
        const std::function<void(
            std::size_t,
            const DirectX::XMFLOAT4&)>& setter)
    {
        if (shaderPath.empty() || !setter)
        {
            return;
        }
        const auto& properties =
            ShaderPropertiesFor(shaderPath);
        if (!properties.declared)
        {
            return;
        }
        // 宣言のある枠だけを、0から組み立て直して書き込みます。
        // 前に割り当てていたShaderの値が成分単位で残ると、意味の
        // 違う数字が混ざって原因不明の見え方になるためです。
        std::array<
            DirectX::XMFLOAT4,
            LitMaterial::CustomParameterCount> values{};
        std::array<
            bool,
            LitMaterial::CustomParameterCount> touched{};
        for (const auto& field : properties.fields)
        {
            if (!field.defaultValue.has_value()
                || field.parameterIndex >= values.size())
            {
                continue;
            }
            float* const target =
                &values[field.parameterIndex].x;
            for (std::size_t component = 0;
                component < field.componentCount;
                ++component)
            {
                target[field.components[component]] =
                    (*field.defaultValue)[component];
            }
            touched[field.parameterIndex] = true;
        }
        for (std::size_t index = 0;
            index < values.size();
            ++index)
        {
            if (touched[index])
            {
                setter(index, values[index]);
            }
        }
    }

    bool EditorLayer::DrawCustomShaderParameters(
        const std::filesystem::path& shaderPath,
        const char* identifier,
        const std::function<
            DirectX::XMFLOAT4(std::size_t)>& getter,
        const std::function<void(
            std::size_t,
            const DirectX::XMFLOAT4&)>& setter,
        const std::function<
            std::filesystem::path(std::size_t)>&
            textureGetter,
        const std::function<void(
            std::size_t,
            std::filesystem::path)>& textureSetter)
    {
        bool changed = false;
        ImGui::PushID(identifier);

        const auto& properties =
            ShaderPropertiesFor(shaderPath);
        if (!properties.error.empty())
        {
            ImGui::TextWrapped(
                "Shaderのパラメーター宣言に誤りがあります: %s",
                properties.error.c_str());
        }

        if (properties.declared
            && !properties.fields.empty())
        {
            for (std::size_t index = 0;
                index < properties.fields.size();
                ++index)
            {
                const auto& field = properties.fields[index];
                auto parameter =
                    getter(field.parameterIndex);
                float* const values = &parameter.x;
                // 宣言された成分だけを取り出して編集し、書き戻します。
                std::array<float, 4> editing{};
                for (std::size_t component = 0;
                    component < field.componentCount;
                    ++component)
                {
                    editing[component] =
                        values[field.components[component]];
                }

                ImGui::PushID(static_cast<int>(index));
                if (field.kind
                    == ShaderPropertyKind::Texture)
                {
                    // 追加テクスチャ枠。画像をドロップするか、
                    // Asset Browserで選んでから割り当てます。
                    const auto current = textureGetter
                        ? textureGetter(
                            field.parameterIndex)
                        : std::filesystem::path{};
                    ImGui::TextWrapped(
                        "%s: %s",
                        field.name.c_str(),
                        current.empty()
                            ? "未設定（白）"
                            : PathToUtf8(current).c_str());
                    const std::string dropLabel =
                        "画像をここへドロップ##CustomTexture"
                        + std::to_string(index);
                    ImGui::Button(
                        dropLabel.c_str(),
                        ImVec2{ -1.0f, 0.0f });
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(
                                AssetPayload))
                        {
                            const auto dropped =
                                PathFromUtf8(
                                    static_cast<
                                        const char*>(
                                            payload->Data));
                            if (IsTextureAsset(dropped)
                                && textureSetter)
                            {
                                textureSetter(
                                    field.parameterIndex,
                                    dropped);
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (IsTextureAsset(m_selectedAsset)
                        && textureSetter
                        && ImGui::Button(
                            "選択中の画像を割り当て"))
                    {
                        textureSetter(
                            field.parameterIndex,
                            m_selectedAsset);
                        changed = true;
                    }
                    if (!current.empty()
                        && textureSetter)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("解除"))
                        {
                            textureSetter(
                                field.parameterIndex,
                                {});
                            changed = true;
                        }
                    }
                    ImGui::PopID();
                    continue;
                }
                bool fieldChanged = false;
                switch (field.kind)
                {
                case ShaderPropertyKind::Color:
                    fieldChanged =
                        field.componentCount >= 4
                            ? ImGui::ColorEdit4(
                                field.name.c_str(),
                                editing.data())
                            : ImGui::ColorEdit3(
                                field.name.c_str(),
                                editing.data());
                    break;
                case ShaderPropertyKind::Boolean:
                {
                    bool enabled = editing[0] >= 0.5f;
                    fieldChanged = ImGui::Checkbox(
                        field.name.c_str(),
                        &enabled);
                    editing[0] = enabled ? 1.0f : 0.0f;
                    break;
                }
                case ShaderPropertyKind::Vector:
                    // 宣言された成分数だけ編集欄を出します。
                    fieldChanged = field.componentCount >= 4
                        ? ImGui::DragFloat4(
                            field.name.c_str(),
                            editing.data(),
                            0.01f)
                        : (field.componentCount == 3
                            ? ImGui::DragFloat3(
                                field.name.c_str(),
                                editing.data(),
                                0.01f)
                            : ImGui::DragFloat2(
                                field.name.c_str(),
                                editing.data(),
                                0.01f));
                    break;
                case ShaderPropertyKind::Float:
                default:
                    fieldChanged = field.hasRange
                        ? ImGui::SliderFloat(
                            field.name.c_str(),
                            editing.data(),
                            field.minimum,
                            field.maximum,
                            "%.3f")
                        : ImGui::DragFloat(
                            field.name.c_str(),
                            editing.data(),
                            0.01f);
                    break;
                }
                ImGui::PopID();

                if (fieldChanged)
                {
                    for (std::size_t component = 0;
                        component < field.componentCount;
                        ++component)
                    {
                        values[field.components[component]] =
                            editing[component];
                    }
                    setter(field.parameterIndex, parameter);
                    changed = true;
                }
            }

            if (ImGui::Button("既定値に戻す"))
            {
                for (const auto& field : properties.fields)
                {
                    if (!field.defaultValue.has_value())
                    {
                        continue;
                    }
                    auto parameter =
                        getter(field.parameterIndex);
                    float* const values = &parameter.x;
                    for (std::size_t component = 0;
                        component < field.componentCount;
                        ++component)
                    {
                        values[field.components[component]] =
                            (*field.defaultValue)[component];
                    }
                    setter(field.parameterIndex, parameter);
                }
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "Shaderの宣言から生成");
        }
        else
        {
            // 宣言が無いShaderは従来どおり生のfloat4を編集します。
            for (std::size_t index = 0;
                index < LitMaterial::CustomParameterCount;
                ++index)
            {
                auto parameter = getter(index);
                const std::string label =
                    "Parameter "
                    + std::to_string(index + 1);
                if (ImGui::DragFloat4(
                    label.c_str(),
                    &parameter.x,
                    0.01f))
                {
                    setter(index, parameter);
                    changed = true;
                }
            }
            if (!properties.declared)
            {
                ImGui::TextDisabled(
                    "Shaderへ LAMAPON_PROPERTIES を書くと、"
                    "名前付きの調整UIになります");
            }
        }

        ImGui::PopID();
        return changed;
    }

    RenderTexturePickerResult
        EditorLayer::DrawRenderTexturePicker(
            const char* id,
            const std::string& current)
    {
        RenderTexturePickerResult result;
        ImGui::PushID(id);

        std::array<char, 64> buffer{};
        strncpy_s(
            buffer.data(),
            buffer.size(),
            current.c_str(),
            _TRUNCATE);
        if (ImGui::InputTextWithHint(
                "Render Texture",
                "Cameraの描画先名（空で通常テクスチャ）",
                buffer.data(),
                buffer.size()))
        {
            result.value = std::string{ buffer.data() };
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            result.commit = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Cameraの「描画先（Render Texture）」に付けた名前を"
                "入れると、そのカメラの映像を表示します"
                "（ミニマップ・防犯カメラ・装備プレビュー）");
        }

        // シーン内のCameraが実際に使っている名前から選べます。
        std::vector<std::string> names;
        for (const auto& gameObject : m_scene.GameObjects())
        {
            const auto* camera =
                gameObject->GetComponent<CameraComponent>();
            if (camera == nullptr
                || !camera->RendersToTexture())
            {
                continue;
            }
            if (std::ranges::find(
                    names,
                    camera->TargetTexture())
                == names.end())
            {
                names.push_back(
                    camera->TargetTexture());
            }
        }
        if (names.empty())
        {
            ImGui::TextDisabled(
                "描画先を設定したCameraがありません");
        }
        else if (ImGui::BeginCombo(
            "Cameraから選ぶ",
            current.empty()
                ? "(未指定)"
                : current.c_str()))
        {
            if (ImGui::Selectable(
                "(未指定)",
                current.empty()))
            {
                result.value = std::string{};
                result.commit = true;
            }
            for (const auto& name : names)
            {
                if (ImGui::Selectable(
                    name.c_str(),
                    name == current))
                {
                    result.value = name;
                    result.commit = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::PopID();
        return result;
    }

    void EditorLayer::DrawInspector()
    {
        if (m_sceneEnvironmentOpen)
        {
            ImGui::SetNextWindowSize(
                ImVec2{ 430.0f, 680.0f },
                ImGuiCond_FirstUseEver);

            constexpr ImGuiWindowFlags environmentFlags =
                ImGuiWindowFlags_NoCollapse;
            const bool environmentVisible = ImGui::Begin(
                "シーン環境",
                &m_sceneEnvironmentOpen,
                environmentFlags);
            if (environmentVisible)
            {
                ImGui::BeginDisabled(m_playing);

                auto ambientColor = m_scene.AmbientLightColor();
                if (ImGui::ColorEdit3(
                    "環境光カラー",
                    &ambientColor.x))
                {
                    m_scene.SetAmbientLightColor(ambientColor);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float ambientIntensity =
                    m_scene.AmbientLightIntensity();
                if (ImGui::SliderFloat(
                    "環境光の強度",
                    &ambientIntensity,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetAmbientLightIntensity(
                        ambientIntensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::TextDisabled(
                    "3Dレンダラー共通。2D描画には影響しません。");

                ImGui::SeparatorText("描画カリング");
                bool frustumCulling =
                    m_scene.FrustumCullingEnabled();
                if (ImGui::Checkbox(
                    "Frustum Culling",
                    &frustumCulling))
                {
                    m_scene.SetFrustumCullingEnabled(
                        frustumCulling);
                    RecordHistory();
                }
                bool occlusionCulling =
                    m_scene.OcclusionCullingEnabled();
                if (ImGui::Checkbox(
                    "Occlusion Culling",
                    &occlusionCulling))
                {
                    m_scene.SetOcclusionCullingEnabled(
                        occlusionCulling);
                    RecordHistory();
                }
                const auto& visibility =
                    m_scene.VisibilityStats();
                ImGui::Text(
                    "表示 %zu / %zu  LOD %zu",
                    visibility.visibleRendererCount,
                    visibility.rendererCount,
                    visibility.lodCulledCount);
                ImGui::TextDisabled(
                    "視錐台外 %zu  遮蔽 %zu  LOD Group %zu",
                    visibility.frustumCulledCount,
                    visibility.occlusionCulledCount,
                    visibility.lodGroupCount);

                ImGui::SeparatorText("物理 Broad Phase");
                float cellSize =
                    m_scene.PhysicsBroadPhaseCellSize();
                if (ImGui::DragFloat(
                    "セルサイズ",
                    &cellSize,
                    0.25f,
                    0.25f,
                    100.0f,
                    "%.2f"))
                {
                    m_scene.SetPhysicsBroadPhaseCellSize(
                        cellSize);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::SeparatorText("Skybox");
                auto sky = m_scene.Sky();
                if (ImGui::Checkbox(
                    "空を表示",
                    &sky.enabled))
                {
                    m_scene.SetSkySettings(sky);
                    RecordHistory();
                }
                if (ImGui::ColorEdit3(
                    "空の上部",
                    &sky.topColor.x,
                    ImGuiColorEditFlags_Float))
                {
                    m_scene.SetSkySettings(sky);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::ColorEdit3(
                    "地平線",
                    &sky.horizonColor.x,
                    ImGuiColorEditFlags_Float))
                {
                    m_scene.SetSkySettings(sky);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::ColorEdit3(
                    "空の下部",
                    &sky.groundColor.x,
                    ImGuiColorEditFlags_Float))
                {
                    m_scene.SetSkySettings(sky);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "空の明るさ",
                    &sky.intensity,
                    0.0f,
                    8.0f,
                    "%.2f"))
                {
                    m_scene.SetSkySettings(sky);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                {
                    std::array<char, 512> cubemapBuffer{};
                    strncpy_s(
                        cubemapBuffer.data(),
                        cubemapBuffer.size(),
                        PathToUtf8(sky.cubemapPath).c_str(),
                        _TRUNCATE);

                    const auto validateCubemap =
                        [this](
                            const std::filesystem::path& candidate,
                            std::string& error)
                        {
                            if (candidate.empty())
                            {
                                error.clear();
                                return true;
                            }
                            if (Lowercase(
                                    PathToUtf8(
                                        candidate.extension()))
                                != ".dds")
                            {
                                error =
                                    "SkyboxにはキューブマップDDSを"
                                    "指定してください";
                                return false;
                            }
                            if (!m_graphics.Assets().FileExists(
                                    candidate))
                            {
                                error =
                                    "キューブマップが見つかりません: "
                                    + PathToUtf8(candidate);
                                return false;
                            }
                            try
                            {
                                const auto texture =
                                    m_graphics.Assets().LoadTexture(
                                        candidate);
                                if (texture == nullptr
                                    || !texture->isCube)
                                {
                                    error =
                                        "キューブマップではありません: "
                                        + PathToUtf8(candidate);
                                    return false;
                                }
                            }
                            catch (const std::exception& exception)
                            {
                                error =
                                    "キューブマップを読み込めません: "
                                    + PathToUtf8(candidate)
                                    + " ("
                                    + exception.what()
                                    + ")";
                                return false;
                            }
                            error.clear();
                            return true;
                        };

                    const auto applyCubemap =
                        [&](std::filesystem::path candidate)
                        {
                            candidate = candidate.empty()
                                ? std::filesystem::path{}
                                : candidate.lexically_normal();
                            std::string error;
                            if (!validateCubemap(candidate, error))
                            {
                                m_skyboxValidationError = error;
                                SetStatus(error, true);
                                return false;
                            }
                            sky.cubemapPath = candidate;
                            m_scene.SetSkySettings(sky);
                            m_skyboxValidationPath = candidate;
                            m_skyboxValidationError.clear();
                            RecordHistory();
                            SetStatus(
                                candidate.empty()
                                    ? "Skyboxのキューブマップを解除しました"
                                    : "Skyboxへキューブマップを設定しました");
                            return true;
                        };

                    if (m_skyboxValidationPath
                        != sky.cubemapPath)
                    {
                        m_skyboxValidationPath = sky.cubemapPath;
                        m_skyboxValidationError.clear();
                        if (!validateCubemap(
                                sky.cubemapPath,
                                m_skyboxValidationError))
                        {
                            SetStatus(
                                m_skyboxValidationError,
                                true);
                        }
                    }

                    if (ImGui::InputTextWithHint(
                            "キューブマップ(.dds)",
                            "assets相対パス（空で単色空）",
                            cubemapBuffer.data(),
                            cubemapBuffer.size()))
                    {
                        // 編集途中では現在のSkyboxを維持し、入力が
                        // 確定した時点でだけ存在とキューブ形式を検証します。
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        applyCubemap(
                            PathFromUtf8(cubemapBuffer.data()));
                    }

                    auto selectedCubemap = sky.cubemapPath;
                    if (DrawCubemapAssetSelector(
                            "アセットから選択",
                            selectedCubemap))
                    {
                        applyCubemap(std::move(selectedCubemap));
                    }

                    const bool selectedDds =
                        !m_selectedAsset.empty()
                        && Lowercase(
                            PathToUtf8(
                                m_selectedAsset.extension()))
                            == ".dds";
                    ImGui::BeginDisabled(!selectedDds);
                    if (ImGui::Button(
                            "選択中のDDSをSkyboxへ設定"))
                    {
                        applyCubemap(m_selectedAsset);
                    }
                    ImGui::EndDisabled();

                    ImGui::Button(
                        "キューブマップDDSをここへドロップ",
                        ImVec2{ -1.0f, 0.0f });
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(
                                    AssetPayload))
                        {
                            applyCubemap(PathFromUtf8(
                                static_cast<const char*>(
                                    payload->Data)));
                        }
                        ImGui::EndDragDropTarget();
                    }

                    if (!m_skyboxValidationError.empty())
                    {
                        ImGui::TextColored(
                            ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                            "%s",
                            m_skyboxValidationError.c_str());
                    }
                    if (ImGui::SliderFloat(
                        "IBL強度",
                        &sky.iblIntensity,
                        0.0f,
                        4.0f,
                        "%.2f"))
                    {
                        m_scene.SetSkySettings(sky);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::TextDisabled(
                        "キューブマップDDSを設定すると空と"
                        "環境反射（IBL）に使われます");
                }

                if (ImGui::Checkbox(
                    "朝昼夜（Directional Lightの向きで変える）",
                    &sky.sunDriven))
                {
                    m_scene.SetSkySettings(sky);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Directional Lightを回すだけで、"
                        "夜明け→昼→夕焼け→夜と変わります。\n"
                        "空の3色と環境光は太陽の高度から自動で"
                        "決まるようになり、上の色設定は使われ"
                        "ません。\n"
                        "空には太陽そのものも描かれます"
                        "（大きさはライトの「見かけの大きさ」）。");
                }
                if (sky.sunDriven)
                {
                    ImGui::TextDisabled(
                        "X回転が高度です。-90度で真上（正午）、"
                        "0度で地平線、+90度で真下（真夜中）。");
                }

                ImGui::SeparatorText("Fog");
                auto fog = m_scene.Fog();
                if (ImGui::Checkbox(
                    "フォグを有効化",
                    &fog.enabled))
                {
                    m_scene.SetFogSettings(fog);
                    RecordHistory();
                }
                if (ImGui::ColorEdit3(
                    "フォグ色",
                    &fog.color.x,
                    ImGuiColorEditFlags_Float))
                {
                    m_scene.SetFogSettings(fog);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::DragFloat(
                    "開始距離",
                    &fog.startDistance,
                    0.25f,
                    0.0f,
                    100000.0f,
                    "%.2f"))
                {
                    m_scene.SetFogSettings(fog);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::DragFloat(
                    "終了距離",
                    &fog.endDistance,
                    0.25f,
                    fog.startDistance + 0.01f,
                    100000.0f,
                    "%.2f"))
                {
                    m_scene.SetFogSettings(fog);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::DragFloat(
                    "フォグ密度",
                    &fog.density,
                    0.001f,
                    0.0f,
                    10.0f,
                    "%.3f"))
                {
                    m_scene.SetFogSettings(fog);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                ImGui::SeparatorText(
                    "SSAO");
                auto occlusion = m_scene.AmbientOcclusion();
                if (ImGui::Checkbox(
                    "SSAOを有効化",
                    &occlusion.enabled))
                {
                    m_scene.SetAmbientOcclusionSettings(
                        occlusion);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    // このラムダ（showItemTooltip）はもっと後で
                    // 定義されるため、ここでは直接呼びます。
                    ImGui::SetTooltip(
                        "物が接している隙間や角を暗くして、接地感を"
                        "出します。深度だけから計算する軽量な方式です");
                }
                if (ImGui::SliderFloat(
                    "陰りを探す距離",
                    &occlusion.radius,
                    0.05f,
                    3.0f,
                    "%.2f m"))
                {
                    m_scene.SetAmbientOcclusionSettings(
                        occlusion);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "陰りの濃さ",
                    &occlusion.strength,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetAmbientOcclusionSettings(
                        occlusion);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (!m_graphics.Settings()
                    .ambientOcclusionEnabled)
                {
                    ImGui::TextDisabled(
                        "グラフィック品質でSSAOが無効です"
                        "（High以上、またはCustomで有効化）");
                }

                ImGui::SeparatorText(
                    "TAA（時間的アンチエイリアス）");
                auto temporal =
                    m_scene.TemporalAntiAliasing();
                if (ImGui::Checkbox(
                    "TAAを有効化",
                    &temporal.enabled))
                {
                    m_scene
                        .SetTemporalAntiAliasingSettings(
                            temporal);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "毎フレーム少しずらして描き、前フレームと"
                        "混ぜて輪郭のギザギザを消します。1枚のなかで"
                        "平均を取るMSAAより安く、テクスチャの"
                        "ちらつきにも効きます");
                }
                if (ImGui::SliderFloat(
                    "前フレームを残す比率",
                    &temporal.historyWeight,
                    0.0f,
                    0.98f,
                    "%.2f"))
                {
                    m_scene
                        .SetTemporalAntiAliasingSettings(
                            temporal);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "上げるほど滑らかになりますが、動きに対して"
                        "眠く（残像っぽく）なります");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ずらす量",
                    &temporal.jitterScale,
                    0.0f,
                    2.0f,
                    "%.2f px"))
                {
                    m_scene
                        .SetTemporalAntiAliasingSettings(
                            temporal);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "履歴を許す幅",
                    &temporal.clampTolerance,
                    0.0f,
                    8.0f,
                    "%.2f"))
                {
                    m_scene
                        .SetTemporalAntiAliasingSettings(
                            temporal);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "小さいほど前フレームを捨てやすく、動く物の"
                        "にじみが減る代わりにギザギザ取りも弱く"
                        "なります");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (temporal.enabled
                    && m_graphics.Settings()
                        .antiAliasingEnabled)
                {
                    ImGui::TextDisabled(
                        "FXAAも有効です。二重にかかるので"
                        "どちらかを切るのがおすすめ");
                }

                ImGui::SeparatorText(
                    "SSR（画面空間反射）");
                auto reflection =
                    m_scene.ScreenSpaceReflection();
                if (ImGui::Checkbox(
                    "画面空間反射を有効化",
                    &reflection.enabled))
                {
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "濡れた床や磨いた金属に、周りの景色を映します。"
                        "映せるのは画面に写っているものだけです"
                        "（画面の外や物の裏側は情報がありません）。"
                        "足りない分はリフレクションプローブやSkyの"
                        "環境反射へ戻します");
                }
                if (ImGui::SliderFloat(
                    "反射の強さ",
                    &reflection.intensity,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "映る距離",
                    &reflection.maximumDistance,
                    0.5f,
                    100.0f,
                    "%.1f m"))
                {
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                int reflectionSteps = static_cast<int>(
                    reflection.stepCount);
                if (ImGui::SliderInt(
                    "レイのサンプル数",
                    &reflectionSteps,
                    4,
                    128))
                {
                    reflection.stepCount =
                        static_cast<std::uint32_t>(
                            std::max(reflectionSteps, 1));
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "物の厚み",
                    &reflection.thickness,
                    0.01f,
                    5.0f,
                    "%.2f m"))
                {
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "小さすぎると反射が途中で水平に切れ、"
                        "大きすぎると物の裏の床にも映り込みます。"
                        "既定の1.2mは人間サイズの物を想定した値です。"
                        "手すりや板のような薄い物ばかりの場面では"
                        "下げてください");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "掛ける粗さの上限",
                    &reflection.roughnessCutoff,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene
                        .SetScreenSpaceReflectionSettings(
                            reflection);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "これより粗い面には掛けません。ざらざらした面の"
                        "反射はぼやけていて、1本のレイでは表せない"
                        "ためです");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                ImGui::SeparatorText(
                    "ベイクした間接光（GI）");
                auto bakedGi =
                    m_scene.BakedGlobalIllumination();
                if (ImGui::Checkbox(
                    "間接光を有効化",
                    &bakedGi.enabled))
                {
                    m_scene
                        .SetBakedGlobalIlluminationSettings(
                            bakedGi);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "シーンへ格子状のプローブを敷いて周囲の光を"
                        "焼き、場所ごとに違う環境光にします。赤い壁の"
                        "そばの床がほんのり赤くなる、はね返りの光が"
                        "出ます。設定したら下のベイクを押してください");
                }
                float bakedGiCenter[3]{
                    bakedGi.center.x,
                    bakedGi.center.y,
                    bakedGi.center.z };
                if (ImGui::DragFloat3(
                    "範囲の中心",
                    bakedGiCenter,
                    0.25f))
                {
                    bakedGi.center = {
                        bakedGiCenter[0],
                        bakedGiCenter[1],
                        bakedGiCenter[2] };
                    m_scene
                        .SetBakedGlobalIlluminationSettings(
                            bakedGi);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                float bakedGiSize[3]{
                    bakedGi.size.x,
                    bakedGi.size.y,
                    bakedGi.size.z };
                if (ImGui::DragFloat3(
                    "範囲の大きさ",
                    bakedGiSize,
                    0.25f,
                    0.1f,
                    10000.0f))
                {
                    bakedGi.size = {
                        bakedGiSize[0],
                        bakedGiSize[1],
                        bakedGiSize[2] };
                    m_scene
                        .SetBakedGlobalIlluminationSettings(
                            bakedGi);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                int bakedGiResolution[3]{
                    static_cast<int>(bakedGi.resolutionX),
                    static_cast<int>(bakedGi.resolutionY),
                    static_cast<int>(bakedGi.resolutionZ) };
                if (ImGui::SliderInt3(
                    "格子の分割数",
                    bakedGiResolution,
                    2,
                    32))
                {
                    bakedGi.resolutionX =
                        static_cast<std::uint32_t>(
                            std::max(
                                bakedGiResolution[0], 1));
                    bakedGi.resolutionY =
                        static_cast<std::uint32_t>(
                            std::max(
                                bakedGiResolution[1], 1));
                    bakedGi.resolutionZ =
                        static_cast<std::uint32_t>(
                            std::max(
                                bakedGiResolution[2], 1));
                    m_scene
                        .SetBakedGlobalIlluminationSettings(
                            bakedGi);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "各軸のプローブの個数。多いほど間接光の変化が"
                        "細かくなりますが、ベイクに時間がかかります");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "間接光の強さ",
                    &bakedGi.intensity,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene
                        .SetBakedGlobalIlluminationSettings(
                            bakedGi);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                const float bakedGiProgress =
                    m_scene
                        .BakedGlobalIlluminationBakeProgress();
                if (bakedGiProgress >= 0.0f)
                {
                    ImGui::ProgressBar(
                        bakedGiProgress,
                        ImVec2(-1.0f, 0.0f));
                }
                else if (ImGui::Button("間接光をベイク"))
                {
                    m_scene
                        .RequestBakedGlobalIlluminationBake();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "範囲内の各プローブで周囲を描いて焼き込みます。"
                        "シーンを変えたら押し直してください。結果は"
                        "シーンと一緒に保存されます");
                }
                if (!m_scene.HasBakedGlobalIllumination()
                    && bakedGi.enabled
                    && bakedGiProgress < 0.0f)
                {
                    ImGui::TextDisabled(
                        "まだベイクされていません。上のボタンを"
                        "押してください");
                }

                // 並びは実際にかかる順（SSAO→光の筋→Bloom）に
                // 合わせています。光の筋はHDRのうちにかけるので、
                // 明るい筋はこの後のBloomで滲みます。
                ImGui::SeparatorText(
                    "光の筋（ボリュメトリック）");
                auto volumetric = m_scene.VolumetricLight();
                if (ImGui::Checkbox(
                    "光の筋を有効化",
                    &volumetric.enabled))
                {
                    m_scene.SetVolumetricLightSettings(
                        volumetric);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "空気中の塵に光が散る様子を描きます。窓から"
                        "差す光や木漏れ日がこれです。影を有効にした"
                        "平行光源（Directional Light）が必要です");
                }
                if (ImGui::SliderFloat(
                    "筋の濃さ",
                    &volumetric.intensity,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetVolumetricLightSettings(
                        volumetric);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                int volumetricSamples =
                    static_cast<int>(
                        volumetric.sampleCount);
                if (ImGui::SliderInt(
                    "レイのサンプル数",
                    &volumetricSamples,
                    4,
                    128))
                {
                    volumetric.sampleCount =
                        static_cast<std::uint32_t>(
                            std::max(volumetricSamples, 1));
                    m_scene.SetVolumetricLightSettings(
                        volumetric);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "多いほど滑らかになりますが重くなります。"
                        "少なすぎると縞模様が出ます");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "届く距離",
                    &volumetric.maximumDistance,
                    1.0f,
                    500.0f,
                    "%.1f m"))
                {
                    m_scene.SetVolumetricLightSettings(
                        volumetric);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "前方散乱",
                    &volumetric.scattering,
                    0.0f,
                    0.95f,
                    "%.2f"))
                {
                    m_scene.SetVolumetricLightSettings(
                        volumetric);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "上げるほど、光源の方を向いたときだけ強く"
                        "光ります。0にすると全方向へ均一に散ります");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (volumetric.enabled
                    && !m_graphics.Settings().shadowsEnabled)
                {
                    ImGui::TextDisabled(
                        "グラフィック品質で影が無効です"
                        "（影が無いと光の筋は出ません）");
                }

                ImGui::SeparatorText("Bloom");
                auto bloom = m_scene.Bloom();
                if (ImGui::Checkbox(
                    "ブルームを有効化",
                    &bloom.enabled))
                {
                    m_scene.SetBloomSettings(bloom);
                    RecordHistory();
                }
                if (ImGui::SliderFloat(
                    "発光しきい値##LensFlare",
                    &bloom.threshold,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetBloomSettings(bloom);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "発光の強さ",
                    &bloom.intensity,
                    0.0f,
                    8.0f,
                    "%.2f"))
                {
                    m_scene.SetBloomSettings(bloom);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "発光の広がり",
                    &bloom.radius,
                    0.25f,
                    12.0f,
                    "%.2f"))
                {
                    m_scene.SetBloomSettings(bloom);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                ImGui::SeparatorText("画面アウトライン");
                auto screenOutline = m_scene.ScreenOutline();
                if (ImGui::Checkbox(
                    "画面アウトラインを有効化",
                    &screenOutline.enabled))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "シーンの深度からシルエットと形状の折れ目を検出して、"
                        "3Dだけへ線を重ねます。法線マップの細かな凹凸は"
                        "対象にしません");
                }
                if (ImGui::ColorEdit3(
                    "線の色##ScreenOutline",
                    &screenOutline.color.x,
                    ImGuiColorEditFlags_Float))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "線の強さ##ScreenOutline",
                    &screenOutline.intensity,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "線の太さ##ScreenOutline",
                    &screenOutline.thickness,
                    1.0f,
                    4.0f,
                    "%.1f px"))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "深度の感度##ScreenOutline",
                    &screenOutline.depthThreshold,
                    0.001f,
                    0.25f,
                    "%.3f"))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "小さいほど奥行きの小さな段差も輪郭として拾います");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "法線の感度##ScreenOutline",
                    &screenOutline.normalThreshold,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenOutlineSettings(screenOutline);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "小さいほど緩やかな面の折れ目も輪郭として拾います");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                ImGui::SeparatorText(
                    "Screen Space Lens Flare");
                auto lensFlare = m_scene.ScreenSpaceLensFlare();
                if (ImGui::Checkbox(
                    "レンズフレアを有効化",
                    &lensFlare.enabled))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "高輝度部分からゴースト、ハロー、放射状の筋を"
                        "作ります。プロジェクト品質設定でも有効である"
                        "必要があります");
                }
                if (ImGui::SliderFloat(
                    "発光しきい値",
                    &lensFlare.threshold,
                    0.0f,
                    8.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "フレアの強さ##LensFlare",
                    &lensFlare.intensity,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ゴーストの分散##LensFlare",
                    &lensFlare.ghostDispersal,
                    0.01f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ハローの位置##LensFlare",
                    &lensFlare.haloWidth,
                    0.05f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "色収差##LensFlare",
                    &lensFlare.chromaticAberration,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "放射状の筋##LensFlare",
                    &lensFlare.streakIntensity,
                    0.0f,
                    2.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "筋の長さ##LensFlare",
                    &lensFlare.streakLength,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                int streakDirections = static_cast<int>(
                    lensFlare.streakDirections);
                if (ImGui::SliderInt(
                    "筋の本数##LensFlare",
                    &streakDirections,
                    1,
                    4))
                {
                    lensFlare.streakDirections =
                        static_cast<std::uint32_t>(
                            streakDirections);
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "1で横一本（アナモルフィック風）、"
                        "2で十字、3以上で放射状。\n"
                        "本数ぶんパスが増えるので、上げると"
                        "少し重くなります。");
                }
                if (ImGui::SliderFloat(
                    "筋の角度##LensFlare",
                    &lensFlare.streakAngleDegrees,
                    0.0f,
                    180.0f,
                    "%.0f度"))
                {
                    m_scene.SetScreenSpaceLensFlareSettings(
                        lensFlare);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (lensFlare.enabled
                    && !m_graphics.Settings()
                        .screenSpaceLensFlareEnabled)
                {
                    ImGui::TextDisabled(
                        "グラフィック品質でScreen Space Lens Flareが"
                        "無効です");
                }

                ImGui::SeparatorText("被写界深度 (DoF)");
                auto depthOfField = m_scene.DepthOfField();
                if (ImGui::Checkbox(
                    "被写界深度を有効化",
                    &depthOfField.enabled))
                {
                    m_scene.SetDepthOfFieldSettings(
                        depthOfField);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "ピントの合う距離から離れたものをぼかします。"
                        "プロジェクト品質設定でも有効である必要が"
                        "あります");
                }
                if (ImGui::DragFloat(
                    "ピントの合う距離##DepthOfField",
                    &depthOfField.focusDistance,
                    0.1f,
                    0.01f,
                    10000.0f,
                    "%.2f m"))
                {
                    m_scene.SetDepthOfFieldSettings(
                        depthOfField);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "カメラからこの距離にあるものが鋭く写ります。"
                        "見せたい被写体までの距離を入れてください");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ピントの合う幅##DepthOfField",
                    &depthOfField.focusRange,
                    0.0f,
                    50.0f,
                    "%.2f m"))
                {
                    m_scene.SetDepthOfFieldSettings(
                        depthOfField);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "この幅の中はまったくぼけません。\n"
                        "0にすると焦点面ちょうどだけが鋭くなり、"
                        "手前も奥もすぐにぼけ始めます");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ぼけの強さ##DepthOfField",
                    &depthOfField.blurStrength,
                    0.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetDepthOfFieldSettings(
                        depthOfField);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ぼけ半径の上限##DepthOfField",
                    &depthOfField.maximumRadius,
                    0.0f,
                    32.0f,
                    "%.1f px"))
                {
                    m_scene.SetDepthOfFieldSettings(
                        depthOfField);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "ぼけの大きさと処理コストの上限です。\n"
                        "サンプル数は品質設定側なので、ここだけ広げると"
                        "ぼけの粒が目立ってきます");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (depthOfField.enabled
                    && !m_graphics.Settings()
                        .depthOfFieldEnabled)
                {
                    ImGui::TextDisabled(
                        "グラフィック品質で被写界深度が無効です");
                }

                ImGui::SeparatorText(
                    "モーションブラー");
                auto motionBlur = m_scene.MotionBlur();
                if (ImGui::Checkbox(
                    "モーションブラーを有効化",
                    &motionBlur.enabled))
                {
                    m_scene.SetMotionBlurSettings(motionBlur);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "カメラが動いたぶんだけ絵を伸ばします。\n"
                        "物体ごとの速度は持っていないので、止まって"
                        "いるカメラの前を走るキャラクターはブレません"
                        "（カメラを振る・走る・乗り物の演出に効きます）");
                }
                if (ImGui::SliderFloat(
                    "ブレの強さ##MotionBlur",
                    &motionBlur.intensity,
                    0.0f,
                    2.0f,
                    "%.2f"))
                {
                    m_scene.SetMotionBlurSettings(motionBlur);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "1.0で「前フレームから今フレームまでに動いた"
                        "ぶん」をそのまま伸ばします\n"
                        "（シャッターが開いている割合に相当します）");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ブレの上限##MotionBlur",
                    &motionBlur.maximumRadius,
                    0.0f,
                    64.0f,
                    "%.0f px"))
                {
                    m_scene.SetMotionBlurSettings(motionBlur);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "上限を切らないと、カメラを素早く振ったときに"
                        "画面全体が溶けます。\n"
                        "サンプル数は品質設定側なので、ここだけ広げると"
                        "ブレが縞に分かれて見えます");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (motionBlur.enabled
                    && !m_graphics.Settings().motionBlurEnabled)
                {
                    ImGui::TextDisabled(
                        "グラフィック品質でモーションブラーが無効です");
                }

                ImGui::SeparatorText("自動露出");
                auto autoExposure = m_scene.AutoExposure();
                if (ImGui::Checkbox(
                    "自動露出を有効化",
                    &autoExposure.enabled))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "画面の明るさを測って露出を上下させます。\n"
                        "暗い洞窟から外へ出たときに目が慣れていく、"
                        "あの表現です");
                }
                if (ImGui::SliderFloat(
                    "目標の明るさ##AutoExposure",
                    &autoExposure.keyValue,
                    0.01f,
                    1.0f,
                    "%.3f"))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "画面全体をこの明るさへ寄せます。"
                        "写真の18%%グレー（0.18）が既定です");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::DragFloat(
                    "明るさの下限##AutoExposure",
                    &autoExposure.minimumLuminance,
                    0.005f,
                    0.0001f,
                    100.0f,
                    "%.4f"))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::DragFloat(
                    "明るさの上限##AutoExposure",
                    &autoExposure.maximumLuminance,
                    0.05f,
                    0.0001f,
                    100.0f,
                    "%.3f"))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "測った明るさをこの範囲へ丸めます。\n"
                        "真っ暗なシーンで開けきると暗部のノイズだけが"
                        "持ち上がり、真っ白なシーンで絞りきると何も"
                        "見えなくなるためです");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "明るい方へ慣れる速さ##AutoExposure",
                    &autoExposure.speedToBright,
                    0.0f,
                    10.0f,
                    "%.2f /秒"))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "暗い方へ慣れる速さ##AutoExposure",
                    &autoExposure.speedToDark,
                    0.0f,
                    10.0f,
                    "%.2f /秒"))
                {
                    m_scene.SetAutoExposureSettings(
                        autoExposure);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "本物の目も暗順応のほうが遅いので、既定を"
                        "分けています（明3.0 / 暗1.0）");
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (autoExposure.enabled)
                {
                    // 今どれだけ効いているかを出します。数字が動いて
                    // いれば順応が回っていると分かるので、「効いて
                    // いないように見える」ときの切り分けが早いです。
                    const auto& target =
                        m_activeViewport == ViewportMode::Scene
                            ? m_sceneRenderTarget
                            : m_gameRenderTarget;
                    if (target.AdaptedLuminance() > 0.0f)
                    {
                        ImGui::TextDisabled(
                            "測定中の明るさ %.4f → 補正 %+.2f 段",
                            target.AdaptedLuminance(),
                            target.AutoExposureStops());
                    }
                    if (!m_graphics.Settings()
                        .autoExposureEnabled)
                    {
                        ImGui::TextDisabled(
                            "グラフィック品質で自動露出が無効です");
                    }
                    else if (!m_scene.ColorGrading()
                        .toneMappingEnabled)
                    {
                        ImGui::TextDisabled(
                            "トーンマッピングが無効なので効きません"
                            "（露出はトーンマップの中で掛かります）");
                    }
                }

                ImGui::SeparatorText("Color Grading");
                auto colorGrading = m_scene.ColorGrading();
                if (ImGui::Checkbox(
                    "トーンマッピングを有効化",
                    &colorGrading.toneMappingEnabled))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                    RecordHistory();
                }
                if (ImGui::Checkbox(
                    "カラー調整を有効化",
                    &colorGrading.enabled))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                    RecordHistory();
                }
                if (ImGui::SliderFloat(
                    "露出",
                    &colorGrading.exposure,
                    -4.0f,
                    4.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "コントラスト",
                    &colorGrading.contrast,
                    0.5f,
                    2.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "彩度",
                    &colorGrading.saturation,
                    0.0f,
                    2.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "色温度",
                    &colorGrading.temperature,
                    -1.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "色かぶり",
                    &colorGrading.tint,
                    -1.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                if (ImGui::SliderFloat(
                    "ビネット",
                    &colorGrading.vignette,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    m_scene.SetColorGradingSettings(colorGrading);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();
                ImGui::EndDisabled();

                const auto& physics =
                    m_scene.PhysicsStats();
                const std::size_t totalColliders =
                    physics.colliderCount2D
                    + physics.colliderCount3D;
                const std::size_t candidatePairs =
                    physics.candidatePairCount2D
                    + physics.candidatePairCount3D;
                const std::size_t narrowTests =
                    physics.narrowPhaseTestCount2D
                    + physics.narrowPhaseTestCount3D;
                ImGui::Text(
                    "Collider: %zu（2D %zu / 3D %zu）",
                    totalColliders,
                    physics.colliderCount2D,
                    physics.colliderCount3D);
                ImGui::Text(
                    "候補ペア: %zu / Narrow Phase: %zu",
                    candidatePairs,
                    narrowTests);
                ImGui::Text(
                    "使用セル: %zu / 接触中: %zu",
                    physics.occupiedCellCount2D
                        + physics.occupiedCellCount3D,
                    physics.activeContactCount);
                if (physics.oversizedColliderCount2D
                        + physics.oversizedColliderCount3D
                    != 0)
                {
                    ImGui::TextDisabled(
                        "巨大Collider %zu件は安全な全候補比較を使用",
                        physics.oversizedColliderCount2D
                            + physics.oversizedColliderCount3D);
                }
                if (!m_playing)
                {
                    ImGui::TextDisabled(
                        "統計は再生中に更新されます。");
                }
            }
            ImGui::End();
        }

        ImGui::SetNextWindowSize(
            ImVec2{ InspectorWidth, 640.0f },
            ImGuiCond_FirstUseEver);

        constexpr ImGuiWindowFlags inspectorFlags =
            ImGuiWindowFlags_NoCollapse;

        ImGui::Begin(
            "インスペクター",
            nullptr,
            inspectorFlags);
        // スクリーンショットモードの「:bottom」指定。下の方の
        // コンポーネント（コライダーのレイヤー等）を撮るため、
        // 撮影まで毎フレーム末尾へスクロールし続けます。
        if (m_screenshotScrollToBottom
            && !m_screenshotRequest.imagePath.empty())
        {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        }

        if (IsMaterialAsset(m_selectedAsset))
        {
            DrawMaterialAssetInspector();
            ImGui::End();
            return;
        }
        if (IsDataAsset(m_selectedAsset))
        {
            DrawDataAssetInspector();
            ImGui::End();
            return;
        }
        if (IsFbxAsset(m_selectedAsset))
        {
            DrawModelAssetInspector();
            ImGui::End();
            return;
        }

        auto* selected = m_scene.FindGameObject(m_selectedObjectId);
        if (selected == nullptr)
        {
            ImGui::TextDisabled("ヒエラルキーからGameObjectを選択してください。");
            ImGui::End();
            return;
        }

        ImGui::BeginDisabled(m_playing);

        const auto showItemTooltip = [](const char* text)
        {
            if (ImGui::IsItemHovered(
                    ImGuiHoveredFlags_AllowWhenDisabled
                    | ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::SetTooltip("%s", text);
            }
        };

        std::array<char, 256> nameBuffer{};
        strncpy_s(
            nameBuffer.data(),
            nameBuffer.size(),
            selected->Name().c_str(),
            _TRUNCATE);
        bool enabled = selected->IsEnabled();
        bool persistent = selected->IsPersistent();
        if (ImGui::BeginTable(
                "ObjectHeader",
                3,
                ImGuiTableFlags_NoSavedSettings
                    | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn(
                "Enabled",
                ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFrameHeight());
            ImGui::TableSetupColumn(
                "Name",
                ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "Persistent",
                ImGuiTableColumnFlags_WidthFixed,
                ImGui::GetFrameHeight());
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("##ObjectEnabled", &enabled))
            {
                selected->SetEnabled(enabled);
                RecordHistory();
            }
            showItemTooltip("有効 / 無効");

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText(
                    "##ObjectName",
                    nameBuffer.data(),
                    nameBuffer.size()))
            {
                selected->SetName(nameBuffer.data());
            }
            showItemTooltip("名前");
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                RecordHistory();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(
                selected->Parent() != nullptr);
            if (ImGui::Checkbox(
                    "##PersistentAcrossScenes",
                    &persistent))
            {
                if (persistent)
                {
                    m_scene.DontDestroyOnLoad(
                        *selected);
                }
                else
                {
                    m_scene.DestroyOnLoad(
                        *selected);
                }
                RecordHistory();
            }
            ImGui::EndDisabled();
            showItemTooltip(
                selected->Parent() != nullptr
                    ? "シーン切替後も維持（ルートGameObjectで設定）"
                    : "シーン切替後も維持");

            ImGui::EndTable();
        }
        {
            // プロジェクト設定のタグ一覧から選択します。未登録の
            // タグが設定されている場合は警告と登録ボタンを出します。
            const bool tagRegistered =
                selected->Tag().empty()
                || m_projectSettings.tags.empty()
                || std::ranges::find(
                        m_projectSettings.tags,
                        selected->Tag())
                    != m_projectSettings.tags.end();
            const std::string tagPreview =
                selected->Tag().empty()
                    ? std::string{ "Tag（未設定）" }
                    : (tagRegistered
                        ? selected->Tag()
                        : selected->Tag() + "（未登録）");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo(
                    "##ObjectTag",
                    tagPreview.c_str()))
            {
                if (ImGui::Selectable(
                        "（未設定）",
                        selected->Tag().empty()))
                {
                    selected->SetTag({});
                    RecordHistory();
                }
                for (const auto& tag :
                    m_projectSettings.tags)
                {
                    const bool active =
                        selected->Tag() == tag;
                    if (ImGui::Selectable(
                            tag.c_str(),
                            active))
                    {
                        selected->SetTag(tag);
                        RecordHistory();
                    }
                    if (active)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::Separator();
                ImGui::SetNextItemWidth(160.0f);
                ImGui::InputTextWithHint(
                    "##NewTagName",
                    "新規タグ名",
                    m_newTagBuffer.data(),
                    m_newTagBuffer.size());
                ImGui::SameLine();
                if (ImGui::Button("追加")
                    && m_newTagBuffer[0] != '\0')
                {
                    if (AddProjectTag(
                            m_newTagBuffer.data()))
                    {
                        selected->SetTag(
                            m_newTagBuffer.data());
                        m_newTagBuffer.fill('\0');
                        RecordHistory();
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndCombo();
            }
            showItemTooltip(
                "Tag。FindGameObjectByTagで検索できます。"
                "候補はプロジェクト設定のタグ一覧です");
            if (!tagRegistered)
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.75f, 0.3f, 1.0f },
                    "未登録のタグです");
                ImGui::SameLine();
                if (ImGui::SmallButton(
                        "タグ一覧へ登録"))
                {
                    static_cast<void>(
                        AddProjectTag(selected->Tag()));
                }
            }
        }
        // 描画カリングの設定はRenderCullingコンポーネントへ移り
        // ました（全オブジェクトに常時出ていた項目を、必要な
        // オブジェクトにだけ付ける形にするため）。
        if (selected->IsPersistent())
        {
            std::array<char, 256>
                persistenceKey{};
            strncpy_s(
                persistenceKey.data(),
                persistenceKey.size(),
                selected->PersistenceKey().c_str(),
                _TRUNCATE);
            if (ImGui::InputText(
                "永続キー",
                persistenceKey.data(),
                persistenceKey.size()))
            {
                try
                {
                    m_scene.DontDestroyOnLoad(
                        *selected,
                        persistenceKey.data());
                }
                catch (const std::exception&
                    exception)
                {
                    SetStatus(exception.what());
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                RecordHistory();
            }
        }

        bool prefabReverted = false;
        if (auto* prefabRoot =
                m_scene.FindPrefabInstanceRoot(*selected);
            prefabRoot != nullptr)
        {
            ImGui::SeparatorText("Prefab");
            ImGui::TextWrapped(
                "元Prefab: %s",
                PathToUtf8(
                    prefabRoot->PrefabAssetPath()).c_str());
            // 元アセットへすぐ辿れるようにします。
            if (ImGui::SmallButton(
                "元Prefabを選択##PrefabLocate"))
            {
                const auto prefabAsset =
                    prefabRoot->PrefabAssetPath();
                m_selectedAsset = prefabAsset;
                m_assetDirectory =
                    prefabAsset.parent_path();
                m_assetBrowserPanelOpen = true;
                RefreshAssets();
                SetStatus(
                    "Prefabアセットを選択しました: "
                    + PathToUtf8(prefabAsset.filename()));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(
                "インスタンスルートを選択##PrefabRoot"))
            {
                SelectObject(prefabRoot->Id(), false);
            }
            if (prefabRoot != selected)
            {
                ImGui::Text(
                    "インスタンスルート: %s",
                    prefabRoot->Name().c_str());
            }

            const double now = ImGui::GetTime();
            if (m_prefabStatusRootId
                    != prefabRoot->Id()
                || now >= m_nextPrefabStatusRefresh)
            {
                m_prefabStatusRootId =
                    prefabRoot->Id();
                m_nextPrefabStatusRefresh =
                    now + 0.5;
                m_prefabStatusError.clear();
                m_prefabOverrides.clear();
                try
                {
                    const auto overrides =
                        m_scene.GetPrefabOverrides(
                            *prefabRoot);
                    m_prefabOverrides.reserve(
                        overrides.size());
                    for (const auto& value :
                        overrides)
                    {
                        m_prefabOverrides.push_back(
                            PrefabOverrideDisplay{
                                value.path,
                                FormatPrefabOverridePath(
                                    value.path),
                                TruncatePrefabValue(
                                    value.sourceValue),
                                TruncatePrefabValue(
                                    value.instanceValue),
                                value.canApplyIndividually
                            });
                    }
                    m_prefabHasOverrides =
                        !m_prefabOverrides.empty();
                }
                catch (const std::exception& exception)
                {
                    m_prefabHasOverrides = false;
                    m_prefabStatusError =
                        exception.what();
                }
            }

            if (!m_prefabStatusError.empty())
            {
                ImGui::TextWrapped(
                    "状態: 読み込みエラー - %s",
                    m_prefabStatusError.c_str());
            }
            else if (m_prefabHasOverrides)
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.72f, 0.2f, 1.0f },
                    "状態: Overrideあり");
            }
            else
            {
                ImGui::TextDisabled(
                    "状態: 元Prefabと同じ");
            }

            ImGui::BeginDisabled(
                !m_prefabStatusError.empty()
                || !m_prefabHasOverrides);
            if (ImGui::Button("Apply"))
            {
                ApplySelectedPrefab();
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert"))
            {
                prefabReverted =
                    RevertSelectedPrefab();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                "Apply: 元Prefabへ反映 / Revert: 変更を破棄");

            const std::string overrideHeader =
                "Override詳細 ("
                + std::to_string(
                    m_prefabOverrides.size())
                + ")";
            if (!m_prefabOverrides.empty()
                && ImGui::TreeNode(
                    overrideHeader.c_str()))
            {
                for (std::size_t index = 0;
                    index < m_prefabOverrides.size();
                    ++index)
                {
                    bool stopOverrideLoop = false;
                    const auto& entry =
                        m_prefabOverrides[index];
                    ImGui::PushID(
                        static_cast<int>(index));
                    ImGui::TextWrapped(
                        "%s",
                        entry.label.c_str());
                    ImGui::TextDisabled(
                        "元: %s",
                        entry.sourceValue.c_str());
                    ImGui::TextWrapped(
                        "現在: %s",
                        entry.instanceValue.c_str());

                    if (entry.canApplyIndividually)
                    {
                        if (ImGui::SmallButton(
                            "この項目をApply"))
                        {
                            ApplySelectedPrefabOverride(
                                entry.path);
                            stopOverrideLoop = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(
                            "この項目をRevert"))
                        {
                            prefabReverted =
                                RevertSelectedPrefabOverride(
                                    entry.path);
                            stopOverrideLoop = true;
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled(
                            "構造差分は階層全体のApply／Revertを使用");
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                    if (stopOverrideLoop)
                    {
                        break;
                    }
                }
                ImGui::TreePop();
            }
        }

        if (prefabReverted)
        {
            ImGui::EndDisabled();
            ImGui::End();
            return;
        }

        auto& transform = selected->GetTransform();
        if (ImGui::CollapsingHeader(
                "Transform",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            // 表示桁数はプロジェクト設定で変えられます（既定は1桁）。
            // ImGuiの既定は"%.3f"で、位置や角度をざっと見るには
            // 桁が多くて読みにくいためです。丸めるのは表示だけで、
            // 内部の値は変わりません。
            const auto decimalsFormat = InspectorFloatFormat();
            ImGui::InputFloat3(
                "位置",
                &transform.position.x,
                decimalsFormat.c_str());
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                RecordHistory();
            }

            // 回転の正本はクォータニオンなので、表示するオイラー角は
            // そこから変換して作ります。ただし同じ回転を表す
            // オイラー角は複数あるため、変換した値をそのまま
            // 入力欄へ入れると編集中に数値が飛びます（90度付近で
            // 別の等価な組み合わせへ切り替わる）。
            // そこで編集中だけは入力した値を覚えておき、そちらを
            // 表示します。
            const auto currentEuler =
                transform.EulerAngles();
            DirectX::XMFLOAT3 rotationDegrees{
                DirectX::XMConvertToDegrees(
                    currentEuler.x),
                DirectX::XMConvertToDegrees(
                    currentEuler.y),
                DirectX::XMConvertToDegrees(
                    currentEuler.z)
            };
            if (m_rotationEditObjectId
                    == selected->Id()
                && m_rotationEditActive)
            {
                rotationDegrees = m_rotationEditDegrees;
            }
            if (ImGui::InputFloat3(
                "回転",
                &rotationDegrees.x,
                decimalsFormat.c_str()))
            {
                m_rotationEditObjectId = selected->Id();
                m_rotationEditActive = true;
                m_rotationEditDegrees = rotationDegrees;
                transform.SetEulerAngles({
                    DirectX::XMConvertToRadians(
                        rotationDegrees.x),
                    DirectX::XMConvertToRadians(
                        rotationDegrees.y),
                    DirectX::XMConvertToRadians(
                        rotationDegrees.z)
                });
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                RecordHistory();
            }
            if (!ImGui::IsItemActive()
                && m_rotationEditObjectId
                    == selected->Id())
            {
                // 編集から離れたら、次はクォータニオンから作った
                // 値を見せます（他の操作で回された結果を反映）。
                m_rotationEditActive = false;
            }

            ImGui::InputFloat3(
                "拡縮",
                &transform.scale.x,
                decimalsFormat.c_str());
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                RecordHistory();
            }
        }

        Component* componentToRemove{};
        // コンポーネントの並び替え要求。走査中にm_componentsを
        // 並べ替えるとfor文を壊すので、削除と同じく走査後に
        // まとめて適用します。
        struct PendingComponentReorder final
        {
            const Component* moved{};
            const Component* reference{};
            bool insertAfter{};
            bool requested{};
        };
        PendingComponentReorder pendingComponentReorder{};

        // PBRマップ（粗さ・金属度・遮蔽・発光）と付随する値のUI。
        // MeshRendererとModelRendererで枠の形が同じなので、1つの
        // ラムダにまとめて両方から呼びます。
        const auto drawPbrMapSlots =
            [this](auto* renderer)
            {
                if (!ImGui::TreeNode("PBRマップ（粗さ・金属度・遮蔽・発光）"))
                {
                    return;
                }
                ImGui::TextDisabled(
                    "粗さ・金属度マップは上の数値へ掛け算されます。"
                    "遮蔽は環境光にだけ掛かり、発光はライティングと"
                    "無関係に足されます（Bloomが滲ませます）。");

                struct Slot final
                {
                    const char* label;
                    std::filesystem::path (*get)(
                        decltype(renderer));
                    void (*set)(
                        decltype(renderer),
                        std::filesystem::path);
                };
                const Slot slots[]{
                    {
                        "粗さマップ",
                        [](decltype(renderer) target)
                        {
                            return target->RoughnessTexturePath();
                        },
                        [](decltype(renderer) target,
                            std::filesystem::path path)
                        {
                            target->SetRoughnessTexturePath(
                                std::move(path));
                        }
                    },
                    {
                        "金属度マップ",
                        [](decltype(renderer) target)
                        {
                            return target->MetallicTexturePath();
                        },
                        [](decltype(renderer) target,
                            std::filesystem::path path)
                        {
                            target->SetMetallicTexturePath(
                                std::move(path));
                        }
                    },
                    {
                        "遮蔽（AO）マップ",
                        [](decltype(renderer) target)
                        {
                            return target->OcclusionTexturePath();
                        },
                        [](decltype(renderer) target,
                            std::filesystem::path path)
                        {
                            target->SetOcclusionTexturePath(
                                std::move(path));
                        }
                    },
                    {
                        "発光マップ",
                        [](decltype(renderer) target)
                        {
                            return target->EmissiveTexturePath();
                        },
                        [](decltype(renderer) target,
                            std::filesystem::path path)
                        {
                            target->SetEmissiveTexturePath(
                                std::move(path));
                        }
                    }
                };

                for (const auto& slot : slots)
                {
                    ImGui::PushID(slot.label);
                    const auto current = slot.get(renderer);
                    const auto currentText = PathToUtf8(current);
                    ImGui::TextWrapped(
                        "%s: %s",
                        slot.label,
                        currentText.empty()
                            ? "（未設定）"
                            : currentText.c_str());
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(
                                AssetPayload))
                        {
                            const auto droppedPath =
                                PathFromUtf8(
                                    static_cast<const char*>(
                                        payload->Data));
                            if (!IsTextureAsset(droppedPath))
                            {
                                SetStatus(
                                    "画像ファイルをドロップしてください",
                                    true);
                            }
                            else
                            {
                                try
                                {
                                    slot.set(
                                        renderer,
                                        droppedPath);
                                    RecordHistory();
                                    SetStatus(
                                        "マップを割り当てました");
                                }
                                catch (
                                    const std::exception&
                                        exception)
                                {
                                    SetStatus(
                                        exception.what(),
                                        true);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::BeginDisabled(
                        !IsTextureAsset(m_selectedAsset));
                    if (ImGui::Button("選択画像を割り当て"))
                    {
                        try
                        {
                            slot.set(renderer, m_selectedAsset);
                            RecordHistory();
                            SetStatus(
                                "マップを割り当てました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!current.empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("解除"))
                        {
                            slot.set(renderer, {});
                            RecordHistory();
                        }
                    }
                    ImGui::PopID();
                }

                float occlusionStrength =
                    renderer->OcclusionStrength();
                if (ImGui::SliderFloat(
                    "遮蔽の強さ",
                    &occlusionStrength,
                    0.0f,
                    1.0f))
                {
                    renderer->SetOcclusionStrength(
                        occlusionStrength);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto emissive = renderer->EmissiveColor();
                if (ImGui::ColorEdit3(
                    "発光色",
                    &emissive.x,
                    ImGuiColorEditFlags_HDR
                        | ImGuiColorEditFlags_Float))
                {
                    renderer->SetEmissiveColor(emissive);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "発光色を1より大きくするとBloomが強く出ます。");
                ImGui::TreePop();
            };

        // 折りたたみ状態のIDは「型名＋同じ型の中での順番」から作ります。
        // ポインタをIDにすると、再生の停止（LoadFromJsonでの作り直し）
        // で別アドレスになり、畳んだヘッダーが勝手に開きます
        // （DefaultOpenが効くため）。この作り方なら作り直しでも
        // 並べ替えでも同じIDになります。
        std::unordered_map<std::string_view, int>
            componentTypeOrdinals;

        for (auto& component : selected->Components())
        {
            const auto componentTypeName =
                component->TypeName();
            const std::string componentIdKey =
                std::string(componentTypeName)
                + '#'
                + std::to_string(
                    componentTypeOrdinals[
                        componentTypeName]++);
            ImGui::PushID(componentIdKey.c_str());
            std::string componentLabel;
            if (const auto* nativeScript =
                    dynamic_cast<
                        const NativeScriptComponent*>(
                            component.get()))
            {
                componentLabel =
                    nativeScript->DisplayName()
                    + " [C++]";
            }
            else
            {
                componentLabel =
                    ComponentDisplayName(
                        component->TypeName());
            }
            bool componentEnabled = component->IsEnabled();
            bool componentOpen = false;
            ImGui::Separator();
            if (ImGui::BeginTable(
                    "ComponentHeader",
                    3,
                    ImGuiTableFlags_NoSavedSettings
                        | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn(
                    "Enabled",
                    ImGuiTableColumnFlags_WidthFixed,
                    ImGui::GetFrameHeight());
                ImGui::TableSetupColumn(
                    "Name",
                    ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(
                    "Remove",
                    ImGuiTableColumnFlags_WidthFixed,
                    ImGui::GetFrameHeight());
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Checkbox(
                        "##ComponentEnabled",
                        &componentEnabled))
                {
                    component->SetEnabled(componentEnabled);
                    RecordHistory();
                }
                showItemTooltip("有効 / 無効");

                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("×"))
                {
                    componentToRemove = component.get();
                }
                showItemTooltip("コンポーネントを削除");

                ImGui::TableSetColumnIndex(1);
                componentOpen = ImGui::CollapsingHeader(
                    componentLabel.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen
                        | ImGuiTreeNodeFlags_FramePadding
                        | ImGuiTreeNodeFlags_SpanAvailWidth);

                // ヘッダーをつかんで並び替えられるようにします。
                // ドラッグ中に折りたたみが切り替わらないよう、
                // ソースにしたときはヘッダー名だけを見せます。
                if (!m_playing
                    && ImGui::BeginDragDropSource())
                {
                    const Component* pointer =
                        component.get();
                    ImGui::SetDragDropPayload(
                        ComponentPayload,
                        &pointer,
                        sizeof(pointer));
                    ImGui::TextUnformatted(
                        componentLabel.c_str());
                    ImGui::EndDragDropSource();
                }
                if (!m_playing
                    && ImGui::BeginDragDropTarget())
                {
                    // コンポーネントに「子」は無いので、行の上半分＝
                    // 手前へ、下半分＝直後へ、の2択だけです。
                    const ImVec2 headerMinimum =
                        ImGui::GetItemRectMin();
                    const ImVec2 headerMaximum =
                        ImGui::GetItemRectMax();
                    const float headerHeight = std::max(
                        headerMaximum.y - headerMinimum.y,
                        1.0f);
                    const bool insertAfter =
                        (ImGui::GetMousePos().y
                            - headerMinimum.y)
                            / headerHeight
                        > 0.5f;
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(
                                ComponentPayload,
                                ImGuiDragDropFlags_AcceptBeforeDelivery
                                    | ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    {
                        // 挿入位置の横線と左端の丸印
                        // （ヒエラルキーと同じ見た目）。
                        auto* const drawList =
                            ImGui::GetWindowDrawList();
                        const auto highlight =
                            ImGui::GetColorU32(
                                ImGuiCol_DragDropTarget);
                        const float lineY = insertAfter
                            ? headerMaximum.y
                            : headerMinimum.y;
                        drawList->AddLine(
                            ImVec2{ headerMinimum.x, lineY },
                            ImVec2{ headerMaximum.x, lineY },
                            highlight,
                            3.0f);
                        drawList->AddCircleFilled(
                            ImVec2{
                                headerMinimum.x + 3.0f,
                                lineY },
                            4.0f,
                            highlight);
                        if (payload->IsDelivery())
                        {
                            const Component* dragged{};
                            std::memcpy(
                                &dragged,
                                payload->Data,
                                sizeof(dragged));
                            pendingComponentReorder = {
                                dragged,
                                component.get(),
                                insertAfter,
                                true
                            };
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::EndTable();
            }

            if (!componentOpen)
            {
                ImGui::PopID();
                continue;
            }

            if (auto* camera = dynamic_cast<CameraComponent*>(component.get()))
            {
                float fieldOfView = DirectX::XMConvertToDegrees(
                    camera->VerticalFieldOfView());
                if (ImGui::SliderFloat(
                    "視野角",
                    &fieldOfView,
                    20.0f,
                    120.0f,
                    "%.1f deg"))
                {
                    camera->SetVerticalFieldOfView(
                        DirectX::XMConvertToRadians(fieldOfView));
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::Text(
                    "近距離/遠距離: %.3f / %.1f",
                    camera->NearPlane(),
                    camera->FarPlane());

                if (m_scene.MainCamera() != camera)
                {
                    if (ImGui::Button("メインカメラに設定"))
                    {
                        m_scene.SetMainCamera(*camera);
                        RecordHistory();
                    }
                }
                else
                {
                    ImGui::TextDisabled("メインカメラ");
                }

                ImGui::SeparatorText("描画先（Render Texture）");
                std::array<char, 64> targetName{};
                strncpy_s(
                    targetName.data(),
                    targetName.size(),
                    camera->TargetTexture().c_str(),
                    _TRUNCATE);
                if (ImGui::InputText(
                    "テクスチャ名##CameraTarget",
                    targetName.data(),
                    targetName.size()))
                {
                    camera->SetTargetTexture(
                        targetName.data());
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                showItemTooltip(
                    "名前を入れると画面ではなくテクスチャへ描画します。"
                    "同じ名前をSprite RendererやUI Imageの"
                    "「Render Texture」に指定すると画面に出せます"
                    "（ミニマップ・防犯カメラ）。空にすると通常の"
                    "カメラに戻ります");
                if (camera->RendersToTexture())
                {
                    int size[2]{
                        static_cast<int>(
                            camera->TargetTextureWidth()),
                        static_cast<int>(
                            camera->TargetTextureHeight())
                    };
                    if (ImGui::DragInt2(
                        "解像度##CameraTarget",
                        size,
                        1.0f,
                        16,
                        4096))
                    {
                        camera->SetTargetTextureSize(
                            static_cast<std::uint32_t>(
                                std::clamp(size[0], 16, 4096)),
                            static_cast<std::uint32_t>(
                                std::clamp(size[1], 16, 4096)));
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    auto clearColor =
                        camera->TargetClearColor();
                    if (ImGui::ColorEdit4(
                        "背景色##CameraTarget",
                        &clearColor.x))
                    {
                        camera->SetTargetClearColor(
                            clearColor);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::TextDisabled(
                        "2D/UIはテクスチャへ描かれません");
                }
            }
            else if (auto* directionalLight =
                dynamic_cast<DirectionalLightComponent*>(
                    component.get()))
            {
                auto color = directionalLight->Color();
                if (ImGui::ColorEdit3("ライト色", &color.x))
                {
                    directionalLight->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float intensity = directionalLight->Intensity();
                if (ImGui::SliderFloat(
                    "強度",
                    &intensity,
                    0.0f,
                    16.0f,
                    "%.2f"))
                {
                    directionalLight->SetIntensity(intensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float angularDiameter =
                    directionalLight->
                        AngularDiameterDegrees();
                if (ImGui::SliderFloat(
                    "見かけの大きさ（度）",
                    &angularDiameter,
                    0.0f,
                    20.0f,
                    "%.2f"))
                {
                    directionalLight->
                        SetAngularDiameterDegrees(
                            angularDiameter);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "空に見えている光源の直径です。"
                        "本物の太陽は0.53度。\n"
                        "つるつるした物のハイライトの大きさが"
                        "これで決まります。\n"
                        "0にすると点になり、"
                        "ハイライトが消えたように見えます。");
                }

                bool castsShadows =
                    directionalLight->CastsShadows();
                if (ImGui::Checkbox(
                    "影を落とす",
                    &castsShadows))
                {
                    directionalLight->SetCastsShadows(
                        castsShadows);
                    RecordHistory();
                }

                if (directionalLight->CastsShadows())
                {
                    float shadowDistance =
                        directionalLight->ShadowDistance();
                    if (ImGui::DragFloat(
                        "影の描画距離",
                        &shadowDistance,
                        0.5f,
                        2.0f,
                        100.0f,
                        "%.1f"))
                    {
                        directionalLight->SetShadowDistance(
                            shadowDistance);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    int cascadeCount = static_cast<int>(
                        directionalLight->
                            ShadowCascadeCount());
                    if (ImGui::SliderInt(
                        "カスケード数",
                        &cascadeCount,
                        1,
                        4))
                    {
                        directionalLight->
                            SetShadowCascadeCount(
                                static_cast<std::uint32_t>(
                                    cascadeCount));
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float splitLambda =
                        directionalLight->
                            ShadowSplitLambda();
                    if (ImGui::SliderFloat(
                        "分割バランス",
                        &splitLambda,
                        0.0f,
                        1.0f,
                        "%.2f"))
                    {
                        directionalLight->
                            SetShadowSplitLambda(
                                splitLambda);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "0: 均等分割 / 1: 近景を高精細化");
                    }

                    float shadowStrength =
                        directionalLight->ShadowStrength();
                    if (ImGui::SliderFloat(
                        "影の濃さ",
                        &shadowStrength,
                        0.0f,
                        1.0f,
                        "%.2f"))
                    {
                        directionalLight->SetShadowStrength(
                            shadowStrength);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float shadowBias =
                        directionalLight->ShadowBias();
                    if (ImGui::DragFloat(
                        "影バイアス",
                        &shadowBias,
                        0.0001f,
                        0.0f,
                        0.02f,
                        "%.5f"))
                    {
                        directionalLight->SetShadowBias(
                            shadowBias);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float normalBias =
                        directionalLight->ShadowNormalBias();
                    if (ImGui::DragFloat(
                        "法線バイアス",
                        &normalBias,
                        0.0001f,
                        0.0f,
                        0.1f,
                        "%.5f"))
                    {
                        directionalLight->
                            SetShadowNormalBias(normalBias);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    ImGui::TextDisabled(
                        "最初の有効な影対応ライトを"
                        "2048px x 最大4分割で描画します。");
                    ImGui::TextDisabled(
                        "影範囲はテクセル単位に固定され、"
                        "カメラ移動時の揺れを抑えます。");
                }

                const auto direction =
                    directionalLight->WorldDirection();
                ImGui::Text(
                    "ワールド方向: %.2f, %.2f, %.2f",
                    direction.x,
                    direction.y,
                    direction.z);
                ImGui::TextDisabled(
                    "Transformの回転で方向を変更します。"
                    "有効な先頭3灯まで描画されます。");
            }
            else if (auto* pointLight =
                dynamic_cast<PointLightComponent*>(
                    component.get()))
            {
                auto color = pointLight->Color();
                if (ImGui::ColorEdit3("ライト色", &color.x))
                {
                    pointLight->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float intensity = pointLight->Intensity();
                if (ImGui::SliderFloat(
                    "強度",
                    &intensity,
                    0.0f,
                    64.0f,
                    "%.2f"))
                {
                    pointLight->SetIntensity(intensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float range = pointLight->Range();
                if (ImGui::DragFloat(
                    "範囲",
                    &range,
                    0.1f,
                    0.1f,
                    1000.0f,
                    "%.1f"))
                {
                    pointLight->SetRange(range);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool pointCastsShadows =
                    pointLight->CastsShadows();
                if (ImGui::Checkbox(
                        "影を落とす##PointLight",
                        &pointCastsShadows))
                {
                    pointLight->SetCastsShadows(
                        pointCastsShadows);
                    RecordHistory();
                }
                if (pointCastsShadows)
                {
                    float pointShadowStrength =
                        pointLight->ShadowStrength();
                    if (ImGui::SliderFloat(
                        "影の濃さ##PointLight",
                        &pointShadowStrength,
                        0.0f,
                        1.0f,
                        "%.2f"))
                    {
                        pointLight->SetShadowStrength(
                            pointShadowStrength);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    float pointShadowBias =
                        pointLight->ShadowBias();
                    if (ImGui::SliderFloat(
                        "影バイアス##PointLight",
                        &pointShadowBias,
                        0.0f,
                        0.02f,
                        "%.4f"))
                    {
                        pointLight->SetShadowBias(
                            pointShadowBias);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::TextDisabled(
                        "影を落とせるポイントライトは"
                        "同時に1灯です");
                }
                ImGui::TextDisabled(
                    "距離に応じて二次減衰します。"
                    "有効な先頭16灯まで描画されます。");
            }
            else if (auto* light2D =
                dynamic_cast<Light2DComponent*>(
                    component.get()))
            {
                auto light2DColor = light2D->Color();
                if (ImGui::ColorEdit3(
                        "ライト色##Light2D",
                        &light2DColor.x))
                {
                    light2D->SetColor(light2DColor);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float light2DIntensity =
                    light2D->Intensity();
                if (ImGui::SliderFloat(
                    "強度##Light2D",
                    &light2DIntensity,
                    0.0f,
                    16.0f,
                    "%.2f"))
                {
                    light2D->SetIntensity(
                        light2DIntensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float light2DRadius = light2D->Radius();
                if (ImGui::DragFloat(
                    "半径##Light2D",
                    &light2DRadius,
                    1.0f,
                    1.0f,
                    100000.0f,
                    "%.0f"))
                {
                    light2D->SetRadius(light2DRadius);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool light2DAffectsUI = light2D->AffectsUI();
                if (ImGui::Checkbox(
                    "UIも照らす##Light2D",
                    &light2DAffectsUI))
                {
                    light2D->SetAffectsUI(light2DAffectsUI);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "既定は切です。"
                        "UIは読めることが最優先なので、"
                        "普通は照らしません。\n"
                        "ランタンでメニューを照らすような"
                        "演出のときだけ入れてください。");
                }

                ImGui::TextDisabled(
                    "Sprite RendererとTilemapを加算式に"
                    "照らします（Particleや独自Shader付き"
                    "Spriteは対象外。暗くはならず、近いほど"
                    "明るく色づきます。画面で16灯まで）。");
            }
            else if (auto* spotLight =
                dynamic_cast<SpotLightComponent*>(
                    component.get()))
            {
                auto color = spotLight->Color();
                if (ImGui::ColorEdit3("ライト色", &color.x))
                {
                    spotLight->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float intensity = spotLight->Intensity();
                if (ImGui::SliderFloat(
                    "強度",
                    &intensity,
                    0.0f,
                    64.0f,
                    "%.2f"))
                {
                    spotLight->SetIntensity(intensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float range = spotLight->Range();
                if (ImGui::DragFloat(
                    "範囲",
                    &range,
                    0.1f,
                    0.1f,
                    1000.0f,
                    "%.1f"))
                {
                    spotLight->SetRange(range);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float innerAngle = DirectX::XMConvertToDegrees(
                    spotLight->InnerConeAngle());
                if (ImGui::SliderFloat(
                    "内側角度",
                    &innerAngle,
                    1.0f,
                    89.0f,
                    "%.1f deg"))
                {
                    spotLight->SetInnerConeAngle(
                        DirectX::XMConvertToRadians(innerAngle));
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float outerAngle = DirectX::XMConvertToDegrees(
                    spotLight->OuterConeAngle());
                if (ImGui::SliderFloat(
                    "外側角度",
                    &outerAngle,
                    1.0f,
                    89.0f,
                    "%.1f deg"))
                {
                    spotLight->SetOuterConeAngle(
                        DirectX::XMConvertToRadians(outerAngle));
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool spotCastsShadows =
                    spotLight->CastsShadows();
                if (ImGui::Checkbox(
                        "影を落とす##SpotLight",
                        &spotCastsShadows))
                {
                    spotLight->SetCastsShadows(
                        spotCastsShadows);
                    RecordHistory();
                }
                if (spotCastsShadows)
                {
                    float spotShadowStrength =
                        spotLight->ShadowStrength();
                    if (ImGui::SliderFloat(
                        "影の濃さ##SpotLight",
                        &spotShadowStrength,
                        0.0f,
                        1.0f,
                        "%.2f"))
                    {
                        spotLight->SetShadowStrength(
                            spotShadowStrength);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    float spotShadowBias =
                        spotLight->ShadowBias();
                    if (ImGui::SliderFloat(
                        "影バイアス##SpotLight",
                        &spotShadowBias,
                        0.0f,
                        0.02f,
                        "%.4f"))
                    {
                        spotLight->SetShadowBias(
                            spotShadowBias);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::TextDisabled(
                        "影を落とせるスポットライトは"
                        "同時に4灯です");
                }

                ImGui::TextDisabled(
                    "Transformの回転で方向を変更します。"
                    "有効な先頭8灯まで描画されます。");
            }
            else if (auto* collider2D =
                dynamic_cast<BoxCollider2DComponent*>(component.get()))
            {
                auto size = collider2D->Size();
                if (ImGui::InputFloat2("サイズ", &size.x))
                {
                    collider2D->SetSize(size);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto offset = collider2D->Offset();
                if (ImGui::InputFloat2("オフセット", &offset.x))
                {
                    collider2D->SetOffset(offset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool trigger = collider2D->IsTrigger();
                if (ImGui::Checkbox("トリガー", &trigger))
                {
                    collider2D->SetTrigger(trigger);
                    RecordHistory();
                }

                std::uint32_t layer = collider2D->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー",
                    m_projectSettings.physics.layerNames,
                    layer))
                {
                    collider2D->SetLayer(layer);
                    RecordHistory();
                }

                std::uint32_t mask =
                    collider2D->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク",
                    m_projectSettings.physics.layerNames,
                    mask))
                {
                    collider2D->SetCollisionMask(mask);
                    RecordHistory();
                }

                auto material = collider2D->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数",
                    &material.friction,
                    0.0f,
                    4.0f))
                {
                    collider2D->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                material = collider2D->Material();
                if (ImGui::SliderFloat(
                    "反発係数",
                    &material.restitution,
                    0.0f,
                    1.0f))
                {
                    collider2D->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* circle2D =
                dynamic_cast<CircleCollider2DComponent*>(
                    component.get()))
            {
                float circleRadius = circle2D->Radius();
                if (ImGui::InputFloat(
                        "半径##Circle2D",
                        &circleRadius))
                {
                    circle2D->SetRadius(circleRadius);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                auto circleOffset = circle2D->Offset();
                if (ImGui::InputFloat2(
                        "オフセット##Circle2D",
                        &circleOffset.x))
                {
                    circle2D->SetOffset(circleOffset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                bool circleTrigger =
                    circle2D->IsTrigger();
                if (ImGui::Checkbox(
                        "トリガー##Circle2D",
                        &circleTrigger))
                {
                    circle2D->SetTrigger(circleTrigger);
                    RecordHistory();
                }
                std::uint32_t circleLayer =
                    circle2D->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー##Circle2D",
                    m_projectSettings.physics.layerNames,
                    circleLayer))
                {
                    circle2D->SetLayer(circleLayer);
                    RecordHistory();
                }
                std::uint32_t circleMask =
                    circle2D->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク##Circle2D",
                    m_projectSettings.physics.layerNames,
                    circleMask))
                {
                    circle2D->SetCollisionMask(circleMask);
                    RecordHistory();
                }
                auto circleMaterial =
                    circle2D->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数##Circle2D",
                    &circleMaterial.friction,
                    0.0f,
                    4.0f))
                {
                    circle2D->SetMaterial(
                        circleMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                circleMaterial = circle2D->Material();
                if (ImGui::SliderFloat(
                    "反発係数##Circle2D",
                    &circleMaterial.restitution,
                    0.0f,
                    1.0f))
                {
                    circle2D->SetMaterial(
                        circleMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* polygon2D =
                dynamic_cast<
                    PolygonCollider2DComponent*>(
                        component.get()))
            {
                auto polygonOffset = polygon2D->Offset();
                if (ImGui::InputFloat2(
                        "オフセット##Polygon2D",
                        &polygonOffset.x))
                {
                    polygon2D->SetOffset(polygonOffset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                bool polygonTrigger =
                    polygon2D->IsTrigger();
                if (ImGui::Checkbox(
                        "トリガー##Polygon2D",
                        &polygonTrigger))
                {
                    polygon2D->SetTrigger(polygonTrigger);
                    RecordHistory();
                }
                std::uint32_t polygonLayer =
                    polygon2D->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー##Polygon2D",
                    m_projectSettings.physics.layerNames,
                    polygonLayer))
                {
                    polygon2D->SetLayer(polygonLayer);
                    RecordHistory();
                }
                std::uint32_t polygonMask =
                    polygon2D->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク##Polygon2D",
                    m_projectSettings.physics.layerNames,
                    polygonMask))
                {
                    polygon2D->SetCollisionMask(polygonMask);
                    RecordHistory();
                }
                auto polygonMaterial =
                    polygon2D->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数##Polygon2D",
                    &polygonMaterial.friction,
                    0.0f,
                    4.0f))
                {
                    polygon2D->SetMaterial(
                        polygonMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                polygonMaterial = polygon2D->Material();
                if (ImGui::SliderFloat(
                    "反発係数##Polygon2D",
                    &polygonMaterial.restitution,
                    0.0f,
                    1.0f))
                {
                    polygon2D->SetMaterial(
                        polygonMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::Separator();
                ImGui::Text(
                    "頂点 (%zu)",
                    polygon2D->Vertices().size());
                const auto polygonVertices =
                    polygon2D->Vertices();
                int polygonRemoveIndex = -1;
                for (std::size_t index{};
                    index < polygonVertices.size();
                    ++index)
                {
                    ImGui::PushID(
                        static_cast<int>(index));
                    auto vertex = polygonVertices[index];
                    if (ImGui::DragFloat2(
                        "##polygonVertex",
                        &vertex.x,
                        0.05f))
                    {
                        polygon2D->SetVertex(index, vertex);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("削除"))
                    {
                        polygonRemoveIndex =
                            static_cast<int>(index);
                    }
                    ImGui::PopID();
                }
                if (polygonRemoveIndex >= 0)
                {
                    polygon2D->RemoveVertex(
                        static_cast<std::size_t>(
                            polygonRemoveIndex));
                    RecordHistory();
                }
                if (ImGui::Button("頂点を追加##Polygon2D"))
                {
                    polygon2D->AddVertex();
                    RecordHistory();
                }
            }
            else if (auto* collider3D =
                dynamic_cast<BoxCollider3DComponent*>(component.get()))
            {
                auto size = collider3D->Size();
                if (ImGui::InputFloat3("サイズ", &size.x))
                {
                    collider3D->SetSize(size);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto offset = collider3D->Offset();
                if (ImGui::InputFloat3("オフセット", &offset.x))
                {
                    collider3D->SetOffset(offset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool trigger = collider3D->IsTrigger();
                if (ImGui::Checkbox("トリガー", &trigger))
                {
                    collider3D->SetTrigger(trigger);
                    RecordHistory();
                }

                std::uint32_t layer = collider3D->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー",
                    m_projectSettings.physics.layerNames,
                    layer))
                {
                    collider3D->SetLayer(layer);
                    RecordHistory();
                }

                std::uint32_t mask =
                    collider3D->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク",
                    m_projectSettings.physics.layerNames,
                    mask))
                {
                    collider3D->SetCollisionMask(mask);
                    RecordHistory();
                }

                auto material = collider3D->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数",
                    &material.friction,
                    0.0f,
                    4.0f))
                {
                    collider3D->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                material = collider3D->Material();
                if (ImGui::SliderFloat(
                    "反発係数",
                    &material.restitution,
                    0.0f,
                    1.0f))
                {
                    collider3D->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* capsule =
                dynamic_cast<
                    CapsuleCollider3DComponent*>(
                        component.get()))
            {
                float radius = capsule->Radius();
                if (ImGui::DragFloat(
                    "半径",
                    &radius,
                    0.01f,
                    0.01f,
                    1000.0f))
                {
                    capsule->SetRadius(radius);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float height = capsule->Height();
                if (ImGui::DragFloat(
                    "高さ",
                    &height,
                    0.01f,
                    0.02f,
                    2000.0f))
                {
                    capsule->SetHeight(height);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto offset = capsule->Offset();
                if (ImGui::InputFloat3(
                    "オフセット",
                    &offset.x))
                {
                    capsule->SetOffset(offset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool trigger = capsule->IsTrigger();
                if (ImGui::Checkbox("トリガー", &trigger))
                {
                    capsule->SetTrigger(trigger);
                    RecordHistory();
                }

                std::uint32_t layer = capsule->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー",
                    m_projectSettings.physics.layerNames,
                    layer))
                {
                    capsule->SetLayer(layer);
                    RecordHistory();
                }

                std::uint32_t mask =
                    capsule->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク",
                    m_projectSettings.physics.layerNames,
                    mask))
                {
                    capsule->SetCollisionMask(mask);
                    RecordHistory();
                }

                auto material = capsule->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数",
                    &material.friction,
                    0.0f,
                    4.0f))
                {
                    capsule->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                material = capsule->Material();
                if (ImGui::SliderFloat(
                    "反発係数",
                    &material.restitution,
                    0.0f,
                    1.0f))
                {
                    capsule->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* sphere =
                dynamic_cast<
                    SphereCollider3DComponent*>(
                        component.get()))
            {
                float radius = sphere->Radius();
                if (ImGui::DragFloat(
                    "半径",
                    &radius,
                    0.01f,
                    0.01f,
                    1000.0f))
                {
                    sphere->SetRadius(radius);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto offset = sphere->Offset();
                if (ImGui::InputFloat3(
                    "オフセット",
                    &offset.x))
                {
                    sphere->SetOffset(offset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool trigger = sphere->IsTrigger();
                if (ImGui::Checkbox(
                    "トリガー",
                    &trigger))
                {
                    sphere->SetTrigger(trigger);
                    RecordHistory();
                }

                std::uint32_t layer = sphere->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー",
                    m_projectSettings.physics.layerNames,
                    layer))
                {
                    sphere->SetLayer(layer);
                    RecordHistory();
                }

                std::uint32_t mask =
                    sphere->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク",
                    m_projectSettings.physics.layerNames,
                    mask))
                {
                    sphere->SetCollisionMask(mask);
                    RecordHistory();
                }

                auto material = sphere->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数",
                    &material.friction,
                    0.0f,
                    4.0f))
                {
                    sphere->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                material = sphere->Material();
                if (ImGui::SliderFloat(
                    "反発係数",
                    &material.restitution,
                    0.0f,
                    1.0f))
                {
                    sphere->SetMaterial(material);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* hull =
                dynamic_cast<
                    ConvexHullCollider3DComponent*>(
                        component.get()))
            {
                auto hullOffset = hull->Offset();
                if (ImGui::InputFloat3(
                    "オフセット",
                    &hullOffset.x))
                {
                    hull->SetOffset(hullOffset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool hullTrigger = hull->IsTrigger();
                if (ImGui::Checkbox(
                    "トリガー",
                    &hullTrigger))
                {
                    hull->SetTrigger(hullTrigger);
                    RecordHistory();
                }

                std::uint32_t hullLayer = hull->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー",
                    m_projectSettings.physics.layerNames,
                    hullLayer))
                {
                    hull->SetLayer(hullLayer);
                    RecordHistory();
                }

                std::uint32_t hullMask =
                    hull->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク",
                    m_projectSettings.physics.layerNames,
                    hullMask))
                {
                    hull->SetCollisionMask(hullMask);
                    RecordHistory();
                }

                auto hullMaterial = hull->Material();
                if (ImGui::SliderFloat(
                    "摩擦係数",
                    &hullMaterial.friction,
                    0.0f,
                    4.0f))
                {
                    hull->SetMaterial(hullMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                hullMaterial = hull->Material();
                if (ImGui::SliderFloat(
                    "反発係数",
                    &hullMaterial.restitution,
                    0.0f,
                    1.0f))
                {
                    hull->SetMaterial(hullMaterial);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::Separator();
                ImGui::Text(
                    "頂点 (%zu)",
                    hull->Points().size());
                const auto hullPoints = hull->Points();
                int hullRemoveIndex = -1;
                for (std::size_t index{};
                    index < hullPoints.size();
                    ++index)
                {
                    ImGui::PushID(
                        static_cast<int>(index));
                    auto point = hullPoints[index];
                    if (ImGui::DragFloat3(
                        "##hullPoint",
                        &point.x,
                        0.05f))
                    {
                        hull->SetPoint(index, point);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("削除"))
                    {
                        hullRemoveIndex =
                            static_cast<int>(index);
                    }
                    ImGui::PopID();
                }
                if (hullRemoveIndex >= 0)
                {
                    hull->RemovePoint(
                        static_cast<std::size_t>(
                            hullRemoveIndex));
                    RecordHistory();
                }
                if (ImGui::Button("頂点を追加"))
                {
                    hull->AddPoint();
                    RecordHistory();
                }
            }
            else if (auto* meshCollider =
                dynamic_cast<
                    MeshCollider3DComponent*>(
                        component.get()))
            {
                if (!meshCollider->ModelPath().empty())
                {
                    ImGui::TextWrapped(
                        "モデル: %s",
                        meshCollider->ModelPath()
                            .u8string().c_str());
                }
                else
                {
                    ImGui::TextDisabled(
                        "モデル未設定");
                }
                if (meshCollider->HasMesh())
                {
                    ImGui::TextDisabled(
                        "三角形数: %zu",
                        meshCollider->TriangleCount());
                }
                else if (!meshCollider->LastError()
                    .empty())
                {
                    ImGui::TextWrapped(
                        "読み込みエラー: %s",
                        meshCollider->LastError()
                            .c_str());
                }
                if (ImGui::Button(
                        "ModelRendererと同期##MeshCollider"))
                {
                    if (const auto* renderer =
                        selected->GetComponent<
                            ModelRendererComponent>())
                    {
                        meshCollider->SetModelPath(
                            renderer->ModelPath());
                        RecordHistory();
                    }
                    else
                    {
                        SetStatus(
                            "ModelRendererがありません",
                            true);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(
                        "モデルを解除##MeshCollider"))
                {
                    meshCollider->SetModelPath({});
                    RecordHistory();
                }
                auto meshOffset =
                    meshCollider->Offset();
                if (ImGui::DragFloat3(
                        "オフセット##MeshCollider",
                        &meshOffset.x,
                        0.05f))
                {
                    meshCollider->SetOffset(meshOffset);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                bool meshTrigger =
                    meshCollider->IsTrigger();
                if (ImGui::Checkbox(
                        "トリガー##MeshCollider",
                        &meshTrigger))
                {
                    meshCollider->SetTrigger(
                        meshTrigger);
                    RecordHistory();
                }
                std::uint32_t meshLayer =
                    meshCollider->Layer();
                if (DrawCollisionLayerCombo(
                    "レイヤー##MeshCollider",
                    m_projectSettings.physics.layerNames,
                    meshLayer))
                {
                    meshCollider->SetLayer(meshLayer);
                    RecordHistory();
                }
                std::uint32_t meshMask =
                    meshCollider->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "衝突マスク##MeshCollider",
                    m_projectSettings.physics.layerNames,
                    meshMask))
                {
                    meshCollider->SetCollisionMask(
                        meshMask);
                    RecordHistory();
                }
                float meshFriction =
                    meshCollider->Material().friction;
                if (ImGui::SliderFloat(
                        "摩擦##MeshCollider",
                        &meshFriction,
                        0.0f,
                        1.0f))
                {
                    auto material =
                        meshCollider->Material();
                    material.friction = meshFriction;
                    meshCollider->SetMaterial(material);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                float meshRestitution =
                    meshCollider->Material()
                        .restitution;
                if (ImGui::SliderFloat(
                        "反発##MeshCollider",
                        &meshRestitution,
                        0.0f,
                        1.0f))
                {
                    auto material =
                        meshCollider->Material();
                    material.restitution =
                        meshRestitution;
                    meshCollider->SetMaterial(material);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "静的なステージ用です。Rigidbodyと"
                    "併用しないでください");
            }
            else if (auto* navMesh =
                dynamic_cast<
                    NavMeshComponent*>(
                        component.get()))
            {
                auto surfaceSize =
                    navMesh->SurfaceSize();
                if (ImGui::DragFloat2(
                        "Surfaceサイズ X/Z",
                        &surfaceSize.x,
                        0.5f,
                        1.0f,
                        4096.0f,
                        "%.1f"))
                {
                    navMesh->SetSurfaceSize(
                        surfaceSize);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float cellSize =
                    navMesh->CellSize();
                if (ImGui::DragFloat(
                        "セルサイズ",
                        &cellSize,
                        0.05f,
                        0.1f,
                        64.0f,
                        "%.2f"))
                {
                    navMesh->SetCellSize(
                        cellSize);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float agentRadius =
                    navMesh->AgentRadius();
                if (ImGui::DragFloat(
                        "Agent半径",
                        &agentRadius,
                        0.05f,
                        0.0f,
                        32.0f,
                        "%.2f"))
                {
                    navMesh->
                        SetAgentRadius(
                            agentRadius);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float agentHeight =
                    navMesh->AgentHeight();
                if (ImGui::DragFloat(
                        "Agent高さ",
                        &agentHeight,
                        0.05f,
                        0.1f,
                        64.0f,
                        "%.2f"))
                {
                    navMesh->
                        SetAgentHeight(
                            agentHeight);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                ImGui::Text(
                    "Grid: %u x %u",
                    navMesh->GridWidth(),
                    navMesh->GridDepth());
                ImGui::Text(
                    "状態: %s / 障害セル: %zu",
                    navMesh->IsBaked()
                        ? "Baked"
                        : "未Bake",
                    navMesh->
                        BlockedCellCount());

                if (ImGui::Button(
                        "3D ColliderからBake",
                        ImVec2{
                            -1.0f,
                            0.0f }))
                {
                    std::vector<Bounds3D>
                        obstacles;
                    for (const auto&
                        object :
                        m_scene.GameObjects())
                    {
                        if (object.get()
                                == selected
                            || !object->
                                IsEnabled())
                        {
                            continue;
                        }
                        const auto* collider =
                            object->GetComponent<
                                BoxCollider3DComponent>();
                        if (collider != nullptr
                            && collider->
                                IsEnabled()
                            && !collider->
                                IsTrigger())
                        {
                            obstacles.push_back(
                                collider->
                                    WorldBounds());
                        }
                        const auto* obstacleCapsule =
                            object->GetComponent<
                                CapsuleCollider3DComponent>();
                        if (obstacleCapsule != nullptr
                            && obstacleCapsule->IsEnabled()
                            && !obstacleCapsule->IsTrigger())
                        {
                            obstacles.push_back(
                                obstacleCapsule->WorldBounds());
                        }
                        const auto* obstacleSphere =
                            object->GetComponent<
                                SphereCollider3DComponent>();
                        if (obstacleSphere != nullptr
                            && obstacleSphere->IsEnabled()
                            && !obstacleSphere->IsTrigger())
                        {
                            obstacles.push_back(
                                obstacleSphere->WorldBounds());
                        }
                        const auto* obstacleHull =
                            object->GetComponent<
                                ConvexHullCollider3DComponent>();
                        if (obstacleHull != nullptr
                            && obstacleHull->IsEnabled()
                            && !obstacleHull->IsTrigger())
                        {
                            obstacles.push_back(
                                obstacleHull->WorldBounds());
                        }
                        const auto* obstacleMesh =
                            object->GetComponent<
                                MeshCollider3DComponent>();
                        if (obstacleMesh != nullptr
                            && obstacleMesh->IsEnabled()
                            && !obstacleMesh->IsTrigger()
                            && obstacleMesh->HasMesh())
                        {
                            obstacles.push_back(
                                obstacleMesh
                                    ->WorldBounds());
                        }
                    }
                    navMesh->Bake(obstacles);
                    RecordHistory();
                    SetStatus(
                        "NavMeshをBakeしました: "
                        + std::to_string(
                            navMesh->
                                BlockedCellCount())
                        + " 障害セル");
                }
                ImGui::TextDisabled(
                    "赤=障害物、青=歩行可能。SurfaceのY位置が歩行面です。");
            }
            else if (auto* agent =
                dynamic_cast<
                    NavMeshAgentComponent*>(
                        component.get()))
            {
                float speed = agent->Speed();
                if (ImGui::DragFloat(
                        "移動速度",
                        &speed,
                        0.1f,
                        0.0f,
                        1000.0f,
                        "%.2f"))
                {
                    agent->SetSpeed(speed);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float stoppingDistance =
                    agent->
                        StoppingDistance();
                if (ImGui::DragFloat(
                        "停止距離",
                        &stoppingDistance,
                        0.01f,
                        0.0f,
                        100.0f,
                        "%.2f"))
                {
                    agent->
                        SetStoppingDistance(
                            stoppingDistance);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool rotateToPath =
                    agent->RotateToPath();
                if (ImGui::Checkbox(
                        "進行方向へ回転",
                        &rotateToPath))
                {
                    agent->SetRotateToPath(
                        rotateToPath);
                    RecordHistory();
                }

                auto destination =
                    agent->Destination();
                if (ImGui::InputFloat3(
                        "目的地",
                        &destination.x))
                {
                    agent->SetPath(
                        destination,
                        {});
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                if (ImGui::Button(
                        "経路を計算",
                        ImVec2{
                            -1.0f,
                            0.0f }))
                {
                    NavMeshComponent*
                        activeNavMesh{};
                    for (const auto&
                        object :
                        m_scene.GameObjects())
                    {
                        auto* candidate =
                            object->GetComponent<
                                NavMeshComponent>();
                        if (object->
                                IsEnabled()
                            && candidate != nullptr
                            && candidate->
                                IsEnabled()
                            && candidate->
                                IsBaked())
                        {
                            activeNavMesh =
                                candidate;
                            break;
                        }
                    }
                    if (activeNavMesh
                            == nullptr)
                    {
                        SetStatus(
                            "Bake済みNavMeshがありません",
                            true);
                    }
                    else if (agent->
                        SetDestination(
                            agent->
                                Destination(),
                            *activeNavMesh))
                    {
                        RecordHistory();
                        SetStatus(
                            "A*経路を計算しました: "
                            + std::to_string(
                                agent->
                                    Path().size())
                            + " waypoint");
                    }
                    else
                    {
                        SetStatus(
                            "目的地へ到達できる経路がありません",
                            true);
                    }
                }
                ImGui::Text(
                    "Path: %zu waypoint / %s",
                    agent->Path().size(),
                    agent->HasArrived()
                        ? "停止"
                        : "移動待機");
                ImGui::TextDisabled(
                    "経路計算後にPlayすると黄色い経路に沿って移動します。");
            }
            else if (DrawUIComponentInspector(*component, UIInspectorContext{
                m_selectedAsset,
                m_graphics.UIWidth(),
                m_graphics.UIHeight(),
                [this] { RecordHistory(); },
                [this](const std::string& message, const bool error)
                {
                    SetStatus(message, error);
                },
                [this](const char* id, const std::string& current)
                {
                    return DrawRenderTexturePicker(id, current);
                } }))
            {
                // UI固有の編集は担当へ委譲。削除・並び替えとUndoの
                // 適用順序は他のコンポーネントと同じ入口で管理します。
            }
            else if (auto* particles2D =
                dynamic_cast<
                    SpriteParticles2DComponent*>(
                        component.get()))
            {
                ImGui::Text(
                    "Active: %zu / %u",
                    particles2D->ActiveParticleCount(),
                    particles2D->MaxParticles());
                if (ImGui::Button("Burst 8##SpriteParticles2D"))
                {
                    particles2D->Emit(8);
                }
                ImGui::SameLine();
                if (ImGui::Button("Burst 32##SpriteParticles2D"))
                {
                    particles2D->Emit(32);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear##SpriteParticles2D"))
                {
                    particles2D->Clear();
                }

                int maxParticles = static_cast<int>(
                    particles2D->MaxParticles());
                if (ImGui::SliderInt(
                        "Max Particles##SpriteParticles2D",
                        &maxParticles,
                        1,
                        4096))
                {
                    particles2D->SetMaxParticles(
                        static_cast<std::uint32_t>(maxParticles));
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto lifetime2D = particles2D->Lifetime();
                if (ImGui::DragFloat2(
                        "Lifetime Min / Max##SpriteParticles2D",
                        &lifetime2D.x,
                        0.01f,
                        0.01f,
                        120.0f,
                        "%.2f"))
                {
                    particles2D->SetLifetime(lifetime2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto speed2D = particles2D->StartSpeed();
                if (ImGui::DragFloat2(
                        "Speed Min / Max##SpriteParticles2D",
                        &speed2D.x,
                        1.0f,
                        0.0f,
                        10000.0f,
                        "%.1f"))
                {
                    particles2D->SetStartSpeed(speed2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto size2D = particles2D->StartSize();
                if (ImGui::DragFloat2(
                        "Start Size Min / Max##SpriteParticles2D",
                        &size2D.x,
                        0.25f,
                        0.001f,
                        1000.0f,
                        "%.2f"))
                {
                    particles2D->SetStartSize(size2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float growth2D = particles2D->SizeGrowth();
                if (ImGui::DragFloat(
                        "Size Growth##SpriteParticles2D",
                        &growth2D,
                        0.25f,
                        -10000.0f,
                        10000.0f,
                        "%.2f"))
                {
                    particles2D->SetSizeGrowth(growth2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto gravity2D = particles2D->Gravity();
                if (ImGui::DragFloat2(
                        "Gravity##SpriteParticles2D",
                        &gravity2D.x,
                        1.0f,
                        -10000.0f,
                        10000.0f,
                        "%.1f"))
                {
                    particles2D->SetGravity(gravity2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float drag2D = particles2D->Drag();
                if (ImGui::DragFloat(
                        "Air Drag##SpriteParticles2D",
                        &drag2D,
                        0.01f,
                        0.0f,
                        100.0f,
                        "%.2f"))
                {
                    particles2D->SetDrag(drag2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto startColor2D = particles2D->StartColor();
                if (ImGui::ColorEdit4(
                        "Start Color##SpriteParticles2D",
                        &startColor2D.x))
                {
                    particles2D->SetStartColor(startColor2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                auto endColor2D = particles2D->EndColor();
                if (ImGui::ColorEdit4(
                        "End Color##SpriteParticles2D",
                        &endColor2D.x))
                {
                    particles2D->SetEndColor(endColor2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                int sortOrder2D = particles2D->SortOrder();
                if (ImGui::InputInt(
                        "Sort Order##SpriteParticles2D",
                        &sortOrder2D))
                {
                    particles2D->SetSortOrder(sortOrder2D);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                const auto texturePath2D =
                    PathToUtf8(particles2D->TexturePath());
                ImGui::TextWrapped(
                    "Texture: %s",
                    texturePath2D.empty()
                        ? "(white square)"
                        : texturePath2D.c_str());
                const char* builtInTextureNames2D[] = {
                    "Custom / current",
                    "builtin/circle",
                    "builtin/triangle",
                    "builtin/ring"
                };
                int builtInTexture2D = 0;
                auto normalizedTexturePath2D =
                    Lowercase(texturePath2D);
                std::ranges::replace(
                    normalizedTexturePath2D,
                    '\\',
                    '/');
                for (int index = 1;
                    index < static_cast<int>(
                        std::size(builtInTextureNames2D));
                    ++index)
                {
                    if (normalizedTexturePath2D
                        == builtInTextureNames2D[index])
                    {
                        builtInTexture2D = index;
                        break;
                    }
                }
                if (ImGui::Combo(
                        "Built-in Texture##SpriteParticles2D",
                        &builtInTexture2D,
                        builtInTextureNames2D,
                        static_cast<int>(
                            std::size(builtInTextureNames2D)))
                    && builtInTexture2D > 0)
                {
                    particles2D->SetTexturePath(
                        PathFromUtf8(
                            builtInTextureNames2D[builtInTexture2D]));
                    RecordHistory();
                }
                ImGui::BeginDisabled(
                    !IsTextureAsset(m_selectedAsset));
                if (ImGui::Button(
                        "Use Selected Texture##SpriteParticles2D"))
                {
                    particles2D->SetTexturePath(m_selectedAsset);
                    RecordHistory();
                }
                ImGui::EndDisabled();
                if (!texturePath2D.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button(
                            "Clear Texture##SpriteParticles2D"))
                    {
                        particles2D->SetTexturePath({});
                        RecordHistory();
                    }
                }
            }
            else if (auto* particles =
                dynamic_cast<
                    ParticleSystemComponent*>(
                        component.get()))
            {
                ImGui::Text(
                    "Active: %zu / %u",
                    particles->
                        ActiveParticleCount(),
                    particles->MaxParticles());

                if (ImGui::Button(
                        "再生"))
                {
                    particles->Play();
                }
                ImGui::SameLine();
                if (ImGui::Button(
                        "停止"))
                {
                    particles->Stop(false);
                }
                ImGui::SameLine();
                if (ImGui::Button(
                        "リスタート"))
                {
                    particles->Restart();
                }
                ImGui::SameLine();
                if (ImGui::Button(
                        "Burst 32"))
                {
                    particles->Emit(32);
                }

                int maxParticles =
                    static_cast<int>(
                        particles->
                            MaxParticles());
                if (ImGui::SliderInt(
                        "最大Particle数",
                        &maxParticles,
                        1,
                        4096))
                {
                    particles->
                        SetMaxParticles(
                            static_cast<
                                std::uint32_t>(
                                    maxParticles));
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float emissionRate =
                    particles->
                        EmissionRate();
                if (ImGui::DragFloat(
                        "毎秒放出数",
                        &emissionRate,
                        1.0f,
                        0.0f,
                        10000.0f,
                        "%.1f"))
                {
                    particles->
                        SetEmissionRate(
                            emissionRate);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float duration =
                    particles->Duration();
                if (ImGui::DragFloat(
                        "Duration",
                        &duration,
                        0.1f,
                        0.01f,
                        3600.0f,
                        "%.2f"))
                {
                    particles->SetDuration(
                        duration);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool looping =
                    particles->Looping();
                if (ImGui::Checkbox(
                        "Loop",
                        &looping))
                {
                    particles->SetLooping(
                        looping);
                    RecordHistory();
                }
                bool playOnStart =
                    particles->PlayOnStart();
                if (ImGui::Checkbox(
                        "Play On Start",
                        &playOnStart))
                {
                    particles->
                        SetPlayOnStart(
                            playOnStart);
                    RecordHistory();
                }
                bool preview =
                    particles->
                        PreviewInEditor();
                if (ImGui::Checkbox(
                        "Editorでプレビュー",
                        &preview))
                {
                    particles->
                        SetPreviewInEditor(
                            preview);
                    RecordHistory();
                }
                bool additive =
                    particles->Additive();
                if (ImGui::Checkbox(
                        "加算ブレンド（発光）",
                        &additive))
                {
                    particles->SetAdditive(
                        additive);
                    RecordHistory();
                }

                const char* renderModeNames[]{
                    "カメラ向き",
                    "水平（XZ平面）"
                };
                int selectedRenderMode =
                    static_cast<int>(
                        particles->RenderMode());
                if (ImGui::Combo(
                        "描画方向",
                        &selectedRenderMode,
                        renderModeNames,
                        static_cast<int>(
                            std::size(
                                renderModeNames))))
                {
                    particles->SetRenderMode(
                        static_cast<
                            ParticleRenderMode>(
                                selectedRenderMode));
                    RecordHistory();
                }

                const char* shapeNames[]{
                    "Cone",
                    "Sphere",
                    "Box"
                };
                int selectedShape =
                    static_cast<int>(
                        particles->
                            EmitterShape());
                if (ImGui::Combo(
                        "放出形状",
                        &selectedShape,
                        shapeNames,
                        static_cast<int>(
                            std::size(
                                shapeNames))))
                {
                    particles->
                        SetEmitterShape(
                            static_cast<
                                ParticleEmitterShape>(
                                    selectedShape));
                    particles->Restart();
                    RecordHistory();
                }

                if (particles->EmitterShape()
                    == ParticleEmitterShape::
                        Cone)
                {
                    float coneDegrees =
                        DirectX::
                            XMConvertToDegrees(
                                particles->
                                    ConeAngle());
                    if (ImGui::SliderFloat(
                            "Cone角度",
                            &coneDegrees,
                            0.0f,
                            90.0f,
                            "%.1f°"))
                    {
                        particles->
                            SetConeAngle(
                                DirectX::
                                    XMConvertToRadians(
                                        coneDegrees));
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        particles->Restart();
                        RecordHistory();
                    }
                }
                else if (
                    particles->EmitterShape()
                    == ParticleEmitterShape::Box)
                {
                    auto emitterSize =
                        particles->
                            EmitterSize();
                    if (ImGui::DragFloat3(
                            "Emitterサイズ",
                            &emitterSize.x,
                            0.05f,
                            0.0f,
                            1000.0f,
                            "%.2f"))
                    {
                        particles->
                            SetEmitterSize(
                                emitterSize);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        particles->Restart();
                        RecordHistory();
                    }
                }

                auto lifetime =
                    particles->Lifetime();
                if (ImGui::DragFloat2(
                        "寿命 Min / Max",
                        &lifetime.x,
                        0.05f,
                        0.01f,
                        120.0f,
                        "%.2f"))
                {
                    particles->
                        SetLifetime(lifetime);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto startSpeed =
                    particles->StartSpeed();
                if (ImGui::DragFloat2(
                        "速度 Min / Max",
                        &startSpeed.x,
                        0.05f,
                        0.0f,
                        1000.0f,
                        "%.2f"))
                {
                    particles->
                        SetStartSpeed(
                            startSpeed);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto startSize =
                    particles->StartSize();
                if (ImGui::DragFloat2(
                        "開始サイズ Min / Max",
                        &startSize.x,
                        0.01f,
                        0.001f,
                        100.0f,
                        "%.3f"))
                {
                    particles->
                        SetStartSize(
                            startSize);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float endSize =
                    particles->
                        EndSizeMultiplier();
                if (ImGui::SliderFloat(
                        "終了サイズ倍率",
                        &endSize,
                        0.0f,
                        4.0f,
                        "%.2f"))
                {
                    particles->
                        SetEndSizeMultiplier(
                            endSize);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto startColor =
                    particles->StartColor();
                if (ImGui::ColorEdit4(
                        "開始色",
                        &startColor.x,
                        ImGuiColorEditFlags_AlphaBar))
                {
                    particles->
                        SetStartColor(
                            startColor);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto endColor =
                    particles->EndColor();
                if (ImGui::ColorEdit4(
                        "終了色",
                        &endColor.x,
                        ImGuiColorEditFlags_AlphaBar))
                {
                    particles->
                        SetEndColor(
                            endColor);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto gravity =
                    particles->Gravity();
                if (ImGui::DragFloat3(
                        "重力",
                        &gravity.x,
                        0.05f,
                        -1000.0f,
                        1000.0f,
                        "%.2f"))
                {
                    particles->SetGravity(
                        gravity);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                const auto texturePath =
                    PathToUtf8(
                        particles->
                            TexturePath());
                ImGui::TextWrapped(
                    "Particle Texture: %s",
                    texturePath.empty()
                        ? "白いQuad"
                        : texturePath.c_str());
                ImGui::Button(
                    "画像をここへドロップ##Particle",
                    ImVec2{
                        -1.0f,
                        0.0f });
                if (ImGui::
                    BeginDragDropTarget())
                {
                    if (const ImGuiPayload*
                        payload =
                            ImGui::
                                AcceptDragDropPayload(
                                    AssetPayload))
                    {
                        const auto droppedPath =
                            PathFromUtf8(
                                static_cast<
                                    const char*>(
                                        payload->
                                            Data));
                        if (IsTextureAsset(
                                droppedPath))
                        {
                            try
                            {
                                particles->
                                    SetTexturePath(
                                        droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "Particle Textureを設定しました");
                            }
                            catch (
                                const std::
                                    exception&
                                        exception)
                            {
                                SetStatus(
                                    exception.what(),
                                    true);
                            }
                        }
                        else
                        {
                            SetStatus(
                                "画像ファイルをドロップしてください",
                                true);
                        }
                    }
                    ImGui::
                        EndDragDropTarget();
                }
                ImGui::BeginDisabled(
                    !IsTextureAsset(
                        m_selectedAsset));
                if (ImGui::Button(
                        "選択中の画像を使用",
                        ImVec2{
                            -1.0f,
                            0.0f }))
                {
                    try
                    {
                        particles->
                            SetTexturePath(
                                m_selectedAsset);
                        RecordHistory();
                        SetStatus(
                            "Particle Textureを設定しました");
                    }
                    catch (
                        const std::exception&
                            exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }
                ImGui::EndDisabled();
                if (!texturePath.empty()
                    && ImGui::Button(
                        "Particle Textureを解除"))
                {
                    particles->
                        SetTexturePath({});
                    RecordHistory();
                }

                auto particleShader =
                    particles->ShaderPath();
                if (DrawShaderAssetSelector(
                        "Particle Shader##ParticleSystem",
                        particleShader))
                {
                    particles->SetShaderPath(
                        particleShader);
                    ApplyShaderPropertyDefaults(
                        particleShader,
                        [particles](
                            const std::size_t index,
                            const DirectX::XMFLOAT4& value)
                        {
                            particles->SetCustomParameter(
                                index,
                                value);
                        });
                    RecordHistory();
                }
                if (!particles->ShaderPath().empty())
                {
                    if (ImGui::Button(
                            "Particle Shader 再コンパイル"))
                    {
                        particles->ReloadShader();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(
                            "Particle Shader 解除"))
                    {
                        particles->SetShaderPath({});
                        RecordHistory();
                    }
                }
                if (!particles->ShaderError().empty())
                {
                    ImGui::TextColored(
                        ImVec4{
                            1.0f,
                            0.35f,
                            0.30f,
                            1.0f },
                        "Shader Error: %s",
                        particles->ShaderError().c_str());
                }

                const auto auxiliaryPath =
                    PathToUtf8(
                        particles->
                            AuxiliaryTexturePath());
                ImGui::TextWrapped(
                    "Auxiliary Texture: %s",
                    auxiliaryPath.empty()
                        ? "未設定"
                        : auxiliaryPath.c_str());
                ImGui::BeginDisabled(
                    !IsTextureAsset(
                        m_selectedAsset));
                if (ImGui::Button(
                        "選択中の画像を補助テクスチャへ",
                        ImVec2{ -1.0f, 0.0f }))
                {
                    particles->
                        SetAuxiliaryTexturePath(
                            m_selectedAsset);
                    RecordHistory();
                }
                ImGui::EndDisabled();
                if (!auxiliaryPath.empty()
                    && ImGui::Button(
                        "補助テクスチャを解除"))
                {
                    particles->
                        SetAuxiliaryTexturePath({});
                    RecordHistory();
                }

                if (ImGui::TreeNode(
                        "Particle Shader パラメータ"))
                {
                    for (std::size_t index = 0;
                        index
                            < ParticleSystemComponent::
                                CustomParameterCount;
                        ++index)
                    {
                        auto parameter =
                            particles->
                                CustomParameter(index);
                        const std::string label =
                            "Parameter "
                            + std::to_string(index + 1)
                            + "##ParticleShaderParameter"
                            + std::to_string(index);
                        if (ImGui::DragFloat4(
                                label.c_str(),
                                &parameter.x,
                                0.01f))
                        {
                            particles->
                                SetCustomParameter(
                                    index,
                                    parameter);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    }
                    ImGui::TreePop();
                }
            }
            else if (auto* mesh = dynamic_cast<MeshRendererComponent*>(component.get()))
            {
                if (mesh->HasProceduralMesh())
                {
                    ImGui::Text(
                        "形状: 実行時メッシュ (%zu頂点 / %zu三角形)",
                        mesh->ProceduralVertices().size(),
                        mesh->ProceduralIndices().size() / 3u);
                    ImGui::TextDisabled(
                        "頂点データはScriptが再生成します（Scene保存対象外）");
                }
                else
                {
                    ImGui::Text("形状: %s", ShapeName(mesh->Shape()));
                }
                const auto materialPath =
                    mesh->MaterialAssetPath();
                const auto materialLabel =
                    PathToUtf8(materialPath);
                ImGui::TextWrapped(
                    "Material: %s",
                    materialLabel.empty()
                        ? "（個別設定）"
                        : materialLabel.c_str());
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(
                                payload->Data));
                        if (IsMaterialAsset(droppedPath))
                        {
                            try
                            {
                                mesh->SetMaterialAssetPath(
                                    droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "Lit Materialを割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                        else
                        {
                            SetStatus(
                                "Materialアセットをドロップしてください",
                                true);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    !IsMaterialAsset(m_selectedAsset));
                if (ImGui::Button("選択Materialを割当"))
                {
                    try
                    {
                        mesh->SetMaterialAssetPath(
                            m_selectedAsset);
                        RecordHistory();
                        SetStatus(
                            "Lit Materialを割り当てました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();
                if (!materialPath.empty())
                {
                    if (ImGui::Button("Materialへ保存"))
                    {
                        try
                        {
                            SaveLitMaterialAsset(
                                m_graphics.Assets().ResolvePath(
                                    materialPath),
                                mesh->Material(),
                                &m_graphics.Assets().
                                    Database());
                            ReloadSharedMaterial(materialPath);
                            SetStatus(
                                "共有Materialを保存しました: "
                                + materialLabel);
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("再読込"))
                    {
                        try
                        {
                            mesh->ReloadMaterialAsset();
                            RecordHistory();
                            SetStatus(
                                "Materialを再読み込みしました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("個別化"))
                    {
                        mesh->SetMaterialAssetPath({});
                        RecordHistory();
                        SetStatus(
                            "Material参照を解除し、現在値を保持しました");
                    }
                    ImGui::TextDisabled(
                        "保存すると同じMaterialを使う全オブジェクトへ反映されます。");
                }

                auto color = mesh->Color();
                if (ImGui::ColorEdit4("色", &color.x))
                {
                    mesh->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool worldOverlay = mesh->IsWorldOverlay();
                if (ImGui::Checkbox(
                    "3Dオーバーレイ（深度なし）",
                    &worldOverlay))
                {
                    mesh->SetWorldOverlay(worldOverlay);
                    RecordHistory();
                }

                float roughness = mesh->Roughness();
                if (ImGui::SliderFloat(
                    "粗さ",
                    &roughness,
                    0.02f,
                    1.0f,
                    "%.2f"))
                {
                    mesh->SetRoughness(roughness);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float meshMetallic = mesh->Metallic();
                if (ImGui::SliderFloat(
                    "メタリック",
                    &meshMetallic,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    mesh->SetMetallic(meshMetallic);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float normalStrength =
                    mesh->NormalStrength();
                if (ImGui::SliderFloat(
                    "法線強度",
                    &normalStrength,
                    0.0f,
                    2.0f,
                    "%.2f"))
                {
                    mesh->SetNormalStrength(normalStrength);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto shaderSelection = mesh->ShaderPath();
                if (DrawShaderAssetSelector(
                        "Shaderを選択##MeshRenderer",
                        shaderSelection))
                {
                    mesh->SetShaderPath(shaderSelection);
                    ApplyShaderPropertyDefaults(
                        shaderSelection,
                        [mesh](
                            const std::size_t index,
                            const DirectX::XMFLOAT4& value)
                        {
                            mesh->SetCustomParameter(index, value);
                        });
                    RecordHistory();
                    SetStatus(
                        shaderSelection.empty()
                            ? "標準Lit Shaderへ戻しました"
                            : "カスタムShaderを設定しました");
                }
                const auto shaderPath =
                    PathToUtf8(mesh->ShaderPath());
                ImGui::TextWrapped(
                    "Shader: %s",
                    shaderPath.empty()
                        ? "（LamaPon Lit）"
                        : shaderPath.c_str());
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (IsAssignableShaderAsset(droppedPath))
                        {
                            mesh->SetShaderPath(droppedPath);
                            ApplyShaderPropertyDefaults(
                                droppedPath,
                                [mesh](
                                    const std::size_t index,
                                    const DirectX::XMFLOAT4& value)
                                {
                                    mesh->SetCustomParameter(index, value);
                                });
                            RecordHistory();
                            SetStatus("カスタムShaderを設定しました");
                        }
                        else
                        {
                            SetStatus(
                                IsShaderErrorPlaceholder(droppedPath)
                                    ? "このShaderはエンジンが"
                                      "「壊れている印」に使うため、"
                                      "割り当てられません"
                                    : "HLSLファイルをドロップして"
                                      "ください",
                                true);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::BeginDisabled(
                    !IsAssignableShaderAsset(m_selectedAsset));
                if (ImGui::Button("選択Shaderを設定"))
                {
                    mesh->SetShaderPath(m_selectedAsset);
                    ApplyShaderPropertyDefaults(
                        m_selectedAsset,
                        [mesh](
                            const std::size_t index,
                            const DirectX::XMFLOAT4& value)
                        {
                            mesh->SetCustomParameter(index, value);
                        });
                    RecordHistory();
                }
                // 押せない理由を出します。灰色のまま黙っていると
                // 「壊れている」と読まれます。
                if (IsShaderErrorPlaceholder(m_selectedAsset))
                {
                    showItemTooltip(
                        "このShaderはエンジンが「壊れている印」に"
                        "使うため、割り当てられません");
                }
                ImGui::EndDisabled();
                if (!mesh->ShaderPath().empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Shader再コンパイル"))
                    {
                        mesh->ReloadShader();
                        SetStatus(
                            mesh->ShaderError().empty()
                                ? "Shaderを再コンパイルしました"
                                : mesh->ShaderError(),
                            !mesh->ShaderError().empty());
                    }
                    if (ImGui::Button("標準Litへ戻す"))
                    {
                        mesh->SetShaderPath({});
                        RecordHistory();
                    }
                }
                if (!mesh->ShaderError().empty())
                {
                    ImGui::TextColored(
                        ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                        "Shader Error: %s",
                        mesh->ShaderError().c_str());
                }
                if (m_graphics.IsShaderCompiling(
                        mesh->ShaderPath(),
                        mesh->ShaderKeywords()))
                {
                    ImGui::TextColored(
                        ImVec4{ 0.55f, 0.80f, 1.0f, 1.0f },
                        "Shaderをコンパイル中… "
                        "（出来るまで標準Litで描いています）");
                }
                if (DrawShaderKeywordToggles(
                        mesh->ShaderPath(),
                        "MeshShaderKeywords",
                        mesh->ShaderKeywords(),
                        [mesh](ShaderKeywordSet keywords)
                        {
                            mesh->SetShaderKeywords(
                                std::move(keywords));
                        }))
                {
                    RecordHistory();
                }
                if (ImGui::TreeNode("カスタムShaderパラメーター"))
                {
                    if (DrawCustomShaderParameters(
                        mesh->ShaderPath(),
                        "MeshShaderParameters",
                        [mesh](const std::size_t index)
                        {
                            return mesh->CustomParameter(
                                index);
                        },
                        [mesh](
                            const std::size_t index,
                            const DirectX::XMFLOAT4& value)
                        {
                            mesh->SetCustomParameter(
                                index,
                                value);
                        },
                        [mesh](const std::size_t index)
                        {
                            return mesh->Material()
                                .CustomTexture(index);
                        },
                        [mesh](
                            const std::size_t index,
                            std::filesystem::path path)
                        {
                            mesh->SetCustomTexturePath(
                                index,
                                std::move(path));
                        }))
                    {
                        // スライダーを離した時点で履歴へ入れます。
                        if (ImGui::IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    }
                    ImGui::TreePop();
                }

                const auto albedoPath =
                    PathToUtf8(mesh->AlbedoTexturePath());
                ImGui::TextWrapped(
                    "アルベド: %s",
                    albedoPath.empty()
                        ? "（白テクスチャ）"
                        : albedoPath.c_str());
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (!IsTextureAsset(droppedPath))
                        {
                            SetStatus(
                                "画像ファイルをドロップしてください",
                                true);
                        }
                        else
                        {
                            try
                            {
                                mesh->SetAlbedoTexturePath(
                                    droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "アルベド画像を割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    !IsTextureAsset(m_selectedAsset));
                if (ImGui::Button("選択画像をアルベドへ"))
                {
                    try
                    {
                        mesh->SetAlbedoTexturePath(
                            m_selectedAsset);
                        RecordHistory();
                        SetStatus(
                            "アルベド画像を割り当てました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();
                if (!mesh->AlbedoTexturePath().empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("アルベド解除"))
                    {
                        mesh->SetAlbedoTexturePath({});
                        RecordHistory();
                    }
                }

                const auto normalPath =
                    PathToUtf8(mesh->NormalTexturePath());
                ImGui::TextWrapped(
                    "法線マップ: %s",
                    normalPath.empty()
                        ? "（未設定）"
                        : normalPath.c_str());
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (!IsTextureAsset(droppedPath))
                        {
                            SetStatus(
                                "画像ファイルをドロップしてください",
                                true);
                        }
                        else
                        {
                            try
                            {
                                mesh->SetNormalTexturePath(
                                    droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "法線マップを割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    !IsTextureAsset(m_selectedAsset));
                if (ImGui::Button("選択画像を法線へ"))
                {
                    try
                    {
                        mesh->SetNormalTexturePath(
                            m_selectedAsset);
                        RecordHistory();
                        SetStatus(
                            "法線マップを割り当てました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();
                if (!mesh->NormalTexturePath().empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("法線解除"))
                    {
                        mesh->SetNormalTexturePath({});
                        RecordHistory();
                    }
                }

                drawPbrMapSlots(mesh);
            }
            else if (auto* model = dynamic_cast<ModelRendererComponent*>(component.get()))
            {
                const auto modelPath = PathToUtf8(model->ModelPath());
                ImGui::TextWrapped(
                    "モデル: %s",
                    modelPath.empty() ? "（未設定）" : modelPath.c_str());

                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const std::filesystem::path droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (IsModelAsset(droppedPath))
                        {
                            try
                            {
                                model->SetModelPath(droppedPath);
                                RecordHistory();
                                SetStatus("3Dモデルを割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (model->AnimationCount() > 0)
                {
                    const auto controllerPath =
                        PathToUtf8(
                            model->AnimationControllerPath());
                    ImGui::SeparatorText(
                        "スケルタルアニメーション");
                    ImGui::TextWrapped(
                        "Animator Controller: %s",
                        controllerPath.empty()
                            ? "（未設定）"
                            : controllerPath.c_str());
                    ImGui::Button(
                        "Animator Controllerをドロップ",
                        ImVec2{ -1.0f, 0.0f });
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(
                                    AssetPayload))
                        {
                            const auto droppedPath =
                                PathFromUtf8(
                                    static_cast<const char*>(
                                        payload->Data));
                            if (!IsAnimatorControllerAsset(
                                    droppedPath))
                            {
                                SetStatus(
                                    "Animator Controllerをドロップしてください",
                                    true);
                            }
                            else
                            {
                                try
                                {
                                    model->
                                        SetAnimationControllerPath(
                                            droppedPath);
                                    RecordHistory();
                                    SetStatus(
                                        "モデルへAnimator Controllerを割り当てました");
                                }
                                catch (const std::exception&
                                    exception)
                                {
                                    SetStatus(
                                        exception.what(),
                                        true);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::BeginDisabled(
                        !IsAnimatorControllerAsset(
                            m_selectedAsset));
                    if (ImGui::Button(
                            "選択中Controllerを割り当て"))
                    {
                        try
                        {
                            model->SetAnimationControllerPath(
                                m_selectedAsset);
                            RecordHistory();
                            SetStatus(
                                "モデルへAnimator Controllerを割り当てました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(
                                exception.what(),
                                true);
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(
                        controllerPath.empty());
                    if (ImGui::Button("Controller再読込"))
                    {
                        try
                        {
                            model->ReloadAnimationController();
                            SetStatus(
                                "モデルのAnimator Controllerを再読み込みしました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(
                                exception.what(),
                                true);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("解除"))
                    {
                        model->SetAnimationControllerPath({});
                        RecordHistory();
                    }
                    ImGui::EndDisabled();

                    if (!controllerPath.empty())
                    {
                        ImGui::Text(
                            "現在State: %s%s",
                            model->CurrentAnimationState().empty()
                                ? "（初期化待ち）"
                                : model->CurrentAnimationState()
                                    .c_str(),
                            model->IsAnimationTransitioning()
                                ? "（ブレンド中）"
                                : "");
                        for (const auto& trigger :
                            model->AnimationTriggers())
                        {
                            const std::string label =
                                trigger + "##ModelAnimatorTrigger";
                            if (ImGui::Button(label.c_str()))
                            {
                                model->SetAnimationTrigger(
                                    trigger);
                            }
                            ImGui::SameLine();
                        }
                        ImGui::NewLine();
                        for (const auto& parameter :
                            model->
                                AnimationFloatParameters())
                        {
                            float value =
                                model->AnimationFloat(
                                    parameter.name);
                            const std::string label =
                                parameter.name
                                + "##ModelAnimatorFloat";
                            if (ImGui::DragFloat(
                                    label.c_str(),
                                    &value,
                                    0.01f))
                            {
                                model->SetAnimationFloat(
                                    parameter.name,
                                    value);
                            }
                        }
                        if (model->
                                PendingAnimationEventCount()
                            > 0)
                        {
                            ImGui::Text(
                                "未処理Animation Event: %zu",
                                model->
                                    PendingAnimationEventCount());
                            if (ImGui::Button(
                                    "次のEventを確認"))
                            {
                                AnimationEventNotification
                                    event;
                                if (model->
                                    PollAnimationEvent(
                                        event))
                                {
                                    SetStatus(
                                        "Animation Event ["
                                        + event.state
                                        + "] "
                                        + event.name
                                        + (event.payload.empty()
                                            ? std::string{}
                                            : " / "
                                                + event.payload));
                                }
                            }
                        }
                    }

                    ImGui::SeparatorText("Root Motion");
                    bool applyRootMotion =
                        model->ApplyRootMotion();
                    if (ImGui::Checkbox(
                            "Root Motionを適用",
                            &applyRootMotion))
                    {
                        model->SetApplyRootMotion(
                            applyRootMotion);
                        RecordHistory();
                    }
                    const std::string rootMotionPreview =
                        model->RootMotionNode().empty()
                        ? "自動（最上位ノード）"
                        : model->RootMotionNode();
                    if (ImGui::BeginCombo(
                            "Root Motionノード",
                            rootMotionPreview.c_str()))
                    {
                        if (ImGui::Selectable(
                                "自動（最上位ノード）",
                                model->RootMotionNode()
                                    .empty()))
                        {
                            model->SetRootMotionNode({});
                            RecordHistory();
                        }
                        for (const auto& nodeName :
                            model->SkeletonNodeNames())
                        {
                            if (ImGui::Selectable(
                                    nodeName.c_str(),
                                    nodeName
                                        == model->
                                            RootMotionNode()))
                            {
                                model->SetRootMotionNode(
                                    nodeName);
                                RecordHistory();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TextDisabled(
                        "Play中にルート移動とY回転をGameObjectへ反映します。");

                    const auto modelObjectId =
                        model->Owner().Id();
                    const bool editorPreviewing =
                        !m_playing
                        && m_modelAnimationPreviewObjectId
                            == modelObjectId;
                    if (editorPreviewing)
                    {
                        model->AdvanceAnimation(
                            ImGui::GetIO().DeltaTime,
                            false);
                    }
                    const std::string currentClip(
                        model->AnimationName(
                            model->AnimationIndex()));
                    ImGui::BeginDisabled(
                        !controllerPath.empty());
                    if (ImGui::BeginCombo(
                            "アニメーションクリップ",
                            currentClip.c_str()))
                    {
                        for (std::size_t index = 0;
                            index < model->AnimationCount();
                            ++index)
                        {
                            const bool clipSelected =
                                index
                                == model->AnimationIndex();
                            const std::string name(
                                model->AnimationName(index));
                            if (ImGui::Selectable(
                                    name.c_str(),
                                    clipSelected))
                            {
                                model->SetAnimationIndex(index);
                                RecordHistory();
                            }
                            if (clipSelected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();

                    const bool animationUiPlaying =
                        m_playing
                            ? model->IsAnimationPlaying()
                            : editorPreviewing;
                    if (animationUiPlaying)
                    {
                        if (ImGui::Button(
                                "アニメーション一時停止"))
                        {
                            model->PauseAnimation();
                            if (!m_playing)
                            {
                                m_modelAnimationPreviewObjectId =
                                    {};
                            }
                        }
                    }
                    else if (ImGui::Button(
                        "アニメーション再生"))
                    {
                        if (!m_playing)
                        {
                            m_modelAnimationPreviewObjectId =
                                modelObjectId;
                        }
                        model->PlayAnimation();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(
                            "アニメーション停止"))
                    {
                        model->StopAnimation();
                        if (!m_playing)
                        {
                            m_modelAnimationPreviewObjectId =
                                {};
                        }
                    }

                    float animationTime =
                        model->AnimationTime();
                    if (ImGui::SliderFloat(
                            "再生位置",
                            &animationTime,
                            0.0f,
                            model->AnimationDuration(),
                            "%.2f 秒"))
                    {
                        model->SetAnimationTime(
                            animationTime);
                    }
                    float animationSpeed =
                        model->AnimationSpeed();
                    if (ImGui::DragFloat(
                            "再生速度",
                            &animationSpeed,
                            0.05f,
                            -8.0f,
                            8.0f,
                            "%.2fx"))
                    {
                        model->SetAnimationSpeed(
                            animationSpeed);
                        RecordHistory();
                    }
                    bool animationLoop =
                        model->AnimationLoop();
                    if (ImGui::Checkbox(
                            "ループ再生",
                            &animationLoop))
                    {
                        model->SetAnimationLoop(
                            animationLoop);
                        RecordHistory();
                    }
                    bool playOnStart =
                        model->AnimationPlayOnStart();
                    if (ImGui::Checkbox(
                            "ゲーム開始時に再生",
                            &playOnStart))
                    {
                        model->SetAnimationPlayOnStart(
                            playOnStart);
                        RecordHistory();
                    }
                }

                const auto materialPath =
                    model->MaterialAssetPath();
                const auto materialLabel =
                    PathToUtf8(materialPath);
                ImGui::TextWrapped(
                    "Material: %s",
                    materialLabel.empty()
                        ? "（個別設定）"
                        : materialLabel.c_str());
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(
                                payload->Data));
                        if (IsMaterialAsset(droppedPath))
                        {
                            try
                            {
                                model->SetMaterialAssetPath(
                                    droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "Lit Materialを割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                        else
                        {
                            SetStatus(
                                "Materialアセットをドロップしてください",
                                true);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    !IsMaterialAsset(m_selectedAsset));
                if (ImGui::Button(
                    "選択Materialをモデルへ"))
                {
                    try
                    {
                        model->SetMaterialAssetPath(
                            m_selectedAsset);
                        RecordHistory();
                        SetStatus(
                            "Lit Materialを割り当てました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();
                if (!materialPath.empty())
                {
                    if (ImGui::Button(
                        "モデルMaterialへ保存"))
                    {
                        try
                        {
                            SaveLitMaterialAsset(
                                m_graphics.Assets().ResolvePath(
                                    materialPath),
                                model->Material(),
                                &m_graphics.Assets().
                                    Database());
                            ReloadSharedMaterial(materialPath);
                            SetStatus(
                                "共有Materialを保存しました: "
                                + materialLabel);
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("モデル再読込"))
                    {
                        try
                        {
                            model->ReloadMaterialAsset();
                            RecordHistory();
                            SetStatus(
                                "Materialを再読み込みしました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    if (ImGui::Button("モデルMaterialを個別化"))
                    {
                        model->SetMaterialAssetPath({});
                        RecordHistory();
                        SetStatus(
                            "Material参照を解除し、現在値を保持しました");
                    }
                    ImGui::TextDisabled(
                        "保存すると同じMaterialを使う全オブジェクトへ反映されます。");
                }

                bool wireframe = model->IsWireframe();
                if (ImGui::Checkbox("ワイヤーフレーム", &wireframe))
                {
                    model->SetWireframe(wireframe);
                    RecordHistory();
                }

                bool materialOverride =
                    model->IsMaterialOverrideEnabled();
                if (ImGui::Checkbox(
                    "マテリアルを上書き",
                    &materialOverride))
                {
                    try
                    {
                        model->SetMaterialOverrideEnabled(
                            materialOverride);
                        RecordHistory();
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }

                bool legacyShading =
                    model->UsesLegacyShading();
                if (ImGui::Checkbox(
                    "従来のDirectXTK描画を使う",
                    &legacyShading))
                {
                    try
                    {
                        model->SetUseLegacyShading(
                            legacyShading);
                        RecordHistory();
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "オフ（既定）はLamaPon LitのPBRで描き、"
                        "法線マップ・metallicが効きます。"
                        "旧バージョンと同じ見た目に戻したいときだけ"
                        "オンにしてください");
                }

                bool preserveEmbeddedColor =
                    model->PreserveEmbeddedMaterialColor();
                if (ImGui::Checkbox(
                    "モデル内の色を保持",
                    &preserveEmbeddedColor))
                {
                    model->SetPreserveEmbeddedMaterialColor(
                        preserveEmbeddedColor);
                    RecordHistory();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "CMO/SDKMESH内のDiffuseColorへ上書き色を掛けます");
                }

                // 上書きの有無に関係なく実際の描画経路を出します
                // （Litか従来のDirectXTKかが分かるように）。
                if (model->UsesLamaPonLit())
                {
                    const auto status =
                        model->CommonLitStatus();
                    ImGui::TextDisabled(
                        "描画経路: %.*s",
                        static_cast<int>(status.size()),
                        status.data());
                }
                else
                {
                    ImGui::TextDisabled(
                        "描画経路: DirectXTK内蔵マテリアル");
                }

                if (model->IsMaterialOverrideEnabled())
                {
                    auto color = model->Color();
                    if (ImGui::ColorEdit3(
                        "ベースカラー",
                        &color.x))
                    {
                        model->SetColor(color);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float roughness = model->Roughness();
                    if (ImGui::SliderFloat(
                        "ラフネス",
                        &roughness,
                        0.02f,
                        1.0f,
                        "%.2f"))
                    {
                        model->SetRoughness(roughness);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float modelMetallic =
                        model->Metallic();
                    if (ImGui::SliderFloat(
                        "メタリック##Model",
                        &modelMetallic,
                        0.0f,
                        1.0f,
                        "%.2f"))
                    {
                        model->SetMetallic(modelMetallic);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float normalStrength =
                        model->NormalStrength();
                    if (ImGui::SliderFloat(
                        "モデル法線強度",
                        &normalStrength,
                        0.0f,
                        2.0f,
                        "%.2f"))
                    {
                        model->SetNormalStrength(
                            normalStrength);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    auto shaderSelection =
                        model->ShaderPath();
                    if (DrawShaderAssetSelector(
                            "Shaderを選択##ModelRenderer",
                            shaderSelection))
                    {
                        model->SetShaderPath(
                            shaderSelection);
                        ApplyShaderPropertyDefaults(
                            shaderSelection,
                            [model](
                                const std::size_t index,
                                const DirectX::XMFLOAT4& value)
                            {
                                model->SetCustomParameter(
                                    index,
                                    value);
                            });
                        RecordHistory();
                        SetStatus(
                            shaderSelection.empty()
                                ? "標準Lit Shaderへ戻しました"
                                : "モデルへカスタムShaderを設定しました");
                    }
                    const auto shaderPath =
                        PathToUtf8(model->ShaderPath());
                    ImGui::TextWrapped(
                        "Shader: %s",
                        shaderPath.empty()
                            ? "（LamaPon Lit）"
                            : shaderPath.c_str());
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(AssetPayload))
                        {
                            const auto droppedPath = PathFromUtf8(
                                static_cast<const char*>(payload->Data));
                            if (IsAssignableShaderAsset(droppedPath))
                            {
                                model->SetShaderPath(droppedPath);
                                ApplyShaderPropertyDefaults(
                                    droppedPath,
                                    [model](
                                        const std::size_t index,
                                        const DirectX::XMFLOAT4& value)
                                    {
                                        model->SetCustomParameter(index, value);
                                    });
                                RecordHistory();
                                SetStatus("モデルへカスタムShaderを設定しました");
                            }
                            else
                            {
                                SetStatus(
                                    IsShaderErrorPlaceholder(
                                        droppedPath)
                                        ? "このShaderはエンジンが"
                                          "「壊れている印」に使うため、"
                                          "割り当てられません"
                                        : "HLSLファイルをドロップして"
                                          "ください",
                                    true);
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::BeginDisabled(
                        !IsAssignableShaderAsset(m_selectedAsset));
                    if (ImGui::Button("選択Shaderをモデルへ"))
                    {
                        model->SetShaderPath(m_selectedAsset);
                        ApplyShaderPropertyDefaults(
                            m_selectedAsset,
                            [model](
                                const std::size_t index,
                                const DirectX::XMFLOAT4& value)
                            {
                                model->SetCustomParameter(index, value);
                            });
                        RecordHistory();
                    }
                    // 押せない理由を出します。灰色のまま黙っていると
                    // 「壊れている」と読まれます。
                    if (IsShaderErrorPlaceholder(m_selectedAsset))
                    {
                        showItemTooltip(
                            "このShaderはエンジンが「壊れている印」に"
                            "使うため、割り当てられません");
                    }
                    ImGui::EndDisabled();
                    if (!model->ShaderPath().empty())
                    {
                        if (ImGui::Button("モデルShader再コンパイル"))
                        {
                            model->ReloadShader();
                            SetStatus(
                                model->ShaderError().empty()
                                    ? "Shaderを再コンパイルしました"
                                    : model->ShaderError(),
                                !model->ShaderError().empty());
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("モデルを標準Litへ"))
                        {
                            model->SetShaderPath({});
                            RecordHistory();
                        }
                    }
                    if (!model->ShaderError().empty())
                    {
                        ImGui::TextColored(
                            ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                            "Shader Error: %s",
                            model->ShaderError().c_str());
                    }
                    if (m_graphics.IsShaderCompiling(
                            model->ShaderPath(),
                            model->ShaderKeywords()))
                    {
                        ImGui::TextColored(
                            ImVec4{ 0.55f, 0.80f, 1.0f, 1.0f },
                            "Shaderをコンパイル中… "
                            "（出来るまで標準Litで描いています）");
                    }
                    if (DrawShaderKeywordToggles(
                            model->ShaderPath(),
                            "ModelShaderKeywords",
                            model->ShaderKeywords(),
                            [model](
                                ShaderKeywordSet keywords)
                            {
                                model->SetShaderKeywords(
                                    std::move(keywords));
                            }))
                    {
                        RecordHistory();
                    }
                    if (ImGui::TreeNode(
                        "モデルShaderパラメーター"))
                    {
                        if (DrawCustomShaderParameters(
                            model->ShaderPath(),
                            "ModelShaderParameters",
                            [model](
                                const std::size_t index)
                            {
                                return model
                                    ->CustomParameter(index);
                            },
                            [model](
                                const std::size_t index,
                                const DirectX::XMFLOAT4& value)
                            {
                                model->SetCustomParameter(
                                    index,
                                    value);
                            },
                            [model](
                                const std::size_t index)
                            {
                                return model->Material()
                                    .CustomTexture(index);
                            },
                            [model](
                                const std::size_t index,
                                std::filesystem::path path)
                            {
                                model->SetCustomTexturePath(
                                    index,
                                    std::move(path));
                            }))
                        {
                            if (ImGui::IsItemDeactivatedAfterEdit())
                            {
                                RecordHistory();
                            }
                        }
                        ImGui::TreePop();
                    }

                    const auto albedoPath =
                        PathToUtf8(
                            model->AlbedoTexturePath());
                    ImGui::TextWrapped(
                        "アルベド: %s",
                        albedoPath.empty()
                            ? model->UsesCommonLit()
                                ? "（白テクスチャ）"
                                : "（モデル内蔵を使用）"
                            : albedoPath.c_str());
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(
                                AssetPayload))
                        {
                            const auto droppedPath =
                                PathFromUtf8(
                                    static_cast<const char*>(
                                        payload->Data));
                            if (IsTextureAsset(droppedPath))
                            {
                                try
                                {
                                    model->SetAlbedoTexturePath(
                                        droppedPath);
                                    RecordHistory();
                                    SetStatus(
                                        "モデルのアルベド画像を割り当てました");
                                }
                                catch (const std::exception& exception)
                                {
                                    SetStatus(
                                        exception.what(),
                                        true);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    ImGui::BeginDisabled(
                        !IsTextureAsset(m_selectedAsset));
                    if (ImGui::Button(
                        "選択画像をモデルのアルベドへ"))
                    {
                        try
                        {
                            model->SetAlbedoTexturePath(
                                m_selectedAsset);
                            RecordHistory();
                            SetStatus(
                                "モデルのアルベド画像を割り当てました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!model->AlbedoTexturePath().empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("モデルアルベド解除"))
                        {
                            try
                            {
                                model->SetAlbedoTexturePath({});
                                RecordHistory();
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }

                    const auto normalPath =
                        PathToUtf8(
                            model->NormalTexturePath());
                    ImGui::TextWrapped(
                        "法線マップ: %s",
                        normalPath.empty()
                            ? model->UsesCommonLit()
                                ? "（フラット法線）"
                                : "（モデル内蔵を使用）"
                            : normalPath.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            model->UsesCommonLit()
                                ? "LamaPon Litの法線マッピングを使用します"
                                : "DirectXTKのNormalMapEffectを使うモデルで有効です");
                    }
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(
                                AssetPayload))
                        {
                            const auto droppedPath =
                                PathFromUtf8(
                                    static_cast<const char*>(
                                        payload->Data));
                            if (IsTextureAsset(droppedPath))
                            {
                                try
                                {
                                    model->SetNormalTexturePath(
                                        droppedPath);
                                    RecordHistory();
                                    SetStatus(
                                        "モデルの法線マップを割り当てました");
                                }
                                catch (const std::exception& exception)
                                {
                                    SetStatus(
                                        exception.what(),
                                        true);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    ImGui::BeginDisabled(
                        !IsTextureAsset(m_selectedAsset));
                    if (ImGui::Button(
                        "選択画像をモデルの法線へ"))
                    {
                        try
                        {
                            model->SetNormalTexturePath(
                                m_selectedAsset);
                            RecordHistory();
                            SetStatus(
                                "モデルの法線マップを割り当てました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(exception.what(), true);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!model->NormalTexturePath().empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("モデル法線解除"))
                        {
                            try
                            {
                                model->SetNormalTexturePath({});
                                RecordHistory();
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                }

                drawPbrMapSlots(model);

                if (!modelPath.empty() && ImGui::Button("モデルを解除"))
                {
                    model->SetModelPath({});
                    RecordHistory();
                    SetStatus("3Dモデルを解除しました");
                }
            }
            else if (auto* sprite = dynamic_cast<SpriteRendererComponent*>(component.get()))
            {
                const auto pickedRenderTexture =
                    DrawRenderTexturePicker(
                        "SpriteRenderTexture",
                        sprite->RenderTexture());
                if (pickedRenderTexture.value.has_value())
                {
                    sprite->SetRenderTexture(
                        *pickedRenderTexture.value);
                }
                if (pickedRenderTexture.commit)
                {
                    RecordHistory();
                }

                auto size = sprite->Size();
                if (ImGui::InputFloat2("サイズ", &size.x))
                {
                    sprite->SetSize(size);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto color = sprite->Color();
                if (ImGui::ColorEdit4("色", &color.x))
                {
                    sprite->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto pivot = sprite->Pivot();
                if (ImGui::InputFloat2("基準点", &pivot.x))
                {
                    sprite->SetPivot(pivot);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                if (selected->GetComponent<
                        UIRectTransformComponent>()
                    != nullptr)
                {
                    ImGui::TextDisabled(
                        "UI Rect Transformがあるので使われません"
                        "（位置はRect Transform、回転は矩形の中心）");
                }
                else
                {
                    ImGui::TextDisabled(
                        "0,0で左上、0.5,0.5で中心。位置が指す点と"
                        "回転の中心になります");
                }

                auto sortOrder = sprite->SortOrder();
                if (ImGui::InputInt("描画順", &sortOrder))
                {
                    sprite->SetSortOrder(sortOrder);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "数値が大きいほど手前に表示されます");

                const char* maskInteractionNames[]{
                    "なし",
                    "マスクの内側だけ表示",
                    "マスクの外側だけ表示"
                };
                int selectedMaskInteraction =
                    static_cast<int>(
                        sprite->MaskInteraction());
                if (ImGui::Combo(
                        "Sprite Mask",
                        &selectedMaskInteraction,
                        maskInteractionNames,
                        static_cast<int>(
                            std::size(
                                maskInteractionNames))))
                {
                    sprite->SetMaskInteraction(
                        static_cast<
                            SpriteMaskInteraction>(
                                selectedMaskInteraction));
                    RecordHistory();
                }

                // 正規化ソース矩形（0～1）。SpriteAnimator使用時は
                // 毎フレーム上書きされます。
                auto sourceRect = sprite->SourceRect();
                if (ImGui::InputFloat4(
                        "切り出し矩形 (U, V, 幅, 高さ / 0～1)",
                        &sourceRect.x))
                {
                    sprite->SetSourceRect(sourceRect);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "{0,0,1,1}で画像全体を表示します");
                const bool hasSourceRect =
                    sourceRect.x != 0.0f
                    || sourceRect.y != 0.0f
                    || sourceRect.z != 1.0f
                    || sourceRect.w != 1.0f;
                if (hasSourceRect
                    && ImGui::Button("切り出し矩形を解除"))
                {
                    sprite->SetSourceRect(
                        { 0.0f, 0.0f, 1.0f, 1.0f });
                    RecordHistory();
                }

                auto spriteShader = sprite->ShaderPath();
                if (DrawShaderAssetSelector(
                        "UIシェーダー##SpriteRenderer",
                        spriteShader))
                {
                    sprite->SetShaderPath(spriteShader);
                    ApplyShaderPropertyDefaults(
                        spriteShader,
                        [sprite](
                            const std::size_t index,
                            const DirectX::XMFLOAT4& value)
                        {
                            sprite->SetCustomParameter(index, value);
                        });
                    RecordHistory();
                }
                const auto spriteShaderPath =
                    PathToUtf8(sprite->ShaderPath());
                ImGui::TextWrapped(
                    "UI Shader: %s",
                    spriteShaderPath.empty()
                        ? "標準 Sprite"
                        : spriteShaderPath.c_str());
                if (!sprite->ShaderPath().empty())
                {
                    if (ImGui::Button(
                            "UIシェーダーを再コンパイル"))
                    {
                        sprite->ReloadShader();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(
                            "標準Spriteへ戻す"))
                    {
                        sprite->SetShaderPath({});
                        RecordHistory();
                    }
                }
                if (!sprite->ShaderError().empty())
                {
                    ImGui::TextColored(
                        ImVec4{
                            1.0f,
                            0.35f,
                            0.30f,
                            1.0f
                        },
                        "Shader Error: %s",
                        sprite->ShaderError().c_str());
                }
                if (ImGui::TreeNode(
                        "UIシェーダーパラメーター"))
                {
                    for (std::size_t index = 0;
                        index
                            < SpriteRendererComponent::
                                CustomParameterCount;
                        ++index)
                    {
                        auto parameter =
                            sprite->CustomParameter(index);
                        const std::string label =
                            "Parameter "
                            + std::to_string(index + 1)
                            + "##SpriteShaderParameter"
                            + std::to_string(index);
                        if (ImGui::DragFloat4(
                                label.c_str(),
                                &parameter.x,
                                0.01f))
                        {
                            sprite->SetCustomParameter(
                                index,
                                parameter);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    }
                    ImGui::TreePop();
                }

                const auto texturePath = PathToUtf8(sprite->TexturePath());
                ImGui::TextWrapped(
                    "テクスチャ: %s",
                    texturePath.empty() ? "（白）" : texturePath.c_str());

                const char* builtInTextureNames[] = {
                    "Custom / current",
                    "builtin/circle",
                    "builtin/triangle",
                    "builtin/ring"
                };
                int builtInTexture = 0;
                auto normalizedTexturePath =
                    Lowercase(texturePath);
                std::ranges::replace(
                    normalizedTexturePath,
                    '\\',
                    '/');
                for (int index = 1;
                    index < static_cast<int>(
                        std::size(builtInTextureNames));
                    ++index)
                {
                    if (normalizedTexturePath
                        == builtInTextureNames[index])
                    {
                        builtInTexture = index;
                        break;
                    }
                }
                if (ImGui::Combo(
                        "Built-in Texture##SpriteRenderer",
                        &builtInTexture,
                        builtInTextureNames,
                        static_cast<int>(
                            std::size(builtInTextureNames)))
                    && builtInTexture > 0)
                {
                    sprite->SetTexturePath(
                        PathFromUtf8(
                            builtInTextureNames[builtInTexture]));
                    RecordHistory();
                }

                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (!IsTextureAsset(droppedPath))
                        {
                            SetStatus(
                                "画像ファイルをドロップしてください",
                                true);
                        }
                        else
                        {
                            try
                            {
                                sprite->SetTexturePath(droppedPath);
                                RecordHistory();
                                SetStatus("スプライト画像を割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (!texturePath.empty() && ImGui::Button("画像を解除"))
                {
                    sprite->SetTexturePath({});
                    RecordHistory();
                    SetStatus("スプライト画像を解除しました");
                }
            }
            else if (auto* spriteMask =
                dynamic_cast<SpriteMaskComponent*>(
                    component.get()))
            {
                const char* maskShapeNames[]{
                    "矩形",
                    "円"
                };
                int selectedMaskShape =
                    static_cast<int>(spriteMask->Shape());
                if (ImGui::Combo(
                        "形状##SpriteMask",
                        &selectedMaskShape,
                        maskShapeNames,
                        static_cast<int>(
                            std::size(maskShapeNames))))
                {
                    spriteMask->SetShape(
                        static_cast<SpriteMaskShape>(
                            selectedMaskShape));
                    RecordHistory();
                }

                auto maskSize = spriteMask->Size();
                if (ImGui::InputFloat2(
                        spriteMask->Shape()
                                == SpriteMaskShape::Circle
                            ? "直径##SpriteMask"
                            : "サイズ##SpriteMask",
                        &maskSize.x))
                {
                    spriteMask->SetSize(maskSize);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "円の場合は幅の値だけを直径として使います。"
                    "MaskInteractionをVisibleInsideMask／"
                    "VisibleOutsideMaskに設定したSprite Renderer"
                    "だけがこのマスクの影響を受けます。");
            }
            else if (auto* reflectionProbe =
                dynamic_cast<ReflectionProbeComponent*>(
                    component.get()))
            {
                float probeRange =
                    reflectionProbe->Range();
                if (ImGui::DragFloat(
                        "影響範囲",
                        &probeRange,
                        0.25f,
                        0.1f,
                        10000.0f,
                        "%.1f"))
                {
                    reflectionProbe->SetRange(probeRange);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                showItemTooltip(
                    "中心がこの半径に入っているオブジェクトの反射が"
                    "このプローブの結果へ置き換わります。");

                float probeIntensity =
                    reflectionProbe->Intensity();
                if (ImGui::DragFloat(
                        "強さ",
                        &probeIntensity,
                        0.05f,
                        0.0f,
                        10.0f,
                        "%.2f"))
                {
                    reflectionProbe->SetIntensity(
                        probeIntensity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto boxExtents =
                    reflectionProbe->BoxExtents();
                if (ImGui::DragFloat3(
                        "箱の大きさ（半径）",
                        &boxExtents.x,
                        0.1f,
                        0.0f,
                        10000.0f,
                        "%.2f"))
                {
                    reflectionProbe->SetBoxExtents(
                        boxExtents);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                showItemTooltip(
                    "部屋の大きさを入れると、壁・床・天井が正しい"
                    "距離で映ります（ボックス射影）。0のままだと"
                    "映り込みが無限遠にあるように見え、カメラを"
                    "動かしても動きません。屋外や空を映すなら0で"
                    "構いません。");
                if (!reflectionProbe->UsesBoxProjection())
                {
                    ImGui::TextDisabled(
                        "ボックス射影は無効（3軸すべてに"
                        "正の値が必要）");
                }

                float blendDistance =
                    reflectionProbe->BlendDistance();
                if (ImGui::DragFloat(
                        "混ぜ始める距離",
                        &blendDistance,
                        0.1f,
                        0.0f,
                        10000.0f,
                        "%.2f"))
                {
                    reflectionProbe->SetBlendDistance(
                        blendDistance);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                showItemTooltip(
                    "隣のプローブと混ぜ始める距離です（影響範囲の"
                    "内側から測った厚み）。0のままだと境界をまたいだ"
                    "瞬間に映り込みが切り替わります。値を入れると"
                    "少しずつ移るので、部屋をまたぐときの飛びが"
                    "無くなります。");

                if (ImGui::Button("ベイクし直す"))
                {
                    reflectionProbe->RequestBake();
                }
                showItemTooltip(
                    "この位置から見たシーンを撮り直します。物を"
                    "動かした後に押してください。");
                ImGui::SameLine();
                ImGui::TextDisabled(
                    reflectionProbe->IsBakeRequested()
                        ? "ベイク待ち"
                        : (reflectionProbe->IsBaked()
                            ? "ベイク済み"
                            : "未ベイク"));
                ImGui::TextDisabled(
                    "シーン読み込み時に自動でベイクされます。"
                    "結果はファイルへは保存されません。");
            }
            else if (auto* renderCulling =
                dynamic_cast<RenderCullingComponent*>(
                    component.get()))
            {
                bool alwaysVisible =
                    renderCulling->AlwaysVisible();
                if (ImGui::Checkbox(
                        "カメラ外でも描画",
                        &alwaysVisible))
                {
                    renderCulling->SetAlwaysVisible(
                        alwaysVisible);
                    RecordHistory();
                }
                showItemTooltip(
                    "視錐台・遮蔽カリングの対象から外し、常に描画します。"
                    "広い地形や背景向けの設定です。");

                float cullingMargin =
                    renderCulling->CullingMargin();
                ImGui::BeginDisabled(alwaysVisible);
                if (ImGui::DragFloat(
                        "カリング余白",
                        &cullingMargin,
                        0.25f,
                        0.0f,
                        100000.0f,
                        "%.2f"))
                {
                    renderCulling->SetCullingMargin(
                        cullingMargin);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::EndDisabled();
                showItemTooltip(
                    "視錐台カリングの境界をワールド単位で広げます。"
                    "0 は追加の余白なしです。");
            }
            else if (auto* spriteAnimator =
                dynamic_cast<SpriteAnimatorComponent*>(
                    component.get()))
            {
                // シートの分割数
                int grid[2]{
                    spriteAnimator->Columns(),
                    spriteAnimator->Rows() };
                if (ImGui::InputInt2(
                        "シート分割（列×行）",
                        grid))
                {
                    spriteAnimator->SetSheetGrid(
                        grid[0],
                        grid[1]);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "スプライトシートを等分するコマ数です。"
                    "コマ番号は左上から右へ数えます");

                float speed = spriteAnimator->Speed();
                if (ImGui::DragFloat(
                        "再生速度",
                        &speed,
                        0.05f,
                        0.0f,
                        10.0f,
                        "%.2f"))
                {
                    spriteAnimator->SetSpeed(speed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                bool playOnStart =
                    spriteAnimator->PlayOnStart();
                if (ImGui::Checkbox(
                        "自動再生",
                        &playOnStart))
                {
                    spriteAnimator->SetPlayOnStart(
                        playOnStart);
                    RecordHistory();
                }

                // 既定クリップの選択
                const std::string defaultPreview =
                    spriteAnimator->DefaultClip().empty()
                        ? std::string{ "（先頭クリップ）" }
                        : spriteAnimator->DefaultClip();
                if (ImGui::BeginCombo(
                        "既定クリップ",
                        defaultPreview.c_str()))
                {
                    if (ImGui::Selectable(
                            "（先頭クリップ）",
                            spriteAnimator
                                ->DefaultClip()
                                .empty()))
                    {
                        spriteAnimator->SetDefaultClip(
                            {});
                        RecordHistory();
                    }
                    for (const auto& clip :
                        spriteAnimator->Clips())
                    {
                        if (ImGui::Selectable(
                                clip.name.c_str(),
                                spriteAnimator
                                    ->DefaultClip()
                                    == clip.name))
                        {
                            spriteAnimator
                                ->SetDefaultClip(
                                    clip.name);
                            RecordHistory();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SeparatorText("クリップ");
                auto& clips = spriteAnimator->Clips();
                std::optional<std::size_t> clipToDelete;
                for (std::size_t clipIndex = 0;
                    clipIndex < clips.size();
                    ++clipIndex)
                {
                    auto& clip = clips[clipIndex];
                    ImGui::PushID(
                        static_cast<int>(clipIndex)
                        + 82000);
                    std::array<char, 64> clipNameBuffer{};
                    strncpy_s(
                        clipNameBuffer.data(),
                        clipNameBuffer.size(),
                        clip.name.c_str(),
                        _TRUNCATE);
                    ImGui::SetNextItemWidth(140.0f);
                    if (ImGui::InputText(
                            "名前",
                            clipNameBuffer.data(),
                            clipNameBuffer.size()))
                    {
                        clip.name = clipNameBuffer.data();
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("削除"))
                    {
                        clipToDelete = clipIndex;
                    }
                    if (m_playing
                        && !clip.name.empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("再生"))
                        {
                            static_cast<void>(
                                spriteAnimator->Play(
                                    clip.name));
                        }
                    }
                    int frameRange[2]{
                        clip.startFrame,
                        clip.frameCount };
                    if (ImGui::InputInt2(
                            "開始コマ / コマ数",
                            frameRange))
                    {
                        clip.startFrame = std::max(
                            frameRange[0],
                            0);
                        clip.frameCount = std::max(
                            frameRange[1],
                            1);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    if (ImGui::DragFloat(
                            "コマ/秒",
                            &clip.framesPerSecond,
                            0.5f,
                            0.5f,
                            120.0f,
                            "%.1f"))
                    {
                        clip.framesPerSecond =
                            std::max(
                                clip.framesPerSecond,
                                0.01f);
                    }
                    if (ImGui::
                        IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    if (ImGui::Checkbox(
                            "ループ",
                            &clip.loop))
                    {
                        RecordHistory();
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (clipToDelete)
                {
                    clips.erase(
                        clips.begin()
                        + static_cast<std::ptrdiff_t>(
                            *clipToDelete));
                    RecordHistory();
                }
                if (ImGui::Button("クリップを追加"))
                {
                    SpriteAnimationClip clip;
                    clip.name =
                        "clip"
                        + std::to_string(
                            clips.size() + 1);
                    spriteAnimator->AddClip(
                        std::move(clip));
                    RecordHistory();
                }
                if (spriteAnimator->IsPlaying())
                {
                    ImGui::TextDisabled(
                        "再生中: %s（コマ %d）",
                        spriteAnimator
                            ->ActiveClipName()
                            .c_str(),
                        spriteAnimator->CurrentFrame());
                }
            }
            else if (auto* tilemap =
                dynamic_cast<
                    TilemapComponent*>(
                        component.get()))
            {
                ImGui::Text(
                    "配置セル: %zu",
                    tilemap->Cells().size());

                auto tilemapSortOrder =
                    tilemap->SortOrder();
                if (ImGui::InputInt(
                        "描画順##Tilemap",
                        &tilemapSortOrder))
                {
                    tilemap->SetSortOrder(
                        tilemapSortOrder);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "数値が大きいほど手前に表示されます"
                    "（背景／地形／前景の重ね順に）");

                auto tileSize =
                    tilemap->TileSize();
                if (ImGui::DragFloat2(
                        "セルサイズ",
                        &tileSize.x,
                        1.0f,
                        1.0f,
                        4096.0f,
                        "%.0f"))
                {
                    tilemap->SetTileSize(
                        tileSize);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                int atlasGrid[]{
                    static_cast<int>(
                        tilemap->
                            AtlasColumns()),
                    static_cast<int>(
                        tilemap->
                            AtlasRows())
                };
                if (ImGui::InputInt2(
                        "Atlas 列 / 行",
                        atlasGrid))
                {
                    tilemap->SetAtlasGrid(
                        static_cast<
                            std::uint32_t>(
                                std::max(
                                    atlasGrid[0],
                                    1)),
                        static_cast<
                            std::uint32_t>(
                                std::max(
                                    atlasGrid[1],
                                    1)));
                    m_tilePaletteSelectedTile =
                        std::min(
                            m_tilePaletteSelectedTile,
                            tilemap->
                                TileCount() - 1);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto color =
                    tilemap->Color();
                if (ImGui::ColorEdit4(
                        "色",
                        &color.x))
                {
                    tilemap->SetColor(color);
                }
                if (ImGui::
                    IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                const auto texturePath =
                    PathToUtf8(
                        tilemap->
                            TexturePath());
                ImGui::TextWrapped(
                    "タイルシート: %s",
                    texturePath.empty()
                        ? "（未設定）"
                        : texturePath.c_str());
                ImGui::Button(
                    "画像をここへドロップ##Tilemap",
                    ImVec2{ -1.0f, 0.0f });
                if (ImGui::
                    BeginDragDropTarget())
                {
                    if (const ImGuiPayload*
                        payload =
                            ImGui::
                                AcceptDragDropPayload(
                                    AssetPayload))
                    {
                        const auto droppedPath =
                            PathFromUtf8(
                                static_cast<
                                    const char*>(
                                        payload->
                                            Data));
                        if (IsTextureAsset(
                                droppedPath))
                        {
                            try
                            {
                                tilemap->
                                    SetTexturePath(
                                        droppedPath);
                                RecordHistory();
                                SetStatus(
                                    "Tilemapへタイルシートを設定しました");
                            }
                            catch (
                                const std::
                                    exception&
                                        exception)
                            {
                                SetStatus(
                                    exception.what(),
                                    true);
                            }
                        }
                        else
                        {
                            SetStatus(
                                "画像ファイルをドロップしてください",
                                true);
                        }
                    }
                    ImGui::
                        EndDragDropTarget();
                }

                if (!texturePath.empty()
                    && ImGui::Button(
                        "タイルシートを解除"))
                {
                    tilemap->
                        SetTexturePath({});
                    RecordHistory();
                    SetStatus(
                        "Tilemapのタイルシートを解除しました");
                }
                ImGui::TextDisabled(
                    "配置編集は「タイルパレット」タブで行います。");

                ImGui::Separator();
                if (ImGui::Button(
                    "コライダーを生成##Tilemap"))
                {
                    constexpr const char*
                        GeneratedColliderName =
                            "タイルコライダー（自動生成）";
                    auto& tilemapObject =
                        tilemap->Owner();
                    std::vector<GameObject*>
                        previousColliders;
                    for (auto* child :
                        tilemapObject.Children())
                    {
                        if (child->Name()
                            == GeneratedColliderName)
                        {
                            previousColliders
                                .push_back(child);
                        }
                    }
                    for (auto* child :
                        previousColliders)
                    {
                        m_scene.DestroyGameObject(
                            *child);
                    }

                    const auto rects =
                        tilemap
                            ->ComputeCollisionRects();
                    for (const auto& rect : rects)
                    {
                        auto& colliderObject =
                            m_scene.CreateGameObject(
                                GeneratedColliderName);
                        colliderObject.SetParent(
                            &tilemapObject);
                        colliderObject
                            .GetTransform()
                            .position = {
                                rect.center.x,
                                rect.center.y,
                                0.0f };
                        colliderObject.AddComponent<
                            BoxCollider2DComponent>(
                                rect.size);
                    }
                    RecordHistory();
                    SetStatus(
                        rects.empty()
                            ? "コライダーを生成できるセルがありません"
                            : "Colliderを"
                                + std::to_string(
                                    rects.size())
                                + "個生成しました");
                }
                ImGui::TextDisabled(
                    "配置済みセルをまとめてBox Collider 2D"
                    "の子オブジェクトを生成します（既存の"
                    "生成済みColliderは置き換えられます）。"
                    "当たり判定不要の装飾タイルは別の"
                    "Tilemapへ分けてください。");
            }
            else if (auto* parallax =
                dynamic_cast<
                    ParallaxLayerComponent*>(
                        component.get()))
            {
                const auto* reference =
                    m_scene.FindGameObject(
                        parallax->ReferenceId());
                std::string parallaxPreview =
                    parallax->ReferenceId() == 0
                        ? "Main Camera（既定）"
                        : (reference != nullptr
                            ? reference->Name()
                            : "未設定");
                if (ImGui::BeginCombo(
                    "参照##ParallaxLayer",
                    parallaxPreview.c_str()))
                {
                    if (ImGui::Selectable(
                        "Main Camera（既定）",
                        parallax->ReferenceId() == 0))
                    {
                        parallax->SetReferenceId(0);
                        RecordHistory();
                    }
                    for (const auto& candidate :
                        m_scene.GameObjects())
                    {
                        if (candidate.get() == selected)
                        {
                            continue;
                        }
                        std::string label =
                            candidate->Name()
                            + " (ID "
                            + std::to_string(
                                candidate->Id())
                            + ")";
                        if (ImGui::Selectable(
                            label.c_str(),
                            parallax->ReferenceId()
                                == candidate->Id()))
                        {
                            parallax->SetReferenceId(
                                candidate->Id());
                            RecordHistory();
                        }
                    }
                    ImGui::EndCombo();
                }

                auto parallaxFactor =
                    parallax->Factor();
                if (ImGui::DragFloat2(
                        "倍率##ParallaxLayer",
                        &parallaxFactor.x,
                        0.01f))
                {
                    parallax->SetFactor(
                        parallaxFactor);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "参照（既定はMain Camera）の移動量に"
                    "この倍率を掛けた分だけ、このGameObjectを"
                    "追従させます。1.0で参照と同じ速さ、"
                    "0.5で半分の速さ（遠い背景）、0で画面に"
                    "固定、1.0超で参照より速く動きます"
                    "（近景）。背景Tilemapに付けて奥行きの"
                    "あるスクロールを作れます。");
            }
            else if (auto* listener =
                dynamic_cast<AudioListenerComponent*>(
                    component.get()))
            {
                const bool isActive =
                    m_graphics.Audio().ActiveListener()
                    == listener;
                ImGui::Text(
                    "状態: %s",
                    isActive ? "有効" : "待機中");
                ImGui::TextWrapped(
                    "GameObjectの位置と向きを3D音声の聴取位置として使用します。");
                ImGui::TextDisabled(
                    "通常はメインカメラに1つ追加します");
            }
            else if (auto* audio = dynamic_cast<AudioSourceComponent*>(component.get()))
            {
                const auto audioPath = PathToUtf8(audio->AudioPath());
                ImGui::TextWrapped(
                    "WAV / OGG: %s",
                    audioPath.empty() ? "（未設定）" : audioPath.c_str());
                ImGui::TextDisabled(
                    "アセットブラウザーから WAV / OGG をドロップできます");

                bool audioStreaming =
                    audio->IsStreaming();
                if (ImGui::Checkbox(
                        "ストリーミング再生",
                        &audioStreaming))
                {
                    try
                    {
                        audio->SetStreaming(
                            audioStreaming);
                        RecordHistory();
                    }
                    catch (const std::exception&
                        exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "長尺BGM向け（3D定位は非対応）");
                int audioBus = static_cast<int>(
                    audio->Bus());
                if (ImGui::Combo(
                        "バス",
                        &audioBus,
                        "効果音\0BGM\0"))
                {
                    audio->SetBus(
                        static_cast<AudioBus>(
                            audioBus));
                    RecordHistory();
                }

                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(AssetPayload))
                    {
                        const auto droppedPath = PathFromUtf8(
                            static_cast<const char*>(payload->Data));
                        if (!IsAudioAsset(droppedPath))
                        {
                            SetStatus(
                                "WAV / OGG ファイルをドロップしてください",
                                true);
                        }
                        else
                        {
                            try
                            {
                                audio->SetAudioPath(droppedPath);
                                RecordHistory();
                                SetStatus("音声を割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    m_selectedAsset.empty()
                    || !IsAudioAsset(m_selectedAsset));
                if (ImGui::Button("選択中の音声を割り当て"))
                {
                    try
                    {
                        audio->SetAudioPath(m_selectedAsset);
                        RecordHistory();
                        SetStatus("音声を割り当てました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                ImGui::EndDisabled();

                float volume = audio->Volume();
                if (ImGui::SliderFloat(
                    "音量",
                    &volume,
                    0.0f,
                    1.0f,
                    "%.2f"))
                {
                    audio->SetVolume(volume);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float pitch = audio->Pitch();
                if (ImGui::SliderFloat(
                    "ピッチ",
                    &pitch,
                    -1.0f,
                    1.0f,
                    "%.2f"))
                {
                    audio->SetPitch(pitch);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool spatial = audio->IsSpatial();
                if (ImGui::Checkbox("3D空間オーディオ", &spatial))
                {
                    try
                    {
                        audio->SetSpatial(spatial);
                        RecordHistory();
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }

                float pan = audio->Pan();
                ImGui::BeginDisabled(spatial);
                if (ImGui::SliderFloat(
                    "パン（左右）",
                    &pan,
                    -1.0f,
                    1.0f,
                    "%.2f"))
                {
                    audio->SetPan(pan);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::EndDisabled();

                if (spatial)
                {
                    float minimumDistance =
                        audio->MinimumDistance();
                    if (ImGui::DragFloat(
                        "最小距離",
                        &minimumDistance,
                        0.1f,
                        0.01f,
                        1000.0f,
                        "%.2f"))
                    {
                        audio->SetMinimumDistance(
                            minimumDistance);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float maximumDistance =
                        audio->MaximumDistance();
                    if (ImGui::DragFloat(
                        "最大距離",
                        &maximumDistance,
                        0.1f,
                        0.02f,
                        10000.0f,
                        "%.2f"))
                    {
                        audio->SetMaximumDistance(
                            maximumDistance);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                    ImGui::TextDisabled(
                        "最小距離までは等音量、最大距離で無音");
                    if (m_graphics.Audio().ActiveListener()
                        == nullptr)
                    {
                        ImGui::TextColored(
                            ImVec4{ 1.0f, 0.65f, 0.25f, 1.0f },
                            "有効なAudioListenerがありません");
                    }
                }

                bool loop = audio->Loop();
                if (ImGui::Checkbox("ループ", &loop))
                {
                    audio->SetLoop(loop);
                    RecordHistory();
                }

                bool playOnStart = audio->PlayOnStart();
                if (ImGui::Checkbox(
                    "ゲーム開始時に再生",
                    &playOnStart))
                {
                    audio->SetPlayOnStart(playOnStart);
                    RecordHistory();
                }

                const char* stateName = "停止";
                if (audio->State() == DirectX::PLAYING)
                {
                    stateName = "再生中";
                }
                else if (audio->State() == DirectX::PAUSED)
                {
                    stateName = "一時停止";
                }
                ImGui::Text("状態: %s", stateName);

                ImGui::BeginDisabled(audioPath.empty());
                if (ImGui::Button("再生"))
                {
                    audio->Play();
                }
                ImGui::SameLine();
                if (ImGui::Button("One Shot"))
                {
                    audio->PlayOneShot();
                }
                ImGui::SameLine();
                if (ImGui::Button("一時停止"))
                {
                    audio->Pause();
                }
                ImGui::SameLine();
                if (ImGui::Button("再開"))
                {
                    audio->Resume();
                }
                if (ImGui::Button("停止"))
                {
                    audio->Stop();
                }
                ImGui::EndDisabled();

                if (!audioPath.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("音声を解除"))
                    {
                        audio->SetAudioPath({});
                        RecordHistory();
                        SetStatus("音声を解除しました");
                    }
                }
            }
            else if (auto* text = dynamic_cast<TextRendererComponent*>(component.get()))
            {
                std::array<char, 512> textBuffer{};
                strncpy_s(
                    textBuffer.data(),
                    textBuffer.size(),
                    text->Text().c_str(),
                    _TRUNCATE);
                if (ImGui::InputTextMultiline(
                    "テキスト",
                    textBuffer.data(),
                    textBuffer.size(),
                    ImVec2{ -1.0f, 72.0f }))
                {
                    try
                    {
                        text->SetText(textBuffer.data());
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float fontSize = text->FontSize();
                if (ImGui::SliderFloat(
                    "文字サイズ",
                    &fontSize,
                    8.0f,
                    128.0f,
                    "%.0f px"))
                {
                    text->SetFontSize(fontSize);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto layoutSize = text->LayoutSize();
                if (ImGui::InputFloat2(
                    "レイアウトサイズ",
                    &layoutSize.x,
                    "%.0f px"))
                {
                    text->SetLayoutSize(layoutSize);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled("0 はテキストに合わせた自動サイズ");

                bool wordWrap = text->WordWrap();
                if (ImGui::Checkbox("自動折り返し", &wordWrap))
                {
                    text->SetWordWrap(wordWrap);
                    RecordHistory();
                }
                if (wordWrap && text->LayoutSize().x <= 0.0f)
                {
                    ImGui::TextDisabled("折り返しにはレイアウト幅を指定してください");
                }

                constexpr const char* horizontalAlignmentNames[] = {
                    "左揃え",
                    "中央揃え",
                    "右揃え"
                };
                int horizontalAlignment =
                    static_cast<int>(text->HorizontalAlignment());
                if (ImGui::Combo(
                    "横方向",
                    &horizontalAlignment,
                    horizontalAlignmentNames,
                    static_cast<int>(std::size(horizontalAlignmentNames))))
                {
                    text->SetHorizontalAlignment(
                        static_cast<TextHorizontalAlignment>(
                            horizontalAlignment));
                    RecordHistory();
                }

                constexpr const char* verticalAlignmentNames[] = {
                    "上揃え",
                    "中央揃え",
                    "下揃え"
                };
                int verticalAlignment =
                    static_cast<int>(text->VerticalAlignment());
                if (ImGui::Combo(
                    "縦方向",
                    &verticalAlignment,
                    verticalAlignmentNames,
                    static_cast<int>(std::size(verticalAlignmentNames))))
                {
                    text->SetVerticalAlignment(
                        static_cast<TextVerticalAlignment>(
                            verticalAlignment));
                    RecordHistory();
                }

                auto color = text->Color();
                if (ImGui::ColorEdit4("文字色", &color.x))
                {
                    text->SetColor(color);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto textSortOrder = text->SortOrder();
                if (ImGui::InputInt("描画順", &textSortOrder))
                {
                    text->SetSortOrder(textSortOrder);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "数値が大きいほど手前に表示されます");

                ImGui::Text("フォント: %s", text->FontFamily().c_str());
            }
            else if (auto* billboard =
                dynamic_cast<BillboardComponent*>(
                    component.get()))
            {
                static constexpr const char* modeNames[]{
                    "カメラの位置を向く",
                    "画面と平行（カメラの向きに合わせる）",
                    "立ったままカメラの位置を向く",
                    "立ったまま画面の向きに合わせる",
                    "指定した座標を向く"
                };
                int mode = static_cast<int>(
                    billboard->Mode());
                if (ImGui::Combo(
                        "向け方",
                        &mode,
                        modeNames,
                        static_cast<int>(
                            std::size(modeNames))))
                {
                    billboard->SetMode(
                        static_cast<BillboardMode>(mode));
                    RecordHistory();
                }
                showItemTooltip(
                    "「位置を向く」は板ごとにカメラの座標へ向くので、"
                    "画面の端の板はこちらへ振り向くように回って"
                    "見えます。「画面と平行」はどこにあっても同じ"
                    "向きになるので歪みません（板状の絵はこちらが"
                    "自然）。「立ったまま」は上下に傾かないので、"
                    "木・草・頭上のHPバーに向きます。");

                static constexpr const char* axisNames[]{
                    "上（+Y）… Planeの画像",
                    "前（+Z）… モデルの正面"
                };
                int axis = static_cast<int>(
                    billboard->FacingAxis());
                if (ImGui::Combo(
                        "向ける面",
                        &axis,
                        axisNames,
                        static_cast<int>(
                            std::size(axisNames))))
                {
                    billboard->SetFacingAxis(
                        static_cast<BillboardFacingAxis>(
                            axis));
                    RecordHistory();
                }
                showItemTooltip(
                    "Mesh RendererのPlaneは面が真上（+Y）を向いて"
                    "いるので、画像を貼る用途では「上」のままで"
                    "構いません。");

                if (billboard->Mode()
                    == BillboardMode::LookAtPosition)
                {
                    auto target =
                        billboard->TargetPosition();
                    if (ImGui::DragFloat3(
                            "向く座標",
                            &target.x,
                            0.1f))
                    {
                        billboard->SetTargetPosition(
                            target);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                }
                else if (m_scene.MainCamera() == nullptr)
                {
                    ImGui::TextDisabled(
                        "メインカメラが無いので向きません");
                }
                ImGui::TextDisabled(
                    "向くのは再生中だけです"
                    "（シーンビューでは編集した向きのまま）");
            }
            else if (auto* rotator = dynamic_cast<RotatorComponent*>(component.get()))
            {
                auto velocity = rotator->AngularVelocity();
                if (ImGui::InputFloat3("回転速度", &velocity.x))
                {
                    rotator->SetAngularVelocity(velocity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
            }
            else if (auto* nativeScript =
                dynamic_cast<NativeScriptComponent*>(
                    component.get()))
            {
                ImGui::TextWrapped(
                    "登録型: %s",
                    nativeScript->ScriptType().c_str());
                ImGui::PushTextWrapPos(0.0f);
                if (nativeScript->IsResolved())
                {
                    ImGui::TextColored(
                        ImVec4{ 0.3f, 0.9f, 0.45f, 1.0f },
                        "状態: Game Moduleに接続済み");
                }
                else
                {
                    ImGui::TextColored(
                        ImVec4{ 1.0f, 0.55f, 0.25f, 1.0f },
                        "状態: 未解決");
                }
                ImGui::PopTextWrapPos();
                if (!nativeScript->LastError().empty())
                {
                    ImGui::TextWrapped(
                        "詳細: %s",
                        nativeScript->LastError().c_str());
                }

                static NativeScriptComponent*
                    editedNativeScript{};
                static std::array<char, 4096>
                    nativePropertiesBuffer{};
                if (editedNativeScript != nativeScript)
                {
                    editedNativeScript = nativeScript;
                    nativePropertiesBuffer.fill('\0');
                    strncpy_s(
                        nativePropertiesBuffer.data(),
                        nativePropertiesBuffer.size(),
                        nativeScript->PropertiesJson().c_str(),
                        _TRUNCATE);
                }

                bool hasTypedSchema{};
                const auto schemaJson =
                    nativeScript->PropertiesSchemaJson();
                if (!schemaJson.empty())
                {
                    try
                    {
                        auto properties = nlohmann::json::parse(
                            nativeScript->PropertiesJson());
                        ImGui::SeparatorText("公開プロパティ");
                        ImGui::BeginDisabled(m_playing);
                        const auto edit =
                            DrawNativeScriptProperties(
                                properties,
                                schemaJson,
                                [this](
                                    const char* controlId,
                                    const nlohmann::json& field,
                                    std::string& value)
                                {
                                    return
                                        DrawAssetReferenceField(
                                            controlId,
                                            field,
                                            value);
                                });
                        ImGui::EndDisabled();
                        hasTypedSchema = edit.hasSchema;
                        if (!edit.error.empty())
                        {
                            ImGui::TextColored(
                                ImVec4{ 1.0f, 0.55f, 0.25f, 1.0f },
                                "Inspectorスキーマ: %s",
                                edit.error.c_str());
                        }
                        if (edit.changed && !m_playing)
                        {
                            nativeScript->SetPropertiesJson(
                                properties.dump());
                            nativePropertiesBuffer.fill('\0');
                            strncpy_s(
                                nativePropertiesBuffer.data(),
                                nativePropertiesBuffer.size(),
                                nativeScript->PropertiesJson().c_str(),
                                _TRUNCATE);
                            SetStatus(
                                "C++ Componentの公開プロパティを更新しました");
                        }
                        if (edit.committed && !m_playing)
                        {
                            RecordHistory();
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }

                const bool showRawJson = !hasTypedSchema
                    || ImGui::CollapsingHeader(
                        "詳細設定 (Properties JSON)");
                if (showRawJson)
                {
                    ImGui::InputTextMultiline(
                        "Properties JSON",
                        nativePropertiesBuffer.data(),
                        nativePropertiesBuffer.size(),
                        ImVec2{ -1.0f, 90.0f });

                    ImGui::BeginDisabled(m_playing);
                    if (ImGui::Button("JSONを適用"))
                    {
                        try
                        {
                            nativeScript->SetPropertiesJson(
                                nativePropertiesBuffer.data());
                            RecordHistory();
                            SetStatus(
                                "C++ ComponentのPropertiesを更新しました");
                        }
                        catch (const std::exception& exception)
                        {
                            SetStatus(
                                exception.what(),
                                true);
                        }
                    }
                    ImGui::EndDisabled();
                }

                if (auto* module =
                        GameModuleHost::Current();
                    module != nullptr)
                {
                    if (ImGui::Button(
                            "Moduleを再読み込み",
                            ImVec2{ -1.0f, 0.0f }))
                    {
                        if (module->Reload())
                        {
                            SetStatus(
                                "Game Moduleを再読み込みしました");
                        }
                        else
                        {
                            SetStatus(
                                module->LastError(),
                                true);
                        }
                    }
                    ImGui::PushStyleColor(
                        ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(
                            ImGuiCol_TextDisabled));
                    ImGui::TextWrapped(
                        "Module: %s",
                        module->IsLoaded()
                            ? module->ModuleName().c_str()
                            : "未読み込み");
                    ImGui::TextWrapped(
                        "DLLを再ビルドすると自動でHot Reloadします");
                    ImGui::PopStyleColor();
                    if (!module->LastError().empty())
                    {
                        ImGui::TextWrapped(
                            "Moduleエラー: %s",
                            module->LastError().c_str());
                    }
                }
            }
            else if (auto* animator =
                dynamic_cast<TransformAnimatorComponent*>(
                    component.get()))
            {
                const auto clipPath =
                    PathToUtf8(animator->ClipPath());
                const auto controllerPath =
                    PathToUtf8(
                        animator->ControllerPath());
                ImGui::TextWrapped(
                    "Animation Clip: %s",
                    clipPath.empty()
                        ? "（未設定）"
                        : clipPath.c_str());
                ImGui::TextWrapped(
                    "Animator Controller: %s",
                    controllerPath.empty()
                        ? "（未設定）"
                        : controllerPath.c_str());
                ImGui::TextDisabled(
                    ".animation.jsonまたは.animator.jsonをドロップできます");
                ImGui::Button(
                    "Animationアセットドロップ領域",
                    ImVec2{ -1.0f, 0.0f });

                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            AssetPayload))
                    {
                        const auto droppedPath =
                            PathFromUtf8(
                                static_cast<const char*>(
                                    payload->Data));
                        if (!IsAnimationAsset(droppedPath)
                            && !IsAnimatorControllerAsset(
                                droppedPath))
                        {
                            SetStatus(
                                "Animation ClipまたはAnimator Controllerをドロップしてください",
                                true);
                        }
                        else
                        {
                            try
                            {
                                if (IsAnimatorControllerAsset(
                                        droppedPath))
                                {
                                    static_cast<void>(
                                        m_graphics.Assets().
                                            LoadAnimatorController(
                                                droppedPath));
                                    animator->SetControllerPath(
                                        droppedPath);
                                }
                                else
                                {
                                    static_cast<void>(
                                        m_graphics.Assets().
                                            LoadAnimationClip(
                                                droppedPath));
                                    animator->SetClipPath(
                                        droppedPath);
                                }
                                RecordHistory();
                                SetStatus(
                                    "Animationアセットを割り当てました");
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(
                                    exception.what(),
                                    true);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::BeginDisabled(
                    clipPath.empty());
                if (ImGui::Button(
                    "Clipを再読み込み"))
                {
                    try
                    {
                        animator->ReloadClip();
                        SetStatus(
                            "Animation Clipを再読み込みしました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(
                    controllerPath.empty());
                if (ImGui::Button(
                    "Controllerを再読み込み"))
                {
                    try
                    {
                        animator->ReloadController();
                        SetStatus(
                            "Animator Controllerを再読み込みしました");
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                }
                ImGui::EndDisabled();

                if (!controllerPath.empty())
                {
                    ImGui::Text(
                        "現在State: %s%s",
                        animator->CurrentState().empty()
                            ? "（初期化待ち）"
                            : animator->CurrentState().c_str(),
                        animator->IsTransitioning()
                            ? "（遷移中）"
                            : "");
                    ImGui::TextDisabled(
                        "Controller使用中はState側のClip／Loopを使用します");
                }
                else if (clipPath.empty())
                {
                    if (ImGui::Button(
                        "新規Animation Clipを作成..."))
                    {
                        CreateAnimationClipForSelected();
                    }
                }
                else
                {
                    if (ImGui::Button(
                        "Timelineを開く"))
                    {
                        OpenAnimationTimeline();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(
                        "別の新規Clip..."))
                    {
                        CreateAnimationClipForSelected();
                    }
                }

                float speed = animator->Speed();
                if (ImGui::DragFloat(
                    "再生速度",
                    &speed,
                    0.05f,
                    0.01f,
                    100.0f,
                    "%.2fx"))
                {
                    try
                    {
                        animator->SetSpeed(speed);
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool loop = animator->Loop();
                ImGui::BeginDisabled(
                    !controllerPath.empty());
                if (ImGui::Checkbox("ループ", &loop))
                {
                    animator->SetLoop(loop);
                    RecordHistory();
                }
                ImGui::EndDisabled();
                bool playOnStart =
                    animator->PlayOnStart();
                if (ImGui::Checkbox(
                    "Play開始時に再生",
                    &playOnStart))
                {
                    animator->SetPlayOnStart(
                        playOnStart);
                    RecordHistory();
                }

                const float duration =
                    animator->Duration();
                ImGui::Text(
                    "長さ: %.2f秒 / Keyframe: %zu",
                    duration,
                    animator->KeyframeCount());
                if (duration > 0.0f)
                {
                    float previewTime =
                        animator->Time();
                    if (ImGui::SliderFloat(
                        "プレビュー時刻",
                        &previewTime,
                        0.0f,
                        duration,
                        "%.2f秒"))
                    {
                        animator->SetTime(
                            previewTime);
                    }
                    if (ImGui::Button("先頭へ戻す"))
                    {
                        animator->SetTime(0.0f);
                    }
                    ImGui::TextDisabled(
                        "Play中はクリップが自動再生されます");
                }
            }
            else if (auto* inputMover =
                dynamic_cast<InputMoverComponent*>(
                    component.get()))
            {
                std::array<char, 96> horizontalAction{};
                strncpy_s(
                    horizontalAction.data(),
                    horizontalAction.size(),
                    inputMover->HorizontalAction().c_str(),
                    _TRUNCATE);
                if (ImGui::InputText(
                    "横移動Action",
                    horizontalAction.data(),
                    horizontalAction.size()))
                {
                    try
                    {
                        inputMover->SetHorizontalAction(
                            horizontalAction.data());
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                std::array<char, 96> verticalAction{};
                strncpy_s(
                    verticalAction.data(),
                    verticalAction.size(),
                    inputMover->VerticalAction().c_str(),
                    _TRUNCATE);
                if (ImGui::InputText(
                    "縦移動Action",
                    verticalAction.data(),
                    verticalAction.size()))
                {
                    try
                    {
                        inputMover->SetVerticalAction(
                            verticalAction.data());
                    }
                    catch (const std::exception& exception)
                    {
                        SetStatus(exception.what(), true);
                    }
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float speed = inputMover->Speed();
                if (ImGui::DragFloat(
                    "移動速度",
                    &speed,
                    0.1f,
                    0.0f,
                    1000.0f,
                    "%.2f"))
                {
                    inputMover->SetSpeed(speed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "既定: WASD またはゲームパッド左Stick");
            }
            else if (auto* controller =
                dynamic_cast<CharacterControllerComponent*>(
                    component.get()))
            {
                float radius = controller->Radius();
                if (ImGui::DragFloat(
                    "半径", &radius, 0.01f, 0.05f, 100.0f, "%.2f"))
                {
                    controller->SetRadius(radius);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float height = controller->Height();
                if (ImGui::DragFloat(
                    "高さ", &height, 0.02f, 0.1f, 1000.0f, "%.2f"))
                {
                    controller->SetHeight(height);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float moveSpeed = controller->MoveSpeed();
                if (ImGui::DragFloat(
                    "移動速度", &moveSpeed, 0.1f, 0.0f, 1000.0f, "%.2f"))
                {
                    controller->SetMoveSpeed(moveSpeed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float gravity = controller->Gravity();
                if (ImGui::DragFloat(
                    "重力", &gravity, 0.1f, 0.0f, 1000.0f, "%.2f"))
                {
                    controller->SetGravity(gravity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float jumpSpeed = controller->JumpSpeed();
                if (ImGui::DragFloat(
                    "ジャンプ速度",
                    &jumpSpeed,
                    0.1f,
                    0.0f,
                    1000.0f,
                    "%.2f"))
                {
                    controller->SetJumpSpeed(jumpSpeed);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float stepOffset = controller->StepOffset();
                if (ImGui::DragFloat(
                    "登れる段差",
                    &stepOffset,
                    0.01f,
                    0.0f,
                    controller->Height(),
                    "%.2f"))
                {
                    controller->SetStepOffset(stepOffset);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                float skinWidth = controller->SkinWidth();
                if (ImGui::DragFloat(
                    "Skin Width",
                    &skinWidth,
                    0.001f,
                    0.001f,
                    controller->Radius() * 0.5f,
                    "%.3f"))
                {
                    controller->SetSkinWidth(skinWidth);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) RecordHistory();

                std::uint32_t layer =
                    controller->Layer();
                if (DrawCollisionLayerCombo(
                    "Layer",
                    m_projectSettings.physics.layerNames,
                    layer))
                {
                    controller->SetLayer(layer);
                    RecordHistory();
                }

                std::uint32_t mask =
                    controller->CollisionMask();
                if (DrawCollisionMaskCombo(
                    "Collision Mask",
                    m_projectSettings.physics.layerNames,
                    mask))
                {
                    controller->SetCollisionMask(mask);
                    RecordHistory();
                }

                bool useInput = controller->UseInput();
                if (ImGui::Checkbox(
                    "入力アクションを使用",
                    &useInput))
                {
                    controller->SetUseInput(useInput);
                    RecordHistory();
                }

                const auto drawAction =
                    [this](
                        const char* label,
                        const std::string& current,
                        auto&& setter)
                    {
                        std::array<char, 96> text{};
                        strncpy_s(
                            text.data(),
                            text.size(),
                            current.c_str(),
                            _TRUNCATE);
                        if (ImGui::InputText(
                            label,
                            text.data(),
                            text.size()))
                        {
                            try
                            {
                                setter(std::string(text.data()));
                            }
                            catch (const std::exception& exception)
                            {
                                SetStatus(exception.what(), true);
                            }
                        }
                        if (ImGui::IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    };
                drawAction(
                    "横移動 Action",
                    controller->HorizontalAction(),
                    [controller](std::string value)
                    {
                        controller->SetHorizontalAction(
                            std::move(value));
                    });
                drawAction(
                    "前後移動 Action",
                    controller->VerticalAction(),
                    [controller](std::string value)
                    {
                        controller->SetVerticalAction(
                            std::move(value));
                    });
                drawAction(
                    "ジャンプ Action",
                    controller->JumpAction(),
                    [controller](std::string value)
                    {
                        controller->SetJumpAction(
                            std::move(value));
                    });

                ImGui::Text(
                    "接地: %s  垂直速度: %.2f",
                    controller->IsGrounded()
                        ? "はい"
                        : "いいえ",
                    controller->VerticalVelocity());
                ImGui::TextDisabled(
                    "Transform の位置が足元になります");
            }
            else if (auto* rigidbody = dynamic_cast<RigidbodyComponent*>(component.get()))
            {
                float mass = rigidbody->Mass();
                if (ImGui::DragFloat(
                    "質量",
                    &mass,
                    0.05f,
                    0.0001f,
                    100000.0f,
                    "%.3f"))
                {
                    rigidbody->SetMass(mass);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto velocity = rigidbody->Velocity();
                if (ImGui::InputFloat3("速度", &velocity.x))
                {
                    rigidbody->SetVelocity(velocity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto angularVelocity =
                    rigidbody->AngularVelocity();
                if (ImGui::InputFloat3(
                    "角速度 (rad/s)",
                    &angularVelocity.x))
                {
                    rigidbody->SetAngularVelocity(
                        angularVelocity);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto centerOfMass =
                    rigidbody->CenterOfMass();
                if (ImGui::InputFloat3(
                    "重心 (ローカル)",
                    &centerOfMass.x))
                {
                    rigidbody->SetCenterOfMass(
                        centerOfMass);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float linearDrag =
                    rigidbody->LinearDrag();
                if (ImGui::DragFloat(
                    "移動抵抗",
                    &linearDrag,
                    0.01f,
                    0.0f,
                    1000.0f,
                    "%.3f"))
                {
                    rigidbody->SetLinearDrag(
                        linearDrag);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                float angularDrag =
                    rigidbody->AngularDrag();
                if (ImGui::DragFloat(
                    "回転抵抗",
                    &angularDrag,
                    0.01f,
                    0.0f,
                    1000.0f,
                    "%.3f"))
                {
                    rigidbody->SetAngularDrag(
                        angularDrag);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                bool useGravity = rigidbody->UsesGravity();
                if (ImGui::Checkbox("重力を使用", &useGravity))
                {
                    rigidbody->SetUseGravity(useGravity);
                    RecordHistory();
                }

                bool kinematic = rigidbody->IsKinematic();
                if (ImGui::Checkbox("キネマティック", &kinematic))
                {
                    rigidbody->SetKinematic(kinematic);
                    RecordHistory();
                }

                ImGui::Text(
                    "物理状態: %s",
                    rigidbody->IsSleeping()
                        ? "Sleep"
                        : "Awake");
                ImGui::BeginDisabled(kinematic);
                if (rigidbody->IsSleeping())
                {
                    if (ImGui::Button("起こす"))
                    {
                        rigidbody->WakeUp();
                    }
                }
                else if (ImGui::Button("休止させる"))
                {
                    rigidbody->Sleep();
                }
                ImGui::EndDisabled();

                auto constraints =
                    rigidbody->Constraints();
                ImGui::SeparatorText("回転の固定");
                bool constraintsChanged{};
                constraintsChanged |= ImGui::Checkbox(
                    "X 軸",
                    &constraints.freezeRotationX);
                ImGui::SameLine();
                constraintsChanged |= ImGui::Checkbox(
                    "Y 軸",
                    &constraints.freezeRotationY);
                ImGui::SameLine();
                constraintsChanged |= ImGui::Checkbox(
                    "Z 軸",
                    &constraints.freezeRotationZ);
                ImGui::SeparatorText("位置の固定");
                constraintsChanged |= ImGui::Checkbox(
                    "X 位置",
                    &constraints.freezePositionX);
                ImGui::SameLine();
                constraintsChanged |= ImGui::Checkbox(
                    "Y 位置",
                    &constraints.freezePositionY);
                ImGui::SameLine();
                constraintsChanged |= ImGui::Checkbox(
                    "Z 位置",
                    &constraints.freezePositionZ);
                ImGui::TextDisabled(
                    "2DゲームはZ位置＋X/Y回転の固定を推奨");
                if (constraintsChanged)
                {
                    rigidbody->SetConstraints(
                        constraints);
                    RecordHistory();
                }

                int collisionDetection =
                    rigidbody->CollisionDetection()
                        == CollisionDetectionMode::Continuous
                    ? 1
                    : 0;
                const char* collisionModes[] = {
                    "Discrete",
                    "Continuous (CCD)"
                };
                if (ImGui::Combo(
                    "衝突検出",
                    &collisionDetection,
                    collisionModes,
                    std::size(collisionModes)))
                {
                    rigidbody->SetCollisionDetection(
                        collisionDetection == 1
                            ? CollisionDetectionMode::Continuous
                            : CollisionDetectionMode::Discrete);
                    RecordHistory();
                }
                bool interpolate =
                    rigidbody->Interpolates();
                if (ImGui::Checkbox(
                        "描画を補間",
                        &interpolate))
                {
                    rigidbody->SetInterpolate(
                        interpolate);
                    RecordHistory();
                }
                ImGui::TextDisabled(
                    "高速物体はContinuous、滑らかな表示には描画補間を使用");
            }
            else if (auto* joint =
                dynamic_cast<JointComponent*>(component.get()))
            {
                int jointType =
                    static_cast<int>(joint->Type());
                const char* jointTypes[] = {
                    "Fixed（固定）",
                    "Hinge（ヒンジ）",
                    "Spring（スプリング）"
                };
                if (ImGui::Combo(
                    "種類",
                    &jointType,
                    jointTypes,
                    std::size(jointTypes)))
                {
                    joint->SetType(
                        static_cast<JointType>(jointType));
                    RecordHistory();
                }

                const auto* connected =
                    m_scene.FindGameObject(
                        joint->ConnectedBodyId());
                std::string connectedPreview =
                    connected != nullptr
                    ? connected->Name()
                    : "未設定";
                if (ImGui::BeginCombo(
                    "接続先",
                    connectedPreview.c_str()))
                {
                    if (ImGui::Selectable(
                        "未設定",
                        joint->ConnectedBodyId() == 0))
                    {
                        joint->SetConnectedBodyId(0);
                        RecordHistory();
                    }
                    for (const auto& candidate :
                        m_scene.GameObjects())
                    {
                        if (candidate.get() == selected)
                        {
                            continue;
                        }
                        std::string label =
                            candidate->Name()
                            + " (ID "
                            + std::to_string(candidate->Id())
                            + ")";
                        if (ImGui::Selectable(
                            label.c_str(),
                            joint->ConnectedBodyId()
                                == candidate->Id()))
                        {
                            joint->SetConnectedBodyId(
                                candidate->Id());
                            RecordHistory();
                        }
                    }
                    ImGui::EndCombo();
                }

                auto anchor = joint->Anchor();
                if (ImGui::InputFloat3(
                    "アンカー",
                    &anchor.x))
                {
                    joint->SetAnchor(anchor);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto connectedAnchor =
                    joint->ConnectedAnchor();
                if (ImGui::InputFloat3(
                    "接続先アンカー",
                    &connectedAnchor.x))
                {
                    joint->SetConnectedAnchor(
                        connectedAnchor);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                if (joint->Type() == JointType::Hinge)
                {
                    auto axis = joint->Axis();
                    if (ImGui::InputFloat3(
                        "回転軸",
                        &axis.x))
                    {
                        joint->SetAxis(axis);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    bool useLimits =
                        joint->UseLimits();
                    if (ImGui::Checkbox(
                        "角度制限を使用",
                        &useLimits))
                    {
                        joint->SetUseLimits(
                            useLimits);
                        RecordHistory();
                    }
                    if (useLimits)
                    {
                        auto limits =
                            joint->Limits();
                        if (ImGui::DragFloat(
                            "最小角度",
                            &limits
                                .minimumAngleDegrees,
                            1.0f,
                            -360.0f,
                            360.0f,
                            "%.1f°"))
                        {
                            joint->SetLimits(limits);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                        limits = joint->Limits();
                        if (ImGui::DragFloat(
                            "最大角度",
                            &limits
                                .maximumAngleDegrees,
                            1.0f,
                            -360.0f,
                            360.0f,
                            "%.1f°"))
                        {
                            joint->SetLimits(limits);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    }

                    bool useMotor =
                        joint->UseMotor();
                    if (ImGui::Checkbox(
                        "モーターを使用",
                        &useMotor))
                    {
                        joint->SetUseMotor(
                            useMotor);
                        RecordHistory();
                    }
                    if (useMotor)
                    {
                        auto motor =
                            joint->Motor();
                        if (ImGui::DragFloat(
                            "目標角速度",
                            &motor
                                .targetVelocityDegrees,
                            1.0f,
                            -100000.0f,
                            100000.0f,
                            "%.1f°/s"))
                        {
                            joint->SetMotor(motor);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                        motor = joint->Motor();
                        if (ImGui::DragFloat(
                            "最大トルク",
                            &motor.maximumTorque,
                            0.1f,
                            0.0f,
                            100000.0f,
                            "%.2f"))
                        {
                            joint->SetMotor(motor);
                        }
                        if (ImGui::
                            IsItemDeactivatedAfterEdit())
                        {
                            RecordHistory();
                        }
                    }
                    ImGui::TextDisabled(
                        "初期姿勢を 0° として回転します");
                }
                else if (joint->Type() == JointType::Spring)
                {
                    float restLength =
                        joint->RestLength();
                    if (ImGui::DragFloat(
                        "自然長",
                        &restLength,
                        0.05f,
                        0.0f,
                        10000.0f))
                    {
                        joint->SetRestLength(
                            restLength);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float stiffness =
                        joint->Stiffness();
                    if (ImGui::DragFloat(
                        "強さ",
                        &stiffness,
                        0.1f,
                        0.0f,
                        10000.0f))
                    {
                        joint->SetStiffness(stiffness);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    float damping = joint->Damping();
                    if (ImGui::DragFloat(
                        "減衰",
                        &damping,
                        0.1f,
                        0.0f,
                        10000.0f))
                    {
                        joint->SetDamping(damping);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }
                }

                bool collideConnected =
                    joint->CollideConnected();
                if (ImGui::Checkbox(
                    "接続した物体同士を衝突",
                    &collideConnected))
                {
                    joint->SetCollideConnected(
                        collideConnected);
                    RecordHistory();
                }
            }
            else if (auto* lodGroup =
                dynamic_cast<LODGroupComponent*>(
                    component.get()))
            {
                float cullDistance =
                    lodGroup->CullDistance();
                if (ImGui::DragFloat(
                    "完全に非表示にする距離",
                    &cullDistance,
                    1.0f,
                    0.0f,
                    100000.0f))
                {
                    lodGroup->SetCullDistance(
                        cullDistance);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    RecordHistory();
                }

                auto levels = lodGroup->Levels();
                std::optional<std::size_t>
                    removeLevel;
                for (std::size_t index{};
                    index < levels.size();
                    ++index)
                {
                    ImGui::PushID(
                        static_cast<int>(index));
                    ImGui::SeparatorText(
                        ("LOD "
                            + std::to_string(index)).
                            c_str());
                    if (ImGui::DragFloat(
                        "最大距離",
                        &levels[index].
                            maximumDistance,
                        1.0f,
                        0.0f,
                        100000.0f))
                    {
                        lodGroup->SetLevels(
                            levels);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        RecordHistory();
                    }

                    const auto* target =
                        m_scene.FindGameObject(
                            levels[index].
                                targetId);
                    const std::string preview =
                        target != nullptr
                        ? target->Name()
                        : "未設定";
                    if (ImGui::BeginCombo(
                        "表示するGameObject",
                        preview.c_str()))
                    {
                        if (ImGui::Selectable(
                            "未設定",
                            levels[index].
                                targetId == 0))
                        {
                            levels[index].
                                targetId = 0;
                            lodGroup->SetLevels(
                                levels);
                            RecordHistory();
                        }
                        for (const auto& candidate :
                            m_scene.GameObjects())
                        {
                            const std::string label =
                                candidate->Name()
                                + " (ID "
                                + std::to_string(
                                    candidate->Id())
                                + ")";
                            if (ImGui::Selectable(
                                label.c_str(),
                                levels[index].
                                    targetId
                                    == candidate->
                                        Id()))
                            {
                                levels[index].
                                    targetId =
                                        candidate->
                                            Id();
                                lodGroup->SetLevels(
                                    levels);
                                RecordHistory();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button(
                        "このLODを削除"))
                    {
                        removeLevel = index;
                    }
                    ImGui::PopID();
                }
                if (removeLevel)
                {
                    lodGroup->RemoveLevel(
                        *removeLevel);
                    RecordHistory();
                }
                ImGui::BeginDisabled(
                    levels.size() >= 8);
                if (ImGui::Button(
                    "LODレベルを追加"))
                {
                    const float distance =
                        levels.empty()
                        ? 25.0f
                        : levels.back().
                            maximumDistance
                            + 25.0f;
                    lodGroup->AddLevel({
                        distance,
                        0
                    });
                    RecordHistory();
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled(
                    "各LOD用の子GameObjectを指定します");
            }

            ImGui::PopID();
        }

        if (componentToRemove != nullptr)
        {
            m_scene.RemoveComponent(*selected, *componentToRemove);
            RecordHistory();
            SetStatus("コンポーネントを削除しました");
        }
        else if (pendingComponentReorder.requested
            && pendingComponentReorder.moved != nullptr
            && pendingComponentReorder.reference != nullptr)
        {
            // ReorderComponentは両方が自分のコンポーネントで
            // なければfalseを返すので、そのまま渡して構いません。
            if (selected->ReorderComponent(
                    *pendingComponentReorder.moved,
                    *pendingComponentReorder.reference,
                    pendingComponentReorder.insertAfter))
            {
                RecordHistory();
                SetStatus("コンポーネントの並びを変更しました");
            }
        }

        DrawAddComponent(*selected);

        ImGui::EndDisabled();
        ImGui::End();
    }

    void EditorLayer::DrawAddComponent(GameObject& gameObject)
    {
        static std::array<char, 128> componentSearch{};
        static std::size_t componentCategoryIndex = 0;
        static constexpr std::array<std::string_view, 9>
            componentCategories{
                "All Categories",
                "Rendering",
                "Physics",
                "Navigation",
                "Animation",
                "Audio",
                "UI",
                "Gameplay",
                "Scripts"
            };

        ImGui::Separator();
        if (ImGui::Button(
                "コンポーネントを追加",
                ImVec2{ -1.0f, 0.0f }))
        {
            componentSearch.fill('\0');
            componentCategoryIndex = 0;
            ImGui::OpenPopup("AddComponentPopup");
        }
        if (!m_playing && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetPayload))
            {
                const auto asset = PathFromUtf8(
                    static_cast<const char*>(payload->Data));
                if (IsCppScriptAsset(asset))
                {
                    QueueCppScriptAttachment(
                        gameObject,
                        asset);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SetNextWindowSizeConstraints(
            ImVec2{ 360.0f, 0.0f },
            ImVec2{ 520.0f, 520.0f });
        if (!ImGui::BeginPopup("AddComponentPopup"))
        {
            return;
        }

        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        const float categoryWidth = 145.0f;
        ImGui::SetNextItemWidth(
            std::max(
                120.0f,
                ImGui::GetContentRegionAvail().x
                    - categoryWidth
                    - ImGui::GetStyle().ItemSpacing.x));
        ImGui::InputTextWithHint(
            "##ComponentSearch",
            "Search components...",
            componentSearch.data(),
            componentSearch.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(categoryWidth);
        if (ImGui::BeginCombo(
                "##ComponentCategory",
                componentCategories[componentCategoryIndex].data()))
        {
            for (std::size_t index = 0;
                 index < componentCategories.size();
                 ++index)
            {
                const bool selected =
                    componentCategoryIndex == index;
                if (ImGui::Selectable(
                        componentCategories[index].data(),
                        selected))
                {
                    componentCategoryIndex = index;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        const std::string loweredSearch =
            Lowercase(componentSearch.data());
        const auto matchesName =
            [&loweredSearch](const std::string_view name)
            {
                return loweredSearch.empty()
                    || Lowercase(std::string{ name }).find(
                        loweredSearch) != std::string::npos;
            };
        const auto matchesCategory =
            [](const std::string_view category)
            {
                return componentCategoryIndex == 0
                    || componentCategories[componentCategoryIndex]
                        == category;
            };
        std::size_t visibleComponentCount = 0;
        const auto showComponent =
            [&matchesName,
             &matchesCategory,
             &visibleComponentCount](
                const std::string_view name,
                const std::string_view category)
            {
                const bool visible =
                    matchesCategory(category)
                    && matchesName(name);
                visibleComponentCount += visible ? 1u : 0u;
                return visible;
            };

        // 検索中／カテゴリー絞り込み中はカテゴリー見出しを省いて
        // フラットな一覧にし、それ以外（All Categories・検索欄が空）
        // ではカテゴリーごとに折りたたんで表示します。
        const bool showCategoryHeaders =
            loweredSearch.empty()
            && componentCategoryIndex == 0;
        const auto beginCategorySection =
            [showCategoryHeaders](const char* label)
            {
                if (!showCategoryHeaders)
                {
                    return true;
                }
                const bool open = ImGui::CollapsingHeader(label);
                if (open)
                {
                    ImGui::Indent();
                }
                return open;
            };
        const auto endCategorySection =
            [showCategoryHeaders]()
            {
                if (showCategoryHeaders)
                {
                    ImGui::Unindent();
                }
            };

        const bool hasCamera = gameObject.GetComponent<CameraComponent>() != nullptr;
        const bool hasDirectionalLight =
            gameObject.GetComponent<
                DirectionalLightComponent>() != nullptr;
        const bool hasPointLight =
            gameObject.GetComponent<PointLightComponent>() != nullptr;
        const bool hasSpotLight =
            gameObject.GetComponent<SpotLightComponent>() != nullptr;
        const bool hasLight2D =
            gameObject.GetComponent<Light2DComponent>() != nullptr;
        const bool hasCollider2D =
            gameObject.GetComponent<BoxCollider2DComponent>() != nullptr;
        const bool hasCircleCollider2D =
            gameObject.GetComponent<
                CircleCollider2DComponent>() != nullptr;
        const bool hasPolygonCollider2D =
            gameObject.GetComponent<
                PolygonCollider2DComponent>() != nullptr;
        const bool hasCollider3D =
            gameObject.GetComponent<BoxCollider3DComponent>() != nullptr;
        const bool hasCapsuleCollider3D =
            gameObject.GetComponent<
                CapsuleCollider3DComponent>() != nullptr;
        const bool hasSphereCollider3D =
            gameObject.GetComponent<
                SphereCollider3DComponent>() != nullptr;
        const bool hasHullCollider3D =
            gameObject.GetComponent<
                ConvexHullCollider3DComponent>() != nullptr;
        const bool hasMeshCollider3D =
            gameObject.GetComponent<
                MeshCollider3DComponent>() != nullptr;
        const bool hasNavMesh =
            gameObject.GetComponent<
                NavMeshComponent>() != nullptr;
        const bool hasNavMeshAgent =
            gameObject.GetComponent<
                NavMeshAgentComponent>() != nullptr;
        const bool hasMesh = gameObject.GetComponent<MeshRendererComponent>() != nullptr;
        const bool hasModel = gameObject.GetComponent<ModelRendererComponent>() != nullptr;
        const bool hasSprite = gameObject.GetComponent<SpriteRendererComponent>() != nullptr;
        const bool hasSpriteMask =
            gameObject.GetComponent<
                SpriteMaskComponent>() != nullptr;
        const bool hasRenderCulling =
            gameObject.GetComponent<
                RenderCullingComponent>() != nullptr;
        const bool hasReflectionProbe =
            gameObject.GetComponent<
                ReflectionProbeComponent>() != nullptr;
        const bool hasSpriteAnimator =
            gameObject.GetComponent<
                SpriteAnimatorComponent>() != nullptr;
        const bool hasTilemap =
            gameObject.GetComponent<
                TilemapComponent>() != nullptr;
        const bool hasParallaxLayer =
            gameObject.GetComponent<
                ParallaxLayerComponent>() != nullptr;
        const bool hasParticles =
            gameObject.GetComponent<
                ParticleSystemComponent>() != nullptr;
        const bool hasSpriteParticles2D =
            gameObject.GetComponent<
                SpriteParticles2DComponent>() != nullptr;
        const bool hasUICanvas =
            gameObject.GetComponent<
                UICanvasComponent>() != nullptr;
        const bool hasUIRect =
            gameObject.GetComponent<
                UIRectTransformComponent>() != nullptr;
        const bool hasUIButton =
            gameObject.GetComponent<
                UIButtonComponent>() != nullptr;
        const bool hasUIImage =
            gameObject.GetComponent<
                UIImageComponent>() != nullptr;
        const bool hasUIToggle =
            gameObject.GetComponent<
                UIToggleComponent>() != nullptr;
        const bool hasUISlider =
            gameObject.GetComponent<
                UISliderComponent>() != nullptr;
        const bool hasUIInputField =
            gameObject.GetComponent<
                UIInputFieldComponent>() != nullptr;
        const bool hasUILayoutGroup =
            gameObject.GetComponent<
                UILayoutGroupComponent>() != nullptr;
        const bool hasUIScrollView =
            gameObject.GetComponent<
                UIScrollViewComponent>() != nullptr;
        const bool hasText = gameObject.GetComponent<TextRendererComponent>() != nullptr;
        const bool hasAudioListener =
            gameObject.GetComponent<
                AudioListenerComponent>() != nullptr;
        const bool hasAudio = gameObject.GetComponent<AudioSourceComponent>() != nullptr;
        const bool hasRotator = gameObject.GetComponent<RotatorComponent>() != nullptr;
        const bool hasBillboard =
            gameObject.GetComponent<
                BillboardComponent>() != nullptr;
        const bool hasAnimator =
            gameObject.GetComponent<
                TransformAnimatorComponent>() != nullptr;
        const bool hasInputMover =
            gameObject.GetComponent<
                InputMoverComponent>() != nullptr;
        const bool hasCharacterController =
            gameObject.GetComponent<
                CharacterControllerComponent>() != nullptr;
        const bool hasRigidbody = gameObject.GetComponent<RigidbodyComponent>() != nullptr;
        const bool hasJoint =
            gameObject.GetComponent<JointComponent>() != nullptr;
        const bool hasLODGroup =
            gameObject.GetComponent<
                LODGroupComponent>() != nullptr;

        if (beginCategorySection("Rendering"))
        {
            ImGui::BeginDisabled(hasCamera);
            if (showComponent("Camera", "Rendering")
                && ImGui::Selectable("Camera"))
            {
                auto& camera = gameObject.AddComponent<CameraComponent>();
                if (m_scene.MainCamera() == nullptr)
                {
                    m_scene.SetMainCamera(camera);
                }
                RecordHistory();
                SetStatus("Cameraを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasDirectionalLight);
            if (showComponent("Directional Light", "Rendering")
                && ImGui::Selectable("Directional Light"))
            {
                gameObject.AddComponent<
                    DirectionalLightComponent>();
                RecordHistory();
                SetStatus("Directional Lightを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasPointLight);
            if (showComponent("Point Light", "Rendering")
                && ImGui::Selectable("Point Light"))
            {
                gameObject.AddComponent<PointLightComponent>();
                RecordHistory();
                SetStatus("Point Lightを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSpotLight);
            if (showComponent("Spot Light", "Rendering")
                && ImGui::Selectable("Spot Light"))
            {
                gameObject.AddComponent<SpotLightComponent>();
                RecordHistory();
                SetStatus("Spot Lightを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasLight2D);
            if (showComponent("Light 2D", "Rendering")
                && ImGui::Selectable("Light 2D"))
            {
                gameObject.AddComponent<Light2DComponent>();
                RecordHistory();
                SetStatus("Light 2Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasRenderCulling);
            if (showComponent("描画カリング", "Rendering")
                && ImGui::Selectable("描画カリング"))
            {
                gameObject.AddComponent<
                    RenderCullingComponent>();
                RecordHistory();
                SetStatus("描画カリングを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasReflectionProbe);
            if (showComponent(
                    "リフレクションプローブ",
                    "Rendering")
                && ImGui::Selectable(
                    "リフレクションプローブ"))
            {
                gameObject.AddComponent<
                    ReflectionProbeComponent>();
                RecordHistory();
                SetStatus(
                    "リフレクションプローブを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasMesh);
            if (showComponent("Mesh Renderer (Cube)", "Rendering")
                && ImGui::Selectable("Mesh Renderer (Cube)"))
            {
                gameObject.AddComponent<MeshRendererComponent>(PrimitiveShape::Cube);
                RecordHistory();
                SetStatus("Mesh Rendererを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasModel);
            if (showComponent("Model Renderer", "Rendering")
                && ImGui::Selectable("Model Renderer"))
            {
                gameObject.AddComponent<ModelRendererComponent>();
                RecordHistory();
                SetStatus("Model Rendererを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSprite);
            if (showComponent("Sprite Renderer", "Rendering")
                && ImGui::Selectable("Sprite Renderer"))
            {
                gameObject.AddComponent<SpriteRendererComponent>();
                RecordHistory();
                SetStatus("Sprite Rendererを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSpriteMask);
            if (showComponent("Sprite Mask", "Rendering")
                && ImGui::Selectable("Sprite Mask"))
            {
                gameObject.AddComponent<
                    SpriteMaskComponent>();
                RecordHistory();
                SetStatus("Sprite Maskを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSpriteAnimator);
            if (showComponent("Sprite Animator", "Rendering")
                && ImGui::Selectable("Sprite Animator"))
            {
                gameObject.AddComponent<
                    SpriteAnimatorComponent>();
                RecordHistory();
                SetStatus("Sprite Animatorを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasTilemap);
            if (showComponent("Tilemap", "Rendering")
                && ImGui::Selectable("Tilemap"))
            {
                gameObject.AddComponent<
                    TilemapComponent>();
                m_tilePaletteSelectedTile = 0;
                m_tilemapTool =
                    TilemapTool::Paint;
                RecordHistory();
                SetStatus(
                    "Tilemapを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasParallaxLayer);
            if (showComponent("Parallax Layer", "Rendering")
                && ImGui::Selectable("Parallax Layer"))
            {
                gameObject.AddComponent<
                    ParallaxLayerComponent>();
                RecordHistory();
                SetStatus(
                    "Parallax Layerを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasParticles);
            if (showComponent("Particle System", "Rendering")
                && ImGui::Selectable("Particle System"))
            {
                gameObject.AddComponent<
                    ParticleSystemComponent>();
                RecordHistory();
                SetStatus(
                    "Particle Systemを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSpriteParticles2D);
            if (showComponent(
                    "2D Sprite Particles",
                    "Rendering")
                && ImGui::Selectable(
                    "2D Sprite Particles"))
            {
                gameObject.AddComponent<
                    SpriteParticles2DComponent>();
                RecordHistory();
                SetStatus(
                    "2D Sprite Particlesを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasText);
            if (showComponent("Text Renderer", "Rendering")
                && ImGui::Selectable("Text Renderer"))
            {
                gameObject.AddComponent<TextRendererComponent>();
                RecordHistory();
                SetStatus("Text Rendererを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasLODGroup);
            if (showComponent("LOD Group", "Rendering")
                && ImGui::Selectable("LOD Group"))
            {
                gameObject.AddComponent<
                    LODGroupComponent>();
                RecordHistory();
                SetStatus(
                    "LOD Groupを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("Physics"))
        {
            ImGui::BeginDisabled(hasCircleCollider2D);
            if (showComponent("Circle Collider 2D", "Physics")
                && ImGui::Selectable("Circle Collider 2D"))
            {
                gameObject.AddComponent<
                    CircleCollider2DComponent>();
                RecordHistory();
                SetStatus(
                    "Circle Collider 2Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasCollider2D);
            if (showComponent("Box Collider 2D", "Physics")
                && ImGui::Selectable("Box Collider 2D"))
            {
                gameObject.AddComponent<BoxCollider2DComponent>();
                RecordHistory();
                SetStatus("Box Collider 2Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasPolygonCollider2D);
            if (showComponent("Polygon Collider 2D", "Physics")
                && ImGui::Selectable("Polygon Collider 2D"))
            {
                gameObject.AddComponent<
                    PolygonCollider2DComponent>();
                RecordHistory();
                SetStatus(
                    "Polygon Collider 2Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasCollider3D);
            if (showComponent("Box Collider 3D", "Physics")
                && ImGui::Selectable("Box Collider 3D"))
            {
                gameObject.AddComponent<BoxCollider3DComponent>();
                RecordHistory();
                SetStatus("Box Collider 3Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasCapsuleCollider3D);
            if (showComponent("Capsule Collider 3D", "Physics")
                && ImGui::Selectable("Capsule Collider 3D"))
            {
                gameObject.AddComponent<
                    CapsuleCollider3DComponent>();
                RecordHistory();
                SetStatus(
                    "Capsule Collider 3Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasSphereCollider3D);
            if (showComponent("Sphere Collider 3D", "Physics")
                && ImGui::Selectable(
                    "Sphere Collider 3D"))
            {
                gameObject.AddComponent<
                    SphereCollider3DComponent>();
                RecordHistory();
                SetStatus(
                    "Sphere Collider 3Dを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasHullCollider3D);
            if (showComponent("Convex Hull Collider", "Physics")
                && ImGui::Selectable(
                    "Convex Hull Collider"))
            {
                gameObject.AddComponent<
                    ConvexHullCollider3DComponent>();
                RecordHistory();
                SetStatus(
                    "Convex Hull Colliderを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasMeshCollider3D);
            if (showComponent("Mesh Collider", "Physics")
                && ImGui::Selectable("Mesh Collider"))
            {
                // モデル未指定ならModelRendererから自動で借ります。
                gameObject.AddComponent<
                    MeshCollider3DComponent>();
                RecordHistory();
                SetStatus(
                    "Mesh Colliderを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasCharacterController);
            if (showComponent("Character Controller", "Physics")
                && ImGui::Selectable("Character Controller"))
            {
                gameObject.AddComponent<
                    CharacterControllerComponent>();
                RecordHistory();
                SetStatus(
                    "Character Controllerを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasRigidbody);
            if (showComponent("Rigidbody", "Physics")
                && ImGui::Selectable("Rigidbody"))
            {
                gameObject.AddComponent<RigidbodyComponent>();
                RecordHistory();
                SetStatus("Rigidbodyを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasJoint);
            if (showComponent("Joint", "Physics")
                && ImGui::Selectable("Joint"))
            {
                gameObject.AddComponent<JointComponent>();
                RecordHistory();
                SetStatus("Jointを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("Navigation"))
        {
            ImGui::BeginDisabled(hasNavMesh);
            if (showComponent("NavMesh Surface", "Navigation")
                && ImGui::Selectable(
                    "NavMesh Surface"))
            {
                gameObject.AddComponent<
                    NavMeshComponent>();
                RecordHistory();
                SetStatus(
                    "NavMesh Surfaceを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(
                hasNavMeshAgent);
            if (showComponent("NavMesh Agent", "Navigation")
                && ImGui::Selectable(
                    "NavMesh Agent"))
            {
                gameObject.AddComponent<
                    NavMeshAgentComponent>();
                RecordHistory();
                SetStatus(
                    "NavMesh Agentを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("Animation"))
        {
            ImGui::BeginDisabled(hasRotator);
            if (showComponent("Rotator", "Animation")
                && ImGui::Selectable("Rotator"))
            {
                gameObject.AddComponent<RotatorComponent>();
                RecordHistory();
                SetStatus("Rotatorを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasBillboard);
            if (showComponent("Billboard", "Animation")
                && ImGui::Selectable("Billboard"))
            {
                gameObject.AddComponent<
                    BillboardComponent>();
                RecordHistory();
                SetStatus(
                    "Billboardを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasAnimator);
            if (showComponent("Transform Animator", "Animation")
                && ImGui::Selectable(
                    "Transform Animator"))
            {
                gameObject.AddComponent<
                    TransformAnimatorComponent>();
                RecordHistory();
                SetStatus(
                    "Transform Animatorを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("Audio"))
        {
            ImGui::BeginDisabled(hasAudioListener);
            if (showComponent("Audio Listener", "Audio")
                && ImGui::Selectable("Audio Listener"))
            {
                gameObject.AddComponent<
                    AudioListenerComponent>();
                RecordHistory();
                SetStatus("Audio Listenerを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasAudio);
            if (showComponent("Audio Source", "Audio")
                && ImGui::Selectable("Audio Source"))
            {
                gameObject.AddComponent<AudioSourceComponent>();
                RecordHistory();
                SetStatus("Audio Sourceを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("UI"))
        {
            ImGui::BeginDisabled(hasUICanvas);
            if (showComponent("UI Canvas", "UI")
                && ImGui::Selectable("UI Canvas"))
            {
                gameObject.AddComponent<
                    UICanvasComponent>();
                RecordHistory();
                SetStatus(
                    "UI Canvasを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIRect);
            if (showComponent("UI Rect Transform", "UI")
                && ImGui::Selectable(
                    "UI Rect Transform"))
            {
                gameObject.AddComponent<
                    UIRectTransformComponent>();
                RecordHistory();
                SetStatus(
                    "UI Rect Transformを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIButton);
            if (showComponent("UI Button", "UI")
                && ImGui::Selectable("UI Button"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UIButtonComponent>();
                RecordHistory();
                SetStatus(
                    "UI Buttonを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIImage);
            if (showComponent("UI Image", "UI")
                && ImGui::Selectable("UI Image"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UIImageComponent>();
                RecordHistory();
                SetStatus("UI Imageを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIToggle);
            if (showComponent("UI Toggle", "UI")
                && ImGui::Selectable("UI Toggle"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UIToggleComponent>();
                RecordHistory();
                SetStatus("UI Toggleを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUISlider);
            if (showComponent("UI Slider", "UI")
                && ImGui::Selectable("UI Slider"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UISliderComponent>();
                RecordHistory();
                SetStatus("UI Sliderを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIInputField);
            if (showComponent("UI Input Field", "UI")
                && ImGui::Selectable("UI Input Field"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UIInputFieldComponent>();
                RecordHistory();
                SetStatus(
                    "UI Input Fieldを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUILayoutGroup);
            if (showComponent("UI Layout Group", "UI")
                && ImGui::Selectable("UI Layout Group"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UILayoutGroupComponent>();
                RecordHistory();
                SetStatus(
                    "UI Layout Groupを追加しました");
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(hasUIScrollView);
            if (showComponent("UI Scroll View", "UI")
                && ImGui::Selectable("UI Scroll View"))
            {
                if (!hasUIRect)
                {
                    gameObject.AddComponent<
                        UIRectTransformComponent>();
                }
                gameObject.AddComponent<
                    UIScrollViewComponent>();
                RecordHistory();
                SetStatus(
                    "UI Scroll Viewを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        if (beginCategorySection("Gameplay"))
        {
            ImGui::BeginDisabled(hasInputMover);
            if (showComponent("Input Mover", "Gameplay")
                && ImGui::Selectable("Input Mover"))
            {
                gameObject.AddComponent<
                    InputMoverComponent>();
                RecordHistory();
                SetStatus("Input Moverを追加しました");
            }
            ImGui::EndDisabled();

            endCategorySection();
        }

        bool showedEmptyModuleNotice = false;
        if (auto* module = GameModuleHost::Current();
            module != nullptr
            && matchesCategory("Scripts")
            && (module->RegisteredComponents().empty()
                ? loweredSearch.empty()
                : std::ranges::any_of(
                    module->RegisteredComponents(),
                    [&matchesName](const auto& registration)
                    {
                        return matchesName(
                            registration.displayName);
                    })))
        {
            if (beginCategorySection("Scripts"))
            {
                ImGui::SeparatorText(
                    "Game Module (C++)");
                if (module->RegisteredComponents().empty())
                {
                    showedEmptyModuleNotice = true;
                    ImGui::TextDisabled(
                        "登録されたC++ Componentはありません");
                    if (!module->LastError().empty())
                    {
                        ImGui::TextWrapped(
                            "%s",
                            module->LastError().c_str());
                    }
                }
                for (const auto& registration :
                    module->RegisteredComponents())
                {
                    const bool alreadyAdded =
                        std::ranges::any_of(
                            gameObject.Components(),
                            [&registration](
                                const std::unique_ptr<
                                    Component>& component)
                            {
                                const auto* script =
                                    dynamic_cast<
                                        const NativeScriptComponent*>(
                                            component.get());
                                return script != nullptr
                                    && script->ScriptType()
                                        == registration.typeName;
                            });
                    ImGui::BeginDisabled(alreadyAdded);
                    const std::string label =
                        registration.displayName
                        + "##"
                        + registration.typeName;
                    if (showComponent(
                            registration.displayName,
                            "Scripts")
                        && ImGui::Selectable(
                            label.c_str()))
                    {
                        gameObject.AddComponent<
                            NativeScriptComponent>(
                                registration.typeName);
                        RecordHistory();
                        SetStatus(
                            registration.displayName
                            + "を追加しました");
                    }
                    ImGui::EndDisabled();
                }
                endCategorySection();
            }
        }

        if (!showCategoryHeaders
            && visibleComponentCount == 0
            && !showedEmptyModuleNotice)
        {
            ImGui::TextDisabled("No components found.");
        }

        ImGui::EndPopup();
    }
}
