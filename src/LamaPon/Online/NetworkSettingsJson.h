#pragma once

#include "LamaPon/Online/NetworkSession.h"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace LamaPon::Detail
{
    inline nlohmann::json NetworkSettingsToJson(const NetworkConfiguration& settings)
    {
        nlohmann::json prefabs = nlohmann::json::array();
        for (const auto& prefab : settings.prefabs)
            prefabs.push_back({ { "key", prefab.key }, { "assetPath", prefab.assetPath } });
        return { { "backend", settings.backend == NetworkBackend::Lan ? "Lan" : (settings.backend == NetworkBackend::Direct ? "Direct" : "EOS") },
            { "gameId", settings.gameId }, { "gameVersion", settings.gameVersion },
            { "sceneId", settings.sceneId }, { "maxPlayers", settings.maxPlayers },
            { "tickRate", settings.tickRate }, { "timeoutSeconds", settings.timeoutSeconds },
            { "port", settings.port }, { "prefabs", prefabs },
            { "syncMode", settings.syncMode == NetworkSyncMode::OnChange ? "OnChange" : "Continuous" },
            { "automaticPortMapping", settings.automaticPortMapping }, { "advertiseLan", settings.advertiseLan },
            { "roomName", settings.roomName }, { "discoveryPort", settings.discoveryPort },
            { "eos", { { "productId", settings.eosProductId }, { "sandboxId", settings.eosSandboxId },
                { "deploymentId", settings.eosDeploymentId }, { "clientId", settings.eosClientId },
                { "clientSecretEnvironment", settings.eosClientSecretEnvironment } } } };
    }

    inline NetworkConfiguration NetworkSettingsFromJson(const nlohmann::json& value)
    {
        if (!value.is_object()) throw std::invalid_argument("Network settings must be an object.");
        NetworkConfiguration result;
        const auto backend = value.value("backend", std::string("Lan"));
        if (backend != "Lan" && backend != "EOS" && backend != "Direct") throw std::invalid_argument("Unknown network backend.");
        result.backend = backend == "Lan" ? NetworkBackend::Lan : (backend == "Direct" ? NetworkBackend::Direct : NetworkBackend::EpicOnlineServices);
        const auto mode = value.value("syncMode", std::string("Continuous"));
        if (mode != "Continuous" && mode != "OnChange") throw std::invalid_argument("Unknown network synchronization mode.");
        result.syncMode = mode == "OnChange" ? NetworkSyncMode::OnChange : NetworkSyncMode::Continuous;
        result.automaticPortMapping = value.value("automaticPortMapping", false);
        result.advertiseLan = value.value("advertiseLan", false);
        result.roomName = value.value("roomName", result.roomName);
        result.gameId = value.value("gameId", result.gameId);
        result.gameVersion = value.value("gameVersion", result.gameVersion);
        result.sceneId = value.value("sceneId", result.sceneId);
        const auto integer = [&value](const char* key, const std::uint32_t fallback)
        {
            if (!value.contains(key)) return fallback;
            const auto& field = value.at(key);
            if (!field.is_number_integer() || field.get<std::int64_t>() < 0
                || field.get<std::uint64_t>() > 65535) throw std::invalid_argument("Invalid network integer.");
            return field.get<std::uint32_t>();
        };
        result.maxPlayers = integer("maxPlayers", result.maxPlayers);
        result.tickRate = integer("tickRate", result.tickRate);
        result.port = static_cast<std::uint16_t>(integer("port", result.port));
        result.discoveryPort = static_cast<std::uint16_t>(integer("discoveryPort", result.discoveryPort));
        result.timeoutSeconds = value.value("timeoutSeconds", result.timeoutSeconds);
        if (value.contains("eos"))
        {
            const auto& eos = value.at("eos");
            if (!eos.is_object()) throw std::invalid_argument("EOS settings must be an object.");
            result.eosProductId = eos.value("productId", result.eosProductId);
            result.eosSandboxId = eos.value("sandboxId", result.eosSandboxId);
            result.eosDeploymentId = eos.value("deploymentId", result.eosDeploymentId);
            result.eosClientId = eos.value("clientId", result.eosClientId);
            result.eosClientSecretEnvironment = eos.value("clientSecretEnvironment", result.eosClientSecretEnvironment);
        }
        if (value.contains("prefabs"))
        {
            const auto& prefabs = value.at("prefabs");
            if (!prefabs.is_array() || prefabs.size() > 64) throw std::invalid_argument("Network prefab registry limit.");
            for (const auto& prefab : prefabs)
                result.prefabs.push_back({ prefab.at("key").get<std::string>(), prefab.at("assetPath").get<std::string>() });
        }
        ValidateNetworkConfiguration(result);
        return result;
    }
}
