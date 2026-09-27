#pragma once

#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Online/NetworkSession.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

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
            const LamaPon::NetworkConfiguration defaults;
            const auto source = nlohmann::json::parse(asset.SerializeToJson()).at("values");
            auto configuration = shared;
            const auto backend = source.value("backend", std::string("Direct"));
            if (backend != "Direct" && backend != "Lan" && backend != "EOS")
                throw std::invalid_argument("未知の接続方式です。");
            configuration.backend = backend == "Direct" ? LamaPon::NetworkBackend::Direct
                : (backend == "Lan" ? LamaPon::NetworkBackend::Lan : LamaPon::NetworkBackend::EpicOnlineServices);
            const auto mode = source.value("syncMode", std::string("Continuous"));
            if (mode != "Continuous" && mode != "OnChange")
                throw std::invalid_argument("未知の同期方式です。");
            configuration.syncMode = mode == "OnChange" ? LamaPon::NetworkSyncMode::OnChange
                : LamaPon::NetworkSyncMode::Continuous;
            // DataAssetは整数もdoubleとして保持します。丸めて設定を変えず、
            // 非整数・範囲外は接続前に拒否します。
            const auto integer = [&source](const char* key, const std::uint32_t fallback)
            {
                if (!source.contains(key)) return fallback;
                const auto& field = source.at(key);
                if (!field.is_number()) throw std::invalid_argument("通信設定の整数項目が不正です。");
                const auto number = field.get<double>();
                if (!std::isfinite(number) || number < 0 || number > 65535
                    || std::floor(number) != number)
                    throw std::invalid_argument("通信設定の整数項目が範囲外です。");
                return static_cast<std::uint32_t>(number);
            };
            configuration.maxPlayers = integer("maxPlayers", defaults.maxPlayers);
            configuration.tickRate = integer("tickRate", defaults.tickRate);
            configuration.port = static_cast<std::uint16_t>(integer("port", 0));
            configuration.discoveryPort = static_cast<std::uint16_t>(integer("discoveryPort", defaults.discoveryPort));
            configuration.sceneId = source.value("sceneId", defaults.sceneId);
            configuration.timeoutSeconds = source.value("timeoutSeconds", defaults.timeoutSeconds);
            configuration.roomName = source.value("roomName", defaults.roomName);
            configuration.advertiseLan = source.value("advertiseLan", defaults.advertiseLan);
            configuration.automaticPortMapping = source.value("automaticPortMapping", defaults.automaticPortMapping);
            configuration.prefabs.clear();
            if (source.contains("prefabs"))
            {
                const auto& prefabs = source.at("prefabs");
                if (!prefabs.is_array() || prefabs.size() > 64)
                    throw std::invalid_argument("同期Prefabは64個まで登録できます。");
                for (const auto& prefab : prefabs)
                    configuration.prefabs.push_back({prefab.at("key").get<std::string>(),
                        prefab.at("assetPath").get<std::string>()});
            }
            LamaPon::ValidateNetworkConfiguration(configuration);
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
