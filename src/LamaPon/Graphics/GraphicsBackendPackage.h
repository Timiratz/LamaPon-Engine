#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace LamaPon
{
    // 描画パッケージのABI版
    inline constexpr std::uint32_t GraphicsBackendPackageAbiVersion = 1;
    // D3D12描画パッケージの名前
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
        // パッケージの描画API
        RenderingApi api{ RenderingApi::DirectX11 };
        // パッケージのABI版
        std::uint32_t abiVersion{};
        // パッケージの公開版
        std::string version;
        // パッケージの配置先
        std::filesystem::path packageDirectory;
        // 読み込む描画DLLのパス
        std::filesystem::path runtimeLibrary;
    };

    struct GraphicsBackendPackageInspection final
    {
        // 描画パッケージの検査結果
        GraphicsBackendPackageState state{
            GraphicsBackendPackageState::Missing };
        // 検査したパッケージの情報
        GraphicsBackendPackageDescriptor descriptor;
        // 検査結果の説明
        std::string message;

        // 組み込みまたは検査済みの利用可能な状態か返す。
        [[nodiscard]] bool IsReady() const noexcept
        {
            return state == GraphicsBackendPackageState::BuiltIn
                || state == GraphicsBackendPackageState::Ready;
        }
    };


    // DLLをロードせずパッケージの宣言と配置を検査する(assetRoot: アセットの基点, api: 選択する描画API, currentEngineVersion: 現行エンジンの版)。
    // D3D12以外は組み込みD3D11として扱い、ファイル存在確認の例外は外へ伝播し得る。
    [[nodiscard]] GraphicsBackendPackageInspection
        InspectGraphicsBackendPackage(
            const std::filesystem::path& assetRoot,
            RenderingApi api,
            std::string_view currentEngineVersion);


    // 初期化前にパッケージ検索の基点を指定する(assetRoot: アセットの基点)。
    void SetGraphicsBackendPackageAssetRoot(
        std::filesystem::path assetRoot);


    // 宣言とDLLのABIを確認して描画DLLを保持する(api: 選択する描画API, currentEngineVersion: 現行エンジンの版)。
    // 成功したDLLはプロセス終了まで保持し、別のDLLへの切替えは拒否する。
    [[nodiscard]] GraphicsBackendPackageInspection
        ActivateGraphicsBackendPackage(
            RenderingApi api,
            std::string_view currentEngineVersion);
}
