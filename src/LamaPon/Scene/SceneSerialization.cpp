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
#include "LamaPon/Components/NetworkIdentityComponent.h"
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
#include "LamaPon/Components/Sway2DComponent.h"
#include "LamaPon/Components/Blink2DComponent.h"
#include "LamaPon/Components/SpriteSkin2DComponent.h"
#include "LamaPon/Components/Rig2DComponent.h"
#include "LamaPon/Components/Keyform2DComponent.h"
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
    // base64の6ビット値の文字表
    constexpr char Base64Characters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz0123456789+/";

    // 生バイト列をパディング付きbase64へ変換します(data: sizeバイト読める入力先頭, size: 入力バイト数)。
    [[nodiscard]] std::string EncodeBase64(
        const std::uint8_t* data,
        const std::size_t size)
    {
        // base64へ符号化した文字列
        std::string output;
        output.reserve((size + 2) / 3 * 4);
        // 処理する入力または配列の添字
        for (std::size_t index = 0; index < size; index += 3)
        {
            // 今回まとめる残り入力バイト数
            const std::uint32_t remaining = static_cast<std::uint32_t>(
                std::min<std::size_t>(3, size - index));
            // 符号化する最大三バイトの値
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

    // 最初のパディングまでbase64を復号し、途中の未知文字では空の列を返します(text: 復号する文字列)。
    // 空白は受け付けず、パディング以降と末尾の未完成ビットは検証しません。
    [[nodiscard]] std::vector<std::uint8_t> DecodeBase64(
        const std::string& text)
    {
        // base64文字を6ビット値へ変換し、未知文字は-1を返します(character: 変換する文字)。
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
        // base64から復号したバイト列
        std::vector<std::uint8_t> output;
        output.reserve(text.size() / 4 * 3);
        // 復号中の6ビット値の蓄積
        std::uint32_t accumulator = 0;
        // 未出力の蓄積ビット数
        int bits = 0;
        // 処理中の入力文字
        for (const char character : text)
        {
            if (character == '=')
            {
                break;
            }
            // base64文字を復号した6ビット値
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

    // 描画機器のアセット台帳を借用し、未初期化なら静的な空の台帳を返します(graphics: 描画デバイス)。
    const LamaPon::AssetDatabase& AssetDatabaseFor(
        LamaPon::GraphicsDevice& graphics)
    {
        // 描画未初期化時の空の台帳
        static const LamaPon::AssetDatabase emptyDatabase;
        // 利用可能なアセット管理器
        const auto* assets = graphics.TryAssets();
        return assets != nullptr
            ? assets->Database()
            : emptyDatabase;
    }

    // キーの~と/をJSON Pointer用にエスケープします(token: 一階層のキー)。
    std::string EscapeJsonPointerToken(
        const std::string_view token)
    {
        // JSON Pointerへ変換したキー
        std::string escaped;
        escaped.reserve(token.size());
        // 処理中の入力文字
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

    // 文字列は内容を、その他はJSON表記を差分表示用に返します(value: 表示する値)。
    std::string DisplayJsonValue(
        const Json& value)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        return value.dump();
    }

    // プリハブ差分の値と存在状態を一覧へ追加します(overrides: 追加先, path: JSON Pointer, source: 元値またはnullptr, instance: 実体値またはnullptr, canApplyIndividually: 両値がある時に個別適用を許すか)。
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

    // 物体またはコンポーネントの構造配列かを返します(path: JSON Pointer)。
    bool IsStructuralPrefabArray(
        const std::string_view path)
    {
        return path == "/objects"
            || path.ends_with("/components");
    }

    // 元データと実体の差分を再帰収集します(source: 元JSON値, instance: 実体JSON値, path: 現在のJSON Pointer, overrides: 差分追加先)。
    // 構造配列の要素は位置で対応させ、要素数やコンポーネント型の変更とキーの追加・削除は個別適用を禁止します。
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
            // 元と実体のキーの和集合
            std::set<std::string> keys;
            // key: 比較対象のJSONキー, value: キー一覧の取得では使わない値
            for (const auto& [key, value] :
                source.items())
            {
                static_cast<void>(value);
                keys.insert(key);
            }
            // key: 比較対象のJSONキー, value: キー一覧の取得では使わない値
            for (const auto& [key, value] :
                instance.items())
            {
                static_cast<void>(value);
                keys.insert(key);
            }

            // 比較するJSONのキー
            for (const auto& key : keys)
            {
                // 元データ内の該当キーの位置
                const auto sourceValue =
                    source.find(key);
                // 実体内の該当キーの位置
                const auto instanceValue =
                    instance.find(key);
                // 比較する子キーのJSON Pointer
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

            // 処理する入力または配列の添字
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

    // 絶対パスを正規化し、相対パスをアセット基準で解決します(graphics: アセットを持つ描画デバイス, path: プリハブのパス)。
    std::filesystem::path ResolvePrefabAssetPath(
        LamaPon::GraphicsDevice& graphics,
        const std::filesystem::path& path)
    {
        return path.is_absolute()
            ? path.lexically_normal()
            : graphics.Assets().ResolvePath(path);
    }

    // 存在するアセットをJSONとして読み、未発見や不正なJSONは例外を伝播します(assets: 読み込み元, path: 文書パス, label: 未発見時の文書名)。
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
        // JSON文書の入力バイト列
        const auto bytes = assets.ReadFileBytes(path);
        return Json::parse(bytes.begin(), bytes.end());
    }

    // 親フォルダーを作成してテキストを保存し、書き込み完了後に置換します(path: 保存先, text: 保存内容)。
    // 保存先に.tmpを付けた固定名を使うため同じ保存先への同時呼び出しを避け、失敗時は例外を伝播します。
    void WriteTextAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        // 置換前に保存する.tmpパス
        auto temporaryPath = path;
        temporaryPath += L".tmp";
        // 置換前のテキスト書込先ストリーム
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
            // 失敗時の一時保存先の削除結果
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
            // 失敗時の一時保存先の削除結果
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not replace file: "
                + LamaPon::PathToUtf8(path));
        }
    }

    // 既定値でない物理材質の合成モードだけを書き込みます(result: 追記先JSON, material: 保存する物理材質)。
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

    // 物理材質を読み、合成モードの番号を0〜4へ制限します(value: 材質のJSONオブジェクト)。
    [[nodiscard]] LamaPon::PhysicsMaterial
        ReadPhysicsMaterial(const Json& value)
    {
        // JSONから読み出した物理材質
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

    // 二成分をXY順のJSON配列へ変換します(value: 保存する二成分)。
    Json ToJson(const DirectX::XMFLOAT2& value)
    {
        return Json::array({ value.x, value.y });
    }

    // 三成分をXYZ順のJSON配列へ変換します(value: 保存する三成分)。
    Json ToJson(const DirectX::XMFLOAT3& value)
    {
        return Json::array({ value.x, value.y, value.z });
    }

    // 四成分をXYZW順のJSON配列へ変換します(value: 保存する四成分)。
    Json ToJson(const DirectX::XMFLOAT4& value)
    {
        return Json::array({ value.x, value.y, value.z, value.w });
    }

    // 2DスキンのボーンIDを新しいIDへ置き換えます(skin: 更新するスキン, translate: 元のIDから新しいIDを返し対応がなければ0を返す処理)。
    template<typename Translate>
    void RemapSpriteSkinBones(
        LamaPon::SpriteSkin2DComponent& skin,
        const Translate& translate)
    {
        // 置き換えるボーンID列
        auto bones = skin.Bones();
        // 置き換えるボーンID
        for (auto& bone : bones)
        {
            bone = translate(bone);
        }
        static_cast<void>(skin.RemapBones(std::move(bones)));
    }

    // 4×4行列を行順の16要素のJSON配列へ変換します(value: 保存する行列)。
    Json ToJson(const DirectX::XMFLOAT4X4& value)
    {
        // 行順に並べる16要素
        Json result = Json::array();
        // 保存する行番号
        for (int row = 0; row < 4; ++row)
        {
            // 保存する列番号
            for (int column = 0; column < 4; ++column)
            {
                result.push_back(value.m[row][column]);
            }
        }
        return result;
    }

    // 行順の16要素の数値配列を読み、長さや型が不正なら例外を伝播します(value: 行列のJSON配列)。
    DirectX::XMFLOAT4X4 ReadFloat4x4(const Json& value)
    {
        if (!value.is_array() || value.size() != 16)
        {
            throw std::runtime_error("Expected a JSON array with sixteen numbers.");
        }
        // 読み込んだ行列
        DirectX::XMFLOAT4X4 result{};
        // 読み込む行番号
        for (int row = 0; row < 4; ++row)
        {
            // 読み込む列番号
            for (int column = 0; column < 4; ++column)
            {
                result.m[row][column] =
                    value.at(static_cast<std::size_t>(row * 4 + column))
                        .get<float>();
            }
        }
        return result;
    }

    // 二要素の数値配列を読み、長さや型が不正なら例外を伝播します(value: XY順のJSON配列)。
    DirectX::XMFLOAT2 ReadFloat2(const Json& value)
    {
        if (!value.is_array() || value.size() != 2)
        {
            throw std::runtime_error("Expected a JSON array with two numbers.");
        }

        return { value.at(0).get<float>(), value.at(1).get<float>() };
    }

    // 三要素の数値配列を読み、長さや型が不正なら例外を伝播します(value: XYZ順のJSON配列)。
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

    // 四要素の数値配列を読み、長さや型が不正なら例外を伝播します(value: XYZW順のJSON配列)。
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

    // クォータニオンを優先して回転を読み、旧形式のオイラー角にも対応します(transformValue: 変換のJSON, transform: 回転の書込先)。
    // 両形式がない場合は単位クォータニオンを設定します。
    void ReadTransformRotation(
        const Json& transformValue,
        LamaPon::Transform& transform)
    {
        if (transformValue.contains("rotationQuaternion"))
        {
            // JSONに保存された回転の四成分
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

    // 横揃えを保存名へ変換し、未知値はLeftにします(alignment: 横揃えの指定)。
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

    // 横揃えの保存名を読み、未知名はLeftにします(value: 横揃えの保存名)。
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

    // 縦揃えを保存名へ変換し、未知値はTopにします(alignment: 縦揃えの指定)。
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

    // 縦揃えの保存名を読み、未知名はTopにします(value: 縦揃えの保存名)。
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

    // 基本形状を保存名へ変換し、未対応値は例外を送出します(shape: 基本形状の種類)。
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

    // 基本形状の保存名を読み、未知名は例外を送出します(name: 基本形状の保存名)。
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

    // 粒子放出形状を保存名へ変換し、未知値はConeにします(shape: 粒子放出形状の種類)。
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

    // 粒子描画を保存名へ変換し、Horizontal以外はBillboardにします(mode: 粒子の描画方式)。
    const char* ParticleRenderModeName(
        const LamaPon::ParticleRenderMode mode)
    {
        return mode
                == LamaPon::ParticleRenderMode::Horizontal
            ? "Horizontal"
            : "Billboard";
    }

    // 粒子描画の保存名を読み、Horizontal以外はBillboardにします(value: 描画方式の保存名)。
    LamaPon::ParticleRenderMode ReadParticleRenderMode(
        const std::string& value)
    {
        return value == "Horizontal"
            ? LamaPon::ParticleRenderMode::Horizontal
            : LamaPon::ParticleRenderMode::Billboard;
    }

    // ビルボード方式を保存名へ変換し、未知値はScreenAlignedにします(mode: ビルボードの向き指定)。
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

    // ビルボードの保存名を読み、未知名はScreenAlignedにします(value: 向き指定の保存名)。
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

    // 正面軸を保存名へ変換し、Forward以外はUpにします(axis: 正面とするローカル軸)。
    const char* BillboardFacingAxisToString(
        const LamaPon::BillboardFacingAxis axis)
    {
        return axis == LamaPon::BillboardFacingAxis::Forward
            ? "Forward"
            : "Up";
    }

    // 正面軸の保存名を読み、Forward以外はUpにします(value: 正面軸の保存名)。
    LamaPon::BillboardFacingAxis
        BillboardFacingAxisFromString(
            const std::string& value)
    {
        return value == "Forward"
            ? LamaPon::BillboardFacingAxis::Forward
            : LamaPon::BillboardFacingAxis::Up;
    }

    // 粒子放出形状の保存名を読み、未知名はConeにします(name: 放出形状の保存名)。
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

    // アセットのパスと取得できたGUIDを保存します(result: 追記先JSON, field: パスのキー名, path: 保存するパス, database: GUIDを得る台帳)。
    void SerializeAssetReference(
        Json& result,
        const std::string_view field,
        const std::filesystem::path& path,
        const LamaPon::AssetDatabase& database)
    {
        // JSONへ格納するアセットのキー名
        const std::string fieldName(field);
        result[fieldName] =
            LamaPon::PathToUtf8(path);
        if (path.empty())
        {
            return;
        }
        // 保存または解決するアセットGUID
        const auto guid =
            database.GuidForPath(path);
        if (!guid.empty())
        {
            result[fieldName + "Guid"] = guid;
        }
    }

    // PBRマップのパス・GUIDと遮蔽・発光の設定を書き込みます(result: 追記先JSON, material: 保存する材質, database: GUIDを得る台帳)。
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
        // 保存する材質の発光色
        const auto& emissive = material.EmissiveColor();
        result["emissiveColor"] = Json::array({
            emissive.x,
            emissive.y,
            emissive.z
        });
    }

    // GUIDを優先してアセット参照を解決し、解決できなければ保存パスを使います(value: 参照元JSON, field: パスのキー名, database: GUIDを解決する台帳)。
    std::filesystem::path ReadAssetReference(
        const Json& value,
        const std::string_view field,
        const LamaPon::AssetDatabase& database)
    {
        // JSONへ格納するアセットのキー名
        const std::string fieldName(field);
        // GUID未解決時の保存パス
        const auto fallback = LamaPon::PathFromUtf8(
            value.value(
                fieldName,
                std::string{}));
        // 保存または解決するアセットGUID
        const auto guid = value.value(
            fieldName + "Guid",
            std::string{});
        return !guid.empty()
            ? database.ResolveGuid(
                guid,
                fallback)
            : fallback;
    }

    // PBRマップの参照と遮蔽・発光の設定を復元します(value: 材質JSON, component: 復元先の描画コンポーネント, database: GUIDを解決する台帳)。
    // 省略されたマップは空パス、遮蔽強度は1を使い、発光色は三要素配列の場合だけ更新します。
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
        // 発光色のJSON内の格納位置
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

    // 対応するコンポーネントの型・有効状態・設定を保存用JSONへ変換します(component: 保存するコンポーネント, database: アセットGUIDを取得する台帳)。
    // 新しい型は保存・復元の双方へ追加し、未対応型はserializable=falseとして記録します。
    Json SerializeComponent(
        const LamaPon::Component& component,
        const LamaPon::AssetDatabase& database)
    {
        // 型と設定を格納するJSON
        Json result{
            { "type", std::string(component.TypeName()) },
            { "enabled", component.IsEnabled() }
        };

        // 保存するネットワーク識別子
        if (const auto* identity = dynamic_cast<const LamaPon::NetworkIdentityComponent*>(&component))
        {
            result["sceneKey"] = identity->SceneKey();
            result["hostOnlySimulation"] = identity->HostOnlySimulation();
            result["interpolationSeconds"] = identity->InterpolationSeconds();
        }
        // 保存するカメラ設定
        else if (const auto* camera = dynamic_cast<const LamaPon::CameraComponent*>(&component))
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
        // 保存する方向光源
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
        // 保存するポイント光源
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
        // 保存するスポット光源
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
        // 保存する二次元光源
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
        // 保存する二次元箱形状
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
        // 保存する円形状
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
        // 保存する多角形状
        else if (const auto* polygon2D =
            dynamic_cast<
                const LamaPon::PolygonCollider2DComponent*>(
                    &component))
        {
            // 多角形頂点のJSON配列
            auto vertices = Json::array();
            // 保存する多角形頂点
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
        // 保存する三次元箱形状
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
        // 保存するカプセル形状
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
        // 保存する球形状
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
        // 保存する凸形状
        else if (const auto* hull =
            dynamic_cast<
                const LamaPon::ConvexHullCollider3DComponent*>(
                    &component))
        {
            // 凸形状頂点のJSON配列
            auto points = Json::array();
            // 保存する凸形状の頂点
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
        // 保存するメッシュ衝突形状
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
        // 保存するメッシュ描画設定
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
            // 独自シェーダーのt7以降の追加テクスチャもGUID付きで保存します。
            // 追加テクスチャの添字
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
            // 保存するシェーダーキーワード
            for (const auto& keyword :
                mesh->ShaderKeywords().Keywords())
            {
                result["shaderKeywords"].push_back(keyword);
            }
            result["customParameters"] = Json::array();
            // 独自描画定数の添字
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
        // 保存するスプライト描画設定
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
            // 画像内の表示範囲の正規化矩形
            const auto& sourceRect = sprite->SourceRect();
            if (sourceRect.x != 0.0f
                || sourceRect.y != 0.0f
                || sourceRect.z != 1.0f
                || sourceRect.w != 1.0f)
            {
                result["sourceRect"] = ToJson(sourceRect);
            }
            // 分割しない既定の矩形以外のときだけ保存します。
            if (sprite->MeshColumns() != 1
                || sprite->MeshRows() != 1)
            {
                result["meshColumns"] = sprite->MeshColumns();
                result["meshRows"] = sprite->MeshRows();
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
            // 独自描画定数の添字
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
        // 保存するスプライトマスク
        else if (const auto* spriteMask =
            dynamic_cast<
                const LamaPon::SpriteMaskComponent*>(
                    &component))
        {
            result["shape"] =
                static_cast<int>(spriteMask->Shape());
            result["size"] = ToJson(spriteMask->Size());
        }
        // 保存する可視判定設定
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
        // 保存する反射プローブ設定
        else if (const auto* reflectionProbe =
            dynamic_cast<
                const LamaPon::ReflectionProbeComponent*>(
                    &component))
        {
            // 反射のベイク結果はシーンJSONに含めず、描画時にキャッシュ復元または再ベイクします。
            result["range"] = reflectionProbe->Range();
            result["intensity"] =
                reflectionProbe->Intensity();
            result["boxExtents"] =
                ToJson(reflectionProbe->BoxExtents());
            result["blendDistance"] =
                reflectionProbe->BlendDistance();
        }
        // 保存するスプライトアニメ設定
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
            // アニメクリップのJSON配列
            auto clips = nlohmann::json::array();
            // 保存するアニメクリップ
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
        // 保存するUI基準画面の設定
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
        // 保存するUI矩形変換
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
        // 保存するボタン設定
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
        // 保存するUI画像設定
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
        // 保存するトグル設定
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
        // 保存するスライダー設定
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
        // 保存する文字入力設定
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
        // 保存するUI整列設定
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
        // 保存するスクロール表示設定
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
        // 保存するナビ格子設定
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
                // ナビ格子の奥行き添字
                for (std::uint32_t z{};
                    z < navMesh->GridDepth();
                    ++z)
                {
                    // ナビ格子の横方向添字
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
        // 保存するナビ移動設定
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
            // 保存する移動経路のワールド点
            for (const auto& point :
                agent->Path())
            {
                result["path"].push_back(
                    ToJson(point));
            }
        }
        // 保存するタイル描画設定
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
            // coordinate: タイルのXY格子座標, tileIndex: 画像内のタイル番号
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
        // 保存する視差移動設定
        else if (const auto* parallax =
            dynamic_cast<
                const LamaPon::ParallaxLayerComponent*>(
                    &component))
        {
            result["factor"] = ToJson(parallax->Factor());
            result["referenceId"] =
                parallax->ReferenceId();
        }
        // 保存する音声再生設定
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
        // 保存するモデル描画設定
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
            // 独自シェーダーのt7以降の追加テクスチャもGUID付きで保存します。
            // 追加テクスチャの添字
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
            // 保存するシェーダーキーワード
            for (const auto& keyword :
                model->ShaderKeywords().Keywords())
            {
                result["shaderKeywords"].push_back(keyword);
            }
            result["customParameters"] = Json::array();
            // 独自描画定数の添字
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
        // 保存する文字描画設定
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
        // 保存する三次元粒子設定
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
            // 独自描画定数の添字
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
        // 保存する二次元粒子設定
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
        // 保存する回転速度設定
        else if (const auto* rotator = dynamic_cast<const LamaPon::RotatorComponent*>(&component))
        {
            result["angularVelocity"] = ToJson(rotator->AngularVelocity());
        }
        // 保存する2D揺れ物設定
        else if (const auto* sway =
            dynamic_cast<
                const LamaPon::Sway2DComponent*>(
                    &component))
        {
            // 保存する補正済みの揺れ設定
            const auto& settings = sway->Settings();
            result["tipOffset"] = ToJson(settings.tipOffset);
            result["stiffness"] = settings.stiffness;
            result["damping"] = settings.damping;
            result["inertia"] = settings.inertia;
            result["gravity"] = ToJson(settings.gravity);
            result["maxAngle"] = settings.maxAngleDegrees;
            result["windAmplitude"] =
                settings.windAmplitudeDegrees;
            result["windFrequency"] =
                settings.windFrequency;
            result["windPhase"] =
                settings.windPhaseDegrees;
        }
        // 保存する2D瞬き設定
        else if (const auto* blink =
            dynamic_cast<
                const LamaPon::Blink2DComponent*>(
                    &component))
        {
            // 保存する補正済みの瞬き設定
            const auto& settings = blink->Settings();
            result["columns"] = settings.columns;
            result["rows"] = settings.rows;
            result["openFrame"] = settings.openFrame;
            result["closingStartFrame"] =
                settings.closingStartFrame;
            result["closingFrameCount"] =
                settings.closingFrameCount;
            result["frameSeconds"] = settings.frameSeconds;
            result["closedSeconds"] = settings.closedSeconds;
            result["intervalMin"] =
                settings.intervalMinSeconds;
            result["intervalMax"] =
                settings.intervalMaxSeconds;
            result["doubleBlinkChance"] =
                settings.doubleBlinkChance;
            result["autoBlink"] = settings.autoBlink;
            result["includeChildren"] =
                settings.includeChildren;
        }
        // 保存する2Dスキン
        else if (const auto* skin =
            dynamic_cast<
                const LamaPon::SpriteSkin2DComponent*>(
                    &component))
        {
            result["bones"] = skin->Bones();
            result["weightFalloff"] = skin->WeightFalloff();
            result["bound"] = skin->IsBound();
            if (skin->IsBound())
            {
                result["boundColumns"] = skin->BoundColumns();
                result["boundRows"] = skin->BoundRows();
                result["spriteBindPose"] =
                    ToJson(skin->SpriteBindPose());
                result["boneBindPoses"] = Json::array();
                // 保存するボーンのバインド姿勢
                for (const auto& pose : skin->BoneBindPoses())
                {
                    result["boneBindPoses"].push_back(ToJson(pose));
                }
                // 番号4個と重み4個を並べた頂点ごとの重み
                result["weights"] = Json::array();
                // 保存する頂点の重み
                for (const auto& weight : skin->Weights())
                {
                    result["weights"].push_back(Json::array({
                        weight.bones[0],
                        weight.bones[1],
                        weight.bones[2],
                        weight.bones[3],
                        weight.weights[0],
                        weight.weights[1],
                        weight.weights[2],
                        weight.weights[3] }));
                }
            }
        }
        // 保存する2Dリグのパラメータ
        else if (const auto* rig =
            dynamic_cast<
                const LamaPon::Rig2DComponent*>(
                    &component))
        {
            result["parameters"] = Json::array();
            // 保存するパラメータ
            for (const auto& parameter : rig->Parameters())
            {
                result["parameters"].push_back({
                    { "name", parameter.name },
                    { "minimum", parameter.minimum },
                    { "maximum", parameter.maximum },
                    { "defaultValue", parameter.defaultValue },
                    { "value", parameter.value },
                    { "autoAmplitude", parameter.autoAmplitude },
                    { "autoFrequency", parameter.autoFrequency } });
            }
        }
        // 保存する2Dキーフォーム
        else if (const auto* keyform =
            dynamic_cast<
                const LamaPon::Keyform2DComponent*>(
                    &component))
        {
            if (keyform->HasRestPose())
            {
                result["rest"] = {
                    { "position", ToJson(keyform->RestPosition()) },
                    { "rotation", ToJson(keyform->RestRotation()) },
                    { "scale", ToJson(keyform->RestScale()) },
                    { "opacity", keyform->RestOpacity() } };
            }
            result["channels"] = Json::array();
            // 保存するチャンネル
            for (const auto& channel : keyform->Channels())
            {
                // 保存するキーの配列
                Json keys = Json::array();
                // 保存するキー
                for (const auto& key : channel.keys)
                {
                    // 保存する1キー
                    Json serializedKey{
                        { "value", key.value },
                        { "position", ToJson(key.positionOffset) },
                        { "rotation", key.rotationDegrees },
                        { "scale", ToJson(key.scale) },
                        { "opacity", key.opacity } };
                    if (!key.vertexOffsets.empty())
                    {
                        serializedKey["vertices"] = Json::array();
                        // 保存する頂点移動
                        for (const auto& offset : key.vertexOffsets)
                        {
                            serializedKey["vertices"].push_back(
                                ToJson(offset));
                        }
                    }
                    keys.push_back(std::move(serializedKey));
                }
                result["channels"].push_back({
                    { "parameter", channel.parameter },
                    { "keys", std::move(keys) } });
            }
        }
        // 保存するビルボード設定
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
        // 保存する変換アニメ設定
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
        // 保存する入力移動設定
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
        // 保存するキャラクター移動設定
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
        // 保存するネイティブスクリプト
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
        // 保存する剛体設定
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
        // 保存するジョイント設定
        else if (const auto* joint =
            dynamic_cast<const LamaPon::JointComponent*>(&component))
        {
            // ジョイント種類の保存名
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
        // 保存するLOD切替設定
        else if (const auto* lodGroup =
            dynamic_cast<
                const LamaPon::LODGroupComponent*>(
                    &component))
        {
            result["cullDistance"] =
                lodGroup->CullDistance();
            result["levels"] = Json::array();
            // 保存するLOD距離と対象番号
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

    // 保存型に応じたコンポーネントを追加して設定を復元します(gameObject: 追加先の物体, value: コンポーネントJSON, database: アセットGUIDを解決する台帳)。
    // 未知型や不正なデータは例外を伝播し、追加後の復元失敗はここではロールバックしません。
    LamaPon::Component& DeserializeComponent(
        LamaPon::GameObject& gameObject,
        const Json& value,
        const LamaPon::AssetDatabase& database)
    {
        // 復元するコンポーネントの型名
        const auto type = value.at("type").get<std::string>();
        // 追加したコンポーネントの参照
        LamaPon::Component* component{};

        if (type == "NetworkIdentity")
        {
            // 復元したネットワーク識別子
            auto& identity = gameObject.AddComponent<LamaPon::NetworkIdentityComponent>(
                value.value("sceneKey", std::string{}));
            identity.SetHostOnlySimulation(value.value("hostOnlySimulation", true));
            identity.SetInterpolationSeconds(value.value("interpolationSeconds", 0.1f));
            component = &identity;
        }
        else if (type == "Camera")
        {
            // 復元したカメラ設定
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
            // 復元した方向光源
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
            // 既存Game Moduleとのコンストラクター互換を保ち、角直径は追加後に設定します。
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
            // 復元したポイント光源
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
            // 復元したスポット光源
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
            // 復元した二次元光源
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
            // 既存Game Moduleとのコンストラクター互換を保ち、UIへの影響は追加後に設定します。
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
            // 復元する多角形頂点の一覧
            std::vector<DirectX::XMFLOAT2> vertices;
            if (value.contains("vertices"))
            {
                // 多角形頂点のJSON配列
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
            // 復元する凸形状の頂点一覧
            std::vector<DirectX::XMFLOAT3> points;
            if (value.contains("points"))
            {
                // 復元する頂点または経路点
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
            // 復元したメッシュ描画設定
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
            // キーワード配列の格納位置
            if (const auto keywords =
                    value.find("shaderKeywords");
                keywords != value.end()
                && keywords->is_array())
            {
                // 有効化するキーワード一覧
                std::vector<std::string> enabled;
                // キーワード配列のJSON値
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
            // 追加テクスチャの添字
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
            // 独自描画定数のJSON格納位置
            if (const auto found = value.find("customParameters");
                found != value.end() && found->is_array())
            {
                // 復元可能な独自描画定数の数
                const auto count = std::min(
                    found->size(),
                    LamaPon::LitMaterial::CustomParameterCount);
                // 復元する独自描画定数の添字
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
            // 復元したスプライト描画設定
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
            sprite.SetMeshGrid(
                value.value("meshColumns", 1),
                value.value("meshRows", 1));
            sprite.SetShaderPath(
                ReadAssetReference(
                    value,
                    "shader",
                    database));
            // 独自描画定数のJSON格納位置
            if (const auto found =
                    value.find("customParameters");
                found != value.end()
                && found->is_array())
            {
                // 復元可能な独自描画定数の数
                const auto count = std::min(
                    found->size(),
                    LamaPon::SpriteRendererComponent::
                        CustomParameterCount);
                // 復元する独自描画定数の添字
                for (std::size_t index = 0;
                    // 復元可能な独自描画定数の数
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
            // 復元したスプライトマスク
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
            // 復元した可視判定設定
            auto& renderCulling =
                gameObject.AddComponent<
                    LamaPon::RenderCullingComponent>(
                    value.value("alwaysVisible", false),
                    value.value("cullingMargin", 0.0f));
            component = &renderCulling;
        }
        else if (type == "ReflectionProbe")
        {
            // 復元した反射プローブ設定
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
            // シーン由来と記録し、初回の自動ベイクをキャッシュ復元へ置き換えられるようにします。
            reflectionProbe.MarkLoadedFromScene();
            component = &reflectionProbe;
        }
        else if (type == "SpriteAnimator")
        {
            // 復元したスプライトアニメ設定
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
            // アニメクリップ配列の格納位置
            if (const auto clips = value.find("clips");
                clips != value.end()
                && clips->is_array())
            {
                // 復元するアニメクリップのJSON
                for (const auto& clipValue : *clips)
                {
                    // 復元したアニメクリップ
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
            // 復元したボタン設定
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
            // 復元したUI画像設定
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
            // 復元したトグル設定
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
            // 復元した初期値を操作による変更として通知しません。
            static_cast<void>(
                toggle.ConsumeValueChanged());
            component = &toggle;
        }
        else if (type == "UISlider")
        {
            // 復元したスライダー設定
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
            // 復元した文字入力設定
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
            // 復元したUI整列設定
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
            // 0〜2へ制限した子の整列指定
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
            // 復元したスクロール表示設定
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
            // 復元したナビ格子設定
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
                // 復元する通行不可セルの座標一覧
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
                    // 復元する格子セルのJSON
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
            // 復元したナビ移動設定
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
            // 復元するワールド移動経路
            std::vector<
                DirectX::XMFLOAT3> path;
            if (value.contains("path")
                && value.at("path").
                    is_array())
            {
                // 復元する頂点または経路点
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
            // 復元したタイル描画設定
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
                // 復元する格子セルのJSON
                for (const auto& cell :
                    value.at("cells"))
                {
                    // 復元する画像内のタイル番号
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
            // 復元した視差移動設定
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
            // 復元した音声再生設定
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
            // 復元したモデル描画設定
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
            // 旧シーンで方式の指定がない場合も既定のPBR描画を使います。
            model.SetUseLegacyShading(
                value.value("useLegacyShading", false));
            model.SetShaderPath(ReadAssetReference(
                value,
                "shader",
                database));
            // キーワード配列の格納位置
            if (const auto keywords =
                    value.find("shaderKeywords");
                keywords != value.end()
                && keywords->is_array())
            {
                // 有効化するキーワード一覧
                std::vector<std::string> enabled;
                // キーワード配列のJSON値
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
            // 追加テクスチャの添字
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
            // 独自描画定数のJSON格納位置
            if (const auto found = value.find("customParameters");
                found != value.end() && found->is_array())
            {
                // 復元可能な独自描画定数の数
                const auto count = std::min(
                    found->size(),
                    LamaPon::LitMaterial::CustomParameterCount);
                // 復元する独自描画定数の添字
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
            // 復元した文字描画設定
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
            // 復元した粒子描画設定
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
            // 独自描画定数のJSON格納位置
            if (const auto found =
                value.find("customParameters");
                found != value.end()
                && found->is_array())
            {
                // 復元可能な独自描画定数の数
                const std::size_t count =
                    std::min(
                        found->size(),
                        LamaPon::
                            ParticleSystemComponent::
                                CustomParameterCount);
                // 復元する独自描画定数の添字
                for (std::size_t index = 0;
                    // 復元可能な独自描画定数の数
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
            // 復元した粒子描画設定
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
            // 復元したビルボード設定
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
        else if (type == "Sway2D")
        {
            // 省略された項目を既定値で埋める揺れ設定
            LamaPon::Sway2DSettings settings{};
            if (value.contains("tipOffset"))
            {
                settings.tipOffset =
                    ReadFloat2(value.at("tipOffset"));
            }
            settings.stiffness =
                value.value("stiffness", settings.stiffness);
            settings.damping =
                value.value("damping", settings.damping);
            settings.inertia =
                value.value("inertia", settings.inertia);
            if (value.contains("gravity"))
            {
                settings.gravity =
                    ReadFloat2(value.at("gravity"));
            }
            settings.maxAngleDegrees =
                value.value(
                    "maxAngle",
                    settings.maxAngleDegrees);
            settings.windAmplitudeDegrees =
                value.value(
                    "windAmplitude",
                    settings.windAmplitudeDegrees);
            settings.windFrequency =
                value.value(
                    "windFrequency",
                    settings.windFrequency);
            settings.windPhaseDegrees =
                value.value(
                    "windPhase",
                    settings.windPhaseDegrees);
            component = &gameObject.AddComponent<
                LamaPon::Sway2DComponent>(settings);
        }
        else if (type == "Blink2D")
        {
            // 省略された項目を既定値で埋める瞬き設定
            LamaPon::Blink2DSettings settings{};
            settings.columns =
                value.value("columns", settings.columns);
            settings.rows =
                value.value("rows", settings.rows);
            settings.openFrame =
                value.value("openFrame", settings.openFrame);
            settings.closingStartFrame =
                value.value(
                    "closingStartFrame",
                    settings.closingStartFrame);
            settings.closingFrameCount =
                value.value(
                    "closingFrameCount",
                    settings.closingFrameCount);
            settings.frameSeconds =
                value.value(
                    "frameSeconds",
                    settings.frameSeconds);
            settings.closedSeconds =
                value.value(
                    "closedSeconds",
                    settings.closedSeconds);
            settings.intervalMinSeconds =
                value.value(
                    "intervalMin",
                    settings.intervalMinSeconds);
            settings.intervalMaxSeconds =
                value.value(
                    "intervalMax",
                    settings.intervalMaxSeconds);
            settings.doubleBlinkChance =
                value.value(
                    "doubleBlinkChance",
                    settings.doubleBlinkChance);
            settings.autoBlink =
                value.value("autoBlink", settings.autoBlink);
            settings.includeChildren =
                value.value(
                    "includeChildren",
                    settings.includeChildren);
            component = &gameObject.AddComponent<
                LamaPon::Blink2DComponent>(settings);
        }
        else if (type == "SpriteSkin2D")
        {
            // 復元した2Dスキン
            auto& skin = gameObject.AddComponent<
                LamaPon::SpriteSkin2DComponent>(
                    value.value(
                        "bones",
                        std::vector<std::uint64_t>{}));
            skin.SetWeightFalloff(
                value.value(
                    "weightFalloff",
                    LamaPon::SpriteSkin2DComponent::
                        DefaultWeightFalloff));
            if (value.value("bound", false)
                && value.contains("spriteBindPose")
                && value.contains("boneBindPoses")
                && value.contains("weights"))
            {
                // 復元するボーンのバインド姿勢
                std::vector<DirectX::XMFLOAT4X4> bonePoses;
                // 読み込むボーンの姿勢
                for (const auto& pose : value.at("boneBindPoses"))
                {
                    bonePoses.push_back(ReadFloat4x4(pose));
                }
                // 復元する頂点ごとの重み
                std::vector<LamaPon::SpriteSkinWeight> weights;
                // 読み込む頂点の重み
                for (const auto& entry : value.at("weights"))
                {
                    if (!entry.is_array() || entry.size() != 8)
                    {
                        throw std::runtime_error(
                            "A SpriteSkin2D weight needs eight numbers.");
                    }
                    // 復元する1頂点の重み
                    LamaPon::SpriteSkinWeight weight{};
                    // 読み込む影響の番号
                    for (std::size_t index = 0; index < 4; ++index)
                    {
                        weight.bones[index] =
                            entry.at(index).get<std::uint16_t>();
                        weight.weights[index] =
                            entry.at(index + 4).get<float>();
                    }
                    weights.push_back(weight);
                }
                static_cast<void>(skin.RestoreBinding(
                    std::move(bonePoses),
                    ReadFloat4x4(value.at("spriteBindPose")),
                    std::move(weights),
                    value.value("boundColumns", 1),
                    value.value("boundRows", 1)));
            }
            component = &skin;
        }
        else if (type == "Rig2D")
        {
            // 復元するパラメータ
            std::vector<LamaPon::Rig2DParameter> parameters;
            if (value.contains("parameters"))
            {
                // 読み込むパラメータ
                for (const auto& entry : value.at("parameters"))
                {
                    // 省略された項目を既定値で埋めるパラメータ
                    LamaPon::Rig2DParameter parameter{};
                    parameter.name =
                        entry.value("name", std::string{});
                    parameter.minimum =
                        entry.value("minimum", parameter.minimum);
                    parameter.maximum =
                        entry.value("maximum", parameter.maximum);
                    parameter.defaultValue =
                        entry.value(
                            "defaultValue",
                            parameter.defaultValue);
                    parameter.value =
                        entry.value("value", parameter.defaultValue);
                    parameter.autoAmplitude =
                        entry.value(
                            "autoAmplitude",
                            parameter.autoAmplitude);
                    parameter.autoFrequency =
                        entry.value(
                            "autoFrequency",
                            parameter.autoFrequency);
                    parameters.push_back(std::move(parameter));
                }
            }
            component = &gameObject.AddComponent<
                LamaPon::Rig2DComponent>(std::move(parameters));
        }
        else if (type == "Keyform2D")
        {
            // 復元するチャンネル
            std::vector<LamaPon::Keyform2DChannel> channels;
            if (value.contains("channels"))
            {
                // 読み込むチャンネル
                for (const auto& entry : value.at("channels"))
                {
                    // 復元する1チャンネル
                    LamaPon::Keyform2DChannel channel;
                    channel.parameter =
                        entry.value("parameter", std::string{});
                    if (entry.contains("keys"))
                    {
                        // 読み込むキー
                        for (const auto& serializedKey : entry.at("keys"))
                        {
                            // 省略された項目を既定値で埋めるキー
                            LamaPon::Keyform2DKey key{};
                            key.value = serializedKey.value("value", 0.0f);
                            if (serializedKey.contains("position"))
                            {
                                key.positionOffset = ReadFloat2(
                                    serializedKey.at("position"));
                            }
                            key.rotationDegrees =
                                serializedKey.value("rotation", 0.0f);
                            if (serializedKey.contains("scale"))
                            {
                                key.scale = ReadFloat2(
                                    serializedKey.at("scale"));
                            }
                            key.opacity =
                                serializedKey.value("opacity", 1.0f);
                            if (serializedKey.contains("vertices"))
                            {
                                // 読み込む頂点移動
                                for (const auto& offset :
                                    serializedKey.at("vertices"))
                                {
                                    key.vertexOffsets.push_back(
                                        ReadFloat2(offset));
                                }
                            }
                            channel.keys.push_back(std::move(key));
                        }
                    }
                    channels.push_back(std::move(channel));
                }
            }
            // 復元した2Dキーフォーム
            auto& keyform = gameObject.AddComponent<
                LamaPon::Keyform2DComponent>(std::move(channels));
            if (value.contains("rest"))
            {
                // 保存した基準姿勢
                const auto& rest = value.at("rest");
                keyform.SetRestPose(
                    ReadFloat3(rest.at("position")),
                    ReadFloat4(rest.at("rotation")),
                    ReadFloat3(rest.at("scale")),
                    rest.value("opacity", 1.0f));
            }
            component = &keyform;
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
            // 復元したキャラクター移動設定
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
            // 復元した連続判定方式
            const auto collisionDetection =
                value.value(
                    "collisionDetection",
                    std::string("discrete"))
                    == "continuous"
                ? LamaPon::CollisionDetectionMode::Continuous
                : LamaPon::CollisionDetectionMode::Discrete;
            // 復元した剛体の軸拘束
            LamaPon::RigidbodyConstraints constraints{};
            if (value.contains("constraints"))
            {
                // 軸拘束を保存したJSON
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
            // ジョイント種類の保存名
            const auto typeName =
                value.value(
                    "jointType",
                    std::string("fixed"));
            // 復元したジョイントの種類
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
            // 復元するLOD距離と対象の一覧
            std::vector<LamaPon::LODLevel>
                levels;
            // LOD距離と対象番号のJSON
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
            // スクリプト属性のJSONオブジェクト
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

    // 物体の変換と対応コンポーネントを保存用JSONへ変換します(gameObject: 保存対象, id: 保存する物体番号, parentId: 保存する親番号, includePrefabLink: プリハブ参照を含むか, includePersistence: 保持指定を含むか, database: アセットGUIDの台帳)。
    // 未対応コンポーネントは除外し、互換用オイラー角と正本のクォータニオンを両方保存します。
    Json SerializeGameObject(
        const LamaPon::GameObject& gameObject,
        const LamaPon::GameObjectId id,
        const std::optional<LamaPon::GameObjectId> parentId,
        const bool includePrefabLink,
        const bool includePersistence,
        const LamaPon::AssetDatabase& database)
    {
        // 保存する物体のローカル変換
        const auto& transform = gameObject.GetTransform();
        // 保存する一物体のJSON
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
                { "rotation",
                    ToJson(transform.EulerAngles()) },
                { "rotationQuaternion",
                    ToJson(transform.rotationQuaternion) },
                { "scale", ToJson(transform.scale) }
            } },
            { "components", Json::array() }
        };

        // 保存可否を調べるコンポーネント
        for (const auto& component : gameObject.Components())
        {
            // コンポーネントの保存用JSON
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

    // 主シーンの物体・環境・物理・描画設定とアセット一覧をJSONへ変換します(scene: 保存対象のシーン, database: アセットGUIDの台帳)。
    // 追加シーンの物体・カメラ・親参照は保存対象から除外し、間接光係数はベイク時の格子形状と保存します。
    Json SerializeScene(
        const LamaPon::Scene& scene,
        const LamaPon::AssetDatabase& database)
    {
        // Main Cameraが追加シーン側のカメラだった場合は、主シーンのファイルに書けないためnullとして保存します。
        // シーンに指定された主カメラ
        const auto* mainCamera = scene.MainCamera();
        // 主カメラが保存対象に属するか
        const bool mainCameraIsSaved =
            mainCamera != nullptr
            && mainCamera->Owner().SourceScene()
                == LamaPon::Scene::PrimarySceneHandle();
        // 主シーンの保存用JSON
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

        // 主シーン所属か調べる保存対象
        for (const auto& gameObject : scene.GameObjects())
        {
            // 追加読み込みしたシーンのGameObjectは、主シーンのファイルへ混ざらないよう保存対象から外します。
            if (gameObject->SourceScene()
                != LamaPon::Scene::PrimarySceneHandle())
            {
                continue;
            }
            // 追加シーンのGameObjectを親にしていた場合は、保存先に親が居なくなるためルート扱いにします。
            // 保存対象の現在の親
            const auto* parent = gameObject->Parent();
            // 親が主シーンの保存対象か
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

    // プリハブの旧形式を更新して物体を生成し、親と内部参照を新番号へ対応付けます(scene: 生成先のシーン, document: 更新するJSONの写し, database: アセットGUIDの台帳)。
    // 1〜4096物体の単一ルート階層を要求し、不正な階層は例外を伝播します。
    LamaPon::GameObject& LoadPrefabHierarchy(
        LamaPon::Scene& scene,
        Json document,
        const LamaPon::AssetDatabase& database)
    {
        static_cast<void>(
            LamaPon::MigrateSerializedDocument(
                document,
                LamaPon::SerializedDocumentKind::Prefab));
        // プリハブの物体配列の格納位置
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

        // プリハブ内の保存ルート番号
        const auto rootId =
            document.at("root")
                .get<LamaPon::GameObjectId>();
        // 保存番号から生成先物体への対応
        std::unordered_map<
            LamaPon::GameObjectId,
            LamaPon::GameObject*> objectsById;
        // 全物体生成後に設定する親参照
        std::vector<
            std::pair<
                LamaPon::GameObject*,
                LamaPon::GameObjectId>> pendingParents;

        // 復元する一物体のJSON
        for (const auto& objectValue : *objects)
        {
            // プリハブ内に保存された物体番号
            const auto id = objectValue.at("id")
                .get<LamaPon::GameObjectId>();
            if (id == 0 || objectsById.contains(id))
            {
                throw std::runtime_error(
                    "Prefab contains an invalid or duplicate GameObject id.");
            }

            // 新しい番号で生成した物体
            auto& gameObject = scene.CreateGameObject(
                objectValue.value(
                    "name",
                    std::string("GameObject")));
            gameObject.SetEnabled(
                objectValue.value("enabled", true));
            gameObject.SetTag(
                objectValue.value("tag", std::string{}));
            // 旧形式の物体直下の可視設定を、既定値でなければRenderCullingへ移します。
            {
                // 旧形式の常時表示指定
                const bool legacyAlwaysVisible =
                    objectValue.value(
                        "alwaysVisible", false);
                // 旧形式の可視境界の拡張幅
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
            // 入れ子プリハブ参照の格納位置
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

            // 保存されたローカル変換のJSON
            const auto& transformValue =
                objectValue.at("transform");
            // 生成先物体のローカル変換
            auto& transform = gameObject.GetTransform();
            transform.position = ReadFloat3(
                transformValue.at("position"));
            ReadTransformRotation(
                transformValue,
                transform);
            transform.scale = ReadFloat3(
                transformValue.at("scale"));

            // 復元するコンポーネントのJSON
            for (const auto& componentValue :
                objectValue.value(
                    "components",
                    Json::array()))
            {
                // コンポーネント単位の復元失敗は記録し、残りの復元を続けます。
                try
                {
                    DeserializeComponent(
                        gameObject,
                        componentValue,
                        database);
                }
                // exception: コンポーネント復元の失敗内容
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

        // child: 生成した子物体, parentId: 保存された親番号
        for (const auto& [child, parentId] :
            pendingParents)
        {
            // 保存親番号に対応する生成物体
            const auto parent =
                objectsById.find(parentId);
            if (parent == objectsById.end())
            {
                throw std::runtime_error(
                    "Prefab references a missing parent GameObject.");
            }
            child->SetParent(parent->second);
        }

        // sourceId: 保存された元番号, gameObject: 新番号で生成した物体
        for (const auto& [sourceId, gameObject] :
            objectsById)
        {
            static_cast<void>(sourceId);
            // 内部参照を更新するジョイント
            auto* joint =
                gameObject->GetComponent<
                    LamaPon::JointComponent>();
            if (joint != nullptr
                && joint->ConnectedBodyId() != 0)
            {
                // 参照先の新しい物体の格納位置
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
            // 内部参照を更新する視差移動
            if (auto* parallax =
                    gameObject->GetComponent<
                        LamaPon::ParallaxLayerComponent>();
                parallax != nullptr
                && parallax->ReferenceId() != 0)
            {
                // 参照先の新しい物体の格納位置
                const auto target = objectsById.find(
                    parallax->ReferenceId());
                parallax->SetReferenceId(
                    target != objectsById.end()
                        ? target->second->Id()
                        : 0);
            }
            // 内部参照を更新するLOD設定
            if (auto* lodGroup =
                    gameObject->GetComponent<
                        LamaPon::LODGroupComponent>())
            {
                // 新番号へ置き換えるLOD一覧
                auto levels =
                    lodGroup->Levels();
                // 新番号へ置き換える一LOD設定
                for (auto& level : levels)
                {
                    // 参照先の新しい物体の格納位置
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
            // 内部参照を更新する2Dスキン
            if (auto* skin =
                    gameObject->GetComponent<
                        LamaPon::SpriteSkin2DComponent>())
            {
                RemapSpriteSkinBones(
                    *skin,
                    // 元のボーンIDを新しいIDへ変えます(id: 保存された元のID)。
                    [&objectsById](const LamaPon::GameObjectId id)
                    {
                        // 参照先の新しい物体の格納位置
                        const auto target = objectsById.find(id);
                        return target != objectsById.end()
                            ? target->second->Id()
                            : LamaPon::GameObjectId{};
                    });
            }
        }

        // 保存ルート番号の対応位置
        const auto root = objectsById.find(rootId);
        if (root == objectsById.end()
            || root->second->Parent() != nullptr)
        {
            throw std::runtime_error(
                "Prefab root is invalid.");
        }
        // id: 保存された元番号, gameObject: 所属階層を検証する物体
        for (const auto& [id, gameObject] : objectsById)
        {
            static_cast<void>(id);
            // ルート所属を調べる祖先物体
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
    // 主シーンの保存用JSONを二スペース字下げの文字列で返します。
    std::string Scene::SerializeToJson() const
    {
        return SerializeScene(
            *this,
            AssetDatabaseFor(m_graphics)).dump(2);
    }

    // 親フォルダーを作成して主シーンのJSONを上書き保存します(path: 保存先)。
    // 保存先を先に切り詰めるため書き込み失敗時に旧内容は保持せず、失敗は例外を伝播します。
    void Scene::SaveToFile(const std::filesystem::path& path) const
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        // 主シーンJSONの上書き先ストリーム
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

    // アセット経由で主シーンを読み込み、成功時に現在のシーンパスを更新します(path: 読み込み元)。
    void Scene::LoadFromFile(const std::filesystem::path& path)
    {
        if (!m_graphics.Assets().FileExists(path))
        {
            throw std::runtime_error("Could not open scene for reading: " + LamaPon::PathToUtf8(path));
        }

        // シーンJSONの入力バイト列
        const auto bytes = m_graphics.Assets().ReadFileBytes(path);
        // アセットから読み込んだJSON文字列
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

        // シーンJSONの入力バイト列
        const auto bytes =
            m_graphics.Assets().ReadFileBytes(path);
        // アセットから読み込んだJSON文字列
        const std::string json{
            bytes.begin(),
            bytes.end()
        };
        // 追加読み込みで割り当てた番号
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
        // 旧形式を更新する入力シーンJSON
        Json document = Json::parse(json.begin(), json.end());

        if (document.value("format", std::string{}) != "LamaPonScene")
        {
            throw std::runtime_error("The JSON data is not a LamaPon scene.");
        }
        static_cast<void>(
            MigrateSerializedDocument(
                document,
                SerializedDocumentKind::Scene));

        // 読み込み先のシーン所属番号
        SceneHandle handle = PrimarySceneHandle();
        if (additive)
        {
            handle = m_nextSceneHandle++;
        }
        else
        {
            Clear();
        }
        // 読み込み中の生成所属を設定し、追加読み込みの失敗時は追加分を破棄するスコープです。
        class LoadScope final
        {
        public:
            // 生成する物体の所属を設定します(scene: 読み込み先, additive: 失敗時に追加分を戻すか, handle: 読み込みシーン番号)。
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
            // 生成所属を復元し、未確定の追加読み込みを破棄します。
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
            // 後始末の重複を防ぐためコピーを禁止します。
            LoadScope(const LoadScope&) = delete;
            // 後始末の重複を防ぐため代入を禁止します。
            LoadScope& operator=(
                const LoadScope&) = delete;

            // 読み込み成功を確定して追加分の破棄を解除します。
            void Commit() noexcept
            {
                m_rollback = false;
            }

        private:
            // 読み込み先のシーン
            Scene& m_scene;
            // 読み込み前の生成所属番号
            SceneHandle m_previous{};
            // 今回読み込むシーン番号
            SceneHandle m_handle{};
            // 失敗時に追加分を破棄するか
            bool m_rollback{};
        };
        // 所属復元と追加分破棄の管理範囲
        LoadScope loadScope{ *this, additive, handle };

        // 環境設定は主シーンのものを維持します（追加シーン側の空・霧・Bloomは無視します）。
        // 復元する環境JSONの格納位置
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
            // 空の設定JSONの格納位置
            if (const auto sky =
                    environment->find("sky");
                sky != environment->end()
                && sky->is_object())
            {
                // 復元する空と環境反射の設定
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
            // 霧の設定JSONの格納位置
            if (const auto fog =
                    environment->find("fog");
                fog != environment->end()
                && fog->is_object())
            {
                // 復元する霧の設定
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
            // 環境遮蔽JSONの格納位置
            if (const auto occlusion =
                    environment->find("ambientOcclusion");
                occlusion != environment->end()
                && occlusion->is_object())
            {
                // 復元する環境遮蔽の設定
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
            // 時間AAのJSON格納位置
            if (const auto temporal =
                    environment->find(
                        "temporalAntiAliasing");
                temporal != environment->end()
                && temporal->is_object())
            {
                // 復元する時間AAの設定
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
            // 画面反射JSONの格納位置
            if (const auto reflection =
                    environment->find(
                        "screenSpaceReflection");
                reflection != environment->end()
                && reflection->is_object())
            {
                // 復元する画面空間反射の設定
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
            // 間接光JSONの格納位置
            if (const auto bakedGi =
                    environment->find(
                        "bakedGlobalIllumination");
                bakedGi != environment->end()
                && bakedGi->is_object())
            {
                // 復元する間接光ベイク設定
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
                // 次回ベイクの格子点数の配列
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

                // ベイク時の格子形状と係数データが揃う場合だけ復元します。
                // 保存係数の格子点数の配列
                if (const auto bakedResolution =
                        bakedGi->find("bakedResolution");
                    bakedResolution != bakedGi->end()
                    && bakedResolution->is_array()
                    && bakedResolution->size() == 3
                    && bakedGi->contains("data")
                    && bakedGi->at("data").is_string())
                {
                    // 保存係数をベイクした格子形状
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
                    // 検証した保存格子のプローブ数
                    const auto probeCount =
                        BakedGlobalIlluminationProbeCount(
                            shape.resolutionX,
                            shape.resolutionY,
                            shape.resolutionZ);
                    if (probeCount.has_value())
                    {
                        // 保存係数の要素数
                        const std::size_t coefficientCount =
                            *probeCount
                            * BakedGlobalIlluminationCoefficientsPerProbe;
                        // 格子から決まる係数バイト数
                        const std::size_t expectedByteCount =
                            coefficientCount
                            * sizeof(std::uint16_t);
                        // 係数base64の必要文字数
                        const std::size_t expectedTextLength =
                            (expectedByteCount + 2) / 3 * 4;
                        // 保存されたbase64係数文字列
                        const auto& encoded =
                            bakedGi->at("data")
                                .get_ref<const std::string&>();
                        // 形から決まる上限をDecodeBase64より先に確認し、壊れたJSONで巨大な一時領域を確保しません。
                        if (encoded.size() == expectedTextLength)
                        {
                            // 復号した係数のバイト列
                            const auto bytes = DecodeBase64(encoded);
                            if (bytes.size() == expectedByteCount)
                            {
                                // 復元するfp16係数の所有列
                                std::vector<std::uint16_t> payload(
                                    coefficientCount);
                                std::memcpy(
                                    payload.data(),
                                    bytes.data(),
                                    expectedByteCount);
                                RestoreBakedGlobalIllumination(
                                    shape,
                                    std::move(payload));
                            }
                        }
                    }
                }
            }
            // 光の積算JSONの格納位置
            if (const auto volumetric =
                    environment->find("volumetricLight");
                volumetric != environment->end()
                && volumetric->is_object())
            {
                // 復元する光の積算設定
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
            // ブルームJSONの格納位置
            if (const auto bloom =
                    environment->find("bloom");
                bloom != environment->end()
                && bloom->is_object())
            {
                // 復元するブルーム設定
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
            // 画面輪郭JSONの格納位置
            if (const auto screenOutline =
                    environment->find("screenOutline");
                screenOutline != environment->end()
                && screenOutline->is_object())
            {
                // 復元する画面輪郭の設定
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
            // レンズフレアJSONの格納位置
            if (const auto lensFlare =
                    environment->find("screenSpaceLensFlare");
                lensFlare != environment->end()
                && lensFlare->is_object())
            {
                // 復元するレンズフレア設定
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
            // 被写界深度JSONの格納位置
            if (const auto depthOfField =
                    environment->find("depthOfField");
                depthOfField != environment->end()
                && depthOfField->is_object())
            {
                // 復元する被写界深度の設定
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
            // モーションブラーJSONの格納位置
            if (const auto motionBlur =
                    environment->find("motionBlur");
                motionBlur != environment->end()
                && motionBlur->is_object())
            {
                // 復元するモーションブラー設定
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
            // 自動露出JSONの格納位置
            if (const auto autoExposure =
                    environment->find("autoExposure");
                autoExposure != environment->end()
                && autoExposure->is_object())
            {
                // 復元する自動露出の設定
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
            // 色補正JSONの格納位置
            if (const auto colorGrading =
                    environment->find("colorGrading");
                colorGrading != environment->end()
                && colorGrading->is_object())
            {
                // 復元する色補正の設定
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

        // 物理設定JSONの格納位置
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
        // 描画設定JSONの格納位置
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

        // 追加物体には新番号を付け、JSON内の参照を解決する対応表は保存番号で引きます。
        if (additive)
        {
            // 正規化した追加シーンの生成パス
            const auto normalizedPath =
                sourcePath.lexically_normal();
            // 追加シーンの表示名
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

        // 保存番号から復元先物体への対応
        std::unordered_map<GameObjectId, GameObject*> objectsById;
        // 全物体復元後に設定する親参照
        std::vector<std::pair<GameObject*, GameObjectId>> pendingParents;
        // 親設定後に登録する保持対象とキー
        std::vector<std::pair<GameObject*, std::string>>
            pendingPersistentObjects;

        // 復元する一物体のJSON
        for (const auto& objectValue : document.at("objects"))
        {
            // 保存された物体番号
            const GameObjectId id = objectValue.at("id").get<GameObjectId>();
            if (id == 0 || objectsById.contains(id))
            {
                throw std::runtime_error("Scene contains an invalid or duplicate GameObject id.");
            }

            // 復元した物体の所有参照
            auto gameObject = std::make_unique<GameObject>(
                additive ? m_nextId++ : id,
                objectValue.value("name", std::string("GameObject")));
            gameObject->m_scene = this;
            gameObject->m_sourceScene = handle;
            // 所有列へ移す復元物体の参照
            auto* gameObjectPointer = gameObject.get();
            gameObjectPointer->SetEnabled(objectValue.value("enabled", true));
            gameObjectPointer->SetTag(
                objectValue.value("tag", std::string{}));
            // 旧形式の物体直下の可視設定を、既定値でなければRenderCullingへ移します。
            {
                // 旧形式の常時表示指定
                const bool legacyAlwaysVisible =
                    objectValue.value(
                        "alwaysVisible", false);
                // 旧形式の可視境界の拡張幅
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
            // プリハブ参照のJSON格納位置
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

            // ローカル変換を保存したJSON
            const auto& transformValue = objectValue.at("transform");
            // 復元先物体のローカル変換
            auto& transform = gameObjectPointer->GetTransform();
            transform.position = ReadFloat3(transformValue.at("position"));
            ReadTransformRotation(transformValue, transform);
            transform.scale = ReadFloat3(transformValue.at("scale"));

            // 復元するコンポーネントのJSON
            for (const auto& componentValue : objectValue.value("components", Json::array()))
            {
                // コンポーネント単位の復元失敗は記録し、残りの復元を続けます。
                try
                {
                    DeserializeComponent(
                        *gameObjectPointer,
                        componentValue,
                        AssetDatabaseFor(m_graphics));
                }
                // exception: コンポーネント復元の失敗内容
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

        // child: 復元した子物体, parentId: 保存された親番号
        for (const auto& [child, parentId] : pendingParents)
        {
            // 保存親番号に対応する物体位置
            const auto parent = objectsById.find(parentId);
            if (parent == objectsById.end())
            {
                throw std::runtime_error("Scene references a missing parent GameObject.");
            }

            child->SetParent(parent->second);
        }

        // 追加読み込みのジョイント・視差移動・LOD参照は新番号へ変え、読み込み対象外への参照は0にします。
        if (additive)
        {
            // documentId: 保存された元番号, gameObject: 新番号で生成した物体
            for (const auto& [documentId, gameObject] :
                objectsById)
            {
                static_cast<void>(documentId);
                // 参照を新番号へ変えるジョイント
                if (auto* joint =
                        gameObject->GetComponent<
                            JointComponent>();
                    joint != nullptr
                    && joint->ConnectedBodyId() != 0)
                {
                    // 内部参照の復元先の格納位置
                    const auto target =
                        objectsById.find(
                            joint->ConnectedBodyId());
                    joint->SetConnectedBodyId(
                        target != objectsById.end()
                            ? target->second->Id()
                            : 0);
                }
                // 参照を新番号へ変える視差移動
                if (auto* parallax =
                        gameObject->GetComponent<
                            ParallaxLayerComponent>();
                    parallax != nullptr
                    && parallax->ReferenceId() != 0)
                {
                    // 内部参照の復元先の格納位置
                    const auto target =
                        objectsById.find(
                            parallax->ReferenceId());
                    parallax->SetReferenceId(
                        target != objectsById.end()
                            ? target->second->Id()
                            : 0);
                }
                // 参照を新番号へ変えるLOD設定
                if (auto* lodGroup =
                    gameObject->GetComponent<
                        LODGroupComponent>())
                {
                    // 内部参照を更新するLOD一覧
                    auto levels = lodGroup->Levels();
                    // 内部参照を更新する一LOD設定
                    for (auto& level : levels)
                    {
                        // 内部参照の復元先の格納位置
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
                // 参照を新番号へ変える2Dスキン
                if (auto* skin =
                        gameObject->GetComponent<
                            SpriteSkin2DComponent>())
                {
                    RemapSpriteSkinBones(
                        *skin,
                        // 元のボーンIDを新しいIDへ変えます(id: 保存された元のID)。
                        [&objectsById](const GameObjectId id)
                        {
                            // 内部参照の復元先の格納位置
                            const auto target = objectsById.find(id);
                            return target != objectsById.end()
                                ? target->second->Id()
                                : GameObjectId{};
                        });
                }
            }
        }

        // gameObject: 保持指定する物体, key: シーン間で照合する保持キー
        for (auto& [gameObject, key] :
            pendingPersistentObjects)
        {
            DontDestroyOnLoad(
                *gameObject,
                std::move(key));
        }

        if (document.contains("mainCamera") && !document.at("mainCamera").is_null())
        {
            // 主カメラ所有物体の保存番号
            const GameObjectId cameraObjectId = document.at("mainCamera").get<GameObjectId>();
            // 主カメラ物体の復元先位置
            const auto cameraObject = objectsById.find(cameraObjectId);
            if (cameraObject == objectsById.end())
            {
                throw std::runtime_error("Scene references a missing main camera GameObject.");
            }

            // 復元した主カメラの参照
            auto* camera = cameraObject->second->GetComponent<CameraComponent>();
            if (camera == nullptr)
            {
                throw std::runtime_error("The main camera GameObject has no Camera component.");
            }

            // 追加シーンのカメラは、主シーンにMain Cameraが無いときだけ採用します。
            if (!additive || m_mainCamera == nullptr)
            {
                SetMainCamera(*camera);
            }
        }

        loadScope.Commit();
        if (additive)
        {
            // 追加シーンに属するルート数
            std::size_t rootCount = 0;
            // 所属と親を調べる復元物体
            for (const auto& object : m_gameObjects)
            {
                if (object->m_sourceScene == handle
                    && object->Parent() == nullptr)
                {
                    ++rootCount;
                }
            }
            // 対象の追加シーンかを照合した格納位置(scene: 読み込み済みシーン情報)。
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

        // 親から子へ集めた保存対象の階層
        std::vector<const GameObject*> hierarchy;
        // ルートから子の順に階層を集めます(self: 再帰呼び出し先, gameObject: 追加する物体)。
        const auto collect =
            [&hierarchy](
                const auto& self,
                const GameObject& gameObject) -> void
            {
                hierarchy.push_back(&gameObject);
                // 保存階層へ加える子物体
                for (const auto* child :
                    gameObject.Children())
                {
                    self(self, *child);
                }
            };
        collect(collect, root);

        // シーン番号からプリハブ内番号への対応
        std::unordered_map<
            GameObjectId,
            GameObjectId> localIds;
        // 保存階層内の物体添字
        for (std::size_t index = 0;
            index < hierarchy.size();
            ++index)
        {
            localIds.emplace(
                hierarchy[index]->Id(),
                static_cast<GameObjectId>(index + 1));
        }

        // プリハブ保存または入力のJSON
        Json document{
            { "format", "LamaPonPrefab" },
            { "version", 1 },
            { "root", 1 },
            { "name", root.Name() },
            { "objects", Json::array() }
        };
        // プリハブ階層の保存対象物体
        for (const auto* gameObject : hierarchy)
        {
            // プリハブ内のローカル親番号
            std::optional<GameObjectId> parentId;
            if (gameObject != &root)
            {
                // 保存親の対応位置または配置先の親
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
            // ローカル番号で保存する物体JSON
            auto serializedObject =
                SerializeGameObject(
                    *gameObject,
                    localIds.at(gameObject->Id()),
                    parentId,
                    gameObject != &root,
                    false,
                    AssetDatabaseFor(m_graphics));
            // 内部参照を更新する成分JSON
            for (auto& component :
                serializedObject["components"])
            {
                // 内部参照を更新する成分の型名
                const auto componentType =
                    component.value(
                        "type",
                        std::string{});
                if (componentType == "LODGroup")
                {
                    // 番号をローカル化するLODのJSON
                    for (auto& level :
                        component["levels"])
                    {
                        // シーンにあるLOD対象番号
                        const auto targetId =
                            level.value(
                                "targetId",
                                GameObjectId{});
                        // LOD対象のローカル番号の位置
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
                if (componentType == "SpriteSkin2D")
                {
                    // 番号をローカル化するボーンID
                    for (auto& bone : component["bones"])
                    {
                        // ボーンのローカル番号の位置
                        const auto local =
                            localIds.find(bone.get<GameObjectId>());
                        bone = local != localIds.end()
                            ? local->second
                            : GameObjectId{};
                    }
                    continue;
                }
                if (componentType == "ParallaxLayer")
                {
                    // シーンにある視差基準物体番号
                    const auto referenceId =
                        component.value(
                            "referenceId",
                            GameObjectId{});
                    if (referenceId != 0)
                    {
                        // 視差基準のローカル番号の位置
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
                // シーンにある接続先の物体番号
                const auto connectedBodyId =
                    component.value(
                        "connectedBodyId",
                        GameObjectId{});
                // 接続先のローカル番号の位置
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
        // 読み込みのstd::exceptionは警告に記録して空のDataAssetを返します。
        try
        {
            if (!path.empty())
            {
                return m_graphics.Assets().LoadDataAsset(
                    path);
            }
        }
        // exception: データアセット読み込みの失敗内容
        catch (const std::exception& exception)
        {
            Logger::Instance().Warning(
                std::string{
                    "Data asset could not be loaded: "
                }
                + exception.what());
        }
        // 読み込み失敗時に共有する空データ
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
        // リンク先プリハブの解決済みパス
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
        // 読み込んだプリハブのバイト列
        const auto bytes =
            m_graphics.Assets().ReadFileBytes(resolvedPath);
        // 読み込んだプリハブのJSON文字列
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

        // プリハブ保存または入力のJSON
        const Json document =
            Json::parse(json.begin(), json.end());
        // 複製元の階層を復元する別シーン
        Scene prefabScene(m_graphics);
        // 別シーンへ復元した配置元のルート
        auto& prefabRoot =
            LoadPrefabHierarchy(
                prefabScene,
                document,
                AssetDatabaseFor(m_graphics));
        // 配置先へ複製した新しいルート
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
        // プリハブルートを探している祖先
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

        // 実体がリンクするプリハブパス
        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        // リンク先プリハブの解決済みパス
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        // ファイルから読んだ元プリハブJSON
        const Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        // 現在の実体を保存したプリハブJSON
        const Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));

        // 元プリハブの階層を検証する別シーン
        Scene validationScene(m_graphics);
        static_cast<void>(
            LoadPrefabHierarchy(
                validationScene,
                sourceDocument,
                AssetDatabaseFor(m_graphics)));

        // 元と実体の設定差分の一覧
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
        // 元と実体の設定差分の一覧
        const auto overrides =
            GetPrefabOverrides(instanceRoot);
        // 指定パスに一致する個別操作候補(value: 調べるプリハブ差分)。
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

        // 実体がリンクするプリハブパス
        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        // リンク先プリハブの解決済みパス
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        // ファイルから読んだ元プリハブJSON
        Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        // 現在の実体を保存したプリハブJSON
        const Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));
        // 個別操作する値のJSON Pointer
        const Json::json_pointer pointer{
            std::string{ path }
        };
        sourceDocument.at(pointer) =
            instanceDocument.at(pointer);

        // 元プリハブの階層を検証する別シーン
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
        // 元と実体の設定差分の一覧
        const auto overrides =
            GetPrefabOverrides(instanceRoot);
        // 指定パスに一致する個別操作候補(value: 調べるプリハブ差分)。
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

        // 実体がリンクするプリハブパス
        const auto assetPath =
            instanceRoot.PrefabAssetPath();
        // リンク先プリハブの解決済みパス
        const auto resolvedPath =
            ResolvePrefabAssetPath(
                m_graphics,
                assetPath);
        // ファイルから読んだ元プリハブJSON
        const Json sourceDocument =
            ReadJsonDocument(
                m_graphics.Assets(),
                resolvedPath,
                "linked prefab");
        // 現在の実体を保存したプリハブJSON
        Json instanceDocument =
            Json::parse(
                SerializePrefabToJson(instanceRoot));
        // 個別操作する値のJSON Pointer
        const Json::json_pointer pointer{
            std::string{ path }
        };
        instanceDocument.at(pointer) =
            sourceDocument.at(pointer);

        // 保存親の対応位置または配置先の親
        auto* parent = instanceRoot.Parent();
        // 元の階層を置き換える新ルート
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

        // 実体がリンクするプリハブパス
        const auto& assetPath =
            instanceRoot.PrefabAssetPath();
        // リンク先プリハブの解決済みパス
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

        // 実体がリンクするプリハブパス
        const auto assetPath =
            instanceRoot.PrefabAssetPath();
        // 保存親の対応位置または配置先の親
        auto* parent = instanceRoot.Parent();
        // 元の階層を置き換える新ルート
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
