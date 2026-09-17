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

    // GraphicsDevice初期化より前にプロジェクトのassetsを指定します。
    // 未指定なら従来どおり組み込みBackendを使用します。
    void SetGraphicsBackendPackageAssetRoot(
        std::filesystem::path assetRoot);

    // manifestとABI entry pointを検証し、プロセス終了までDLLを保持します。
    // パッケージが無い場合はMissingを返し、移行期間中の組み込みD3D12を
    // 継続利用できます。存在するのに壊れている場合はReadyになりません。
    [[nodiscard]] GraphicsBackendPackageInspection
        ActivateGraphicsBackendPackage(
            RenderingApi api,
            std::string_view currentEngineVersion);
}
