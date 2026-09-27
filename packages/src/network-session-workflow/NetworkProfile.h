#pragma once

#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Online/NetworkSettingsJson.h"

#include <cmath>
#include <string>

namespace LamaPonNetworkWorkflow
{
    inline constexpr auto ProfileType = "Network.ConnectionProfile";
    inline constexpr auto DefaultProfilePath =
        "packages/network-session-workflow/profiles/DirectLocal.asset.json";

    inline constexpr char ProfileSchema[] = R"schema({"fields":[
        {"name":"backend","displayName":"接続方式","type":"string","default":"Direct",
         "options":[{"value":"Direct","displayName":"直接接続（暗号化）"},
                    {"value":"Lan","displayName":"LAN / 同じPC"},
                    {"value":"EOS","displayName":"Epic Online Services"}]},
        {"name":"sceneId","displayName":"通信シーンID","type":"string","default":"main"},
        {"name":"maxPlayers","displayName":"最大人数（ホストを含む）","type":"int","default":4,"min":2,"max":4},
        {"name":"tickRate","displayName":"状態送信の頻度（Hz）","type":"int","default":20,"min":1,"max":60},
        {"name":"syncMode","displayName":"オブジェクトの同期","type":"string","default":"Continuous",
         "options":[{"value":"Continuous","displayName":"定期送信"},{"value":"OnChange","displayName":"変更時だけ"}]},
        {"name":"timeoutSeconds","displayName":"切断待ち時間（秒）","type":"float","default":15,"min":5,"max":120},
        {"name":"port","displayName":"待受ポート（0は自動）","type":"int","default":0,"min":0,"max":65535},
        {"name":"roomName","displayName":"部屋の名前","type":"string","default":"Room"},
        {"name":"advertiseLan","displayName":"LAN検索へ公開する","type":"bool","default":false},
        {"name":"discoveryPort","displayName":"LAN検索ポート","type":"int","default":27841,"min":1,"max":65535},
        {"name":"automaticPortMapping","displayName":"UPnPを利用する","type":"bool","default":false},
        {"name":"prefabs","displayName":"同期Prefabの登録","type":"list","item":{"type":"object","fields":[
            {"name":"key","displayName":"キー","type":"string","default":"player"},
            {"name":"assetPath","displayName":"Prefab","type":"asset","assetType":"prefab","default":""}]},"default":[]}
    ]})schema";

    // プロジェクト共通のゲームID・通信バージョン・EOS資格情報は引き継ぎ、
    // Scene固有の条件だけを上書きします。接続先や秘密キーはアセットに置きません。
    [[nodiscard]] inline bool ReadProfile(const LamaPon::DataAsset& asset,
        const LamaPon::NetworkConfiguration& shared,
        LamaPon::NetworkConfiguration& output, std::string& error)
    {
        try
        {
            if (asset.TypeName() != ProfileType || asset.IsEmpty())
                throw std::invalid_argument("通信設定アセットの型または内容が不正です。");
            auto defaults = LamaPon::NetworkConfiguration{};
            defaults.backend = LamaPon::NetworkBackend::Direct;
            defaults.port = 0;
            auto values = LamaPon::Detail::NetworkSettingsToJson(defaults);
            const auto source = nlohmann::json::parse(asset.SerializeToJson()).at("values");
            for (const auto* key : {"backend", "sceneId", "maxPlayers", "tickRate",
                "syncMode", "timeoutSeconds", "port", "roomName", "advertiseLan",
                "discoveryPort", "automaticPortMapping", "prefabs"})
            {
                if (source.contains(key)) values[key] = source.at(key);
            }
            // DataAssetは整数もdoubleとして保持します。丸めて設定を変えず、
            // 非整数・範囲外は接続前に拒否します。
            for (const auto* key : {"maxPlayers", "tickRate", "port", "discoveryPort"})
            {
                const auto& field = values.at(key);
                if (!field.is_number()) throw std::invalid_argument("通信設定の整数項目が不正です。");
                const auto number = field.get<double>();
                if (!std::isfinite(number) || number < 0 || number > 65535
                    || std::floor(number) != number)
                    throw std::invalid_argument("通信設定の整数項目が範囲外です。");
                values[key] = static_cast<std::uint32_t>(number);
            }
            const auto common = LamaPon::Detail::NetworkSettingsToJson(shared);
            for (const auto* key : {"gameId", "gameVersion", "eos"}) values[key] = common.at(key);
            auto configuration = LamaPon::Detail::NetworkSettingsFromJson(values);
            output = std::move(configuration);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
}
