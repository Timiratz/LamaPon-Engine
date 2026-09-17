#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    inline constexpr std::uint32_t GraphicsBackendPackageAbiVersion = 1;
    inline constexpr char DirectX12BackendPackageName[] =
        "directx12-renderer";

    enum class GraphicsBackendPackageState : std::uint8_t
    {
        BuiltIn,
        Missing,
        InvalidManifest,
        IncompatibleEngine,
        IncompatibleAbi,
        RuntimeMissing,
        Ready
    };

    struct GraphicsBackendPackageDescriptor final
    {
        RenderingApi api{ RenderingApi::DirectX11 };
        std::uint32_t abiVersion{};
        std::string version;
        std::filesystem::path packageDirectory;
        std::filesystem::path runtimeLibrary;
    };

    struct GraphicsBackendPackageInspection final
    {
        GraphicsBackendPackageState state{
            GraphicsBackendPackageState::Missing };
        GraphicsBackendPackageDescriptor descriptor;
        std::string message;

        [[nodiscard]] bool IsReady() const noexcept
        {
            return state == GraphicsBackendPackageState::BuiltIn
                || state == GraphicsBackendPackageState::Ready;
        }
    };

    // 起動前に、選択APIに対応する任意バックエンドパッケージを
    // 検査します。DLLはここではロードせず、manifestと互換性だけを
    // 判定するため、壊れたパッケージでもD3D11へ安全に戻せます。
    [[nodiscard]] GraphicsBackendPackageInspection
        InspectGraphicsBackendPackage(
            const std::filesystem::path& assetRoot,
            RenderingApi api,
            std::string_view currentEngineVersion);
}
