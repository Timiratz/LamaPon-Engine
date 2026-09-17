#pragma once

// DirectX 12 ExperimentalでプロジェクトのCompute Shader（CSMain）を
// 名前付きRenderTextureへ書くpipelineです。D3D11のComputeEffectと同じ
// b0／t0／t1／s0／u0の契約で実行します。Runtime内部headerで、SDKには
// installしません。
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
        explicit D3D12ComputeEffectRenderer(D3D12Backend& backend);
        ~D3D12ComputeEffectRenderer() noexcept;

        D3D12ComputeEffectRenderer(
            const D3D12ComputeEffectRenderer&) = delete;
        D3D12ComputeEffectRenderer& operator=(
            const D3D12ComputeEffectRenderer&) = delete;

        // D3D11と同じく保存を250ミリ秒ごとに確かめて作り直し、compileに
        // 失敗したときはdescribeFailureの説明を返しつつ直前の正常版を
        // 保ちます。使えるpipelineがあればtrueです。
        [[nodiscard]] bool Prepare(
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            const std::function<std::string(const char*)>& describeFailure,
            std::string* error);
        // Prepare済みのshaderで、outputの表示用textureへ書きます。
        // inputsはt0／t1へ入る、現在のBackend世代のShaderResource viewです。
        void Dispatch(
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            RenderTarget& output,
            const std::array<GraphicsViewHandle, 2>& inputs,
            const std::array<DirectX::XMFLOAT4, 8>& parameters);
        // 次のPrepareで、保存時刻に関係なく作り直させます。
        void Invalidate(
            AssetManager& assets,
            const std::filesystem::path& shaderPath) noexcept;

    private:
        struct ShaderEntry final
        {
            Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
            std::string error;
            // D3D11のComputeShaderEntryと同じ保存監視の状態です。
            std::chrono::steady_clock::time_point nextCheck{};
            std::filesystem::file_time_type writeTime{};
            bool observed{};
            bool forceReload{};
            bool sourceExists{};
        };

        // ComputeEffect.hのConstantsと同じ144 bytesです。
        struct Constants final
        {
            std::array<DirectX::XMFLOAT4, 8> parameters{};
            // xy=出力の幅と高さ, zw=その逆数。
            DirectX::XMFLOAT4 outputSize{};
        };
        static_assert(sizeof(Constants) == 144u);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        std::unordered_map<std::filesystem::path, ShaderEntry> m_shaders;
    };
}
