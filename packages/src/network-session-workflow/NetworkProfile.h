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
    // Data Assetの登録型名
    inline constexpr auto ProfileType = "Network.ConnectionProfile";
    // 配布する初期接続設定
    inline constexpr auto DefaultProfilePath =
        "packages/network-session-workflow/profiles/DirectLocal.asset.json";

    // ConnectionProfileの入力欄と既定値
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

    // 共通のGame ID・通信版・EOS資格情報を保ち、秘密値を含まないScene設定を読みます(asset: 設定アセット, shared: 共通設定, output: 出力設定, error: 失敗理由)。
    [[nodiscard]] inline bool ReadProfile(const LamaPon::DataAsset& asset,
        const LamaPon::NetworkConfiguration& shared,
        LamaPon::NetworkConfiguration& output, std::string& error)
    {
        try
        {
            // Data Assetの型と内容を検証します。
            if (asset.TypeName() != ProfileType || asset.IsEmpty())
                throw std::invalid_argument("通信設定アセットの型または内容が不正です。");
            // NetworkConfigurationの既定値
            const LamaPon::NetworkConfiguration defaults;
            // Data Assetのvaluesオブジェクト
            const auto source = nlohmann::json::parse(asset.SerializeToJson()).at("values");
            // 共通設定を引き継ぐ出力候補
            auto configuration = shared;
            // 接続方式名
            const auto backend = source.value("backend", std::string("Direct"));
            // 未知の接続方式を拒否します。
            if (backend != "Direct" && backend != "Lan" && backend != "EOS")
                throw std::invalid_argument("未知の接続方式です。");
            configuration.backend = backend == "Direct" ? LamaPon::NetworkBackend::Direct
                : (backend == "Lan" ? LamaPon::NetworkBackend::Lan : LamaPon::NetworkBackend::EpicOnlineServices);
            // 同期方式名
            const auto mode = source.value("syncMode", std::string("Continuous"));
            // 未知の同期方式を拒否します。
            if (mode != "Continuous" && mode != "OnChange")
                throw std::invalid_argument("未知の同期方式です。");
            configuration.syncMode = mode == "OnChange" ? LamaPon::NetworkSyncMode::OnChange
                : LamaPon::NetworkSyncMode::Continuous;
            // 整数欄を検証します(key: 欄名, fallback: 未設定時の値)。
            const auto integer = [&source](const char* key, const std::uint32_t fallback)
            {
                // 未設定欄は既定値を使います。
                if (!source.contains(key)) return fallback;
                // 数値欄のJSON値
                const auto& field = source.at(key);
                // 数値以外を拒否します。
                if (!field.is_number()) throw std::invalid_argument("通信設定の整数項目が不正です。");
                // 整数性を調べる数値
                const auto number = field.get<double>();
                // 小数・範囲外は丸めず拒否します。
                if (!std::isfinite(number) || number < 0 || number > 65535
                    || std::floor(number) != number)
                    throw std::invalid_argument("通信設定の整数項目が範囲外です。");
                return static_cast<std::uint32_t>(number);
            };
            // 接続人数と送信頻度を反映します。
            configuration.maxPlayers = integer("maxPlayers", defaults.maxPlayers);
            configuration.tickRate = integer("tickRate", defaults.tickRate);
            configuration.port = static_cast<std::uint16_t>(integer("port", 0));
            configuration.discoveryPort = static_cast<std::uint16_t>(integer("discoveryPort", defaults.discoveryPort));
            configuration.sceneId = source.value("sceneId", defaults.sceneId);
            configuration.timeoutSeconds = source.value("timeoutSeconds", defaults.timeoutSeconds);
            configuration.roomName = source.value("roomName", defaults.roomName);
            configuration.advertiseLan = source.value("advertiseLan", defaults.advertiseLan);
            configuration.automaticPortMapping = source.value("automaticPortMapping", defaults.automaticPortMapping);
            // 同期Prefabの登録を再構築します。
            configuration.prefabs.clear();
            // 登録済みPrefabがある場合だけ読み込みます。
            if (source.contains("prefabs"))
            {
                // Data AssetのPrefab一覧
                const auto& prefabs = source.at("prefabs");
                // 配列と登録数を検証します。
                if (!prefabs.is_array() || prefabs.size() > 64)
                    throw std::invalid_argument("同期Prefabは64個まで登録できます。");
                // 登録するPrefab
                for (const auto& prefab : prefabs)
                    configuration.prefabs.push_back({prefab.at("key").get<std::string>(),
                        prefab.at("assetPath").get<std::string>()});
            }
            // 接続前に設定全体を検証します。
            LamaPon::ValidateNetworkConfiguration(configuration);
            output = std::move(configuration);
            error.clear();
            return true;
        }
        // 設定読込の失敗理由
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
    }
}
