#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Editor/DataAssetSchema.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right)
    {
        return std::fabs(left - right) < 0.0001f;
    }

    constexpr char SampleSchema[] = R"({
        "fields": [
            {
                "name": "displayName",
                "type": "string",
                "default": "スライム"
            },
            {
                "name": "hitPoints",
                "type": "int",
                "default": 10
            },
            {
                "name": "moveSpeed",
                "type": "float",
                "default": 2.5
            },
            {
                "name": "tintColor",
                "type": "color4"
            },
            {
                "name": "icon",
                "type": "asset",
                "assetType": "texture"
            },
            {
                "name": "dropRates",
                "type": "list",
                "item": { "type": "float" }
            },
            {
                "name": "waves",
                "type": "list",
                "item": {
                    "type": "object",
                    "fields": [
                        { "name": "count", "type": "int",
                          "default": 3 },
                        { "name": "delay", "type": "float",
                          "default": 1.5 }
                    ]
                }
            }
        ]
    })";
}

int main()
{
    try
    {
        // 値の読み出し。
        {
            constexpr char json[] = R"({
                "format": "LamaPonDataAsset",
                "version": 1,
                "type": "Game.EnemyData",
                "values": {
                    "displayName": "ゴブリン",
                    "hitPoints": 24,
                    "moveSpeed": 3.5,
                    "alive": true,
                    "tintColor": [0.25, 0.5, 0.75, 1.0],
                    "spawnPoint": [1.0, 2.0, 3.0],
                    "icon": "textures/goblin.png",
                    "dropRates": [0.5, 0.25, 0.125],
                    "waves": [
                        { "count": 3, "delay": 1.5 },
                        { "count": 5, "delay": 2.0 }
                    ]
                }
            })";
            const auto asset = LamaPon::DataAsset::FromJson(
                json,
                "goblin.asset.json");

            Require(
                asset.TypeName() == "Game.EnemyData",
                "The declared type name must round-trip.");
            Require(
                asset.Name() == "goblin.asset.json",
                "The asset name must be kept.");
            Require(
                asset.GetText("displayName") == "ゴブリン",
                "Text values must be readable.");
            Require(
                asset.GetInt("hitPoints") == 24,
                "Integer values must be readable.");
            Require(
                NearlyEqual(asset.GetFloat("moveSpeed"), 3.5f),
                "Float values must be readable.");
            Require(
                asset.GetBool("alive"),
                "Boolean values must be readable.");

            const auto color = asset.GetColor("tintColor");
            Require(
                NearlyEqual(color.x, 0.25f)
                    && NearlyEqual(color.y, 0.5f)
                    && NearlyEqual(color.z, 0.75f)
                    && NearlyEqual(color.w, 1.0f),
                "Colors must be read component-wise.");

            const auto position =
                asset.GetVector3("spawnPoint");
            Require(
                NearlyEqual(position.x, 1.0f)
                    && NearlyEqual(position.y, 2.0f)
                    && NearlyEqual(position.z, 3.0f),
                "Vectors must be read component-wise.");

            Require(
                asset.GetAssetPath("icon")
                    == std::filesystem::path{
                        "textures/goblin.png"
                    },
                "Asset references must resolve to a path.");

            Require(
                asset.Count("dropRates") == 3
                    && NearlyEqual(
                        asset.GetFloatAt("dropRates", 1),
                        0.25f),
                "Scalar lists must be readable by index.");
            Require(
                asset.Count("waves") == 2
                    && asset.Item("waves", 1).GetInt("count")
                        == 5
                    && NearlyEqual(
                        asset.Item("waves", 1)
                            .GetFloat("delay"),
                        2.0f),
                "Object lists must expose their fields.");
        }

        // 既定値と、壊れた入力での安全側の動作。
        {
            const auto broken =
                LamaPon::DataAsset::FromJson("{ oops");
            Require(
                broken.IsEmpty(),
                "A malformed data asset must parse as empty.");
            Require(
                broken.GetInt("missing", 7) == 7
                    && NearlyEqual(
                        broken.GetFloat("missing", 1.5f),
                        1.5f)
                    && broken.GetText("missing", "既定")
                        == "既定"
                    && !broken.GetBool("missing"),
                "Missing keys must fall back to defaults.");
            Require(
                broken.Count("missing") == 0
                    && broken.Item("missing", 0).IsEmpty(),
                "Out-of-range list access must stay safe.");
            Require(
                LamaPon::DataAsset::Empty().IsEmpty(),
                "The shared empty asset must stay empty.");
        }

        // 保存して読み直しても値が変わらないこと。
        {
            constexpr char json[] = R"({
                "type": "Game.Round",
                "values": {
                    "label": "第1章",
                    "counts": [1, 2, 3],
                    "waves": [ { "count": 4 } ]
                }
            })";
            const auto original =
                LamaPon::DataAsset::FromJson(json);
            const auto restored =
                LamaPon::DataAsset::FromJson(
                    original.SerializeToJson());
            Require(
                restored.TypeName() == "Game.Round"
                    && restored.GetText("label") == "第1章"
                    && restored.Count("counts") == 3
                    && restored.GetIntAt("counts", 2) == 3
                    && restored.Item("waves", 0)
                        .GetInt("count") == 4,
                "Data assets must survive a save and load.");
        }

        Require(
            LamaPon::IsDataAssetPath("data/cards/fire.asset.json")
                && !LamaPon::IsDataAssetPath(
                    "materials/wall.material.json")
                && !LamaPon::IsDataAssetPath("scenes/Main.json"),
            "Only *.asset.json files are data assets.");

        // 新規作成時のドキュメント（スキーマの既定値で埋まること）。
        {
            const auto document =
                LamaPon::EditorDetail::MakeDataAssetDocument(
                    "Game.EnemyData",
                    SampleSchema);
            Require(
                document.at("format") == "LamaPonDataAsset"
                    && document.at("type") == "Game.EnemyData",
                "A new data asset must carry its format and type.");

            const auto created =
                LamaPon::DataAsset::FromJson(document.dump());
            Require(
                created.GetText("displayName") == "スライム"
                    && created.GetInt("hitPoints") == 10
                    && NearlyEqual(
                        created.GetFloat("moveSpeed"),
                        2.5f),
                "Schema defaults must be written into new assets.");
            const auto tint = created.GetColor("tintColor");
            Require(
                NearlyEqual(tint.x, 0.0f)
                    && NearlyEqual(tint.w, 1.0f),
                "A color without a default must stay opaque black.");
            Require(
                created.GetText("icon").empty(),
                "Asset references must start empty.");
            Require(
                document.at("values").at("dropRates").is_array()
                    && document.at("values")
                        .at("dropRates")
                        .empty(),
                "Lists must start empty.");
        }

        // listへ足す要素の初期値。
        {
            const auto schema =
                nlohmann::json::parse(SampleSchema);
            const auto& waves = schema.at("fields").at(6);
            const auto element =
                LamaPon::EditorDetail::SchemaDefaultValue(
                    waves.at("item"));
            Require(
                element.is_object()
                    && element.at("count") == 3
                    && element.at("delay") == 1.5,
                "New list elements must use the item defaults.");
        }

        std::cout << "Data asset tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
