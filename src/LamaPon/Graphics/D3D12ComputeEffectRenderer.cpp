#include "LamaPon/Graphics/D3D12ComputeEffectRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
    // HLSLのnumthreadsと同じ八対八対一のグループ寸法を使う。
    // 計算グループの一辺の寸法
    constexpr std::uint32_t ThreadGroupSize = 8u;

    // 失敗したHRESULTを例外に変える(result: 実行結果, operation: 操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }
}

namespace LamaPon::Detail
{
    D3D12ComputeEffectRenderer::D3D12ComputeEffectRenderer(
        D3D12Backend& backend)
        : m_backend(&backend)
    {
        // 借用する描画デバイス
        auto* const device = backend.Device();
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 compute effect renderer requires an "
                "initialized backend.");
        }

        // b0の定数・t0とt1の入力・u0の出力・s0の線形端固定サンプラーを渡す。
        // 二入力と出力の参照範囲
        std::array<D3D12_DESCRIPTOR_RANGE, 3> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 0;
        // ルート引数の配列
        std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // 参照範囲の要素番号
        for (std::size_t index{}; index < ranges.size(); ++index)
        {
            // 設定するルート引数
            auto& parameter = parameters[index + 1u];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // 線形端固定サンプラーの仕様
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // ルート署名の仕様
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        // 直列化したルート署名
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        // ルート署名の生成エラー列
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // ルート署名の直列化結果
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            // ルート署名の生成失敗説明
            std::string message =
                "D3D12SerializeRootSignature(compute effect) failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(compute effect)");
    }

    D3D12ComputeEffectRenderer::~D3D12ComputeEffectRenderer() noexcept =
        default;

    bool D3D12ComputeEffectRenderer::Prepare(
        AssetManager& assets,
        const std::filesystem::path& shaderPath,
        const std::function<std::string(const char*)>& describeFailure,
        std::string* const error)
    {
        if (error != nullptr)
        {
            error->clear();
        }
        if (shaderPath.empty())
        {
            return false;
        }

        // 正規化した絶対ソースパス
        const auto absolutePath =
            assets.ResolvePath(shaderPath).lexically_normal();
        // 対象の計算効果キャッシュ
        auto& entry = m_shaders[absolutePath];

        // 保存時刻を定期確認し、再生成が失敗しても直前の正常なパイプラインを保持する。
        // 更新を確認する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (!entry.observed
            || entry.forceReload
            || now >= entry.nextCheck)
        {
            entry.nextCheck = now + std::chrono::milliseconds(250);
            // アーカイブを使用している
            const bool archived = assets.IsArchived();
            // 更新時刻の取得エラー
            std::error_code fileError;
            // ソースが存在する
            const bool sourceExists = assets.FileExists(absolutePath);
            // ソースの更新時刻
            const auto writeTime = (sourceExists && !archived)
                ? std::filesystem::last_write_time(absolutePath, fileError)
                : std::filesystem::file_time_type{};
            // 計算効果の再生成が必要
            const bool changed = !entry.observed
                || entry.forceReload
                || entry.sourceExists != sourceExists
                || (sourceExists
                    && !archived
                    && entry.writeTime != writeTime);
            if (changed)
            {
                entry.observed = true;
                entry.forceReload = false;
                entry.sourceExists = sourceExists;
                entry.writeTime = writeTime;
                if (!sourceExists)
                {
                    entry.error =
                        "Compute effect shader file was not found: "
                        + PathToUtf8(absolutePath);
                }
                else
                {
                    try
                    {
                        // CSMainのバイトコード
                        const auto byteCode = CompileShaderCached(
                            assets,
                            absolutePath,
                            "CSMain",
                            "cs_5_0");
                        // 計算パイプラインの仕様
                        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
                        description.pRootSignature = m_rootSignature.Get();
                        description.CS = {
                            byteCode->GetBufferPointer(),
                            byteCode->GetBufferSize()
                        };
                        // 生成中の計算パイプライン
                        Microsoft::WRL::ComPtr<ID3D12PipelineState>
                            pipelineState;
                        ThrowIfFailed(
                            m_backend->Device()->CreateComputePipelineState(
                                &description,
                                IID_PPV_ARGS(
                                    pipelineState.ReleaseAndGetAddressOf())),
                            "ID3D12Device::CreateComputePipelineState"
                            "(compute effect)");
                        entry.pipelineState = std::move(pipelineState);
                        entry.error.clear();
                    }
                    // exception: 計算効果の再生成失敗
                    catch (const std::exception& exception)
                    {
                        entry.error = describeFailure
                            ? describeFailure(exception.what())
                            : std::string(exception.what());
                    }
                }
            }
        }

        if (error != nullptr)
        {
            *error = entry.error;
        }
        return entry.pipelineState != nullptr;
    }

    void D3D12ComputeEffectRenderer::Dispatch(
        AssetManager& assets,
        const std::filesystem::path& shaderPath,
        RenderTarget& output,
        const std::array<GraphicsViewHandle, 2>& inputs,
        const std::array<DirectX::XMFLOAT4, 8>& parameters)
    {
        // 準備済み効果の検索結果
        const auto found = m_shaders.find(
            assets.ResolvePath(shaderPath).lexically_normal());
        if (found == m_shaders.end()
            || found->second.pipelineState == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 compute effect was not prepared.");
        }
        // 借用する参照ヒープ
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr)
        {
            throw std::logic_error(
                "A DirectX 12 compute effect requires a shader resource "
                "descriptor heap.");
        }

        // 計算入力と出力の結合情報
        const auto bindings = m_backend->BeginOffscreenCompute(
            output,
            inputs);
        try
        {
            // 出力寸法を含む計算定数
            Constants constants;
            constants.parameters = parameters;
            constants.outputSize = {
                static_cast<float>(bindings.width),
                static_cast<float>(bindings.height),
                1.0f / static_cast<float>(bindings.width),
                1.0f / static_cast<float>(bindings.height)
            };
            // 計算定数の転送領域
            const auto upload = m_backend->AllocateFrameUpload(
                sizeof(constants),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(upload.data, &constants, sizeof(constants));

            // 計算を記録するコマンド一覧
            auto* const commandList = m_backend->CurrentFrameCommands();
            // 計算処理に設定する参照ヒープ
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetComputeRootSignature(m_rootSignature.Get());
            commandList->SetPipelineState(found->second.pipelineState.Get());
            commandList->SetComputeRootConstantBufferView(
                0,
                upload.gpuAddress);
            commandList->SetComputeRootDescriptorTable(
                1,
                bindings.inputs[0]);
            commandList->SetComputeRootDescriptorTable(
                2,
                bindings.inputs[1]);
            commandList->SetComputeRootDescriptorTable(3, bindings.output);
            // グループ数を切り上げるため、HLSL側で出力範囲外のスレッドを除外する。
            commandList->Dispatch(
                (bindings.width + ThreadGroupSize - 1u) / ThreadGroupSize,
                (bindings.height + ThreadGroupSize - 1u) / ThreadGroupSize,
                1u);
        }
        catch (...)
        {
            m_backend->EndOffscreenCompute(output, inputs);
            throw;
        }
        m_backend->EndOffscreenCompute(output, inputs);
    }

    void D3D12ComputeEffectRenderer::Invalidate(
        AssetManager& assets,
        const std::filesystem::path& shaderPath) noexcept
    {
        if (shaderPath.empty())
        {
            return;
        }
        try
        {
            // 再生成が成功するまで、直前の正常なパイプラインを保持する。
            // 準備済み効果の検索結果
            const auto found = m_shaders.find(
                assets.ResolvePath(shaderPath).lexically_normal());
            if (found != m_shaders.end())
            {
                found->second.forceReload = true;
            }
        }
        catch (...)
        {
        }
    }
}
