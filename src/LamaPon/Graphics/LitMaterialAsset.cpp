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

    // 発光色のようなRGB（3要素）用。ReadColorはRGBA固定なので
    // 別に用意します。
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

        const auto value =
            nlohmann::json::parse(bytes.begin(), bytes.end());
        if (value.value("type", std::string{}) != "LamaPonLitMaterial")
        {
            throw std::runtime_error(
                "Unsupported material asset: "
                + PathToUtf8(path));
        }

        const auto readReference =
            [&value, database](
                const std::string_view field)
            {
                const std::string fieldName(field);
                const auto fallback = PathFromUtf8(
                    value.value(
                        fieldName,
                        std::string{}));
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

        LitMaterial material{
            value.contains("baseColor")
                ? ReadColor(value.at("baseColor"))
                : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
            readReference("albedoTexture"),
            readReference("normalTexture"),
            value.value("roughness", 0.5f),
            value.value("normalStrength", 1.0f)
        };
        // 旧アセットには存在しないため既定0で後方互換。
        material.SetMetallic(
            value.value("metallic", 0.0f));
        // PBRマップと発光も旧アセットには無いので、未指定なら
        // 未設定（発光は黒＝発光なし）のままにします。
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
        // カスタムShaderの追加テクスチャ（t7以降）。旧アセットには
        // 無いため、無ければ未設定のままです。
        if (const auto textures = value.find("customTextures");
            textures != value.end()
            && textures->is_array())
        {
            const auto count = std::min(
                textures->size(),
                LitMaterial::CustomTextureCount);
            for (std::size_t index = 0;
                index < count;
                ++index)
            {
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
        if (const auto found = value.find("customParameters");
            found != value.end() && found->is_array())
        {
            const auto count = std::min(
                found->size(),
                LitMaterial::CustomParameterCount);
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

        const auto& color = material.BaseColor();
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
            const auto& emissive = material.EmissiveColor();
            value["emissiveColor"] = nlohmann::json::array({
                emissive.x,
                emissive.y,
                emissive.z
            });
        }
        value["shader"] = PathToUtf8(material.Shader());
        value["customTextures"] = nlohmann::json::array();
        for (const auto& texture : material.CustomTextures())
        {
            value["customTextures"].push_back(
                PathToUtf8(texture));
        }
        value["customParameters"] = nlohmann::json::array();
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
            const auto albedoGuid =
                database->GuidForPath(
                    material.AlbedoTexture());
            if (!albedoGuid.empty())
            {
                value["albedoTextureGuid"] =
                    albedoGuid;
            }
            const auto normalGuid =
                database->GuidForPath(
                    material.NormalTexture());
            if (!normalGuid.empty())
            {
                value["normalTextureGuid"] =
                    normalGuid;
            }
            const auto shaderGuid =
                database->GuidForPath(material.Shader());
            if (!shaderGuid.empty())
            {
                value["shaderGuid"] = shaderGuid;
            }
            // PBRマップと発光マップも、移動・改名で参照が切れない
            // ようGUIDを残します。
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
            for (const auto& [field, texturePath] :
                pbrReferences)
            {
                const auto guid =
                    database->GuidForPath(*texturePath);
                if (!guid.empty())
                {
                    value[field] = guid;
                }
            }
        }

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
