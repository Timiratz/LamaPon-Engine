#include "LamaPon/Scene/Scene.h"

#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/CharacterControllerComponent.h"
#include "LamaPon/Components/AudioListenerComponent.h"
#include "LamaPon/Components/AudioSourceComponent.h"
#include "LamaPon/Components/BoxCollider2DComponent.h"
#include "LamaPon/Components/BoxCollider3DComponent.h"
#include "LamaPon/Components/CapsuleCollider3DComponent.h"
#include "LamaPon/Components/ConvexHullCollider3DComponent.h"
#include "LamaPon/Components/SphereCollider3DComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/InputMoverComponent.h"
#include "LamaPon/Components/JointComponent.h"
#include "LamaPon/Components/LODGroupComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/NavMeshAgentComponent.h"
#include "LamaPon/Components/NavMeshComponent.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/SpriteParticles2DComponent.h"
#include "LamaPon/Components/UICanvasComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/CircleCollider2DComponent.h"
#include "LamaPon/Components/PolygonCollider2DComponent.h"
#include "LamaPon/Components/Light2DComponent.h"
#include "LamaPon/Components/MeshCollider3DComponent.h"
#include "LamaPon/Components/UIButtonComponent.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIInputFieldComponent.h"
#include "LamaPon/Components/UILayoutGroupComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Components/UISliderComponent.h"
#include "LamaPon/Components/UIToggleComponent.h"
#include "LamaPon/Components/BillboardComponent.h"
#include "LamaPon/Components/RotatorComponent.h"
#include "LamaPon/Components/RigidbodyComponent.h"
#include "LamaPon/Components/SpriteAnimatorComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Components/RenderCullingComponent.h"
#include "LamaPon/Components/SpriteMaskComponent.h"
#include "LamaPon/Components/TextRendererComponent.h"
#include "LamaPon/Components/TilemapComponent.h"
#include "LamaPon/Components/ParallaxLayerComponent.h"
#include "LamaPon/Components/TransformAnimatorComponent.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Core/DocumentMigration.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/SceneManager.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace
{
    // GIの焼き込みデータ（fp16配列）をシーンJSONへ入れるための
    // base64。数値の配列で書くとファイルが5倍以上に膨れるうえ、
    // 差分も読めないので、ひとかたまりの文字列にします。
    constexpr char Base64Characters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz0123456789+/";

    [[nodiscard]] std::string EncodeBase64(
        const std::uint8_t* data,
        const std::size_t size)
    {
        std::string output;
        output.reserve((size + 2) / 3 * 4);
        for (std::size_t index = 0; index < size; index += 3)
        {
            const std::uint32_t remaining = static_cast<std::uint32_t>(
                std::min<std::size_t>(3, size - index));
            std::uint32_t chunk =
                static_cast<std::uint32_t>(data[index]) << 16;
            if (remaining > 1)
            {
                chunk |= static_cast<std::uint32_t>(
                    data[index + 1]) << 8;
            }
            if (remaining > 2)
            {
                chunk |= data[index + 2];
            }
            output.push_back(
                Base64Characters[(chunk >> 18) & 0x3f]);
            output.push_back(
                Base64Characters[(chunk >> 12) & 0x3f]);
            output.push_back(
                remaining > 1
                    ? Base64Characters[(chunk >> 6) & 0x3f]
                    : '=');
            output.push_back(
                remaining > 2
                    ? Base64Characters[chunk & 0x3f]
                    : '=');
        }
        return output;
    }

    [[nodiscard]] std::vector<std::uint8_t> DecodeBase64(
        const std::string& text)
    {
        const auto valueOf = [](const char character)
            -> std::int32_t
        {
            if (character >= 'A' && character <= 'Z')
            {
                return character - 'A';
            }
            if (character >= 'a' && character <= 'z')
            {
                return character - 'a' + 26;
            }
            if (character >= '0' && character <= '9')
            {
                return character - '0' + 52;
            }
            if (character == '+')
            {
                return 62;
            }
            if (character == '/')
            {
                return 63;
            }
            return -1;
        };
        std::vector<std::uint8_t> output;
        output.reserve(text.size() / 4 * 3);
        std::uint32_t accumulator = 0;
        int bits = 0;
        for (const char character : text)
        {
            if (character == '=')
            {
                break;
            }
            const auto value = valueOf(character);
            if (value < 0)
            {
                // 想定外の文字が混ざったファイルは信用しません。
                return {};
            }
            accumulator = (accumulator << 6)
                | static_cast<std::uint32_t>(value);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                output.push_back(static_cast<std::uint8_t>(
                    (accumulator >> bits) & 0xff));
            }
        }
        return output;
    }

    using Json = nlohmann::json;

    const LamaPon::AssetDatabase& AssetDatabaseFor(
        LamaPon::GraphicsDevice& graphics)
    {
        static const LamaPon::AssetDatabase emptyDatabase;
        const auto* assets = graphics.TryAssets();
        return assets != nullptr
            ? assets->Database()
            : emptyDatabase;
    }

    std::string EscapeJsonPointerToken(
        const std::string_view token)
    {
        std::string escaped;
        escaped.reserve(token.size());
        for (const char character : token)
        {
            if (character == '~')
            {
                escaped += "~0";
            }
            else if (character == '/')
            {
                escaped += "~1";
            }
            else
            {
                escaped += character;
            }
        }
        return escaped;
    }

    std::string DisplayJsonValue(
        const Json& value)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        return value.dump();
    }

    void AddPrefabOverride(
        std::vector<LamaPon::PrefabOverride>& overrides,
        std::string path,
        const Json* source,
        const Json* instance,
        const bool canApplyIndividually)
    {
        overrides.push_back(
            LamaPon::PrefabOverride{
                std::move(path),
                source != nullptr
                    ? DisplayJsonValue(*source)
                    : std::string{ "（なし）" },
                instance != nullptr
                    ? DisplayJsonValue(*instance)
                    : std::string{ "（なし）" },
                source != nullptr,
                instance != nullptr,
                canApplyIndividually
                    && source != nullptr
                    && instance != nullptr
            });
    }

    bool IsStructuralPrefabArray(
        const std::string_view path)
    {
        return path == "/objects"
            || path.ends_with("/components");
    }

    void CollectPrefabOverrides(
        const Json& source,
        const Json& instance,
        const std::string& path,
        std::vector<LamaPon::PrefabOverride>& overrides)
    {
        if (source.type() != instance.type())
        {
            AddPrefabOverride(
                overrides,
                path,
                &source,
                &instance,
                true);
            return;
        }

        if (source.is_object())
        {
            std::set<std::string> keys;
            for (const auto& [key, value] :
                source.items())
            {
                static_cast<void>(value);
                keys.insert(key);
            }
            for (const auto& [key, value] :
                instance.items())
            {
                static_cast<void>(value);
                keys.insert(key);
            }

            for (const auto& key : keys)
            {
                const auto sourceValue =
                    source.find(key);
                const auto instanceValue =
                    instance.find(key);
                const std::string childPath =
                    path + "/"
                    + EscapeJsonPointerToken(key);
                if (sourceValue == source.end()
                    || instanceValue == instance.end())
                {
                    AddPrefabOverride(
                        overrides,
                        childPath,
                        sourceValue != source.end()
                            ? &*sourceValue
                            : nullptr,
                        instanceValue != instance.end()
                            ? &*instanceValue
                            : nullptr,
                        false);
                    continue;
                }
                CollectPrefabOverrides(
                    *sourceValue,
                    *instanceValue,
                    childPath,
                    overrides);
            }
            return;
        }

        if (source.is_array())
        {
            if (!IsStructuralPrefabArray(path))
            {
                if (source != instance)
                {
                    AddPrefabOverride(
                        overrides,
                        path,
                        &source,
                        &instance,
                        true);
                }
                return;
            }

            if (source.size() != instance.size())
            {
                AddPrefabOverride(
                    overrides,
                    path,
                    &source,
                    &instance,
                    false);
                return;
            }

            for (std::size_t index = 0;
                index < source.size();
                ++index)
            {
                if (path.ends_with("/components")
                    && source[index].is_object()
                    && instance[index].is_object()
                    && source[index].value(
                        "type",
                        std::string{})
                        != instance[index].value(
                            "type",
                            std::string{}))
                {
                    AddPrefabOverride(
                        overrides,
                        path + "/"
                            + std::to_string(index),
                        &source[index],
                        &instance[index],
                        false);
                    continue;
                }
                CollectPrefabOverrides(
                    source[index],
                    instance[index],
                    path + "/"
                        + std::to_string(index),
                    overrides);
            }
            return;
        }

        if (source != instance)
        {
            AddPrefabOverride(
                overrides,
                path,
                &source,
                &instance,
                true);
        }
    }

    std::filesystem::path ResolvePrefabAssetPath(
        LamaPon::GraphicsDevice& graphics,
        const std::filesystem::path& path)
    {
        return path.is_absolute()
            ? path.lexically_normal()
            : graphics.Assets().ResolvePath(path);
    }

    Json ReadJsonDocument(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const std::string_view label)
    {
        if (!assets.FileExists(path))
        {
            throw std::runtime_error(
                "Could not open "
                + std::string{ label }
                + ": "
                + LamaPon::PathToUtf8(path));
        }
        const auto bytes = assets.ReadFileBytes(path);
        return Json::parse(bytes.begin(), bytes.end());
    }

    void WriteTextAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        auto temporaryPath = path;
        temporaryPath += L".tmp";
        std::ofstream output(
            temporaryPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not open file for writing: "
                + LamaPon::PathToUtf8(
                    temporaryPath));
        }
        output << text;
        output.close();
        if (!output)
        {
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not write file: "
                + LamaPon::PathToUtf8(
                    temporaryPath));
        }
        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not replace file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // 合成モードは既定値のとき省略し、シーンJSONの差分を抑えます。
    void SerializeMaterialCombine(
        Json& result,
        const LamaPon::PhysicsMaterial& material)
    {
        if (material.frictionCombine
            != LamaPon::PhysicsMaterialCombine::
                GeometricMean)
        {
            result["frictionCombine"] =
                static_cast<int>(
                    material.frictionCombine);
        }
        if (material.restitutionCombine
            != LamaPon::PhysicsMaterialCombine::Maximum)
        {
            result["restitutionCombine"] =
                static_cast<int>(
                    material.restitutionCombine);
        }
    }

    [[nodiscard]] LamaPon::PhysicsMaterial
        ReadPhysicsMaterial(const Json& value)
    {
        LamaPon::PhysicsMaterial material{
            value.value("friction", 0.5f),
            value.value("restitution", 0.0f) };
        material.frictionCombine =
            static_cast<LamaPon::PhysicsMaterialCombine>(
                std::clamp(
                    value.value("frictionCombine", 1),
                    0,
                    4));
        material.restitutionCombine =
            static_cast<LamaPon::PhysicsMaterialCombine>(
                std::clamp(
                    value.value(
                        "restitutionCombine",
                        4),
                    0,
                    4));
        return material;
    }

    Json ToJson(const DirectX::XMFLOAT2& value)
    {
        return Json::array({ value.x, value.y });
    }

    Json ToJson(const DirectX::XMFLOAT3& value)
    {
        return Json::array({ value.x, value.y, value.z });
    }

    Json ToJson(const DirectX::XMFLOAT4& value)
    {
        return Json::array({ value.x, value.y, value.z, value.w });
    }

    DirectX::XMFLOAT2 ReadFloat2(const Json& value)
    {
        if (!value.is_array() || value.size() != 2)
        {
            throw std::runtime_error("Expected a JSON array with two numbers.");
        }

        return { value.at(0).get<float>(), value.at(1).get<float>() };
    }

    DirectX::XMFLOAT3 ReadFloat3(const Json& value)
    {
        if (!value.is_array() || value.size() != 3)
        {
            throw std::runtime_error("Expected a JSON array with three numbers.");
        }

        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>()
        };
    }

    DirectX::XMFLOAT4 ReadFloat4(const Json& value)
    {
        if (!value.is_array() || value.size() != 4)
        {
            throw std::runtime_error("Expected a JSON array with four numbers.");
        }

        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>(),
            value.at(3).get<float>()
        };
    }

    // transformの回転を読み込みます。回転の正本はクォータニオン
    // ですが、それが無い古いシーン（オイラー角しか持たない）も
    // そのまま開けるようにします。
    void ReadTransformRotation(
        const Json& transformValue,
        LamaPon::Transform& transform)
    {
        if (transformValue.contains("rotationQuaternion"))
        {
            const auto quaternion = ReadFloat4(
                transformValue.at("rotationQuaternion"));
            transform.SetRotationVector(
                DirectX::XMLoadFloat4(&quaternion));
            return;
        }
        if (transformValue.contains("rotation"))
        {
            transform.SetEulerAngles(
                ReadFloat3(
                    transformValue.at("rotation")));
            return;
        }
        transform.rotationQuaternion = {
            0.0f, 0.0f, 0.0f, 1.0f };
    }

    const char* TextHorizontalAlignmentName(
        const LamaPon::TextHorizontalAlignment alignment)
    {
        switch (alignment)
        {
        case LamaPon::TextHorizontalAlignment::Center:
            return "Center";
        case LamaPon::TextHorizontalAlignment::Right:
            return "Right";
        default:
            return "Left";
        }
    }

    LamaPon::TextHorizontalAlignment ReadTextHorizontalAlignment(
        const std::string_view value)
    {
        if (value == "Center")
        {
            return LamaPon::TextHorizontalAlignment::Center;
        }
        if (value == "Right")
        {
            return LamaPon::TextHorizontalAlignment::Right;
        }
        return LamaPon::TextHorizontalAlignment::Left;
    }

    const char* TextVerticalAlignmentName(
        const LamaPon::TextVerticalAlignment alignment)
    {
        switch (alignment)
        {
        case LamaPon::TextVerticalAlignment::Center:
            return "Center";
        case LamaPon::TextVerticalAlignment::Bottom:
            return "Bottom";
        default:
            return "Top";
        }
    }

    LamaPon::TextVerticalAlignment ReadTextVerticalAlignment(
        const std::string_view value)
    {
        if (value == "Center")
        {
            return LamaPon::TextVerticalAlignment::Center;
        }
        if (value == "Bottom")
        {
            return LamaPon::TextVerticalAlignment::Bottom;
        }
        return LamaPon::TextVerticalAlignment::Top;
    }

    const char* ShapeName(const LamaPon::PrimitiveShape shape)
    {
        switch (shape)
        {
        case LamaPon::PrimitiveShape::Cube:
            return "Cube";
        case LamaPon::PrimitiveShape::Sphere:
            return "Sphere";
        case LamaPon::PrimitiveShape::Cylinder:
            return "Cylinder";
        case LamaPon::PrimitiveShape::Plane:
            return "Plane";
        default:
            throw std::runtime_error("Unsupported primitive shape.");
        }
    }

    LamaPon::PrimitiveShape ReadShape(const std::string& name)
    {
        if (name == "Cube")
        {
            return LamaPon::PrimitiveShape::Cube;
        }
        if (name == "Sphere")
        {
            return LamaPon::PrimitiveShape::Sphere;
        }
        if (name == "Cylinder")
        {
            return LamaPon::PrimitiveShape::Cylinder;
        }
        if (name == "Plane")
        {
            return LamaPon::PrimitiveShape::Plane;
        }

        throw std::runtime_error("Unknown primitive shape: " + name);
    }

    const char* ParticleShapeName(
        const LamaPon::ParticleEmitterShape shape)
    {
        switch (shape)
        {
        case LamaPon::ParticleEmitterShape::Cone:
            return "Cone";
        case LamaPon::ParticleEmitterShape::Sphere:
            return "Sphere";
        case LamaPon::ParticleEmitterShape::Box:
            return "Box";
        default:
            return "Cone";
        }
    }

    const char* ParticleRenderModeName(
        const LamaPon::ParticleRenderMode mode)
    {
        return mode
                == LamaPon::ParticleRenderMode::Horizontal
            ? "Horizontal"
            : "Billboard";
    }

    LamaPon::ParticleRenderMode ReadParticleRenderMode(
        const std::string& value)
    {
        return value == "Horizontal"
            ? LamaPon::ParticleRenderMode::Horizontal
            : LamaPon::ParticleRenderMode::Billboard;
    }

    const char* BillboardModeToString(
        const LamaPon::BillboardMode mode)
    {
        switch (mode)
        {
        case LamaPon::BillboardMode::FaceCameraPosition:
            return "FaceCameraPosition";
        case LamaPon::BillboardMode
                ::UprightFaceCameraPosition:
            return "UprightFaceCameraPosition";
        case LamaPon::BillboardMode::UprightScreenAligned:
            return "UprightScreenAligned";
        case LamaPon::BillboardMode::LookAtPosition:
            return "LookAtPosition";
        case LamaPon::BillboardMode::ScreenAligned:
        default:
            return "ScreenAligned";
        }
    }

    LamaPon::BillboardMode BillboardModeFromString(
        const std::string& value)
    {
        if (value == "FaceCameraPosition")
        {
            return LamaPon::BillboardMode
                ::FaceCameraPosition;
        }
        if (value == "UprightFaceCameraPosition")
        {
            return LamaPon::BillboardMode
                ::UprightFaceCameraPosition;
        }
        if (value == "UprightScreenAligned")
        {
            return LamaPon::BillboardMode
                ::UprightScreenAligned;
        }
        if (value == "LookAtPosition")
        {
            return LamaPon::BillboardMode::LookAtPosition;
        }
        return LamaPon::BillboardMode::ScreenAligned;
    }

    const char* BillboardFacingAxisToString(
        const LamaPon::BillboardFacingAxis axis)
    {
        return axis == LamaPon::BillboardFacingAxis::Forward
            ? "Forward"
            : "Up";
    }

    LamaPon::BillboardFacingAxis
        BillboardFacingAxisFromString(
            const std::string& value)
    {
        return value == "Forward"
            ? LamaPon::BillboardFacingAxis::Forward
            : LamaPon::BillboardFacingAxis::Up;
    }

    LamaPon::ParticleEmitterShape
        ReadParticleShape(
            const std::string& name)
    {
        if (name == "Sphere")
        {
            return LamaPon::
                ParticleEmitterShape::Sphere;
        }
        if (name == "Box")
        {
            return LamaPon::
                ParticleEmitterShape::Box;
        }
        return LamaPon::
            ParticleEmitterShape::Cone;
    }

    void SerializeAssetReference(
        Json& result,
        const std::string_view field,
        const std::filesystem::path& path,
        const LamaPon::AssetDatabase& database)
    {
        const std::string fieldName(field);
        result[fieldName] =
            LamaPon::PathToUtf8(path);
        if (path.empty())
        {
            return;
        }
        const auto guid =
            database.GuidForPath(path);
        if (!guid.empty())
        {
            result[fieldName + "Guid"] = guid;
        }
    }

    // PBRマップ（粗さ・金属度・遮蔽・発光）と付随する値を書き出します。
    // MeshRendererとModelRendererで同じ形なので共通化しています。
    void SerializePbrMapReferences(
        Json& result,
        const LamaPon::LitMaterial& material,
        const LamaPon::AssetDatabase& database)
    {
        SerializeAssetReference(
            result,
            "roughnessTexture",
            material.RoughnessTexture(),
            database);
        SerializeAssetReference(
            result,
            "metallicTexture",
            material.MetallicTexture(),
            database);
        SerializeAssetReference(
            result,
            "occlusionTexture",
            material.OcclusionTexture(),
            database);
        SerializeAssetReference(
            result,
            "emissiveTexture",
            material.EmissiveTexture(),
            database);
        result["occlusionStrength"] =
            material.OcclusionStrength();
        const auto& emissive = material.EmissiveColor();
        result["emissiveColor"] = Json::array({
            emissive.x,
            emissive.y,
            emissive.z
        });
    }

    std::filesystem::path ReadAssetReference(
        const Json& value,
        const std::string_view field,
        const LamaPon::AssetDatabase& database)
    {
        const std::string fieldName(field);
        const auto fallback = LamaPon::PathFromUtf8(
            value.value(
                fieldName,
                std::string{}));
        const auto guid = value.value(
            fieldName + "Guid",
            std::string{});
        return !guid.empty()
            ? database.ResolveGuid(
                guid,
                fallback)
            : fallback;
    }

    // SerializePbrMapReferencesと対になる読み込み。未指定なら未設定の
    // まま（発光は黒＝発光なし）にするので、旧シーンもそのまま開けます。
    // MeshRendererとModelRendererで同じAPI名なのでテンプレートです。
    template<typename Component>
    void ReadPbrMapReferences(
        const Json& value,
        Component& component,
        const LamaPon::AssetDatabase& database)
    {
        component.SetRoughnessTexturePath(
            ReadAssetReference(
                value,
                "roughnessTexture",
                database));
        component.SetMetallicTexturePath(
            ReadAssetReference(
                value,
                "metallicTexture",
                database));
        component.SetOcclusionTexturePath(
            ReadAssetReference(
                value,
                "occlusionTexture",
                database));
        component.SetEmissiveTexturePath(
            ReadAssetReference(
                value,
                "emissiveTexture",
                database));
        component.SetOcclusionStrength(
            value.value("occlusionStrength", 1.0f));
        if (const auto found = value.find("emissiveColor");
            found != value.end()
            && found->is_array()
            && found->size() == 3)
        {
            component.SetEmissiveColor({
                found->at(0).get<float>(),
                found->at(1).get<float>(),
                found->at(2).get<float>()
            });
        }
    }

    Json SerializeComponent(
        const LamaPon::Component& component,
        const LamaPon::AssetDatabase& database)
    {
        Json result{
            { "type", std::string(component.TypeName()) },
            { "enabled", component.IsEnabled() }
        };

        if (const auto* camera = dynamic_cast<const LamaPon::CameraComponent*>(&component))
        {
            result["verticalFieldOfView"] = camera->VerticalFieldOfView();
            result["nearPlane"] = camera->NearPlane();
            result["farPlane"] = camera->FarPlane();
            result["targetTexture"] = camera->TargetTexture();
            result["targetTextureWidth"] =
                camera->TargetTextureWidth();
            result["targetTextureHeight"] =
                camera->TargetTextureHeight();
            result["targetClearColor"] =
                ToJson(camera->TargetClearColor());
        }
        else if (const auto* directionalLight =
            dynamic_cast<
                const LamaPon::DirectionalLightComponent*>(
                &component))
        {
            result["color"] = ToJson(directionalLight->Color());
            result["intensity"] = directionalLight->Intensity();
            result["castsShadows"] =
                directionalLight->CastsShadows();
            result["shadowDistance"] =
                directionalLight->ShadowDistance();
            result["shadowBias"] =
                directionalLight->ShadowBias();
            result["shadowNormalBias"] =
                directionalLight->ShadowNormalBias();
            result["shadowStrength"] =
                directionalLight->ShadowStrength();
            result["shadowCascadeCount"] =
                directionalLight->ShadowCascadeCount();
            result["shadowSplitLambda"] =
                directionalLight->ShadowSplitLambda();
            result["angularDiameterDegrees"] =
                directionalLight->AngularDiameterDegrees();
        }
        else if (const auto* pointLight =
            dynamic_cast<const LamaPon::PointLightComponent*>(
                &component))
        {
            result["color"] = ToJson(pointLight->Color());
            result["intensity"] = pointLight->Intensity();
            result["range"] = pointLight->Range();
            result["castsShadows"] =
                pointLight->CastsShadows();
            result["shadowBias"] =
                pointLight->ShadowBias();
            result["shadowStrength"] =
                pointLight->ShadowStrength();
        }
        else if (const auto* spotLight =
            dynamic_cast<const LamaPon::SpotLightComponent*>(
                &component))
        {
            result["color"] = ToJson(spotLight->Color());
            result["intensity"] = spotLight->Intensity();
            result["range"] = spotLight->Range();
            result["innerConeAngle"] =
                spotLight->InnerConeAngle();
            result["outerConeAngle"] =
                spotLight->OuterConeAngle();
            result["castsShadows"] =
                spotLight->CastsShadows();
            result["shadowBias"] =
                spotLight->ShadowBias();
            result["shadowNormalBias"] =
                spotLight->ShadowNormalBias();
            result["shadowStrength"] =
                spotLight->ShadowStrength();
        }
        else if (const auto* light2D =
            dynamic_cast<
                const LamaPon::Light2DComponent*>(
                    &component))
        {
            result["color"] = ToJson(light2D->Color());
            result["intensity"] = light2D->Intensity();
            result["radius"] = light2D->Radius();
            result["affectsUI"] = light2D->AffectsUI();
        }
        else if (const auto* collider2D =
            dynamic_cast<const LamaPon::BoxCollider2DComponent*>(&component))
        {
            result["size"] = ToJson(collider2D->Size());
            result["offset"] = ToJson(collider2D->Offset());
            result["trigger"] = collider2D->IsTrigger();
            result["layer"] = collider2D->Layer();
            result["mask"] = collider2D->CollisionMask();
            result["friction"] = collider2D->Material().friction;
            result["restitution"] = collider2D->Material().restitution;
            SerializeMaterialCombine(
                result,
                collider2D->Material());
        }
        else if (const auto* circle2D =
            dynamic_cast<
                const LamaPon::CircleCollider2DComponent*>(
                    &component))
        {
            result["radius"] = circle2D->Radius();
            result["offset"] =
                ToJson(circle2D->Offset());
            result["trigger"] = circle2D->IsTrigger();
            result["layer"] = circle2D->Layer();
            result["mask"] = circle2D->CollisionMask();
            result["friction"] =
                circle2D->Material().friction;
            result["restitution"] =
                circle2D->Material().restitution;
            SerializeMaterialCombine(
                result,
                circle2D->Material());
        }
        else if (const auto* polygon2D =
            dynamic_cast<
                const LamaPon::PolygonCollider2DComponent*>(
                    &component))
        {
            auto vertices = Json::array();
            for (const auto& vertex : polygon2D->Vertices())
            {
                vertices.push_back(ToJson(vertex));
            }
            result["vertices"] = std::move(vertices);
            result["offset"] = ToJson(polygon2D->Offset());
            result["trigger"] = polygon2D->IsTrigger();
            result["layer"] = polygon2D->Layer();
            result["mask"] = polygon2D->CollisionMask();
            result["friction"] =
                polygon2D->Material().friction;
            result["restitution"] =
                polygon2D->Material().restitution;
            SerializeMaterialCombine(
                result,
                polygon2D->Material());
        }
        else if (const auto* collider3D =
            dynamic_cast<const LamaPon::BoxCollider3DComponent*>(&component))
        {
            result["size"] = ToJson(collider3D->Size());
            result["offset"] = ToJson(collider3D->Offset());
            result["trigger"] = collider3D->IsTrigger();
            result["layer"] = collider3D->Layer();
            result["mask"] = collider3D->CollisionMask();
            result["friction"] = collider3D->Material().friction;
            result["restitution"] = collider3D->Material().restitution;
            SerializeMaterialCombine(
                result,
                collider3D->Material());
        }
        else if (const auto* capsule =
            dynamic_cast<
                const LamaPon::CapsuleCollider3DComponent*>(
                    &component))
        {
            result["radius"] = capsule->Radius();
            result["height"] = capsule->Height();
            result["offset"] = ToJson(capsule->Offset());
            result["trigger"] = capsule->IsTrigger();
            result["layer"] = capsule->Layer();
            result["mask"] = capsule->CollisionMask();
            result["friction"] = capsule->Material().friction;
            result["restitution"] = capsule->Material().restitution;
            SerializeMaterialCombine(
                result,
                capsule->Material());
        }
        else if (const auto* sphere =
            dynamic_cast<
                const LamaPon::SphereCollider3DComponent*>(
                    &component))
        {
            result["radius"] = sphere->Radius();
            result["offset"] = ToJson(sphere->Offset());
            result["trigger"] = sphere->IsTrigger();
            result["layer"] = sphere->Layer();
            result["mask"] = sphere->CollisionMask();
            result["friction"] =
                sphere->Material().friction;
            result["restitution"] =
                sphere->Material().restitution;
            SerializeMaterialCombine(
                result,
                sphere->Material());
        }
        else if (const auto* hull =
            dynamic_cast<
                const LamaPon::ConvexHullCollider3DComponent*>(
                    &component))
        {
            auto points = Json::array();
            for (const auto& point : hull->Points())
            {
                points.push_back(ToJson(point));
            }
            result["points"] = std::move(points);
            result["offset"] = ToJson(hull->Offset());
            result["trigger"] = hull->IsTrigger();
            result["layer"] = hull->Layer();
            result["mask"] = hull->CollisionMask();
            result["friction"] =
                hull->Material().friction;
            result["restitution"] =
                hull->Material().restitution;
            SerializeMaterialCombine(
                result,
                hull->Material());
        }
        else if (const auto* meshCollider =
            dynamic_cast<
                const LamaPon::MeshCollider3DComponent*>(
                    &component))
        {
            SerializeAssetReference(
                result,
                "model",
                meshCollider->ModelPath(),
                database);
            result["offset"] =
                ToJson(meshCollider->Offset());
            result["trigger"] =
                meshCollider->IsTrigger();
            result["layer"] = meshCollider->Layer();
            result["mask"] =
                meshCollider->CollisionMask();
            result["friction"] =
                meshCollider->Material().friction;
            result["restitution"] =
                meshCollider->Material().restitution;
            SerializeMaterialCombine(
                result,
                meshCollider->Material());
        }
        else if (const auto* mesh = dynamic_cast<const LamaPon::MeshRendererComponent*>(&component))
        {
            result["shape"] = ShapeName(mesh->Shape());
            result["color"] = ToJson(mesh->Color());
            SerializeAssetReference(
                result,
                "albedoTexture",
                mesh->AlbedoTexturePath(),
                database);
            SerializeAssetReference(
                result,
                "normalTexture",
                mesh->NormalTexturePath(),
                database);
            SerializePbrMapReferences(
                result,
                mesh->Material(),
                database);
            // カスタムShaderの追加テクスチャ（t7以降）。GUID付きで
            // 保存するため、移動・改名しても参照が追従します。
            for (std::size_t customIndex = 0;
                customIndex
                    < LamaPon::LitMaterial::CustomTextureCount;
                ++customIndex)
            {
                SerializeAssetReference(
                    result,
                    "customTexture"
                        + std::to_string(customIndex),
                    mesh->Material().CustomTexture(
                        customIndex),
                    database);
            }
            result["roughness"] = mesh->Roughness();
            result["normalStrength"] =
                mesh->NormalStrength();
            result["metallic"] = mesh->Metallic();
            result["worldOverlay"] = mesh->IsWorldOverlay();
            SerializeAssetReference(
                result,
                "shader",
                mesh->ShaderPath(),
                database);
            // バリアントのキーワード（#pragma multi_compile）。
            result["shaderKeywords"] = Json::array();
            for (const auto& keyword :
                mesh->ShaderKeywords().Keywords())
            {
                result["shaderKeywords"].push_back(keyword);
            }
            result["customParameters"] = Json::array();
            for (std::size_t index = 0;
                index < LamaPon::LitMaterial::CustomParameterCount;
                ++index)
            {
                result["customParameters"].push_back(
                    ToJson(mesh->CustomParameter(index)));
            }
            SerializeAssetReference(
                result,
                "materialAsset",
                mesh->MaterialAssetPath(),
                database);
        }
        else if (const auto* sprite = dynamic_cast<const LamaPon::SpriteRendererComponent*>(&component))
        {
            result["size"] = ToJson(sprite->Size());
            result["color"] = ToJson(sprite->Color());
            result["pivot"] = ToJson(sprite->Pivot());
            result["sortOrder"] = sprite->SortOrder();
            result["renderTexture"] = sprite->RenderTexture();
            result["maskInteraction"] =
                static_cast<int>(sprite->MaskInteraction());
            // 既定（全体表示）以外のときだけ保存します。
            const auto& sourceRect = sprite->SourceRect();
            if (sourceRect.x != 0.0f
                || sourceRect.y != 0.0f
                || sourceRect.z != 1.0f
                || sourceRect.w != 1.0f)
            {
                result["sourceRect"] = ToJson(sourceRect);
            }
            SerializeAssetReference(
                result,
                "texture",
                sprite->TexturePath(),
                database);
            SerializeAssetReference(
                result,
                "shader",
                sprite->ShaderPath(),
                database);
            result["customParameters"] = Json::array();
            for (std::size_t index = 0;
                index
                    < LamaPon::SpriteRendererComponent::
                        CustomParameterCount;
                ++index)
            {
                result["customParameters"].push_back(
                    ToJson(
                        sprite->CustomParameter(index)));
            }
        }
        else if (const auto* spriteMask =
            dynamic_cast<
                const LamaPon::SpriteMaskComponent*>(
                    &component))
        {
            result["shape"] =
                static_cast<int>(spriteMask->Shape());
            result["size"] = ToJson(spriteMask->Size());
        }
        else if (const auto* renderCulling =
            dynamic_cast<
                const LamaPon::RenderCullingComponent*>(
                    &component))
        {
            result["alwaysVisible"] =
                renderCulling->AlwaysVisible();
            result["cullingMargin"] =
                renderCulling->CullingMargin();
        }
        else if (const auto* reflectionProbe =
            dynamic_cast<
                const LamaPon::ReflectionProbeComponent*>(
                    &component))
        {
            // ベイク結果は保存しません（読み込み時に焼き直します）。
            result["range"] = reflectionProbe->Range();
            result["intensity"] =
                reflectionProbe->Intensity();
            result["boxExtents"] =
                ToJson(reflectionProbe->BoxExtents());
            result["blendDistance"] =
                reflectionProbe->BlendDistance();
        }
        else if (const auto* spriteAnimator =
            dynamic_cast<
                const LamaPon::SpriteAnimatorComponent*>(
                    &component))
        {
            result["columns"] = spriteAnimator->Columns();
            result["rows"] = spriteAnimator->Rows();
            result["speed"] = spriteAnimator->Speed();
            result["playOnStart"] =
                spriteAnimator->PlayOnStart();
            result["defaultClip"] =
                spriteAnimator->DefaultClip();
            auto clips = nlohmann::json::array();
            for (const auto& clip :
                spriteAnimator->Clips())
            {
                clips.push_back({
                    { "name", clip.name },
                    { "startFrame", clip.startFrame },
                    { "frameCount", clip.frameCount },
                    {
                        "framesPerSecond",
                        clip.framesPerSecond
                    },
                    { "loop", clip.loop }
                });
            }
            result["clips"] = std::move(clips);
        }
        else if (const auto* canvas =
            dynamic_cast<
                const LamaPon::UICanvasComponent*>(
                    &component))
        {
            result["referenceResolution"] =
                ToJson(
                    canvas->ReferenceResolution());
            result["matchWidthOrHeight"] =
                canvas->MatchWidthOrHeight();
        }
        else if (const auto* uiTransform =
            dynamic_cast<
                const LamaPon::
                    UIRectTransformComponent*>(
                        &component))
        {
            result["anchorMin"] =
                ToJson(uiTransform->AnchorMin());
            result["anchorMax"] =
                ToJson(uiTransform->AnchorMax());
            result["pivot"] =
                ToJson(uiTransform->Pivot());
            result["anchoredPosition"] =
                ToJson(
                    uiTransform->
                        AnchoredPosition());
            result["sizeDelta"] =
                ToJson(uiTransform->SizeDelta());
        }
        else if (const auto* button =
            dynamic_cast<
                const LamaPon::UIButtonComponent*>(
                    &component))
        {
            result["label"] = button->Label();
            result["fontFamily"] =
                button->FontFamily();
            result["fontSize"] =
                button->FontSize();
            result["fallbackSize"] =
                ToJson(button->FallbackSize());
            result["normalColor"] =
                ToJson(button->NormalColor());
            result["hoveredColor"] =
                ToJson(button->HoveredColor());
            result["pressedColor"] =
                ToJson(button->PressedColor());
            result["disabledColor"] =
                ToJson(button->DisabledColor());
            result["textColor"] =
                ToJson(button->TextColor());
            result["interactable"] =
                button->Interactable();
            result["circularHitArea"] =
                button->CircularHitArea();
            result["reloadCurrentScene"] =
                button->ReloadCurrentScene();
            result["loadTargetAdditive"] =
                button->LoadTargetAdditive();
            result["clickEvent"] =
                button->ClickEventName();
            result["sortOrder"] =
                button->SortOrder();
            SerializeAssetReference(
                result,
                "texture",
                button->TexturePath(),
                database);
            SerializeAssetReference(
                result,
                "targetScene",
                button->TargetScene(),
                database);
        }
        else if (const auto* image =
            dynamic_cast<
                const LamaPon::UIImageComponent*>(
                    &component))
        {
            result["color"] = ToJson(image->Color());
            result["border"] = ToJson(image->Border());
            result["fallbackSize"] =
                ToJson(image->FallbackSize());
            result["sortOrder"] = image->SortOrder();
            result["renderTexture"] = image->RenderTexture();
            SerializeAssetReference(
                result,
                "texture",
                image->TexturePath(),
                database);
        }
        else if (const auto* toggle =
            dynamic_cast<
                const LamaPon::UIToggleComponent*>(
                    &component))
        {
            result["label"] = toggle->Label();
            result["isOn"] = toggle->IsOn();
            result["fontFamily"] =
                toggle->FontFamily();
            result["fontSize"] = toggle->FontSize();
            result["interactable"] =
                toggle->Interactable();
            result["boxColor"] =
                ToJson(toggle->BoxColor());
            result["checkColor"] =
                ToJson(toggle->CheckColor());
            result["textColor"] =
                ToJson(toggle->TextColor());
            result["fallbackSize"] =
                ToJson(toggle->FallbackSize());
            result["sortOrder"] = toggle->SortOrder();
        }
        else if (const auto* slider =
            dynamic_cast<
                const LamaPon::UISliderComponent*>(
                    &component))
        {
            result["minValue"] =
                slider->MinimumValue();
            result["maxValue"] =
                slider->MaximumValue();
            result["value"] = slider->Value();
            result["wholeNumbers"] =
                slider->WholeNumbers();
            result["interactable"] =
                slider->Interactable();
            result["backgroundColor"] =
                ToJson(slider->BackgroundColor());
            result["fillColor"] =
                ToJson(slider->FillColor());
            result["handleColor"] =
                ToJson(slider->HandleColor());
            result["fallbackSize"] =
                ToJson(slider->FallbackSize());
            result["sortOrder"] = slider->SortOrder();
        }
        else if (const auto* inputField =
            dynamic_cast<
                const LamaPon::UIInputFieldComponent*>(
                    &component))
        {
            result["text"] = inputField->Text();
            result["placeholder"] =
                inputField->Placeholder();
            result["fontFamily"] =
                inputField->FontFamily();
            result["fontSize"] =
                inputField->FontSize();
            result["maxLength"] =
                inputField->MaxLength();
            result["interactable"] =
                inputField->Interactable();
            result["backgroundColor"] =
                ToJson(inputField->BackgroundColor());
            result["focusedColor"] =
                ToJson(inputField->FocusedColor());
            result["textColor"] =
                ToJson(inputField->TextColor());
            result["placeholderColor"] =
                ToJson(
                    inputField->PlaceholderColor());
            result["fallbackSize"] =
                ToJson(inputField->FallbackSize());
            result["sortOrder"] =
                inputField->SortOrder();
        }
        else if (const auto* layoutGroup =
            dynamic_cast<
                const LamaPon::UILayoutGroupComponent*>(
                    &component))
        {
            result["axis"] =
                layoutGroup->Axis()
                    == LamaPon::UILayoutAxis::Horizontal
                    ? "horizontal"
                    : "vertical";
            result["spacing"] =
                layoutGroup->Spacing();
            result["padding"] =
                ToJson(layoutGroup->Padding());
            result["childAlignment"] =
                static_cast<int>(
                    layoutGroup->ChildAlignment());
        }
        else if (const auto* scrollView =
            dynamic_cast<
                const LamaPon::UIScrollViewComponent*>(
                    &component))
        {
            result["scrollSpeed"] =
                scrollView->ScrollSpeed();
            result["interactable"] =
                scrollView->Interactable();
            result["backgroundColor"] =
                ToJson(scrollView->BackgroundColor());
            result["scrollbarColor"] =
                ToJson(scrollView->ScrollbarColor());
            result["sortOrder"] =
                scrollView->SortOrder();
        }
        else if (const auto* navMesh =
            dynamic_cast<
                const LamaPon::NavMeshComponent*>(
                    &component))
        {
            result["surfaceSize"] =
                ToJson(navMesh->SurfaceSize());
            result["cellSize"] =
                navMesh->CellSize();
            result["agentRadius"] =
                navMesh->AgentRadius();
            result["agentHeight"] =
                navMesh->AgentHeight();
            result["blockedCells"] =
                Json::array();
            if (navMesh->IsBaked())
            {
                for (std::uint32_t z{};
                    z < navMesh->GridDepth();
                    ++z)
                {
                    for (std::uint32_t x{};
                        x < navMesh->GridWidth();
                        ++x)
                    {
                        if (navMesh->IsBlocked(
                                x,
                                z))
                        {
                            result[
                                "blockedCells"].
                                    push_back({
                                        x,
                                        z
                                    });
                        }
                    }
                }
            }
            result["baked"] =
                navMesh->IsBaked();
        }
        else if (const auto* agent =
            dynamic_cast<
                const LamaPon::
                    NavMeshAgentComponent*>(
                        &component))
        {
            result["speed"] = agent->Speed();
            result["stoppingDistance"] =
                agent->StoppingDistance();
            result["rotateToPath"] =
                agent->RotateToPath();
            result["destination"] =
                ToJson(agent->Destination());
            result["path"] = Json::array();
            for (const auto& point :
                agent->Path())
            {
                result["path"].push_back(
                    ToJson(point));
            }
        }
        else if (const auto* tilemap =
            dynamic_cast<
                const LamaPon::TilemapComponent*>(
                    &component))
        {
            result["tileSize"] =
                ToJson(tilemap->TileSize());
            result["atlasColumns"] =
                tilemap->AtlasColumns();
            result["atlasRows"] =
                tilemap->AtlasRows();
            result["color"] =
                ToJson(tilemap->Color());
            result["sortOrder"] = tilemap->SortOrder();
            SerializeAssetReference(
                result,
                "texture",
                tilemap->TexturePath(),
                database);
            result["cells"] = Json::array();
            for (const auto& [coordinate, tileIndex] :
                tilemap->Cells())
            {
                result["cells"].push_back({
                    { "x", coordinate.first },
                    { "y", coordinate.second },
                    { "tile", tileIndex }
                });
            }
        }
        else if (const auto* parallax =
            dynamic_cast<
                const LamaPon::ParallaxLayerComponent*>(
                    &component))
        {
            result["factor"] = ToJson(parallax->Factor());
            result["referenceId"] =
                parallax->ReferenceId();
        }
        else if (const auto* audio =
            dynamic_cast<const LamaPon::AudioSourceComponent*>(&component))
        {
            SerializeAssetReference(
                result,
                "audio",
                audio->AudioPath(),
                database);
            result["volume"] = audio->Volume();
            result["pitch"] = audio->Pitch();
            result["pan"] = audio->Pan();
            result["loop"] = audio->Loop();
            result["playOnStart"] = audio->PlayOnStart();
            result["spatial"] = audio->IsSpatial();
            result["minimumDistance"] =
                audio->MinimumDistance();
            result["maximumDistance"] =
                audio->MaximumDistance();
            result["streaming"] = audio->IsStreaming();
            result["bus"] = static_cast<int>(
                audio->Bus());
        }
        else if (dynamic_cast<
            const LamaPon::AudioListenerComponent*>(
                &component) != nullptr)
        {
        }
        else if (const auto* model = dynamic_cast<const LamaPon::ModelRendererComponent*>(&component))
        {
            SerializeAssetReference(
                result,
                "model",
                model->ModelPath(),
                database);
            result["wireframe"] = model->IsWireframe();
            result["materialOverride"] =
                model->IsMaterialOverrideEnabled();
            result["useLegacyShading"] =
                model->UsesLegacyShading();
            result["preserveEmbeddedMaterialColor"] =
                model->PreserveEmbeddedMaterialColor();
            result["color"] = ToJson(model->Color());
            SerializeAssetReference(
                result,
                "albedoTexture",
                model->AlbedoTexturePath(),
                database);
            SerializeAssetReference(
                result,
                "normalTexture",
                model->NormalTexturePath(),
                database);
            SerializePbrMapReferences(
                result,
                model->Material(),
                database);
            // カスタムShaderの追加テクスチャ（t7以降）。GUID付きで
            // 保存するため、移動・改名しても参照が追従します。
            for (std::size_t customIndex = 0;
                customIndex
                    < LamaPon::LitMaterial::CustomTextureCount;
                ++customIndex)
            {
                SerializeAssetReference(
                    result,
                    "customTexture"
                        + std::to_string(customIndex),
                    model->Material().CustomTexture(
                        customIndex),
                    database);
            }
            result["roughness"] = model->Roughness();
            result["normalStrength"] =
                model->NormalStrength();
            result["metallic"] = model->Metallic();
            SerializeAssetReference(
                result,
                "shader",
                model->ShaderPath(),
                database);
            result["shaderKeywords"] = Json::array();
            for (const auto& keyword :
                model->ShaderKeywords().Keywords())
            {
                result["shaderKeywords"].push_back(keyword);
            }
            result["customParameters"] = Json::array();
            for (std::size_t index = 0;
                index < LamaPon::LitMaterial::CustomParameterCount;
                ++index)
            {
                result["customParameters"].push_back(
                    ToJson(model->CustomParameter(index)));
            }
            result["animationIndex"] =
                model->AnimationIndex();
            result["animationSpeed"] =
                model->AnimationSpeed();
            result["animationLoop"] =
                model->AnimationLoop();
            result["animationPlayOnStart"] =
                model->AnimationPlayOnStart();
            SerializeAssetReference(
                result,
                "animationController",
                model->AnimationControllerPath(),
                database);
            result["applyRootMotion"] =
                model->ApplyRootMotion();
            result["rootMotionNode"] =
                model->RootMotionNode();
            SerializeAssetReference(
                result,
                "materialAsset",
                model->MaterialAssetPath(),
                database);
        }
        else if (const auto* text = dynamic_cast<const LamaPon::TextRendererComponent*>(&component))
        {
            result["text"] = text->Text();
            result["fontFamily"] = text->FontFamily();
            result["fontSize"] = text->FontSize();
            result["color"] = ToJson(text->Color());
            result["layoutSize"] = ToJson(text->LayoutSize());
            result["wordWrap"] = text->WordWrap();
            result["horizontalAlignment"] =
                TextHorizontalAlignmentName(text->HorizontalAlignment());
            result["verticalAlignment"] =
                TextVerticalAlignmentName(text->VerticalAlignment());
            result["sortOrder"] = text->SortOrder();
        }
        else if (const auto* particles =
            dynamic_cast<
                const LamaPon::
                    ParticleSystemComponent*>(
                        &component))
        {
            result["maxParticles"] =
                particles->MaxParticles();
            result["emissionRate"] =
                particles->EmissionRate();
            result["lifetime"] =
                ToJson(particles->Lifetime());
            result["startSpeed"] =
                ToJson(particles->StartSpeed());
            result["startSize"] =
                ToJson(particles->StartSize());
            result["endSizeMultiplier"] =
                particles->
                    EndSizeMultiplier();
            result["startColor"] =
                ToJson(
                    particles->StartColor());
            result["endColor"] =
                ToJson(
                    particles->EndColor());
            result["gravity"] =
                ToJson(particles->Gravity());
            result["shape"] =
                ParticleShapeName(
                    particles->
                        EmitterShape());
            result["renderMode"] =
                ParticleRenderModeName(
                    particles->RenderMode());
            result["emitterSize"] =
                ToJson(
                    particles->EmitterSize());
            result["coneAngle"] =
                particles->ConeAngle();
            result["duration"] =
                particles->Duration();
            result["looping"] =
                particles->Looping();
            result["playOnStart"] =
                particles->PlayOnStart();
            result["previewInEditor"] =
                particles->
                    PreviewInEditor();
            result["additive"] =
                particles->Additive();
            SerializeAssetReference(
                result,
                "texture",
                particles->TexturePath(),
                database);
            SerializeAssetReference(
                result,
                "shader",
                particles->ShaderPath(),
                database);
            SerializeAssetReference(
                result,
                "auxiliaryTexture",
                particles->AuxiliaryTexturePath(),
                database);
            result["customParameters"] = Json::array();
            for (std::size_t index = 0;
                index
                    < LamaPon::ParticleSystemComponent::
                        CustomParameterCount;
                ++index)
            {
                result["customParameters"].push_back(
                    ToJson(
                        particles->CustomParameter(index)));
            }
        }
        else if (const auto* particles2D =
            dynamic_cast<
                const LamaPon::SpriteParticles2DComponent*>(
                    &component))
        {
            result["maxParticles"] =
                particles2D->MaxParticles();
            result["lifetime"] =
                ToJson(particles2D->Lifetime());
            result["startSpeed"] =
                ToJson(particles2D->StartSpeed());
            result["startSize"] =
                ToJson(particles2D->StartSize());
            result["sizeGrowth"] =
                particles2D->SizeGrowth();
            result["gravity"] =
                ToJson(particles2D->Gravity());
            result["drag"] = particles2D->Drag();
            result["startColor"] =
                ToJson(particles2D->StartColor());
            result["endColor"] =
                ToJson(particles2D->EndColor());
            result["sortOrder"] =
                particles2D->SortOrder();
            SerializeAssetReference(
                result,
                "texture",
                particles2D->TexturePath(),
                database);
        }
        else if (const auto* rotator = dynamic_cast<const LamaPon::RotatorComponent*>(&component))
        {
            result["angularVelocity"] = ToJson(rotator->AngularVelocity());
        }
        else if (const auto* billboard =
            dynamic_cast<
                const LamaPon::BillboardComponent*>(
                    &component))
        {
            result["mode"] = BillboardModeToString(
                billboard->Mode());
            result["facingAxis"] =
                BillboardFacingAxisToString(
                    billboard->FacingAxis());
            result["targetPosition"] =
                ToJson(billboard->TargetPosition());
        }
        else if (const auto* animator =
            dynamic_cast<
                const LamaPon::TransformAnimatorComponent*>(
                    &component))
        {
            SerializeAssetReference(
                result,
                "clip",
                animator->ClipPath(),
                database);
            result["speed"] = animator->Speed();
            result["loop"] = animator->Loop();
            result["playOnStart"] =
                animator->PlayOnStart();
            SerializeAssetReference(
                result,
                "controller",
                animator->ControllerPath(),
                database);
        }
        else if (const auto* inputMover =
            dynamic_cast<
                const LamaPon::InputMoverComponent*>(
                    &component))
        {
            result["horizontalAction"] =
                inputMover->HorizontalAction();
            result["verticalAction"] =
                inputMover->VerticalAction();
            result["speed"] = inputMover->Speed();
        }
        else if (const auto* controller =
            dynamic_cast<
                const LamaPon::CharacterControllerComponent*>(
                    &component))
        {
            result["radius"] = controller->Radius();
            result["height"] = controller->Height();
            result["moveSpeed"] = controller->MoveSpeed();
            result["gravity"] = controller->Gravity();
            result["jumpSpeed"] = controller->JumpSpeed();
            result["stepOffset"] = controller->StepOffset();
            result["skinWidth"] = controller->SkinWidth();
            result["layer"] = controller->Layer();
            result["collisionMask"] = controller->CollisionMask();
            result["useInput"] = controller->UseInput();
            result["horizontalAction"] = controller->HorizontalAction();
            result["verticalAction"] = controller->VerticalAction();
            result["jumpAction"] = controller->JumpAction();
        }
        else if (const auto* nativeScript =
            dynamic_cast<
                const LamaPon::NativeScriptComponent*>(
                    &component))
        {
            result["script"] =
                nativeScript->ScriptType();
            result["properties"] = Json::parse(
                nativeScript->SerializedProperties());
        }
        else if (const auto* rigidbody = dynamic_cast<const LamaPon::RigidbodyComponent*>(&component))
        {
            result["velocity"] = ToJson(rigidbody->Velocity());
            result["angularVelocity"] =
                ToJson(rigidbody->AngularVelocity());
            result["centerOfMass"] =
                ToJson(rigidbody->CenterOfMass());
            result["mass"] = rigidbody->Mass();
            result["linearDrag"] = rigidbody->LinearDrag();
            result["angularDrag"] = rigidbody->AngularDrag();
            result["constraints"] = {
                {
                    "freezeRotationX",
                    rigidbody->Constraints().freezeRotationX
                },
                {
                    "freezeRotationY",
                    rigidbody->Constraints().freezeRotationY
                },
                {
                    "freezeRotationZ",
                    rigidbody->Constraints().freezeRotationZ
                },
                {
                    "freezePositionX",
                    rigidbody->Constraints().freezePositionX
                },
                {
                    "freezePositionY",
                    rigidbody->Constraints().freezePositionY
                },
                {
                    "freezePositionZ",
                    rigidbody->Constraints().freezePositionZ
                }
            };
            result["useGravity"] = rigidbody->UsesGravity();
            result["kinematic"] = rigidbody->IsKinematic();
            result["interpolate"] =
                rigidbody->Interpolates();
            result["collisionDetection"] =
                rigidbody->CollisionDetection()
                    == LamaPon::CollisionDetectionMode::Continuous
                ? "continuous"
                : "discrete";
        }
        else if (const auto* joint =
            dynamic_cast<const LamaPon::JointComponent*>(&component))
        {
            const char* jointType = "fixed";
            if (joint->Type() == LamaPon::JointType::Hinge)
            {
                jointType = "hinge";
            }
            else if (joint->Type() == LamaPon::JointType::Spring)
            {
                jointType = "spring";
            }
            result["jointType"] = jointType;
            result["connectedBodyId"] = joint->ConnectedBodyId();
            result["anchor"] = ToJson(joint->Anchor());
            result["connectedAnchor"] =
                ToJson(joint->ConnectedAnchor());
            result["axis"] = ToJson(joint->Axis());
            result["restLength"] = joint->RestLength();
            result["stiffness"] = joint->Stiffness();
            result["damping"] = joint->Damping();
            result["useLimits"] = joint->UseLimits();
            result["limitMinimum"] =
                joint->Limits().minimumAngleDegrees;
            result["limitMaximum"] =
                joint->Limits().maximumAngleDegrees;
            result["useMotor"] = joint->UseMotor();
            result["motorTargetVelocity"] =
                joint->Motor().targetVelocityDegrees;
            result["motorMaximumTorque"] =
                joint->Motor().maximumTorque;
            result["collideConnected"] =
                joint->CollideConnected();
        }
        else if (const auto* lodGroup =
            dynamic_cast<
                const LamaPon::LODGroupComponent*>(
                    &component))
        {
            result["cullDistance"] =
                lodGroup->CullDistance();
            result["levels"] = Json::array();
            for (const auto& level :
                lodGroup->Levels())
            {
                result["levels"].push_back({
                    {
                        "maximumDistance",
                        level.maximumDistance
                    },
                    {
                        "targetId",
                        level.targetId
                    }
                });
            }
        }
        else
        {
            result["serializable"] = false;
        }

        return result;
    }

    LamaPon::Component& DeserializeComponent(
        LamaPon::GameObject& gameObject,
        const Json& value,
        const LamaPon::AssetDatabase& database)
    {
        const auto type = value.at("type").get<std::string>();
        LamaPon::Component* component{};

        if (type == "Camera")
        {
            auto& camera =
                gameObject.AddComponent<LamaPon::CameraComponent>(
                    value.value("verticalFieldOfView", DirectX::XM_PIDIV4),
                    value.value("nearPlane", 0.1f),
                    value.value("farPlane", 1000.0f));
            camera.SetTargetTexture(
                value.value(
                    "targetTexture",
                    std::string{}));
            camera.SetTargetTextureSize(
                value.value(
                    "targetTextureWidth",
                    512u),
                value.value(
                    "targetTextureHeight",
                    512u));
            if (value.contains("targetClearColor"))
            {
                camera.SetTargetClearColor(
                    ReadFloat4(
                        value.at("targetClearColor")));
            }
            component = &camera;
        }
        else if (type == "AudioListener")
        {
            component = &gameObject.AddComponent<
                LamaPon::AudioListenerComponent>();
        }
        else if (type == "DirectionalLight")
        {
            auto& directionalLight = gameObject.AddComponent<
                LamaPon::DirectionalLightComponent>(
                value.contains("color")
                    ? ReadFloat3(value.at("color"))
                    : DirectX::XMFLOAT3{
                        1.0f,
                        0.96f,
                        0.88f
                    },
                value.value("intensity", 1.0f),
                value.value("castsShadows", true),
                value.value("shadowDistance", 24.0f),
                value.value("shadowBias", 0.0015f),
                value.value(
                    "shadowNormalBias",
                    0.0025f),
                value.value("shadowStrength", 0.85f),
                value.value("shadowCascadeCount", 4u),
                value.value("shadowSplitLambda", 0.65f));
            // 角度サイズはコンストラクター引数を増やさずに足します
            // （公開シグネチャを変えると、既存のGame Moduleの
            // AddComponent<DirectionalLightComponent>(...)が
            // そのままでは通らなくなるため）。
            directionalLight.SetAngularDiameterDegrees(
                value.value("angularDiameterDegrees", 0.53f));
            component = &directionalLight;
        }
        else if (type == "PointLight")
        {
            component = &gameObject.AddComponent<
                LamaPon::PointLightComponent>(
                value.contains("color")
                    ? ReadFloat3(value.at("color"))
                    : DirectX::XMFLOAT3{
                        1.0f,
                        0.72f,
                        0.42f
                    },
                value.value("intensity", 3.0f),
                value.value("range", 8.0f));
            auto* pointLight =
                static_cast<LamaPon::PointLightComponent*>(
                    component);
            pointLight->SetCastsShadows(
                value.value("castsShadows", false));
            pointLight->SetShadowBias(
                value.value("shadowBias", 0.002f));
            pointLight->SetShadowStrength(
                value.value("shadowStrength", 0.9f));
        }
        else if (type == "SpotLight")
        {
            component = &gameObject.AddComponent<
                LamaPon::SpotLightComponent>(
                value.contains("color")
                    ? ReadFloat3(value.at("color"))
                    : DirectX::XMFLOAT3{
                        1.0f,
                        0.88f,
                        0.68f
                    },
                value.value("intensity", 5.0f),
                value.value("range", 12.0f),
                value.value(
                    "innerConeAngle",
                    DirectX::XMConvertToRadians(22.5f)),
                value.value(
                    "outerConeAngle",
                    DirectX::XMConvertToRadians(35.0f)));
            auto* spotLight =
                static_cast<LamaPon::SpotLightComponent*>(
                    component);
            spotLight->SetCastsShadows(
                value.value("castsShadows", false));
            spotLight->SetShadowBias(
                value.value("shadowBias", 0.002f));
            spotLight->SetShadowNormalBias(
                value.value("shadowNormalBias", 0.01f));
            spotLight->SetShadowStrength(
                value.value("shadowStrength", 0.9f));
        }
        else if (type == "Light2D")
        {
            auto& light2D = gameObject.AddComponent<
                LamaPon::Light2DComponent>(
                value.contains("color")
                    ? ReadFloat3(value.at("color"))
                    : DirectX::XMFLOAT3{
                        1.0f,
                        0.9f,
                        0.7f
                    },
                value.value("intensity", 1.0f),
                value.value("radius", 150.0f));
            // コンストラクター引数は増やしません（既存のGame Moduleの
            // AddComponent<Light2DComponent>(...)を通らなくしないため）。
            light2D.SetAffectsUI(
                value.value("affectsUI", false));
            component = &light2D;
        }
        else if (type == "BoxCollider2D")
        {
            component = &gameObject.AddComponent<LamaPon::BoxCollider2DComponent>(
                value.contains("size")
                    ? ReadFloat2(value.at("size"))
                    : DirectX::XMFLOAT2{ 1.0f, 1.0f },
                value.contains("offset")
                    ? ReadFloat2(value.at("offset"))
                    : DirectX::XMFLOAT2{ 0.0f, 0.0f },
                value.value("trigger", false),
                value.value("layer", 0u),
                value.value("mask", 0xffffffffu),
                ReadPhysicsMaterial(value));
        }
        else if (type == "CircleCollider2D")
        {
            component = &gameObject.AddComponent<
                LamaPon::CircleCollider2DComponent>(
                    value.value("radius", 0.5f),
                    value.contains("offset")
                        ? ReadFloat2(value.at("offset"))
                        : DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "PolygonCollider2D")
        {
            std::vector<DirectX::XMFLOAT2> vertices;
            if (value.contains("vertices"))
            {
                for (const auto& vertex : value.at("vertices"))
                {
                    vertices.push_back(ReadFloat2(vertex));
                }
            }
            component = &gameObject.AddComponent<
                LamaPon::PolygonCollider2DComponent>(
                    std::move(vertices),
                    value.contains("offset")
                        ? ReadFloat2(value.at("offset"))
                        : DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "BoxCollider3D")
        {
            component = &gameObject.AddComponent<LamaPon::BoxCollider3DComponent>(
                value.contains("size")
                    ? ReadFloat3(value.at("size"))
                    : DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                value.contains("offset")
                    ? ReadFloat3(value.at("offset"))
                    : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
                value.value("trigger", false),
                value.value("layer", 0u),
                value.value("mask", 0xffffffffu),
                ReadPhysicsMaterial(value));
        }
        else if (type == "CapsuleCollider3D")
        {
            component = &gameObject.AddComponent<
                LamaPon::CapsuleCollider3DComponent>(
                    value.value("radius", 0.5f),
                    value.value("height", 2.0f),
                    value.contains("offset")
                        ? ReadFloat3(value.at("offset"))
                        : DirectX::XMFLOAT3{},
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "SphereCollider3D")
        {
            component = &gameObject.AddComponent<
                LamaPon::SphereCollider3DComponent>(
                    value.value("radius", 0.5f),
                    value.contains("offset")
                        ? ReadFloat3(value.at("offset"))
                        : DirectX::XMFLOAT3{},
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "ConvexHullCollider3D")
        {
            std::vector<DirectX::XMFLOAT3> points;
            if (value.contains("points"))
            {
                for (const auto& point : value.at("points"))
                {
                    points.push_back(ReadFloat3(point));
                }
            }
            component = &gameObject.AddComponent<
                LamaPon::ConvexHullCollider3DComponent>(
                    std::move(points),
                    value.contains("offset")
                        ? ReadFloat3(value.at("offset"))
                        : DirectX::XMFLOAT3{},
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "MeshCollider3D")
        {
            component = &gameObject.AddComponent<
                LamaPon::MeshCollider3DComponent>(
                    ReadAssetReference(
                        value,
                        "model",
                        database),
                    value.contains("offset")
                        ? ReadFloat3(value.at("offset"))
                        : DirectX::XMFLOAT3{},
                    value.value("trigger", false),
                    value.value("layer", 0u),
                    value.value("mask", 0xffffffffu),
                    ReadPhysicsMaterial(value));
        }
        else if (type == "MeshRenderer")
        {
            auto& mesh = gameObject.AddComponent<LamaPon::MeshRendererComponent>(
                ReadShape(value.value("shape", std::string("Cube"))),
                value.contains("color")
                    ? ReadFloat4(value.at("color"))
                    : DirectX::XMFLOAT4{
                        1.0f,
                        1.0f,
                        1.0f,
                        1.0f
                    },
                ReadAssetReference(
                    value,
                    "albedoTexture",
                    database),
                ReadAssetReference(
                    value,
                    "normalTexture",
                    database),
                value.value("roughness", 0.5f),
                value.value("normalStrength", 1.0f),
                ReadAssetReference(
                    value,
                    "materialAsset",
                    database));
            mesh.SetMetallic(
                value.value("metallic", 0.0f));
            ReadPbrMapReferences(value, mesh, database);
            mesh.SetWorldOverlay(
                value.value("worldOverlay", false));
            mesh.SetShaderPath(ReadAssetReference(
                value,
                "shader",
                database));
            if (const auto keywords =
                    value.find("shaderKeywords");
                keywords != value.end()
                && keywords->is_array())
            {
                std::vector<std::string> enabled;
                for (const auto& keyword : *keywords)
                {
                    if (keyword.is_string())
                    {
                        enabled.push_back(
                            keyword.get<std::string>());
                    }
                }
                mesh.SetShaderKeywords(
                    LamaPon::ShaderKeywordSet{
                        std::move(enabled) });
            }
            for (std::size_t customIndex = 0;
                customIndex
                    < LamaPon::LitMaterial::CustomTextureCount;
                ++customIndex)
            {
                mesh.SetCustomTexturePath(
                    customIndex,
                    ReadAssetReference(
                        value,
                        "customTexture"
                            + std::to_string(customIndex),
                        database));
            }
            if (const auto found = value.find("customParameters");
                found != value.end() && found->is_array())
            {
                const auto count = std::min(
                    found->size(),
                    LamaPon::LitMaterial::CustomParameterCount);
                for (std::size_t index = 0; index < count; ++index)
                {
                    mesh.SetCustomParameter(
                        index,
                        ReadFloat4(found->at(index)));
                }
            }
            component = &mesh;
        }
        else if (type == "SpriteRenderer")
        {
            auto& sprite =
                gameObject.AddComponent<LamaPon::SpriteRendererComponent>(
                value.contains("size")
                    ? ReadFloat2(value.at("size"))
                    : DirectX::XMFLOAT2{ 128.0f, 128.0f },
                value.contains("color")
                    ? ReadFloat4(value.at("color"))
                    : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                ReadAssetReference(
                    value,
                    "texture",
                    database));
            sprite.SetSortOrder(
                value.value("sortOrder", 0));
            sprite.SetRenderTexture(
                value.value(
                    "renderTexture",
                    std::string{}));
            // 省略時は左上のまま（既存のシーンは動きません）。
            if (value.contains("pivot"))
            {
                sprite.SetPivot(
                    ReadFloat2(value.at("pivot")));
            }
            if (value.contains("sourceRect"))
            {
                sprite.SetSourceRect(
                    ReadFloat4(value.at("sourceRect")));
            }
            sprite.SetShaderPath(
                ReadAssetReference(
                    value,
                    "shader",
                    database));
            if (const auto found =
                    value.find("customParameters");
                found != value.end()
                && found->is_array())
            {
                const auto count = std::min(
                    found->size(),
                    LamaPon::SpriteRendererComponent::
                        CustomParameterCount);
                for (std::size_t index = 0;
                    index < count;
                    ++index)
                {
                    sprite.SetCustomParameter(
                        index,
                        ReadFloat4(found->at(index)));
                }
            }
            sprite.SetMaskInteraction(
                static_cast<LamaPon::SpriteMaskInteraction>(
                    value.value(
                        "maskInteraction",
                        static_cast<int>(
                            LamaPon::SpriteMaskInteraction::
                                None))));
            component = &sprite;
        }
        else if (type == "SpriteMask")
        {
            auto& spriteMask = gameObject.AddComponent<
                LamaPon::SpriteMaskComponent>(
                static_cast<LamaPon::SpriteMaskShape>(
                    value.value(
                        "shape",
                        static_cast<int>(
                            LamaPon::SpriteMaskShape::
                                Rectangle))),
                value.contains("size")
                    ? ReadFloat2(value.at("size"))
                    : DirectX::XMFLOAT2{ 128.0f, 128.0f });
            component = &spriteMask;
        }
        else if (type == "RenderCulling")
        {
            auto& renderCulling =
                gameObject.AddComponent<
                    LamaPon::RenderCullingComponent>(
                    value.value("alwaysVisible", false),
                    value.value("cullingMargin", 0.0f));
            component = &renderCulling;
        }
        else if (type == "ReflectionProbe")
        {
            auto& reflectionProbe =
                gameObject.AddComponent<
                    LamaPon::ReflectionProbeComponent>(
                    value.value("range", 10.0f),
                    value.value("intensity", 1.0f));
            if (value.contains("boxExtents"))
            {
                reflectionProbe.SetBoxExtents(
                    ReadFloat3(value.at("boxExtents")));
            }
            reflectionProbe.SetBlendDistance(
                value.value("blendDistance", 0.0f));
            // シーン由来の印。最初の自動ベイクを、ディスクの環境
            // キャッシュからの復元で置き換えてよいのはこの印がある
            // プローブだけです。
            reflectionProbe.MarkLoadedFromScene();
            component = &reflectionProbe;
        }
        else if (type == "SpriteAnimator")
        {
            auto& animator = gameObject.AddComponent<
                LamaPon::SpriteAnimatorComponent>(
                value.value("columns", 1),
                value.value("rows", 1));
            animator.SetSpeed(
                value.value("speed", 1.0f));
            animator.SetPlayOnStart(
                value.value("playOnStart", true));
            animator.SetDefaultClip(
                value.value(
                    "defaultClip",
                    std::string{}));
            if (const auto clips = value.find("clips");
                clips != value.end()
                && clips->is_array())
            {
                for (const auto& clipValue : *clips)
                {
                    LamaPon::SpriteAnimationClip clip;
                    clip.name = clipValue.value(
                        "name",
                        std::string{});
                    clip.startFrame = clipValue.value(
                        "startFrame",
                        0);
                    clip.frameCount = clipValue.value(
                        "frameCount",
                        1);
                    clip.framesPerSecond =
                        clipValue.value(
                            "framesPerSecond",
                            10.0f);
                    clip.loop = clipValue.value(
                        "loop",
                        true);
                    animator.AddClip(std::move(clip));
                }
            }
            component = &animator;
        }
        else if (type == "UICanvas")
        {
            component =
                &gameObject.AddComponent<
                    LamaPon::UICanvasComponent>(
                        value.contains(
                            "referenceResolution")
                            ? ReadFloat2(
                                value.at(
                                    "referenceResolution"))
                            : DirectX::XMFLOAT2{
                                1280.0f,
                                720.0f
                            },
                        value.value(
                            "matchWidthOrHeight",
                            0.5f));
        }
        else if (type == "UIRectTransform")
        {
            component =
                &gameObject.AddComponent<
                    LamaPon::
                        UIRectTransformComponent>(
                            value.contains(
                                "anchorMin")
                                ? ReadFloat2(
                                    value.at(
                                        "anchorMin"))
                                : DirectX::XMFLOAT2{
                                    0.5f,
                                    0.5f
                                },
                            value.contains(
                                "anchorMax")
                                ? ReadFloat2(
                                    value.at(
                                        "anchorMax"))
                                : DirectX::XMFLOAT2{
                                    0.5f,
                                    0.5f
                                },
                            value.contains("pivot")
                                ? ReadFloat2(
                                    value.at("pivot"))
                                : DirectX::XMFLOAT2{
                                    0.5f,
                                    0.5f
                                },
                            value.contains(
                                "anchoredPosition")
                                ? ReadFloat2(
                                    value.at(
                                        "anchoredPosition"))
                                : DirectX::XMFLOAT2{},
                            value.contains(
                                "sizeDelta")
                                ? ReadFloat2(
                                    value.at(
                                        "sizeDelta"))
                                : DirectX::XMFLOAT2{
                                    220.0f,
                                    56.0f
                                });
        }
        else if (type == "UIButton")
        {
            auto& button =
                gameObject.AddComponent<
                    LamaPon::UIButtonComponent>(
                        value.value(
                            "label",
                            std::string{
                                "ボタン" }),
                        value.contains(
                            "fallbackSize")
                            ? ReadFloat2(
                                value.at(
                                    "fallbackSize"))
                            : DirectX::XMFLOAT2{
                                220.0f,
                                56.0f
                            },
                        ReadAssetReference(
                            value,
                            "texture",
                            database));
            button.SetFontFamily(
                value.value(
                    "fontFamily",
                    std::string{
                        "Yu Gothic UI" }));
            button.SetFontSize(
                value.value(
                    "fontSize",
                    24.0f));
            if (value.contains("normalColor"))
            {
                button.SetNormalColor(
                    ReadFloat4(
                        value.at(
                            "normalColor")));
            }
            if (value.contains("hoveredColor"))
            {
                button.SetHoveredColor(
                    ReadFloat4(
                        value.at(
                            "hoveredColor")));
            }
            if (value.contains("pressedColor"))
            {
                button.SetPressedColor(
                    ReadFloat4(
                        value.at(
                            "pressedColor")));
            }
            if (value.contains("disabledColor"))
            {
                button.SetDisabledColor(
                    ReadFloat4(
                        value.at(
                            "disabledColor")));
            }
            if (value.contains("textColor"))
            {
                button.SetTextColor(
                    ReadFloat4(
                        value.at(
                            "textColor")));
            }
            button.SetInteractable(
                value.value(
                    "interactable",
                    true));
            button.SetCircularHitArea(
                value.value(
                    "circularHitArea",
                    false));
            button.SetReloadCurrentScene(
                value.value(
                    "reloadCurrentScene",
                    false));
            button.SetLoadTargetAdditive(
                value.value(
                    "loadTargetAdditive",
                    false));
            button.SetClickEventName(
                value.value(
                    "clickEvent",
                    std::string{}));
            button.SetSortOrder(
                value.value("sortOrder", 0));
            button.SetTargetScene(
                ReadAssetReference(
                    value,
                    "targetScene",
                    database));
            component = &button;
        }
        else if (type == "UIImage")
        {
            auto& image =
                gameObject.AddComponent<
                    LamaPon::UIImageComponent>(
                        ReadAssetReference(
                            value,
                            "texture",
                            database),
                        value.contains("color")
                            ? ReadFloat4(
                                value.at("color"))
                            : DirectX::XMFLOAT4{
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f });
            image.SetRenderTexture(
                value.value(
                    "renderTexture",
                    std::string{}));
            if (value.contains("border"))
            {
                image.SetBorder(
                    ReadFloat4(value.at("border")));
            }
            if (value.contains("fallbackSize"))
            {
                image.SetFallbackSize(
                    ReadFloat2(
                        value.at("fallbackSize")));
            }
            image.SetSortOrder(
                value.value("sortOrder", 0));
            component = &image;
        }
        else if (type == "UIToggle")
        {
            auto& toggle =
                gameObject.AddComponent<
                    LamaPon::UIToggleComponent>(
                        value.value(
                            "label",
                            std::string{ "トグル" }),
                        value.value("isOn", false));
            toggle.SetFontFamily(
                value.value(
                    "fontFamily",
                    std::string{ "Yu Gothic UI" }));
            toggle.SetFontSize(
                value.value("fontSize", 24.0f));
            toggle.SetInteractable(
                value.value("interactable", true));
            if (value.contains("boxColor"))
            {
                toggle.SetBoxColor(
                    ReadFloat4(value.at("boxColor")));
            }
            if (value.contains("checkColor"))
            {
                toggle.SetCheckColor(
                    ReadFloat4(
                        value.at("checkColor")));
            }
            if (value.contains("textColor"))
            {
                toggle.SetTextColor(
                    ReadFloat4(
                        value.at("textColor")));
            }
            if (value.contains("fallbackSize"))
            {
                toggle.SetFallbackSize(
                    ReadFloat2(
                        value.at("fallbackSize")));
            }
            toggle.SetSortOrder(
                value.value("sortOrder", 0));
            // 読み込み直後は「変更あり」にしない
            static_cast<void>(
                toggle.ConsumeValueChanged());
            component = &toggle;
        }
        else if (type == "UISlider")
        {
            auto& slider =
                gameObject.AddComponent<
                    LamaPon::UISliderComponent>(
                        value.value("minValue", 0.0f),
                        value.value("maxValue", 1.0f),
                        value.value("value", 0.5f));
            slider.SetWholeNumbers(
                value.value("wholeNumbers", false));
            slider.SetInteractable(
                value.value("interactable", true));
            if (value.contains("backgroundColor"))
            {
                slider.SetBackgroundColor(
                    ReadFloat4(
                        value.at("backgroundColor")));
            }
            if (value.contains("fillColor"))
            {
                slider.SetFillColor(
                    ReadFloat4(
                        value.at("fillColor")));
            }
            if (value.contains("handleColor"))
            {
                slider.SetHandleColor(
                    ReadFloat4(
                        value.at("handleColor")));
            }
            if (value.contains("fallbackSize"))
            {
                slider.SetFallbackSize(
                    ReadFloat2(
                        value.at("fallbackSize")));
            }
            slider.SetSortOrder(
                value.value("sortOrder", 0));
            static_cast<void>(
                slider.ConsumeValueChanged());
            component = &slider;
        }
        else if (type == "UIInputField")
        {
            auto& inputField =
                gameObject.AddComponent<
                    LamaPon::UIInputFieldComponent>(
                        value.value(
                            "text",
                            std::string{}),
                        value.value(
                            "placeholder",
                            std::string{
                                "テキストを入力..." }));
            inputField.SetFontFamily(
                value.value(
                    "fontFamily",
                    std::string{ "Yu Gothic UI" }));
            inputField.SetFontSize(
                value.value("fontSize", 24.0f));
            inputField.SetMaxLength(
                value.value(
                    "maxLength",
                    static_cast<std::size_t>(256)));
            inputField.SetInteractable(
                value.value("interactable", true));
            if (value.contains("backgroundColor"))
            {
                inputField.SetBackgroundColor(
                    ReadFloat4(
                        value.at("backgroundColor")));
            }
            if (value.contains("focusedColor"))
            {
                inputField.SetFocusedColor(
                    ReadFloat4(
                        value.at("focusedColor")));
            }
            if (value.contains("textColor"))
            {
                inputField.SetTextColor(
                    ReadFloat4(
                        value.at("textColor")));
            }
            if (value.contains("placeholderColor"))
            {
                inputField.SetPlaceholderColor(
                    ReadFloat4(
                        value.at(
                            "placeholderColor")));
            }
            if (value.contains("fallbackSize"))
            {
                inputField.SetFallbackSize(
                    ReadFloat2(
                        value.at("fallbackSize")));
            }
            inputField.SetSortOrder(
                value.value("sortOrder", 0));
            static_cast<void>(
                inputField.ConsumeValueChanged());
            component = &inputField;
        }
        else if (type == "UILayoutGroup")
        {
            auto& layoutGroup =
                gameObject.AddComponent<
                    LamaPon::UILayoutGroupComponent>(
                        value.value(
                            "axis",
                            std::string{ "vertical" })
                            == "horizontal"
                            ? LamaPon::UILayoutAxis::
                                Horizontal
                            : LamaPon::UILayoutAxis::
                                Vertical,
                        value.value("spacing", 8.0f));
            if (value.contains("padding"))
            {
                layoutGroup.SetPadding(
                    ReadFloat4(value.at("padding")));
            }
            const int alignment = std::clamp(
                value.value("childAlignment", 0),
                0,
                2);
            layoutGroup.SetChildAlignment(
                static_cast<LamaPon::UILayoutAlignment>(
                    alignment));
            component = &layoutGroup;
        }
        else if (type == "UIScrollView")
        {
            auto& scrollView =
                gameObject.AddComponent<
                    LamaPon::UIScrollViewComponent>();
            scrollView.SetScrollSpeed(
                value.value("scrollSpeed", 48.0f));
            scrollView.SetInteractable(
                value.value("interactable", true));
            if (value.contains("backgroundColor"))
            {
                scrollView.SetBackgroundColor(
                    ReadFloat4(
                        value.at("backgroundColor")));
            }
            if (value.contains("scrollbarColor"))
            {
                scrollView.SetScrollbarColor(
                    ReadFloat4(
                        value.at("scrollbarColor")));
            }
            scrollView.SetSortOrder(
                value.value("sortOrder", 0));
            component = &scrollView;
        }
        else if (type == "NavMesh")
        {
            auto& navMesh =
                gameObject.AddComponent<
                    LamaPon::NavMeshComponent>(
                        value.contains(
                            "surfaceSize")
                            ? ReadFloat2(
                                value.at(
                                    "surfaceSize"))
                            : DirectX::XMFLOAT2{
                                20.0f,
                                20.0f
                            },
                        value.value(
                            "cellSize",
                            1.0f),
                        value.value(
                            "agentRadius",
                            0.4f),
                        value.value(
                            "agentHeight",
                            1.8f));
            if (value.value("baked", false))
            {
                std::vector<
                    LamaPon::
                        NavMeshComponent::
                            CellCoordinate>
                    blockedCells;
                if (value.contains(
                        "blockedCells")
                    && value.at(
                        "blockedCells").
                            is_array())
                {
                    for (const auto& cell :
                        value.at(
                            "blockedCells"))
                    {
                        if (cell.is_array()
                            && cell.size() == 2)
                        {
                            blockedCells.
                                emplace_back(
                                    cell.at(0).
                                        get<
                                            std::uint32_t>(),
                                    cell.at(1).
                                        get<
                                            std::uint32_t>());
                        }
                    }
                }
                navMesh.RestoreBake(
                    blockedCells);
            }
            component = &navMesh;
        }
        else if (type == "NavMeshAgent")
        {
            auto& agent =
                gameObject.AddComponent<
                    LamaPon::
                        NavMeshAgentComponent>(
                            value.value(
                                "speed",
                                3.0f),
                            value.value(
                                "stoppingDistance",
                                0.1f),
                            value.value(
                                "rotateToPath",
                                true));
            std::vector<
                DirectX::XMFLOAT3> path;
            if (value.contains("path")
                && value.at("path").
                    is_array())
            {
                for (const auto& point :
                    value.at("path"))
                {
                    path.push_back(
                        ReadFloat3(point));
                }
            }
            agent.SetPath(
                value.contains(
                    "destination")
                    ? ReadFloat3(
                        value.at(
                            "destination"))
                    : DirectX::XMFLOAT3{},
                path);
            component = &agent;
        }
        else if (type == "Tilemap")
        {
            auto& tilemap =
                gameObject.AddComponent<
                    LamaPon::TilemapComponent>(
                        value.contains("tileSize")
                            ? ReadFloat2(
                                value.at(
                                    "tileSize"))
                            : DirectX::XMFLOAT2{
                                32.0f,
                                32.0f
                            },
                        value.value(
                            "atlasColumns",
                            1u),
                        value.value(
                            "atlasRows",
                            1u),
                        value.contains("color")
                            ? ReadFloat4(
                                value.at("color"))
                            : DirectX::XMFLOAT4{
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f
                            },
                        ReadAssetReference(
                            value,
                            "texture",
                            database));
            if (value.contains("cells")
                && value.at("cells").is_array())
            {
                for (const auto& cell :
                    value.at("cells"))
                {
                    const auto tileIndex =
                        cell.value("tile", 0u);
                    if (tileIndex
                        < tilemap.TileCount())
                    {
                        tilemap.SetCell(
                            cell.value("x", 0),
                            cell.value("y", 0),
                            tileIndex);
                    }
                }
            }
            tilemap.SetSortOrder(
                value.value("sortOrder", 0));
            component = &tilemap;
        }
        else if (type == "ParallaxLayer")
        {
            auto& parallax = gameObject.AddComponent<
                LamaPon::ParallaxLayerComponent>(
                value.contains("factor")
                    ? ReadFloat2(value.at("factor"))
                    : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                value.value(
                    "referenceId",
                    LamaPon::GameObjectId{}));
            component = &parallax;
        }
        else if (type == "AudioSource")
        {
            component = &gameObject.AddComponent<LamaPon::AudioSourceComponent>(
                ReadAssetReference(
                    value,
                    "audio",
                    database),
                value.value("volume", 1.0f),
                value.value("pitch", 0.0f),
                value.value("pan", 0.0f),
                value.value("loop", false),
                value.value("playOnStart", false),
                value.value("spatial", false),
                value.value("minimumDistance", 1.0f),
                value.value("maximumDistance", 20.0f));
            auto* audioSource =
                static_cast<LamaPon::AudioSourceComponent*>(
                    component);
            audioSource->SetBus(
                static_cast<LamaPon::AudioBus>(
                    std::clamp(
                        value.value("bus", 0),
                        0,
                        static_cast<int>(
                            LamaPon::AudioBus::Count)
                            - 1)));
            audioSource->SetStreaming(
                value.value("streaming", false));
        }
        else if (type == "ModelRenderer")
        {
            auto& model = gameObject.AddComponent<LamaPon::ModelRendererComponent>(
                ReadAssetReference(
                    value,
                    "model",
                    database),
                value.value("wireframe", false),
                value.value("materialOverride", false),
                value.contains("color")
                    ? ReadFloat4(value.at("color"))
                    : DirectX::XMFLOAT4{
                        1.0f,
                        1.0f,
                        1.0f,
                        1.0f
                    },
                ReadAssetReference(
                    value,
                    "albedoTexture",
                    database),
                ReadAssetReference(
                    value,
                    "normalTexture",
                    database),
                value.value("roughness", 0.5f),
                value.value("normalStrength", 1.0f),
                ReadAssetReference(
                    value,
                    "materialAsset",
                    database),
                value.value(
                    "animationIndex",
                    std::size_t{}),
                value.value("animationSpeed", 1.0f),
                value.value("animationLoop", true),
                value.value(
                    "animationPlayOnStart",
                    true),
                ReadAssetReference(
                    value,
                    "animationController",
                    database),
                value.value(
                    "applyRootMotion",
                    false),
                value.value(
                    "rootMotionNode",
                    std::string{}),
                value.value(
                    "preserveEmbeddedMaterialColor",
                    false));
            model.SetMetallic(
                value.value("metallic", 0.0f));
            ReadPbrMapReferences(value, model, database);
            // 未指定は既定のLamaPon Lit（PBR）。旧シーンも
            // そのまま新しい描画で開けます。
            model.SetUseLegacyShading(
                value.value("useLegacyShading", false));
            model.SetShaderPath(ReadAssetReference(
                value,
                "shader",
                database));
            if (const auto keywords =
                    value.find("shaderKeywords");
                keywords != value.end()
                && keywords->is_array())
            {
                std::vector<std::string> enabled;
                for (const auto& keyword : *keywords)
                {
                    if (keyword.is_string())
                    {
                        enabled.push_back(
                            keyword.get<std::string>());
                    }
                }
                model.SetShaderKeywords(
                    LamaPon::ShaderKeywordSet{
                        std::move(enabled) });
            }
            for (std::size_t customIndex = 0;
                customIndex
                    < LamaPon::LitMaterial::CustomTextureCount;
                ++customIndex)
            {
                model.SetCustomTexturePath(
                    customIndex,
                    ReadAssetReference(
                        value,
                        "customTexture"
                            + std::to_string(customIndex),
                        database));
            }
            if (const auto found = value.find("customParameters");
                found != value.end() && found->is_array())
            {
                const auto count = std::min(
                    found->size(),
                    LamaPon::LitMaterial::CustomParameterCount);
                for (std::size_t index = 0; index < count; ++index)
                {
                    model.SetCustomParameter(
                        index,
                        ReadFloat4(found->at(index)));
                }
            }
            component = &model;
        }
        else if (type == "TextRenderer")
        {
            auto& text =
                gameObject.AddComponent<LamaPon::TextRendererComponent>(
                value.value("text", std::string("日本語テキスト")),
                value.value("fontFamily", std::string("Yu Gothic UI")),
                value.value("fontSize", 32.0f),
                value.contains("color")
                    ? ReadFloat4(value.at("color"))
                    : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                value.contains("layoutSize")
                    ? ReadFloat2(value.at("layoutSize"))
                    : DirectX::XMFLOAT2{ 0.0f, 0.0f },
                value.value("wordWrap", false),
                ReadTextHorizontalAlignment(
                    value.value(
                        "horizontalAlignment",
                        std::string("Left"))),
                ReadTextVerticalAlignment(
                    value.value(
                        "verticalAlignment",
                        std::string("Top"))));
            text.SetSortOrder(
                value.value("sortOrder", 0));
            component = &text;
        }
        else if (type == "ParticleSystem")
        {
            auto& particles =
                gameObject.AddComponent<
                    LamaPon::
                        ParticleSystemComponent>(
                            value.value(
                                "maxParticles",
                                512u),
                            value.value(
                                "emissionRate",
                                36.0f),
                            value.contains(
                                "lifetime")
                                ? ReadFloat2(
                                    value.at(
                                        "lifetime"))
                                : DirectX::XMFLOAT2{
                                    0.8f,
                                    1.8f
                                },
                            value.contains(
                                "startSpeed")
                                ? ReadFloat2(
                                    value.at(
                                        "startSpeed"))
                                : DirectX::XMFLOAT2{
                                    1.0f,
                                    3.0f
                                },
                            value.contains(
                                "startSize")
                                ? ReadFloat2(
                                    value.at(
                                        "startSize"))
                                : DirectX::XMFLOAT2{
                                    0.08f,
                                    0.22f
                                },
                            value.contains(
                                "startColor")
                                ? ReadFloat4(
                                    value.at(
                                        "startColor"))
                                : DirectX::XMFLOAT4{
                                    0.20f,
                                    0.72f,
                                    1.0f,
                                    1.0f
                                },
                            value.contains(
                                "endColor")
                                ? ReadFloat4(
                                    value.at(
                                        "endColor"))
                                : DirectX::XMFLOAT4{
                                    0.04f,
                                    0.18f,
                                    0.55f,
                                    0.0f
                                },
                            ReadParticleShape(
                                value.value(
                                    "shape",
                                    std::string{
                                        "Cone" })),
                            ReadAssetReference(
                                value,
                                "texture",
                                database));
            particles.SetEndSizeMultiplier(
                value.value(
                    "endSizeMultiplier",
                    0.15f));
            particles.SetRenderMode(
                ReadParticleRenderMode(
                    value.value(
                        "renderMode",
                        std::string{
                            "Billboard" })));
            if (value.contains("gravity"))
            {
                particles.SetGravity(
                    ReadFloat3(
                        value.at("gravity")));
            }
            if (value.contains(
                    "emitterSize"))
            {
                particles.SetEmitterSize(
                    ReadFloat3(
                        value.at(
                            "emitterSize")));
            }
            particles.SetConeAngle(
                value.value(
                    "coneAngle",
                    DirectX::
                        XMConvertToRadians(
                            25.0f)));
            particles.SetDuration(
                value.value(
                    "duration",
                    5.0f));
            particles.SetLooping(
                value.value(
                    "looping",
                    true));
            particles.SetPlayOnStart(
                value.value(
                    "playOnStart",
                    true));
            particles.SetPreviewInEditor(
                value.value(
                    "previewInEditor",
                    true));
            particles.SetAdditive(
                value.value(
                    "additive",
                    true));
            particles.SetShaderPath(
                ReadAssetReference(
                    value,
                    "shader",
                    database));
            particles.SetAuxiliaryTexturePath(
                ReadAssetReference(
                    value,
                    "auxiliaryTexture",
                    database));
            if (const auto found =
                value.find("customParameters");
                found != value.end()
                && found->is_array())
            {
                const std::size_t count =
                    std::min(
                        found->size(),
                        LamaPon::
                            ParticleSystemComponent::
                                CustomParameterCount);
                for (std::size_t index = 0;
                    index < count;
                    ++index)
                {
                    particles.SetCustomParameter(
                        index,
                        ReadFloat4(
                            found->at(index)));
                }
            }
            component = &particles;
        }
        else if (type == "SpriteParticles2D")
        {
            auto& particles =
                gameObject.AddComponent<
                    LamaPon::SpriteParticles2DComponent>(
                        value.value("maxParticles", 128u),
                        value.contains("lifetime")
                            ? ReadFloat2(value.at("lifetime"))
                            : DirectX::XMFLOAT2{ 0.25f, 0.65f },
                        value.contains("startSpeed")
                            ? ReadFloat2(value.at("startSpeed"))
                            : DirectX::XMFLOAT2{ 24.0f, 72.0f },
                        value.contains("startSize")
                            ? ReadFloat2(value.at("startSize"))
                            : DirectX::XMFLOAT2{ 4.0f, 12.0f },
                        value.contains("startColor")
                            ? ReadFloat4(value.at("startColor"))
                            : DirectX::XMFLOAT4{
                                1.0f, 0.85f, 0.35f, 1.0f },
                        value.contains("endColor")
                            ? ReadFloat4(value.at("endColor"))
                            : DirectX::XMFLOAT4{
                                1.0f, 0.25f, 0.05f, 0.0f });
            particles.SetSizeGrowth(
                value.value("sizeGrowth", -4.0f));
            if (value.contains("gravity"))
            {
                particles.SetGravity(
                    ReadFloat2(value.at("gravity")));
            }
            particles.SetDrag(value.value("drag", 0.8f));
            particles.SetSortOrder(
                value.value("sortOrder", 0));
            particles.SetTexturePath(
                ReadAssetReference(
                    value,
                    "texture",
                    database));
            component = &particles;
        }
        else if (type == "Billboard")
        {
            auto& billboard =
                gameObject.AddComponent<
                    LamaPon::BillboardComponent>(
                    BillboardModeFromString(
                        value.value(
                            "mode",
                            std::string{
                                "ScreenAligned" })),
                    BillboardFacingAxisFromString(
                        value.value(
                            "facingAxis",
                            std::string{ "Up" })));
            if (value.contains("targetPosition"))
            {
                billboard.SetTargetPosition(
                    ReadFloat3(
                        value.at("targetPosition")));
            }
            component = &billboard;
        }
        else if (type == "Rotator")
        {
            component = &gameObject.AddComponent<LamaPon::RotatorComponent>(
                value.contains("angularVelocity")
                    ? ReadFloat3(value.at("angularVelocity"))
                    : DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f });
        }
        else if (type == "TransformAnimator")
        {
            component = &gameObject.AddComponent<
                LamaPon::TransformAnimatorComponent>(
                    ReadAssetReference(
                        value,
                        "clip",
                        database),
                    value.value("speed", 1.0f),
                    value.value("loop", true),
                    value.value(
                        "playOnStart",
                        true),
                    ReadAssetReference(
                        value,
                        "controller",
                        database));
        }
        else if (type == "InputMover")
        {
            component = &gameObject.AddComponent<
                LamaPon::InputMoverComponent>(
                    value.value(
                        "horizontalAction",
                        std::string("MoveHorizontal")),
                    value.value(
                        "verticalAction",
                        std::string("MoveVertical")),
                    value.value("speed", 3.0f));
        }
        else if (type == "CharacterController")
        {
            auto& controller = gameObject.AddComponent<
                LamaPon::CharacterControllerComponent>(
                    value.value("radius", 0.4f),
                    value.value("height", 1.8f),
                    value.value("moveSpeed", 4.0f),
                    value.value("gravity", 20.0f),
                    value.value("jumpSpeed", 7.0f),
                    value.value("stepOffset", 0.3f),
                    value.value("skinWidth", 0.03f),
                    value.value("layer", 2u),
                    value.value("collisionMask", 0xffffffffu));
            controller.SetUseInput(
                value.value("useInput", true));
            controller.SetHorizontalAction(
                value.value(
                    "horizontalAction",
                    std::string("MoveHorizontal")));
            controller.SetVerticalAction(
                value.value(
                    "verticalAction",
                    std::string("MoveVertical")));
            controller.SetJumpAction(
                value.value(
                    "jumpAction",
                    std::string("Jump")));
            component = &controller;
        }
        else if (type == "Rigidbody")
        {
            const auto collisionDetection =
                value.value(
                    "collisionDetection",
                    std::string("discrete"))
                    == "continuous"
                ? LamaPon::CollisionDetectionMode::Continuous
                : LamaPon::CollisionDetectionMode::Discrete;
            LamaPon::RigidbodyConstraints constraints{};
            if (value.contains("constraints"))
            {
                const auto& serializedConstraints =
                    value.at("constraints");
                constraints.freezeRotationX =
                    serializedConstraints.value(
                        "freezeRotationX",
                        false);
                constraints.freezeRotationY =
                    serializedConstraints.value(
                        "freezeRotationY",
                        false);
                constraints.freezeRotationZ =
                    serializedConstraints.value(
                        "freezeRotationZ",
                        false);
                constraints.freezePositionX =
                    serializedConstraints.value(
                        "freezePositionX",
                        false);
                constraints.freezePositionY =
                    serializedConstraints.value(
                        "freezePositionY",
                        false);
                constraints.freezePositionZ =
                    serializedConstraints.value(
                        "freezePositionZ",
                        false);
            }
            component = &gameObject.AddComponent<LamaPon::RigidbodyComponent>(
                value.contains("velocity")
                    ? ReadFloat3(value.at("velocity"))
                    : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
                value.value("useGravity", true),
                value.value("kinematic", false),
                collisionDetection,
                value.value("mass", 1.0f),
                value.contains("angularVelocity")
                    ? ReadFloat3(
                        value.at("angularVelocity"))
                    : DirectX::XMFLOAT3{},
                value.contains("centerOfMass")
                    ? ReadFloat3(
                        value.at("centerOfMass"))
                    : DirectX::XMFLOAT3{},
                value.value("linearDrag", 0.0f),
                value.value("angularDrag", 0.05f),
                constraints,
                value.value("interpolate", true));
        }
        else if (type == "Joint")
        {
            const auto typeName =
                value.value(
                    "jointType",
                    std::string("fixed"));
            LamaPon::JointType jointType =
                LamaPon::JointType::Fixed;
            if (typeName == "hinge")
            {
                jointType = LamaPon::JointType::Hinge;
            }
            else if (typeName == "spring")
            {
                jointType = LamaPon::JointType::Spring;
            }
            component = &gameObject.AddComponent<
                LamaPon::JointComponent>(
                    jointType,
                    value.value(
                        "connectedBodyId",
                        std::uint64_t{}),
                    value.contains("anchor")
                        ? ReadFloat3(value.at("anchor"))
                        : DirectX::XMFLOAT3{},
                    value.contains("connectedAnchor")
                        ? ReadFloat3(
                            value.at("connectedAnchor"))
                        : DirectX::XMFLOAT3{},
                    value.contains("axis")
                        ? ReadFloat3(value.at("axis"))
                        : DirectX::XMFLOAT3{
                            0.0f,
                            1.0f,
                            0.0f },
                    value.value("restLength", 1.0f),
                    value.value("stiffness", 20.0f),
                    value.value("damping", 2.0f),
                    value.value(
                        "collideConnected",
                        false),
                    value.value("useLimits", false),
                    LamaPon::HingeLimits{
                        value.value(
                            "limitMinimum",
                            -90.0f),
                        value.value(
                            "limitMaximum",
                            90.0f)
                    },
                    value.value("useMotor", false),
                    LamaPon::HingeMotor{
                        value.value(
                            "motorTargetVelocity",
                            90.0f),
                        value.value(
                            "motorMaximumTorque",
                            10.0f)
                    });
        }
        else if (type == "LODGroup")
        {
            std::vector<LamaPon::LODLevel>
                levels;
            for (const auto& level :
                value.value(
                    "levels",
                    Json::array()))
            {
                levels.push_back({
                    level.value(
                        "maximumDistance",
                        25.0f),
                    level.value(
                        "targetId",
                        std::uint64_t{})
                });
            }
            component = &gameObject.AddComponent<
                LamaPon::LODGroupComponent>(
                    std::move(levels),
                    value.value(
                        "cullDistance",
                        200.0f));
        }
        else if (type == "NativeScript")
        {
            const auto properties =
                value.value(
                    "properties",
                    Json::object());
            if (!properties.is_object())
            {
                throw std::runtime_error(
                    "Native Script properties must be a JSON object.");
            }
            component = &gameObject.AddComponent<
                LamaPon::NativeScriptComponent>(
                    value.at("script").get<std::string>(),
                    properties.dump());
        }
        else
        {
            throw std::runtime_error("Unknown component type: " + type);
        }

        component->SetEnabled(value.value("enabled", true));
        return *component;
    }

    Json SerializeGameObject(
        const LamaPon::GameObject& gameObject,
        const LamaPon::GameObjectId id,
        const std::optional<LamaPon::GameObjectId> parentId,
        const bool includePrefabLink,
        const bool includePersistence,
        const LamaPon::AssetDatabase& database)
    {
        const auto& transform = gameObject.GetTransform();
        // alwaysVisible / cullingMargin はオブジェクト直下には
        // もう書きません（RenderCullingコンポーネントとして保存
        // されます）。読み込み側は古いシーンとの互換のため、
        // 両キーを今も受け付けてコンポーネントへ変換します。
        Json object{
            { "id", id },
            { "name", gameObject.Name() },
            { "enabled", gameObject.IsEnabled() },
            {
                "parent",
                parentId
                    ? Json(*parentId)
                    : Json(nullptr)
            },
            { "transform", {
                { "position", ToJson(transform.position) },
                // rotationは既存プロジェクトおよび外部ツールとの互換性のために
                // 保存します。rotationQuaternionを回転の正本とし、読み込み時も
                // rotationQuaternionを優先します。
                { "rotation",
                    ToJson(transform.EulerAngles()) },
                { "rotationQuaternion",
                    ToJson(transform.rotationQuaternion) },
                { "scale", ToJson(transform.scale) }
            } },
            { "components", Json::array() }
        };

        for (const auto& component : gameObject.Components())
        {
            const auto serialized =
                SerializeComponent(
                    *component,
                    database);
            if (serialized.value("serializable", true))
            {
                object["components"].push_back(
                    serialized);
            }
        }
        if (includePrefabLink
            && gameObject.IsPrefabInstanceRoot())
        {
            SerializeAssetReference(
                object,
                "prefabAsset",
                gameObject.PrefabAssetPath(),
                database);
        }
        if (includePersistence
            && gameObject.IsPersistent())
        {
            object["persistent"] = true;
            object["persistenceKey"] =
                gameObject.PersistenceKey();
        }
        // 既存シーンとの差分を抑えるため、Tagは設定時のみ保存します。
        if (!gameObject.Tag().empty())
        {
            object["tag"] = gameObject.Tag();
        }
        return object;
    }

    Json SerializeScene(
        const LamaPon::Scene& scene,
        const LamaPon::AssetDatabase& database)
    {
        // Main Cameraが追加シーン側のカメラだった場合は、主シーンの
        // ファイルに書けないためnullとして保存します。
        const auto* mainCamera = scene.MainCamera();
        const bool mainCameraIsSaved =
            mainCamera != nullptr
            && mainCamera->Owner().SourceScene()
                == LamaPon::Scene::PrimarySceneHandle();
        Json document{
            { "format", "LamaPonScene" },
            { "version", 1 },
            { "mainCamera", mainCameraIsSaved
                ? Json(mainCamera->Owner().Id())
                : Json(nullptr) },
            { "environment", {
                {
                    "ambientColor",
                    ToJson(scene.AmbientLightColor())
                },
                {
                    "ambientIntensity",
                    scene.AmbientLightIntensity()
                },
                { "sky", {
                    { "enabled", scene.Sky().enabled },
                    { "topColor", ToJson(scene.Sky().topColor) },
                    { "horizonColor", ToJson(scene.Sky().horizonColor) },
                    { "groundColor", ToJson(scene.Sky().groundColor) },
                    { "intensity", scene.Sky().intensity },
                    {
                        "cubemap",
                        LamaPon::PathToUtf8(
                            scene.Sky().cubemapPath)
                    },
                    {
                        "iblIntensity",
                        scene.Sky().iblIntensity
                    },
                    { "sunDriven", scene.Sky().sunDriven }
                } },
                { "fog", {
                    { "enabled", scene.Fog().enabled },
                    { "color", ToJson(scene.Fog().color) },
                    { "startDistance", scene.Fog().startDistance },
                    { "endDistance", scene.Fog().endDistance },
                    { "density", scene.Fog().density }
                } },
                { "ambientOcclusion", {
                    {
                        "enabled",
                        scene.AmbientOcclusion().enabled
                    },
                    {
                        "radius",
                        scene.AmbientOcclusion().radius
                    },
                    {
                        "strength",
                        scene.AmbientOcclusion().strength
                    }
                } },
                { "temporalAntiAliasing", {
                    {
                        "enabled",
                        scene.TemporalAntiAliasing()
                            .enabled
                    },
                    {
                        "historyWeight",
                        scene.TemporalAntiAliasing()
                            .historyWeight
                    },
                    {
                        "jitterScale",
                        scene.TemporalAntiAliasing()
                            .jitterScale
                    },
                    {
                        "clampTolerance",
                        scene.TemporalAntiAliasing()
                            .clampTolerance
                    }
                } },
                { "screenSpaceReflection", {
                    {
                        "enabled",
                        scene.ScreenSpaceReflection()
                            .enabled
                    },
                    {
                        "intensity",
                        scene.ScreenSpaceReflection()
                            .intensity
                    },
                    {
                        "maximumDistance",
                        scene.ScreenSpaceReflection()
                            .maximumDistance
                    },
                    {
                        "stepCount",
                        scene.ScreenSpaceReflection()
                            .stepCount
                    },
                    {
                        "thickness",
                        scene.ScreenSpaceReflection()
                            .thickness
                    },
                    {
                        "roughnessCutoff",
                        scene.ScreenSpaceReflection()
                            .roughnessCutoff
                    }
                } },
                { "bakedGlobalIllumination", {
                    {
                        "enabled",
                        scene.BakedGlobalIllumination()
                            .enabled
                    },
                    {
                        "center",
                        ToJson(
                            scene.BakedGlobalIllumination()
                                .center)
                    },
                    {
                        "size",
                        ToJson(
                            scene.BakedGlobalIllumination()
                                .size)
                    },
                    {
                        "resolution",
                        Json::array({
                            scene.BakedGlobalIllumination()
                                .resolutionX,
                            scene.BakedGlobalIllumination()
                                .resolutionY,
                            scene.BakedGlobalIllumination()
                                .resolutionZ })
                    },
                    {
                        "intensity",
                        scene.BakedGlobalIllumination()
                            .intensity
                    },
                    // 焼き込み済みデータ。「焼いたときの形」と
                    // 一緒に保存します（設定を変えた後でも表示が
                    // 崩れないように）。
                    {
                        "bakedResolution",
                        Json::array({
                            scene
                                .BakedGlobalIlluminationBakedShape()
                                .resolutionX,
                            scene
                                .BakedGlobalIlluminationBakedShape()
                                .resolutionY,
                            scene
                                .BakedGlobalIlluminationBakedShape()
                                .resolutionZ })
                    },
                    {
                        "bakedCenter",
                        ToJson(
                            scene
                                .BakedGlobalIlluminationBakedShape()
                                .center)
                    },
                    {
                        "bakedSize",
                        ToJson(
                            scene
                                .BakedGlobalIlluminationBakedShape()
                                .size)
                    },
                    {
                        "data",
                        EncodeBase64(
                            reinterpret_cast<
                                const std::uint8_t*>(
                                scene
                                    .BakedGlobalIlluminationPayload()
                                    .data()),
                            scene
                                .BakedGlobalIlluminationPayload()
                                .size() * 2)
                    }
                } },
                { "volumetricLight", {
                    {
                        "enabled",
                        scene.VolumetricLight().enabled
                    },
                    {
                        "intensity",
                        scene.VolumetricLight().intensity
                    },
                    {
                        "sampleCount",
                        scene.VolumetricLight().sampleCount
                    },
                    {
                        "maximumDistance",
                        scene.VolumetricLight()
                            .maximumDistance
                    },
                    {
                        "scattering",
                        scene.VolumetricLight().scattering
                    }
                } },
                { "bloom", {
                    { "enabled", scene.Bloom().enabled },
                    { "threshold", scene.Bloom().threshold },
                    { "intensity", scene.Bloom().intensity },
                    { "radius", scene.Bloom().radius }
                } },
                { "screenOutline", {
                    { "enabled", scene.ScreenOutline().enabled },
                    { "color", ToJson(scene.ScreenOutline().color) },
                    { "intensity", scene.ScreenOutline().intensity },
                    { "thickness", scene.ScreenOutline().thickness },
                    {
                        "depthThreshold",
                        scene.ScreenOutline().depthThreshold
                    },
                    {
                        "normalThreshold",
                        scene.ScreenOutline().normalThreshold
                    }
                } },
                { "screenSpaceLensFlare", {
                    {
                        "enabled",
                        scene.ScreenSpaceLensFlare().enabled
                    },
                    {
                        "threshold",
                        scene.ScreenSpaceLensFlare().threshold
                    },
                    {
                        "intensity",
                        scene.ScreenSpaceLensFlare().intensity
                    },
                    {
                        "ghostDispersal",
                        scene.ScreenSpaceLensFlare().ghostDispersal
                    },
                    {
                        "haloWidth",
                        scene.ScreenSpaceLensFlare().haloWidth
                    },
                    {
                        "chromaticAberration",
                        scene.ScreenSpaceLensFlare()
                            .chromaticAberration
                    },
                    {
                        "streakIntensity",
                        scene.ScreenSpaceLensFlare().streakIntensity
                    },
                    {
                        "streakLength",
                        scene.ScreenSpaceLensFlare().streakLength
                    },
                    {
                        "streakDirections",
                        scene.ScreenSpaceLensFlare()
                            .streakDirections
                    },
                    {
                        "streakAngleDegrees",
                        scene.ScreenSpaceLensFlare()
                            .streakAngleDegrees
                    }
                } },
                { "depthOfField", {
                    {
                        "enabled",
                        scene.DepthOfField().enabled
                    },
                    {
                        "focusDistance",
                        scene.DepthOfField().focusDistance
                    },
                    {
                        "focusRange",
                        scene.DepthOfField().focusRange
                    },
                    {
                        "blurStrength",
                        scene.DepthOfField().blurStrength
                    },
                    {
                        "maximumRadius",
                        scene.DepthOfField().maximumRadius
                    }
                } },
                { "motionBlur", {
                    {
                        "enabled",
                        scene.MotionBlur().enabled
                    },
                    {
                        "intensity",
                        scene.MotionBlur().intensity
                    },
                    {
                        "maximumRadius",
                        scene.MotionBlur().maximumRadius
                    }
                } },
                { "autoExposure", {
                    {
                        "enabled",
                        scene.AutoExposure().enabled
                    },
                    {
                        "keyValue",
                        scene.AutoExposure().keyValue
                    },
                    {
                        "minimumLuminance",
                        scene.AutoExposure().minimumLuminance
                    },
                    {
                        "maximumLuminance",
                        scene.AutoExposure().maximumLuminance
                    },
                    {
                        "speedToBright",
                        scene.AutoExposure().speedToBright
                    },
                    {
                        "speedToDark",
                        scene.AutoExposure().speedToDark
                    }
                } },
                { "colorGrading", {
                    {
                        "toneMappingEnabled",
                        scene.ColorGrading().toneMappingEnabled
                    },
                    { "enabled", scene.ColorGrading().enabled },
                    { "exposure", scene.ColorGrading().exposure },
                    { "contrast", scene.ColorGrading().contrast },
                    { "saturation", scene.ColorGrading().saturation },
                    { "temperature", scene.ColorGrading().temperature },
                    { "tint", scene.ColorGrading().tint },
                    { "vignette", scene.ColorGrading().vignette }
                } }
            } },
            { "physics", {
                {
                    "broadPhaseCellSize",
                    scene.PhysicsBroadPhaseCellSize()
                }
            } },
            { "rendering", {
                {
                    "frustumCulling",
                    scene.FrustumCullingEnabled()
                },
                {
                    "occlusionCulling",
                    scene.OcclusionCullingEnabled()
                }
            } },
            { "objects", Json::array() }
        };

        for (const auto& gameObject : scene.GameObjects())
        {
            // 追加読み込みしたシーンのGameObjectは、主シーンの
            // ファイルへ混ざらないよう保存対象から外します。
            if (gameObject->SourceScene()
                != LamaPon::Scene::PrimarySceneHandle())
            {
                continue;
            }
            // 追加シーンのGameObjectを親にしていた場合は、
            // 保存先に親が居なくなるためルート扱いにします。
            const auto* parent = gameObject->Parent();
            const bool parentIsSaved =
                parent != nullptr
                && parent->SourceScene()
                    == LamaPon::Scene::
                        PrimarySceneHandle();
            document["objects"].push_back(
                SerializeGameObject(
                    *gameObject,
                    gameObject->Id(),
                    parentIsSaved
                        ? std::optional{ parent->Id() }
                        : std::nullopt,
                    true,
                    true,
                    database));
        }

        LamaPon::RefreshSerializedAssetManifest(document);
        return document;
    }

    LamaPon::GameObject& LoadPrefabHierarchy(
        LamaPon::Scene& scene,
        Json document,
        const LamaPon::AssetDatabase& database)
    {
        static_cast<void>(
            LamaPon::MigrateSerializedDocument(
                document,
                LamaPon::SerializedDocumentKind::Prefab));
        const auto objects = document.find("objects");
        if (objects == document.end()
            || !objects->is_array()
            || objects->empty()
            || objects->size() > 4096)
        {
            throw std::runtime_error(
                "Prefab requires between 1 and 4096 GameObjects.");
        }
        if (!document.contains("root"))
        {
            throw std::runtime_error(
                "Prefab root GameObject is missing.");
        }

        const auto rootId =
            document.at("root")
                .get<LamaPon::GameObjectId>();
        std::unordered_map<
            LamaPon::GameObjectId,
            LamaPon::GameObject*> objectsById;
        std::vector<
            std::pair<
                LamaPon::GameObject*,
                LamaPon::GameObjectId>> pendingParents;

        for (const auto& objectValue : *objects)
        {
            const auto id = objectValue.at("id")
                .get<LamaPon::GameObjectId>();
            if (id == 0 || objectsById.contains(id))
            {
                throw std::runtime_error(
                    "Prefab contains an invalid or duplicate GameObject id.");
            }

            auto& gameObject = scene.CreateGameObject(
                objectValue.value(
                    "name",
                    std::string("GameObject")));
            gameObject.SetEnabled(
                objectValue.value("enabled", true));
            gameObject.SetTag(
                objectValue.value("tag", std::string{}));
            // 旧形式との互換。オブジェクト直下にあった描画カリング
            // 設定は、既定値でなければRenderCullingコンポーネントへ
            // 変換します（新形式はコンポーネントとして読まれます）。
            {
                const bool legacyAlwaysVisible =
                    objectValue.value(
                        "alwaysVisible", false);
                const float legacyCullingMargin =
                    objectValue.value(
                        "cullingMargin", 0.0f);
                if (legacyAlwaysVisible
                    || legacyCullingMargin > 0.0f)
                {
                    gameObject.AddComponent<
                        LamaPon::RenderCullingComponent>(
                        legacyAlwaysVisible,
                        legacyCullingMargin);
                }
            }
            scene.WarnUnregisteredTag(gameObject);
            if (const auto prefabAsset =
                    objectValue.find("prefabAsset");
                prefabAsset != objectValue.end()
                && prefabAsset->is_string()
                && !prefabAsset->get_ref<
                    const std::string&>().empty())
            {
                gameObject.SetPrefabAssetPath(
                    ReadAssetReference(
                        objectValue,
                        "prefabAsset",
                        database));
            }

            const auto& transformValue =
                objectValue.at("transform");
            auto& transform = gameObject.GetTransform();
            transform.position = ReadFloat3(
                transformValue.at("position"));
            ReadTransformRotation(
                transformValue,
                transform);
            transform.scale = ReadFloat3(
                transformValue.at("scale"));

            for (const auto& componentValue :
                objectValue.value(
                    "components",
                    Json::array()))
            {
                // 1つのコンポーネントが復元に失敗しても
                // （参照先アセットが欠けている場合など）、
                // シーン全体の読み込みは止めません。
                try
                {
                    DeserializeComponent(
                        gameObject,
                        componentValue,
                        database);
                }
                catch (const std::exception& exception)
                {
                    LamaPon::Logger::Instance().Warning(
                        "コンポーネントを復元できませんでした（"
                        + gameObject.Name()
                        + " / "
                        + componentValue.value(
                            "type",
                            std::string{ "不明" })
                        + "）: "
                        + exception.what());
                }
            }

            if (objectValue.contains("parent")
                && !objectValue.at("parent").is_null())
            {
                pendingParents.emplace_back(
                    &gameObject,
                    objectValue.at("parent")
                        .get<LamaPon::GameObjectId>());
            }
            objectsById.emplace(id, &gameObject);
        }

        for (const auto& [child, parentId] :
            pendingParents)
        {
            const auto parent =
                objectsById.find(parentId);
            if (parent == objectsById.end())
            {
                throw std::runtime_error(
                    "Prefab references a missing parent GameObject.");
            }
            child->SetParent(parent->second);
        }

        for (const auto& [sourceId, gameObject] :
            objectsById)
        {
            static_cast<void>(sourceId);
            auto* joint =
                gameObject->GetComponent<
                    LamaPon::JointComponent>();
            if (joint != nullptr
                && joint->ConnectedBodyId() != 0)
            {
                if (const auto target =
                        objectsById.find(
                            joint->ConnectedBodyId());
                    target != objectsById.end())
                {
                    joint->SetConnectedBodyId(
                        target->second->Id());
                }
                else
                {
                    joint->SetConnectedBodyId(0);
                }
            }
            if (auto* parallax =
                    gameObject->GetComponent<
                        LamaPon::ParallaxLayerComponent>();
                parallax != nullptr
                && parallax->ReferenceId() != 0)
            {
                const auto target = objectsById.find(
                    parallax->ReferenceId());
                parallax->SetReferenceId(
                    target != objectsById.end()
                        ? target->second->Id()
                        : 0);
            }
            if (auto* lodGroup =
                    gameObject->GetComponent<
                        LamaPon::LODGroupComponent>())
            {
                auto levels =
                    lodGroup->Levels();
                for (auto& level : levels)
                {
                    if (const auto target =
                            objectsById.find(
                                level.targetId);
                        target != objectsById.end())
                    {
                        level.targetId =
                            target->second->Id();
                    }
                    else
                    {
                        level.targetId = 0;
                    }
                }
                lodGroup->SetLevels(
                    std::move(levels));
            }
        }

        const auto root = objectsById.find(rootId);
        if (root == objectsById.end()
            || root->second->Parent() != nullptr)
        {
            throw std::runtime_error(
                "Prefab root is invalid.");
        }
        for (const auto& [id, gameObject] : objectsById)
        {
            static_cast<void>(id);
            auto* ancestor = gameObject;
            while (ancestor != nullptr
                && ancestor != root->second)
            {
                ancestor = ancestor->Parent();
            }
            if (ancestor != root->second)
            {
                throw std::runtime_error(
                    "Every Prefab GameObject must belong to the root hierarchy.");
            }
        }
        return *root->second;
    }
}

namespace LamaPon
{
    std::string Scene::SerializeToJson() const
    {
        return SerializeScene(
            *this,
            AssetDatabaseFor(m_graphics)).dump(2);
    }

    void Scene::SaveToFile(const std::filesystem::path& path) const
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("Could not open scene for writing: " + LamaPon::PathToUtf8(path));
        }

        output << SerializeToJson() << '\n';
        if (!output)
        {
            throw std::runtime_error("Failed while writing scene: " + LamaPon::PathToUtf8(path));
        }
    }

    void Scene::LoadFromFile(const std::filesystem::path& path)
    {
        if (!m_graphics.Assets().FileExists(path))
        {
            throw std::runtime_error("Could not open scene for reading: " + LamaPon::PathToUtf8(path));
        }

        const auto bytes = m_graphics.Assets().ReadFileBytes(path);
        const std::string json{
            bytes.begin(),
            bytes.end()
        };
        LoadFromJson(json);
        m_sceneManager->SetCurrentScenePath(
            path);
        Logger::Instance().Info(
            "Scene loaded: "
            + PathToUtf8(path));
    }

    void Scene::LoadFromJson(const std::string_view json)
    {
        static_cast<void>(
            ApplySceneJson(json, false, {}));
    }

    SceneHandle Scene::MergeFromJson(
        const std::string_view json,
        std::filesystem::path sourcePath)
    {
        return ApplySceneJson(
            json,
            true,
            std::move(sourcePath));
    }

    SceneHandle Scene::MergeFromFile(
        const std::filesystem::path& path)
    {
        if (!m_graphics.Assets().FileExists(path))
        {
            throw std::runtime_error(
                "Could not open scene for reading: "
                + LamaPon::PathToUtf8(path));
        }

        const auto bytes =
            m_graphics.Assets().ReadFileBytes(path);
        const std::string json{
            bytes.begin(),
            bytes.end()
        };
        const auto handle =
            MergeFromJson(json, path);
        Logger::Instance().Info(
            "追加シーンを読み込みました: "
            + PathToUtf8(path));
        return handle;
    }

    SceneHandle Scene::ApplySceneJson(
        const std::string_view json,
        const bool additive,
        std::filesystem::path sourcePath)
    {
        Json document = Json::parse(json.begin(), json.end());

        if (document.value("format", std::string{}) != "LamaPonScene")
        {
            throw std::runtime_error("The JSON data is not a LamaPon scene.");
        }
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::Scene));

        SceneHandle handle = PrimarySceneHandle();
        if (additive)
        {
            handle = m_nextSceneHandle++;
        }
        else
        {
            Clear();
        }
        // 追加読み込み中に作られるGameObjectへ、このシーンの
        // ハンドルを自動で付けます。読み込みが途中で失敗しても、
        // 足しかけたGameObjectを残さないよう後始末します。
        class LoadScope final
        {
        public:
            LoadScope(
                Scene& scene,
                const bool additive,
                const SceneHandle handle) noexcept
                : m_scene(scene)
                , m_previous(scene.m_loadingScene)
                , m_handle(handle)
                , m_rollback(additive)
            {
                m_scene.m_loadingScene = handle;
            }
            ~LoadScope()
            {
                m_scene.m_loadingScene = m_previous;
                if (!m_rollback)
                {
                    return;
                }
                try
                {
                    static_cast<void>(
                        m_scene.UnloadScene(m_handle));
                }
                catch (...)
                {
                    // 後始末の失敗で例外を上書きしません。
                }
            }
            LoadScope(const LoadScope&) = delete;
            LoadScope& operator=(
                const LoadScope&) = delete;

            void Commit() noexcept
            {
                m_rollback = false;
            }

        private:
            Scene& m_scene;
            SceneHandle m_previous{};
            SceneHandle m_handle{};
            bool m_rollback{};
        };
        LoadScope loadScope{ *this, additive, handle };

        // 環境設定は主シーンのものを維持します（追加シーン側の
        // 空・霧・Bloomは無視します）。
        if (const auto environment = document.find("environment");
            !additive
            && environment != document.end()
            && environment->is_object())
        {
            if (environment->contains("ambientColor"))
            {
                SetAmbientLightColor(
                    ReadFloat3(environment->at("ambientColor")));
            }
            SetAmbientLightIntensity(
                environment->value(
                    "ambientIntensity",
                    m_ambientLightIntensity));
            if (const auto sky =
                    environment->find("sky");
                sky != environment->end()
                && sky->is_object())
            {
                auto settings = m_sky;
                settings.enabled =
                    sky->value("enabled", settings.enabled);
                if (sky->contains("topColor"))
                {
                    settings.topColor =
                        ReadFloat3(sky->at("topColor"));
                }
                if (sky->contains("horizonColor"))
                {
                    settings.horizonColor =
                        ReadFloat3(
                            sky->at("horizonColor"));
                }
                if (sky->contains("groundColor"))
                {
                    settings.groundColor =
                        ReadFloat3(
                            sky->at("groundColor"));
                }
                settings.intensity =
                    sky->value(
                        "intensity",
                        settings.intensity);
                settings.cubemapPath = PathFromUtf8(
                    sky->value(
                        "cubemap",
                        std::string{}));
                settings.iblIntensity =
                    sky->value(
                        "iblIntensity",
                        settings.iblIntensity);
                settings.sunDriven =
                    sky->value("sunDriven", false);
                SetSkySettings(settings);
            }
            if (const auto fog =
                    environment->find("fog");
                fog != environment->end()
                && fog->is_object())
            {
                auto settings = m_fog;
                settings.enabled =
                    fog->value("enabled", settings.enabled);
                if (fog->contains("color"))
                {
                    settings.color =
                        ReadFloat3(fog->at("color"));
                }
                settings.startDistance =
                    fog->value(
                        "startDistance",
                        settings.startDistance);
                settings.endDistance =
                    fog->value(
                        "endDistance",
                        settings.endDistance);
                settings.density =
                    fog->value(
                        "density",
                        settings.density);
                SetFogSettings(settings);
            }
            if (const auto occlusion =
                    environment->find("ambientOcclusion");
                occlusion != environment->end()
                && occlusion->is_object())
            {
                auto settings = m_ambientOcclusion;
                settings.enabled =
                    occlusion->value(
                        "enabled",
                        settings.enabled);
                settings.radius =
                    occlusion->value(
                        "radius",
                        settings.radius);
                settings.strength =
                    occlusion->value(
                        "strength",
                        settings.strength);
                SetAmbientOcclusionSettings(settings);
            }
            if (const auto temporal =
                    environment->find(
                        "temporalAntiAliasing");
                temporal != environment->end()
                && temporal->is_object())
            {
                auto settings = m_temporalAntiAliasing;
                settings.enabled = temporal->value(
                    "enabled", settings.enabled);
                settings.historyWeight = temporal->value(
                    "historyWeight",
                    settings.historyWeight);
                settings.jitterScale = temporal->value(
                    "jitterScale", settings.jitterScale);
                settings.clampTolerance = temporal->value(
                    "clampTolerance",
                    settings.clampTolerance);
                SetTemporalAntiAliasingSettings(settings);
            }
            if (const auto reflection =
                    environment->find(
                        "screenSpaceReflection");
                reflection != environment->end()
                && reflection->is_object())
            {
                auto settings = m_screenSpaceReflection;
                settings.enabled = reflection->value(
                    "enabled", settings.enabled);
                settings.intensity = reflection->value(
                    "intensity", settings.intensity);
                settings.maximumDistance =
                    reflection->value(
                        "maximumDistance",
                        settings.maximumDistance);
                settings.stepCount = reflection->value(
                    "stepCount", settings.stepCount);
                settings.thickness = reflection->value(
                    "thickness", settings.thickness);
                settings.roughnessCutoff =
                    reflection->value(
                        "roughnessCutoff",
                        settings.roughnessCutoff);
                SetScreenSpaceReflectionSettings(settings);
            }
            if (const auto bakedGi =
                    environment->find(
                        "bakedGlobalIllumination");
                bakedGi != environment->end()
                && bakedGi->is_object())
            {
                auto settings = m_bakedGiSettings;
                settings.enabled = bakedGi->value(
                    "enabled", settings.enabled);
                if (bakedGi->contains("center"))
                {
                    settings.center =
                        ReadFloat3(bakedGi->at("center"));
                }
                if (bakedGi->contains("size"))
                {
                    settings.size =
                        ReadFloat3(bakedGi->at("size"));
                }
                if (const auto resolution =
                        bakedGi->find("resolution");
                    resolution != bakedGi->end()
                    && resolution->is_array()
                    && resolution->size() == 3)
                {
                    settings.resolutionX =
                        resolution->at(0)
                            .get<std::uint32_t>();
                    settings.resolutionY =
                        resolution->at(1)
                            .get<std::uint32_t>();
                    settings.resolutionZ =
                        resolution->at(2)
                            .get<std::uint32_t>();
                }
                settings.intensity = bakedGi->value(
                    "intensity", settings.intensity);
                SetBakedGlobalIlluminationSettings(settings);

                // 焼き込み済みデータの復元。形とデータが揃って
                // いるときだけ受け取ります。
                if (const auto bakedResolution =
                        bakedGi->find("bakedResolution");
                    bakedResolution != bakedGi->end()
                    && bakedResolution->is_array()
                    && bakedResolution->size() == 3
                    && bakedGi->contains("data"))
                {
                    BakedGlobalIlluminationSettings shape =
                        settings;
                    shape.resolutionX =
                        bakedResolution->at(0)
                            .get<std::uint32_t>();
                    shape.resolutionY =
                        bakedResolution->at(1)
                            .get<std::uint32_t>();
                    shape.resolutionZ =
                        bakedResolution->at(2)
                            .get<std::uint32_t>();
                    if (bakedGi->contains("bakedCenter"))
                    {
                        shape.center = ReadFloat3(
                            bakedGi->at("bakedCenter"));
                    }
                    if (bakedGi->contains("bakedSize"))
                    {
                        shape.size = ReadFloat3(
                            bakedGi->at("bakedSize"));
                    }
                    const auto bytes = DecodeBase64(
                        bakedGi->at("data")
                            .get<std::string>());
                    std::vector<std::uint16_t> payload(
                        bytes.size() / 2);
                    std::memcpy(
                        payload.data(),
                        bytes.data(),
                        payload.size() * 2);
                    RestoreBakedGlobalIllumination(
                        shape,
                        std::move(payload));
                }
            }
            if (const auto volumetric =
                    environment->find("volumetricLight");
                volumetric != environment->end()
                && volumetric->is_object())
            {
                auto settings = m_volumetricLight;
                settings.enabled =
                    volumetric->value(
                        "enabled",
                        settings.enabled);
                settings.intensity =
                    volumetric->value(
                        "intensity",
                        settings.intensity);
                settings.sampleCount =
                    volumetric->value(
                        "sampleCount",
                        settings.sampleCount);
                settings.maximumDistance =
                    volumetric->value(
                        "maximumDistance",
                        settings.maximumDistance);
                settings.scattering =
                    volumetric->value(
                        "scattering",
                        settings.scattering);
                SetVolumetricLightSettings(settings);
            }
            if (const auto bloom =
                    environment->find("bloom");
                bloom != environment->end()
                && bloom->is_object())
            {
                auto settings = m_bloom;
                settings.enabled =
                    bloom->value(
                        "enabled",
                        settings.enabled);
                settings.threshold =
                    bloom->value(
                        "threshold",
                        settings.threshold);
                settings.intensity =
                    bloom->value(
                        "intensity",
                        settings.intensity);
                settings.radius =
                    bloom->value(
                        "radius",
                        settings.radius);
                SetBloomSettings(settings);
            }
            if (const auto screenOutline =
                    environment->find("screenOutline");
                screenOutline != environment->end()
                && screenOutline->is_object())
            {
                auto settings = m_screenOutline;
                settings.enabled = screenOutline->value(
                    "enabled", settings.enabled);
                if (screenOutline->contains("color"))
                {
                    settings.color = ReadFloat3(
                        screenOutline->at("color"));
                }
                settings.intensity = screenOutline->value(
                    "intensity", settings.intensity);
                settings.thickness = screenOutline->value(
                    "thickness", settings.thickness);
                settings.depthThreshold = screenOutline->value(
                    "depthThreshold", settings.depthThreshold);
                settings.normalThreshold = screenOutline->value(
                    "normalThreshold", settings.normalThreshold);
                SetScreenOutlineSettings(settings);
            }
            if (const auto lensFlare =
                    environment->find("screenSpaceLensFlare");
                lensFlare != environment->end()
                && lensFlare->is_object())
            {
                auto settings = m_screenSpaceLensFlare;
                settings.enabled = lensFlare->value(
                    "enabled",
                    settings.enabled);
                settings.threshold = lensFlare->value(
                    "threshold",
                    settings.threshold);
                settings.intensity = lensFlare->value(
                    "intensity",
                    settings.intensity);
                settings.ghostDispersal = lensFlare->value(
                    "ghostDispersal",
                    settings.ghostDispersal);
                settings.haloWidth = lensFlare->value(
                    "haloWidth",
                    settings.haloWidth);
                settings.chromaticAberration = lensFlare->value(
                    "chromaticAberration",
                    settings.chromaticAberration);
                settings.streakIntensity = lensFlare->value(
                    "streakIntensity",
                    settings.streakIntensity);
                settings.streakLength = lensFlare->value(
                    "streakLength",
                    settings.streakLength);
                settings.streakDirections =
                    lensFlare->value(
                        "streakDirections",
                        settings.streakDirections);
                settings.streakAngleDegrees =
                    lensFlare->value(
                        "streakAngleDegrees",
                        settings.streakAngleDegrees);
                SetScreenSpaceLensFlareSettings(settings);
            }
            if (const auto depthOfField =
                    environment->find("depthOfField");
                depthOfField != environment->end()
                && depthOfField->is_object())
            {
                auto settings = m_depthOfField;
                settings.enabled = depthOfField->value(
                    "enabled",
                    settings.enabled);
                settings.focusDistance = depthOfField->value(
                    "focusDistance",
                    settings.focusDistance);
                settings.focusRange = depthOfField->value(
                    "focusRange",
                    settings.focusRange);
                settings.blurStrength = depthOfField->value(
                    "blurStrength",
                    settings.blurStrength);
                settings.maximumRadius = depthOfField->value(
                    "maximumRadius",
                    settings.maximumRadius);
                SetDepthOfFieldSettings(settings);
            }
            if (const auto motionBlur =
                    environment->find("motionBlur");
                motionBlur != environment->end()
                && motionBlur->is_object())
            {
                auto settings = m_motionBlur;
                settings.enabled = motionBlur->value(
                    "enabled",
                    settings.enabled);
                settings.intensity = motionBlur->value(
                    "intensity",
                    settings.intensity);
                settings.maximumRadius = motionBlur->value(
                    "maximumRadius",
                    settings.maximumRadius);
                SetMotionBlurSettings(settings);
            }
            if (const auto autoExposure =
                    environment->find("autoExposure");
                autoExposure != environment->end()
                && autoExposure->is_object())
            {
                auto settings = m_autoExposure;
                settings.enabled = autoExposure->value(
                    "enabled",
                    settings.enabled);
                settings.keyValue = autoExposure->value(
                    "keyValue",
                    settings.keyValue);
                settings.minimumLuminance =
                    autoExposure->value(
                        "minimumLuminance",
                        settings.minimumLuminance);
                settings.maximumLuminance =
                    autoExposure->value(
                        "maximumLuminance",
                        settings.maximumLuminance);
                settings.speedToBright = autoExposure->value(
                    "speedToBright",
                    settings.speedToBright);
                settings.speedToDark = autoExposure->value(
                    "speedToDark",
                    settings.speedToDark);
                SetAutoExposureSettings(settings);
            }
            if (const auto colorGrading =
                    environment->find("colorGrading");
                colorGrading != environment->end()
                && colorGrading->is_object())
            {
                auto settings = m_colorGrading;
                settings.toneMappingEnabled = colorGrading->value(
                    "toneMappingEnabled",
                    settings.toneMappingEnabled);
                settings.enabled = colorGrading->value(
                    "enabled", settings.enabled);
                settings.exposure = colorGrading->value(
                    "exposure", settings.exposure);
                settings.contrast = colorGrading->value(
                    "contrast", settings.contrast);
                settings.saturation = colorGrading->value(
                    "saturation", settings.saturation);
                settings.temperature = colorGrading->value(
                    "temperature", settings.temperature);
                settings.tint = colorGrading->value(
                    "tint", settings.tint);
                settings.vignette = colorGrading->value(
                    "vignette", settings.vignette);
                SetColorGradingSettings(settings);
            }
        }

        if (const auto physics = document.find("physics");
            !additive
            && physics != document.end()
            && physics->is_object())
        {
            SetPhysicsBroadPhaseCellSize(
                physics->value(
                    "broadPhaseCellSize",
                    m_physicsBroadPhaseCellSize));
        }
        if (const auto rendering =
                document.find("rendering");
            !additive
            && rendering != document.end()
            && rendering->is_object())
        {
            SetFrustumCullingEnabled(
                rendering->value(
                    "frustumCulling",
                    m_frustumCullingEnabled));
            SetOcclusionCullingEnabled(
                rendering->value(
                    "occlusionCulling",
                    m_occlusionCullingEnabled));
        }

        // 追加読み込みでは、既存GameObjectとID衝突しないように
        // 新しいIDを振り直します。JSON内の親参照は元のIDのままな
        // ので、objectsByIdはJSON側のIDで引きます。
        if (additive)
        {
            const auto normalizedPath =
                sourcePath.lexically_normal();
            std::string name =
                normalizedPath.stem().string();
            if (name.empty())
            {
                name = "Scene "
                    + std::to_string(handle);
            }
            m_additiveScenes.push_back(
                LoadedSceneInfo{
                    handle,
                    normalizedPath,
                    std::move(name),
                    0
                });
        }

        std::unordered_map<GameObjectId, GameObject*> objectsById;
        std::vector<std::pair<GameObject*, GameObjectId>> pendingParents;
        std::vector<std::pair<GameObject*, std::string>>
            pendingPersistentObjects;

        for (const auto& objectValue : document.at("objects"))
        {
            const GameObjectId id = objectValue.at("id").get<GameObjectId>();
            if (id == 0 || objectsById.contains(id))
            {
                throw std::runtime_error("Scene contains an invalid or duplicate GameObject id.");
            }

            auto gameObject = std::make_unique<GameObject>(
                additive ? m_nextId++ : id,
                objectValue.value("name", std::string("GameObject")));
            gameObject->m_scene = this;
            gameObject->m_sourceScene = handle;
            auto* gameObjectPointer = gameObject.get();
            gameObjectPointer->SetEnabled(objectValue.value("enabled", true));
            gameObjectPointer->SetTag(
                objectValue.value("tag", std::string{}));
            // 旧形式との互換（上のローダーと同じ変換）。
            {
                const bool legacyAlwaysVisible =
                    objectValue.value(
                        "alwaysVisible", false);
                const float legacyCullingMargin =
                    objectValue.value(
                        "cullingMargin", 0.0f);
                if (legacyAlwaysVisible
                    || legacyCullingMargin > 0.0f)
                {
                    gameObjectPointer->AddComponent<
                        LamaPon::RenderCullingComponent>(
                        legacyAlwaysVisible,
                        legacyCullingMargin);
                }
            }
            WarnUnregisteredTag(*gameObjectPointer);
            if (const auto prefabAsset =
                    objectValue.find("prefabAsset");
                prefabAsset != objectValue.end()
                && prefabAsset->is_string()
                && !prefabAsset->get_ref<
                    const std::string&>().empty())
            {
                gameObjectPointer->SetPrefabAssetPath(
                    ReadAssetReference(
                        objectValue,
                        "prefabAsset",
                        AssetDatabaseFor(m_graphics)));
            }

            const auto& transformValue = objectValue.at("transform");
            auto& transform = gameObjectPointer->GetTransform();
            transform.position = ReadFloat3(transformValue.at("position"));
            ReadTransformRotation(transformValue, transform);
            transform.scale = ReadFloat3(transformValue.at("scale"));

            for (const auto& componentValue : objectValue.value("components", Json::array()))
            {
                // 欠けたアセット参照などで1つのコンポーネントが
                // 失敗しても、シーン読み込み全体は継続します。
                try
                {
                    DeserializeComponent(
                        *gameObjectPointer,
                        componentValue,
                        AssetDatabaseFor(m_graphics));
                }
                catch (const std::exception& exception)
                {
                    Logger::Instance().Warning(
                        "コンポーネントを復元できませんでした（"
                        + gameObjectPointer->Name()
                        + " / "
                        + componentValue.value(
                            "type",
                            std::string{ "不明" })
                        + "）: "
                        + exception.what());
                }
            }

            if (objectValue.contains("parent") && !objectValue.at("parent").is_null())
            {
                pendingParents.emplace_back(
                    gameObjectPointer,
                    objectValue.at("parent").get<GameObjectId>());
            }
            if (objectValue.value("persistent", false))
            {
                pendingPersistentObjects.emplace_back(
                    gameObjectPointer,
                    objectValue.value(
                        "persistenceKey",
                        gameObjectPointer->Name()));
            }

            if (!additive)
            {
                m_nextId = std::max(m_nextId, id + 1);
            }
            objectsById.emplace(id, gameObjectPointer);
            m_gameObjects.emplace_back(std::move(gameObject));
        }

        for (const auto& [child, parentId] : pendingParents)
        {
            const auto parent = objectsById.find(parentId);
            if (parent == objectsById.end())
            {
                throw std::runtime_error("Scene references a missing parent GameObject.");
            }

            child->SetParent(parent->second);
        }

        // 追加読み込みではIDを振り直したので、GameObjectIdを
        // 持つコンポーネントの参照も新しいIDへ付け替えます
        // （Prefab配置と同じ扱いです）。
        if (additive)
        {
            for (const auto& [documentId, gameObject] :
                objectsById)
            {
                static_cast<void>(documentId);
                if (auto* joint =
                        gameObject->GetComponent<
                            JointComponent>();
                    joint != nullptr
                    && joint->ConnectedBodyId() != 0)
                {
                    const auto target =
                        objectsById.find(
                            joint->ConnectedBodyId());
                    joint->SetConnectedBodyId(
                        target != objectsById.end()
                            ? target->second->Id()
                            : 0);
                }
                if (auto* parallax =
                        gameObject->GetComponent<
                            ParallaxLayerComponent>();
                    parallax != nullptr
                    && parallax->ReferenceId() != 0)
                {
                    const auto target =
                        objectsById.find(
                            parallax->ReferenceId());
                    parallax->SetReferenceId(
                        target != objectsById.end()
                            ? target->second->Id()
                            : 0);
                }
                if (auto* lodGroup =
                    gameObject->GetComponent<
                        LODGroupComponent>())
                {
                    auto levels = lodGroup->Levels();
                    for (auto& level : levels)
                    {
                        const auto target =
                            objectsById.find(
                                level.targetId);
                        level.targetId =
                            target != objectsById.end()
                                ? target->second->Id()
                                : 0;
                    }
                    lodGroup->SetLevels(
                        std::move(levels));
                }
            }
        }

        for (auto& [gameObject, key] :
            pendingPersistentObjects)
        {
            DontDestroyOnLoad(
                *gameObject,
                std::move(key));
        }

        if (document.contains("mainCamera") && !document.at("mainCamera").is_null())
        {
            const GameObjectId cameraObjectId = document.at("mainCamera").get<GameObjectId>();
            const auto cameraObject = objectsById.find(cameraObjectId);
            if (cameraObject == objectsById.end())
            {
                throw std::runtime_error("Scene references a missing main camera GameObject.");
            }

            auto* camera = cameraObject->second->GetComponent<CameraComponent>();
            if (camera == nullptr)
            {
                throw std::runtime_error("The main camera GameObject has no Camera component.");
            }

            // 追加シーンのカメラは、主シーンにMain Cameraが
            // 無いときだけ採用します。
            if (!additive || m_mainCamera == nullptr)
            {
                SetMainCamera(*camera);
            }
        }

        loadScope.Commit();
        if (additive)
        {
            std::size_t rootCount = 0;
            for (const auto& object : m_gameObjects)
            {
                if (object->m_sourceScene == handle
                    && object->Parent() == nullptr)
                {
                    ++rootCount;
                }
            }
            if (auto entry = std::find_if(
                    m_additiveScenes.begin(),
                    m_additiveScenes.end(),
                    [handle](
                        const LoadedSceneInfo& scene)
                    {
                        return scene.handle == handle;
                    });
                entry != m_additiveScenes.end())
            {
                entry->rootCount = rootCount;
            }
        }
        return handle;
    }

    std::string Scene::SerializePrefabToJson(
        const GameObject& root) const
    {
        if (FindGameObject(root.Id()) != &root)
        {
            throw std::invalid_argument(
                "Prefab root does not belong to this Scene.");
        }

        std::vector<const GameObject*> hierarchy;
        const auto collect =
            [&hierarchy](
                const auto& self,
                const GameObject& gameObject) -> void
            {
                hierarchy.push_back(&gameObject);
                for (const auto* child :
                    gameObject.Children())
                {
                    self(self, *child);
                }
            };
        collect(collect, root);

        std::unordered_map<
            GameObjectId,
            GameObjectId> localIds;
        for (std::size_t index = 0;
            index < hierarchy.size();
            ++index)
        {
            localIds.emplace(
                hierarchy[index]->Id(),
                static_cast<GameObjectId>(index + 1));
        }

        Json document{
            { "format", "LamaPonPrefab" },
            { "version", 1 },
            { "root", 1 },
            { "name", root.Name() },
            { "objects", Json::array() }
        };
        for (const auto* gameObject : hierarchy)
        {
            std::optional<GameObjectId> parentId;
            if (gameObject != &root)
            {
                const auto parent =
                    localIds.find(
                        gameObject->Parent()->Id());
                if (parent == localIds.end())
                {
                    throw std::logic_error(
                        "Prefab hierarchy is incomplete.");
                }
                parentId = parent->second;
            }
            auto serializedObject =
                SerializeGameObject(
                    *gameObject,
                    localIds.at(gameObject->Id()),
                    parentId,
                    gameObject != &root,
                    false,
                    AssetDatabaseFor(m_graphics));
            for (auto& component :
                serializedObject["components"])
            {
                const auto componentType =
                    component.value(
                        "type",
                        std::string{});
                if (componentType == "LODGroup")
                {
                    for (auto& level :
                        component["levels"])
                    {
                        const auto targetId =
                            level.value(
                                "targetId",
                                GameObjectId{});
                        if (const auto target =
                                localIds.find(
                                    targetId);
                            target != localIds.end())
                        {
                            level["targetId"] =
                                target->second;
                        }
                        else
                        {
                            level["targetId"] = 0;
                        }
                    }
                    continue;
                }
                if (componentType == "ParallaxLayer")
                {
                    const auto referenceId =
                        component.value(
                            "referenceId",
                            GameObjectId{});
                    if (referenceId != 0)
                    {
                        const auto reference =
                            localIds.find(referenceId);
                        component["referenceId"] =
                            reference != localIds.end()
                                ? reference->second
                                : 0;
                    }
                    continue;
                }
                if (componentType != "Joint")
                {
                    continue;
                }
                const auto connectedBodyId =
                    component.value(
                        "connectedBodyId",
                        GameObjectId{});
                if (const auto connected =
                        localIds.find(connectedBodyId);
                    connected != localIds.end())
                {
                    component["connectedBodyId"] =
                        connected->second;
                }
                else
                {
                    component["connectedBodyId"] = 0;
                }
            }
            document["objects"].push_back(
                std::move(serializedObject));
        }
        RefreshSerializedAssetManifest(document);
        return document.dump(2);
    }

    std::shared_ptr<const DataAsset> Scene::LoadDataAsset(
        const std::filesystem::path& path) const
    {
        // データが1つ欠けただけでゲームが止まらないように、
        // 例外は握って空のDataAssetを返します。読めなかったことは
        // 警告としてログへ残すので、原因は後から追えます。
        try
        {
            if (!path.empty())
            {
                return m_graphics.Assets().LoadDataAsset(
                    path);
            }
        }
        catch (const std::exception& exception)
        {
            Logger::Instance().Warning(
                std::string{
                    "Data asset could not be loaded: "
                }
                + exception.what());
        }
        static const auto empty =
            std::make_shared<const DataAsset>();
        return empty;
    }

    void Scene::SavePrefab(
        const GameObject& root,
        const std::filesystem::path& path) const
    {
        WriteTextAtomically(
            path,
            SerializePrefabToJson(root) + '\n');
    }

    GameObject& Scene::InstantiatePrefab(
        const std::filesystem::path& path,
        GameObject* parent)
    {
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                path);
        if (!m_graphics.Assets().FileExists(resolvedPath))
        {
            throw std::runtime_error(
                "Could not open prefab: "
                + PathToUtf8(resolvedPath));
        }
        const auto bytes =
            m_graphics.Assets().ReadFileBytes(resolvedPath);
        const std::string json{
            bytes.begin(),
            bytes.end()
        };
        return InstantiatePrefabFromJson(
            json,
            parent,
            path.lexically_normal());
    }

    GameObject& Scene::InstantiatePrefabFromJson(
        const std::string_view json,
        GameObject* parent,
        std::filesystem::path prefabAssetPath)
    {
        if (parent != nullptr
            && FindGameObject(parent->Id()) != parent)
        {
            throw std::invalid_argument(
                "Prefab parent does not belong to this Scene.");
        }

        const Json document =
            Json::parse(json.begin(), json.end());
        Scene prefabScene(m_graphics);
        auto& prefabRoot =
            LoadPrefabHierarchy(
                prefabScene,
                document,
                AssetDatabaseFor(m_graphics));
        auto& instance = DuplicateGameObject(
            prefabRoot,
            parent,
            false);
        instance.SetPrefabAssetPath(
            std::move(prefabAssetPath));
        return instance;
    }

    GameObject* Scene::FindPrefabInstanceRoot(
        GameObject& gameObject) const noexcept
    {
        if (FindGameObject(gameObject.Id()) != &gameObject)
        {
            return nullptr;
        }
        for (auto* current = &gameObject;
            current != nullptr;
            current = current->Parent())
        {
            if (current->IsPrefabInstanceRoot())
            {
                return current;
            }
        }
        return nullptr;
    }

    const GameObject* Scene::FindPrefabInstanceRoot(
        const GameObject& gameObject) const noexcept
    {
        return FindPrefabInstanceRoot(
            const_cast<GameObject&>(gameObject));
    }

    bool Scene::HasPrefabOverrides(
        const GameObject& instanceRoot) const
    {
        return !GetPrefabOverrides(
            instanceRoot).empty();
    }

    std::vector<PrefabOverride>
        Scene::GetPrefabOverrides(
            const GameObject& instanceRoot) const
    {
        if (FindGameObject(instanceRoot.Id())
                != &instanceRoot
            || !instanceRoot.IsPrefabInstanceRoot())
        {
            throw std::invalid_argument(
                "GameObject is not a Prefab instance root.");
        }

        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        const Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        const Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));

        Scene validationScene(m_graphics);
        static_cast<void>(
            LoadPrefabHierarchy(
                validationScene,
                sourceDocument,
                AssetDatabaseFor(m_graphics)));

        std::vector<PrefabOverride> overrides;
        CollectPrefabOverrides(
            sourceDocument.at("objects"),
            instanceDocument.at("objects"),
            "/objects",
            overrides);
        return overrides;
    }

    void Scene::ApplyPrefabOverride(
        const GameObject& instanceRoot,
        const std::string_view path) const
    {
        const auto overrides =
            GetPrefabOverrides(instanceRoot);
        const auto selectedOverride =
            std::find_if(
                overrides.begin(),
                overrides.end(),
                [path](const PrefabOverride& value)
                {
                    return value.path == path;
                });
        if (selectedOverride == overrides.end()
            || !selectedOverride->
                canApplyIndividually)
        {
            throw std::invalid_argument(
                "Prefab override cannot be applied individually.");
        }

        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        const Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));
        const Json::json_pointer pointer{
            std::string{ path }
        };
        sourceDocument.at(pointer) =
            instanceDocument.at(pointer);

        Scene validationScene(m_graphics);
        static_cast<void>(
            LoadPrefabHierarchy(
                validationScene,
                sourceDocument,
                AssetDatabaseFor(m_graphics)));
        WriteTextAtomically(
            resolvedPath,
            sourceDocument.dump(2) + '\n');
    }

    GameObject& Scene::RevertPrefabOverride(
        GameObject& instanceRoot,
        const std::string_view path)
    {
        const auto overrides =
            GetPrefabOverrides(instanceRoot);
        const auto selectedOverride =
            std::find_if(
                overrides.begin(),
                overrides.end(),
                [path](const PrefabOverride& value)
                {
                    return value.path == path;
                });
        if (selectedOverride == overrides.end()
            || !selectedOverride->
                canApplyIndividually)
        {
            throw std::invalid_argument(
                "Prefab override cannot be reverted individually.");
        }

        const auto assetPath =
            instanceRoot.PrefabAssetPath();
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        const Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));
        const Json::json_pointer pointer{
            std::string{ path }
        };
        instanceDocument.at(pointer) =
            sourceDocument.at(pointer);

        auto* parent = instanceRoot.Parent();
        auto& replacement =
            InstantiatePrefabFromJson(
                instanceDocument.dump(),
                parent,
                assetPath);
        if (!DestroyGameObject(instanceRoot))
        {
            DestroyGameObject(replacement);
            throw std::runtime_error(
                "Could not replace the Prefab instance.");
        }
        return replacement;
    }

    void Scene::ApplyPrefabInstance(
        const GameObject& instanceRoot) const
    {
        if (FindGameObject(instanceRoot.Id())
                != &instanceRoot
            || !instanceRoot.IsPrefabInstanceRoot())
        {
            throw std::invalid_argument(
                "GameObject is not a Prefab instance root.");
        }

        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        SavePrefab(instanceRoot, resolvedPath);
    }

    GameObject& Scene::RevertPrefabInstance(
        GameObject& instanceRoot)
    {
        if (FindGameObject(instanceRoot.Id())
                != &instanceRoot
            || !instanceRoot.IsPrefabInstanceRoot())
        {
            throw std::invalid_argument(
                "GameObject is not a Prefab instance root.");
        }

        const auto assetPath =
            instanceRoot.PrefabAssetPath();
        auto* parent = instanceRoot.Parent();
        auto& replacement =
            InstantiatePrefab(assetPath, parent);
        if (!DestroyGameObject(instanceRoot))
        {
            DestroyGameObject(replacement);
            throw std::runtime_error(
                "Could not replace the Prefab instance.");
        }
        return replacement;
    }
}
