#include "LamaPon/Graphics/LitMaterialAsset.h"

#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // 四数値のRGBA色を読み、不正なら例外を送出する(value: 色のJSON配列)。
    DirectX::XMFLOAT4 ReadColor(const nlohmann::json& value)
    {
        if (!value.is_array() || value.size() != 4)
        {
            throw std::runtime_error(
                "Material baseColor must contain four numbers.");
        }

        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>(),
            value.at(3).get<float>()
        };
    }


    // 三数値のRGB色を読み、不正なら例外を送出する(value: 色のJSON配列, field: エラーに付ける項目名)。
    DirectX::XMFLOAT3 ReadColor3(
        const nlohmann::json& value,
        const char* field)
    {
        if (!value.is_array() || value.size() != 3)
        {
            throw std::runtime_error(
                std::string{ field }
                + " must contain three numbers.");
        }

        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>()
        };
    }

    // 四数値の独自定数を読み、不正なら例外を送出する(value: 定数のJSON配列, field: エラーに付ける項目名)。
    DirectX::XMFLOAT4 ReadFloat4(
        const nlohmann::json& value,
        const char* field)
    {
        if (!value.is_array() || value.size() != 4)
        {
            throw std::runtime_error(
                std::string{ field }
                + " must contain four numbers.");
        }
        return {
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>(),
            value.at(3).get<float>()
        };
    }
}

namespace LamaPon
{
    LitMaterial LoadLitMaterialAsset(
        const std::filesystem::path& path,
        const AssetDatabase* database,
        AssetManager* assets)
    {
        // 読み込んだ材質のJSON列
        std::vector<std::uint8_t> bytes;
        if (assets != nullptr)
        {
            if (!assets->FileExists(path))
            {
                throw std::runtime_error(
                    "Material asset could not be opened: "
                    + PathToUtf8(path));
            }
            bytes = assets->ReadFileBytes(path);
        }
        else
        {
            // 直接読む材質ファイル
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                throw std::runtime_error(
                    "Material asset could not be opened: "
                    + PathToUtf8(path));
            }
            bytes.assign(
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        }

        // 材質のJSON値
        const auto value =
            nlohmann::json::parse(bytes.begin(), bytes.end());
        if (value.value("type", std::string{}) != "LamaPonLitMaterial")
        {
            throw std::runtime_error(
                "Unsupported material asset: "
                + PathToUtf8(path));
        }

        // GUIDを優先して参照パスを読む(field: JSONのフィールド名)。
        const auto readReference =
            [&value, database](
                const std::string_view field)
            {
                // 参照パスのJSON項目名
                const std::string fieldName(field);
                // GUID未解決時の参照パス
                const auto fallback = PathFromUtf8(
                    value.value(
                        fieldName,
                        std::string{}));
                // 参照先のGUID
                const auto guid = value.value(
                    fieldName + "Guid",
                    std::string{});
                return database != nullptr
                    && !guid.empty()
                    ? database->ResolveGuid(
                        guid,
                        fallback)
                    : fallback;
            };

        // 読み込む材質
        LitMaterial material{
            value.contains("baseColor")
                ? ReadColor(value.at("baseColor"))
                : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
            readReference("albedoTexture"),
            readReference("normalTexture"),
            value.value("roughness", 0.5f),
            value.value("normalStrength", 1.0f)
        };
        // 旧アセットにない金属度はゼロとして読み込む。
        material.SetMetallic(
            value.value("metallic", 0.0f));
        // 未指定のPBR画像は空、発光色は黒として旧アセットとの互換性を保つ。
        material.SetRoughnessTexture(
            readReference("roughnessTexture"));
        material.SetMetallicTexture(
            readReference("metallicTexture"));
        material.SetOcclusionTexture(
            readReference("occlusionTexture"));
        material.SetOcclusionStrength(
            value.value("occlusionStrength", 1.0f));
        material.SetEmissiveTexture(
            readReference("emissiveTexture"));
        if (value.contains("emissiveColor"))
        {
            material.SetEmissiveColor(
                ReadColor3(
                    value.at("emissiveColor"),
                    "Material emissiveColor"));
        }
        material.SetShader(readReference("shader"));
        // 旧アセットで追加画像が省略されている場合は未設定のままにする。
        // 追加画像のJSON配列
        if (const auto textures = value.find("customTextures");
            textures != value.end()
            && textures->is_array())
        {
            // 読み込む要素数の上限
            const auto count = std::min(
                textures->size(),
                LitMaterial::CustomTextureCount);
            // 追加画像・定数の番号
            for (std::size_t index = 0;
                // 読み込む要素数の上限
                index < count;
                ++index)
            {
                // 追加画像のJSON要素
                const auto& entry = textures->at(index);
                if (!entry.is_string())
                {
                    continue;
                }
                material.SetCustomTexture(
                    index,
                    PathFromUtf8(
                        entry.get<std::string>()));
            }
        }
        // 追加定数のJSON配列
        if (const auto found = value.find("customParameters");
            found != value.end() && found->is_array())
        {
            // 読み込む要素数の上限
            const auto count = std::min(
                found->size(),
                LitMaterial::CustomParameterCount);
            // 追加画像・定数の番号
            for (std::size_t index = 0; index < count; ++index)
            {
                material.SetCustomParameter(
                    index,
                    ReadFloat4(
                        found->at(index),
                        "Material custom parameter"));
            }
        }
        return material;
    }

    void SaveLitMaterialAsset(
        const std::filesystem::path& path,
        const LitMaterial& material,
        const AssetDatabase* database)
    {
        if (path.empty())
        {
            throw std::invalid_argument(
                "Material asset path is empty.");
        }

        // 保存する材質の基準色
        const auto& color = material.BaseColor();
        // 材質のJSON値
        nlohmann::json value{
            { "type", "LamaPonLitMaterial" },
            { "version", 2 },
            {
                "baseColor",
                nlohmann::json::array({
                    color.x,
                    color.y,
                    color.z,
                    color.w
                })
            },
            {
                "albedoTexture",
                PathToUtf8(material.AlbedoTexture())
            },
            {
                "normalTexture",
                PathToUtf8(material.NormalTexture())
            },
            { "roughness", material.Roughness() },
            { "normalStrength", material.NormalStrength() },
            { "metallic", material.Metallic() }
        };
        value["roughnessTexture"] =
            PathToUtf8(material.RoughnessTexture());
        value["metallicTexture"] =
            PathToUtf8(material.MetallicTexture());
        value["occlusionTexture"] =
            PathToUtf8(material.OcclusionTexture());
        value["occlusionStrength"] =
            material.OcclusionStrength();
        value["emissiveTexture"] =
            PathToUtf8(material.EmissiveTexture());
        {
            // 保存する発光色
            const auto& emissive = material.EmissiveColor();
            value["emissiveColor"] = nlohmann::json::array({
                emissive.x,
                emissive.y,
                emissive.z
            });
        }
        value["shader"] = PathToUtf8(material.Shader());
        value["customTextures"] = nlohmann::json::array();
        // 保存する追加画像のパス
        for (const auto& texture : material.CustomTextures())
        {
            value["customTextures"].push_back(
                PathToUtf8(texture));
        }
        value["customParameters"] = nlohmann::json::array();
        // 保存する追加定数
        for (const auto& parameter : material.CustomParameters())
        {
            value["customParameters"].push_back(
                nlohmann::json::array({
                    parameter.x,
                    parameter.y,
                    parameter.z,
                    parameter.w
                }));
        }
        if (database != nullptr)
        {
            // 基準色画像のGUID
            const auto albedoGuid =
                database->GuidForPath(
                    material.AlbedoTexture());
            if (!albedoGuid.empty())
            {
                value["albedoTextureGuid"] =
                    albedoGuid;
            }
            // 法線画像のGUID
            const auto normalGuid =
                database->GuidForPath(
                    material.NormalTexture());
            if (!normalGuid.empty())
            {
                value["normalTextureGuid"] =
                    normalGuid;
            }
            // シェーダーのGUID
            const auto shaderGuid =
                database->GuidForPath(material.Shader());
            if (!shaderGuid.empty())
            {
                value["shaderGuid"] = shaderGuid;
            }
            // 画像の移動や改名に追従できるよう、パスと一緒にGUIDを保存する。
            // GUIDを保存するPBR画像の組
            const std::pair<
                const char*,
                const std::filesystem::path*> pbrReferences[]{
                {
                    "roughnessTextureGuid",
                    &material.RoughnessTexture()
                },
                {
                    "metallicTextureGuid",
                    &material.MetallicTexture()
                },
                {
                    "occlusionTextureGuid",
                    &material.OcclusionTexture()
                },
                {
                    "emissiveTextureGuid",
                    &material.EmissiveTexture()
                }
            };
            // field: GUIDのJSON項目名、texturePath: 参照画像のパス
            for (const auto& [field, texturePath] :
                pbrReferences)
            {
                // 参照先のGUID
                const auto guid =
                    database->GuidForPath(*texturePath);
                if (!guid.empty())
                {
                    value[field] = guid;
                }
            }
        }

        // 上書き保存する材質ファイル
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Material asset could not be saved: "
                + PathToUtf8(path));
        }
        output << value.dump(2) << '\n';
        if (!output)
        {
            throw std::runtime_error(
                "Material asset write failed: "
                + PathToUtf8(path));
        }
    }
}
