#include "LamaPon/Core/DocumentMigration.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    constexpr std::array<std::string_view, 15>
        AssetReferenceFields{
            "albedoTexture",
            "normalTexture",
            "shader",
            "materialAsset",
            "texture",
            "targetScene",
            "audio",
            "model",
            "animationController",
            "clip",
            "controller",
            "prefabAsset",
            "fontAsset",
            "mesh",
            "sourceAsset"
        };

    std::string_view ExpectedFormat(
        const LamaPon::SerializedDocumentKind kind)
    {
        switch (kind)
        {
        case LamaPon::SerializedDocumentKind::Scene:
            return "LamaPonScene";
        case LamaPon::SerializedDocumentKind::Prefab:
            return "LamaPonPrefab";
        case LamaPon::SerializedDocumentKind::PlayerPrefs:
            return "LamaPonPlayerPrefs";
        case LamaPon::SerializedDocumentKind::SaveData:
            return "LamaPonSaveData";
        default:
            return {};
        }
    }

    void ValidateFormat(
        const Json& document,
        const LamaPon::SerializedDocumentKind kind)
    {
        const auto format =
            document.value("format", std::string{});
        if (kind
            == LamaPon::SerializedDocumentKind::ProjectSettings)
        {
            if (format != "LamaPonProject"
                && format != "LamaPonGame")
            {
                throw std::runtime_error(
                    "Unsupported LamaPon project document.");
            }
            return;
        }

        if (format != ExpectedFormat(kind))
        {
            throw std::runtime_error(
                "Serialized LamaPon document has an unexpected format.");
        }
    }

    Json TypedPreferenceValue(const Json& value)
    {
        if (value.is_boolean())
        {
            return Json{
                { "type", "boolean" },
                { "value", value }
            };
        }
        if (value.is_number_integer()
            || value.is_number_unsigned())
        {
            return Json{
                { "type", "integer" },
                { "value", value }
            };
        }
        if (value.is_number_float())
        {
            return Json{
                { "type", "number" },
                { "value", value }
            };
        }
        if (value.is_string())
        {
            return Json{
                { "type", "string" },
                { "value", value }
            };
        }
        return value;
    }

    void MigrateVersionZero(
        Json& document,
        const LamaPon::SerializedDocumentKind kind)
    {
        switch (kind)
        {
        case LamaPon::SerializedDocumentKind::Scene:
        case LamaPon::SerializedDocumentKind::Prefab:
            if (!document.contains("objects")
                && document.contains("entities"))
            {
                document["objects"] =
                    std::move(document["entities"]);
                document.erase("entities");
            }
            break;

        case LamaPon::SerializedDocumentKind::ProjectSettings:
            if (!document.contains("gameName")
                && document.contains("title"))
            {
                document["gameName"] =
                    std::move(document["title"]);
                document.erase("title");
            }
            if (!document.contains("window")
                && (document.contains("windowWidth")
                    || document.contains("windowHeight")))
            {
                document["window"] = Json::object();
                if (document.contains("windowWidth"))
                {
                    document["window"]["width"] =
                        std::move(document["windowWidth"]);
                    document.erase("windowWidth");
                }
                if (document.contains("windowHeight"))
                {
                    document["window"]["height"] =
                        std::move(document["windowHeight"]);
                    document.erase("windowHeight");
                }
            }
            break;

        case LamaPon::SerializedDocumentKind::PlayerPrefs:
            if (auto values = document.find("values");
                values != document.end()
                && values->is_object())
            {
                for (auto& [key, value] : values->items())
                {
                    static_cast<void>(key);
                    if (!value.is_object()
                        || !value.contains("type"))
                    {
                        value = TypedPreferenceValue(value);
                    }
                }
            }
            break;

        case LamaPon::SerializedDocumentKind::SaveData:
            if (!document.contains("data")
                && document.contains("payload"))
            {
                document["data"] =
                    std::move(document["payload"]);
                document.erase("payload");
            }
            break;
        }

        document["version"] =
            LamaPon::CurrentSerializedDocumentVersion;
    }

    bool IsAssetReferenceField(
        const std::string_view field)
    {
        return std::ranges::find(
            AssetReferenceFields,
            field) != AssetReferenceFields.end();
    }

    std::wstring PathKey(
        const std::filesystem::path& path)
    {
        auto key = path.lexically_normal().wstring();
        std::ranges::transform(
            key,
            key.begin(),
            std::towlower);
        return key;
    }

    void AddPath(
        const Json& value,
        std::set<std::wstring>& keys,
        std::vector<std::filesystem::path>& paths)
    {
        if (!value.is_string())
        {
            return;
        }
        const auto text = value.get<std::string>();
        if (text.empty() || text.size() > 32768)
        {
            return;
        }
        const auto path =
            LamaPon::PathFromUtf8(text).lexically_normal();
        if (path.empty())
        {
            return;
        }
        if (keys.emplace(PathKey(path)).second)
        {
            paths.push_back(path);
        }
    }

    void CollectPaths(
        const Json& value,
        std::set<std::wstring>& keys,
        std::vector<std::filesystem::path>& paths)
    {
        if (paths.size() >= 4096)
        {
            return;
        }
        if (value.is_array())
        {
            for (const auto& child : value)
            {
                CollectPaths(child, keys, paths);
            }
            return;
        }
        if (!value.is_object())
        {
            return;
        }

        for (const auto& [field, child] : value.items())
        {
            if (field == "assetManifest"
                && child.is_array())
            {
                for (const auto& entry : child)
                {
                    AddPath(entry, keys, paths);
                }
            }
            else if (IsAssetReferenceField(field))
            {
                AddPath(child, keys, paths);
            }

            if (field != "assetManifest")
            {
                CollectPaths(child, keys, paths);
            }
        }
    }
}

namespace LamaPon
{
    DocumentMigrationReport MigrateSerializedDocument(
        nlohmann::json& document,
        const SerializedDocumentKind kind)
    {
        if (!document.is_object())
        {
            throw std::runtime_error(
                "Serialized LamaPon document must be a JSON object.");
        }
        ValidateFormat(document, kind);

        const auto versionValue =
            document.find("version");
        if (versionValue != document.end()
            && !versionValue->is_number_unsigned()
            && !versionValue->is_number_integer())
        {
            throw std::runtime_error(
                "Serialized LamaPon document has an invalid version.");
        }

        const auto signedVersion =
            document.value("version", std::int64_t{});
        if (signedVersion < 0
            || signedVersion
                > CurrentSerializedDocumentVersion)
        {
            throw std::runtime_error(
                "Serialized LamaPon document requires a newer engine.");
        }

        DocumentMigrationReport report{
            static_cast<std::uint32_t>(signedVersion),
            static_cast<std::uint32_t>(signedVersion),
            false
        };
        if (signedVersion == 0)
        {
            MigrateVersionZero(document, kind);
            report.targetVersion =
                CurrentSerializedDocumentVersion;
            report.changed = true;
        }
        return report;
    }

    std::vector<std::filesystem::path>
        CollectSerializedAssetPaths(
            const nlohmann::json& document)
    {
        std::set<std::wstring> keys;
        std::vector<std::filesystem::path> paths;
        CollectPaths(document, keys, paths);
        return paths;
    }

    void RefreshSerializedAssetManifest(
        nlohmann::json& document)
    {
        document.erase("assetManifest");
        const auto paths =
            CollectSerializedAssetPaths(document);
        document["assetManifest"] =
            nlohmann::json::array();
        for (const auto& path : paths)
        {
            document["assetManifest"].push_back(
                PathToUtf8(path));
        }
    }
}
