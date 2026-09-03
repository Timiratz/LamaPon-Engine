#pragma once

#include "LamaPon/Core/Api.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace LamaPon
{
    enum class SerializedDocumentKind
    {
        Scene,
        Prefab,
        ProjectSettings,
        PlayerPrefs,
        SaveData
    };

    struct DocumentMigrationReport final
    {
        std::uint32_t sourceVersion{};
        std::uint32_t targetVersion{};
        bool changed{};
    };

    inline constexpr std::uint32_t
        CurrentSerializedDocumentVersion = 1;

    [[nodiscard]] LAMAPON_API DocumentMigrationReport
        MigrateSerializedDocument(
            nlohmann::json& document,
            SerializedDocumentKind kind);

    [[nodiscard]] LAMAPON_API
        std::vector<std::filesystem::path>
        CollectSerializedAssetPaths(
            const nlohmann::json& document);

    LAMAPON_API void RefreshSerializedAssetManifest(
        nlohmann::json& document);
}
