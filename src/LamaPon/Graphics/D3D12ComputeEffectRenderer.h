#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace LamaPon
{
    class AssetManager;
    class D3D12Backend;
    class RenderTarget;
}

namespace LamaPon::Detail
{
    class D3D12ComputeEffectRenderer final
    {
    public:
        // 計算効果の入力契約を生成する(backend: 寿命が長い初期化済み基盤)。
        explicit D3D12ComputeEffectRenderer(D3D12Backend& backend);
        // 計算パイプラインとルート署名を解放する。
        ~D3D12ComputeEffectRenderer() noexcept;

        // 計算効果の複製を禁止する。
        D3D12ComputeEffectRenderer(
            const D3D12ComputeEffectRenderer&) = delete;
        // 計算効果の複製代入を禁止する。
        D3D12ComputeEffectRenderer& operator=(
            const D3D12ComputeEffectRenderer&) = delete;


        // 計算効果を更新し、正常なパイプラインの有無を返す(assets: アセット管理, shaderPath: ソースパス, describeFailure: 失敗説明の変換処理, error: 任意の失敗説明出力)。
        // 保存時刻を250ミリ秒ごとに確認し、読込・生成の失敗時も直前の正常な版を保持する。
        [[nodiscard]] bool Prepare(
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            const std::function<std::string(const char*)>& describeFailure,
            std::string* error);

        // 準備済みCSMainで表示面へ書き込む(assets: アセット管理, shaderPath: ソースパス, output: 計算書込み可能な描画先, inputs: 二つの中立入力参照, parameters: 八つの独自定数)。
        // 入力は現在の基盤の世代に属する参照とし、HLSL側で八対八グループの範囲外を除外する。
        void Dispatch(
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            RenderTarget& output,
            const std::array<GraphicsViewHandle, 2>& inputs,
            const std::array<DirectX::XMFLOAT4, 8>& parameters);

        // 次回のPrepareで再生成を試す(assets: アセット管理, shaderPath: 再生成するソースパス)。
        void Invalidate(
            AssetManager& assets,
            const std::filesystem::path& shaderPath) noexcept;

    private:
        struct ShaderEntry final
        {
            // 直近の正常な計算パイプライン
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
            // 直近の再生成失敗の説明
            std::string error;
            // 次回ソース確認時刻
            std::chrono::steady_clock::time_point nextCheck{};
            // 確認したソース更新時刻
            std::filesystem::file_time_type writeTime{};
            // ソース確認実施済み
            bool observed{};
            // 次回確認で再生成する
            bool forceReload{};
            // 確認時のソース存在有無
            bool sourceExists{};
        };

        // ComputeEffectの定数と同じ144バイトの配置を維持する。
        struct Constants final
        {
            // 八つの独自定数
            std::array<DirectX::XMFLOAT4, 8> parameters{};
            // 出力の幅・高さと各逆数
            DirectX::XMFLOAT4 outputSize{};
        };
        static_assert(sizeof(Constants) == 144u);

        // 借用するD3D12の描画基盤
        D3D12Backend* m_backend{};
        // 計算処理のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        // 絶対パス別の計算効果
        std::unordered_map<std::filesystem::path, ShaderEntry> m_shaders;
    };
}
