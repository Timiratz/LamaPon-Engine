#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
    void ThrowIfFailed(
        const HRESULT result,
        const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    // カリングCSへ渡す定数。シェーダーのClusterCullingBufferと
    // 並びを一致させています。
    struct CullingConstants final
    {
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4 gridParameters{};
        DirectX::XMFLOAT4 depthParameters{};
        DirectX::XMFLOAT4 frustumParameters{};
    };
}

namespace LamaPon
{
    ClusteredLights::ClusteredLights(
        ID3D11Device* const device,
        AssetManager& assets,
        const std::filesystem::path& shaderPath)
    {
        // 共通のディスクキャッシュを使ってCompute Shaderを
        // コンパイルします。
        const auto byteCode = CompileShaderCached(
            assets,
            shaderPath,
            "CSMain",
            "cs_5_0");
        ThrowIfFailed(
            device->CreateComputeShader(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                nullptr,
                m_cullingShader
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateComputeShader"
            "(light culling)");

        // 定数バッファ。
        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth =
            sizeof(CullingConstants);
        constantDescription.Usage =
            D3D11_USAGE_DEFAULT;
        constantDescription.BindFlags =
            D3D11_BIND_CONSTANT_BUFFER;
        ThrowIfFailed(
            device->CreateBuffer(
                &constantDescription,
                nullptr,
                m_constantBuffer
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer"
            "(light culling constants)");

        // ライト一覧（CPU書き込みの動的StructuredBuffer）。
        D3D11_BUFFER_DESC lightDescription{};
        lightDescription.ByteWidth =
            sizeof(GpuLight) * MaximumClusteredLights;
        lightDescription.Usage = D3D11_USAGE_DYNAMIC;
        lightDescription.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;
        lightDescription.CPUAccessFlags =
            D3D11_CPU_ACCESS_WRITE;
        lightDescription.MiscFlags =
            D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        lightDescription.StructureByteStride =
            sizeof(GpuLight);
        ThrowIfFailed(
            device->CreateBuffer(
                &lightDescription,
                nullptr,
                m_lightBuffer.ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateBuffer(cluster lights)");
        D3D11_SHADER_RESOURCE_VIEW_DESC lightView{};
        lightView.Format = DXGI_FORMAT_UNKNOWN;
        lightView.ViewDimension =
            D3D11_SRV_DIMENSION_BUFFER;
        lightView.Buffer.NumElements =
            MaximumClusteredLights;
        ThrowIfFailed(
            device->CreateShaderResourceView(
                m_lightBuffer.Get(),
                &lightView,
                m_lightShaderResourceView
                    .ReleaseAndGetAddressOf()),
            "ID3D11Device::CreateShaderResourceView"
            "(cluster lights)");

        // クラスタごとの番号表（GPU書き込み）。
        const auto createUavBuffer =
            [device](
                const std::uint32_t elementCount,
                Microsoft::WRL::ComPtr<ID3D11Buffer>&
                    buffer,
                Microsoft::WRL::ComPtr<
                    ID3D11UnorderedAccessView>&
                    unorderedView,
                Microsoft::WRL::ComPtr<
                    ID3D11ShaderResourceView>&
                    shaderView,
                const char* name)
            {
                D3D11_BUFFER_DESC description{};
                description.ByteWidth =
                    sizeof(std::uint32_t) * elementCount;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags =
                    D3D11_BIND_SHADER_RESOURCE
                    | D3D11_BIND_UNORDERED_ACCESS;
                description.MiscFlags =
                    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                description.StructureByteStride =
                    sizeof(std::uint32_t);
                ThrowIfFailed(
                    device->CreateBuffer(
                        &description,
                        nullptr,
                        buffer.ReleaseAndGetAddressOf()),
                    name);
                D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = DXGI_FORMAT_UNKNOWN;
                uav.ViewDimension =
                    D3D11_UAV_DIMENSION_BUFFER;
                uav.Buffer.NumElements = elementCount;
                ThrowIfFailed(
                    device->CreateUnorderedAccessView(
                        buffer.Get(),
                        &uav,
                        unorderedView
                            .ReleaseAndGetAddressOf()),
                    name);
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_UNKNOWN;
                srv.ViewDimension =
                    D3D11_SRV_DIMENSION_BUFFER;
                srv.Buffer.NumElements = elementCount;
                ThrowIfFailed(
                    device->CreateShaderResourceView(
                        buffer.Get(),
                        &srv,
                        shaderView
                            .ReleaseAndGetAddressOf()),
                    name);
            };
        createUavBuffer(
            ClusterCount * MaximumLightsPerCluster,
            m_indexListBuffer,
            m_indexListUnorderedView,
            m_indexListShaderResourceView,
            "ClusteredLights(index list)");
        createUavBuffer(
            ClusterCount,
            m_countBuffer,
            m_countUnorderedView,
            m_countShaderResourceView,
            "ClusteredLights(counts)");
    }

    void ClusteredLights::Update(
        ID3D11DeviceContext* const context,
        LightingState& lighting,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        lighting.clustered = {};

        DirectX::XMFLOAT4X4 projectionValues{};
        DirectX::XMStoreFloat4x4(
            &projectionValues,
            projection);
        // 正射影（2Dシーン）はクラスタの奥行き分割が成立しないので
        // 従来経路に任せます。透視射影は_44が0、正射影は1です。
        const bool perspective =
            std::abs(projectionValues._44) < 0.5f;
        if (!perspective
            || lighting.clusteredLights.empty()
            || context == nullptr)
        {
            return;
        }

        // 射影行列からnear/farと視野の広がりを取り出します
        // （XMMatrixPerspectiveFovRH: _33=f/(n-f), _43=n*f/(n-f)）。
        const float nearPlane =
            projectionValues._43 / projectionValues._33;
        const float farPlane =
            projectionValues._43
            / (projectionValues._33 + 1.0f);
        if (!(nearPlane > 0.0f)
            || !(farPlane > nearPlane))
        {
            return;
        }
        const float tanHalfX =
            1.0f / projectionValues._11;
        const float tanHalfY =
            1.0f / projectionValues._22;

        const auto lightCount =
            static_cast<std::uint32_t>(
                std::min(
                    lighting.clusteredLights.size(),
                    MaximumClusteredLights));

        // ライト一覧をアップロードします。
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(
                m_lightBuffer.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped)))
        {
            return;
        }
        std::memcpy(
            mapped.pData,
            lighting.clusteredLights.data(),
            sizeof(GpuLight) * lightCount);
        context->Unmap(m_lightBuffer.Get(), 0);

        // 定数を書きます。
        CullingConstants constants{};
        DirectX::XMStoreFloat4x4(&constants.view, view);
        constants.gridParameters = {
            static_cast<float>(GridWidth),
            static_cast<float>(GridHeight),
            static_cast<float>(GridDepth),
            static_cast<float>(lightCount)
        };
        constants.depthParameters = {
            nearPlane,
            farPlane,
            std::log(farPlane / nearPlane),
            static_cast<float>(MaximumLightsPerCluster)
        };
        constants.frustumParameters = {
            tanHalfX,
            tanHalfY,
            0.0f,
            0.0f
        };
        context->UpdateSubresource(
            m_constantBuffer.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        // Litシェーダーが前フレームのSRVを掴んだままだと書き込み先に
        // できないので、先に外します。
        ID3D11ShaderResourceView* nullViews[3]{};
        context->PSSetShaderResources(
            16,
            3,
            nullViews);

        // ディスパッチ（1スレッド=1クラスタ、64スレッド/グループ）。
        ID3D11Buffer* constantBuffers[]{
            m_constantBuffer.Get()
        };
        context->CSSetConstantBuffers(
            0, 1, constantBuffers);
        ID3D11ShaderResourceView* resources[]{
            m_lightShaderResourceView.Get()
        };
        context->CSSetShaderResources(0, 1, resources);
        ID3D11UnorderedAccessView* unorderedViews[]{
            m_indexListUnorderedView.Get(),
            m_countUnorderedView.Get()
        };
        context->CSSetUnorderedAccessViews(
            0,
            static_cast<UINT>(
                std::size(unorderedViews)),
            unorderedViews,
            nullptr);
        context->CSSetShader(
            m_cullingShader.Get(), nullptr, 0);
        context->Dispatch(
            (ClusterCount + 63u) / 64u, 1, 1);

        // ピクセルシェーダーから読めるよう、UAVを外します。
        ID3D11UnorderedAccessView* nullUnordered[2]{};
        context->CSSetUnorderedAccessViews(
            0, 2, nullUnordered, nullptr);
        ID3D11ShaderResourceView* nullResource[1]{};
        context->CSSetShaderResources(0, 1, nullResource);
        context->CSSetShader(nullptr, nullptr, 0);

        lighting.clustered.lights =
            m_lightShaderResourceView.Get();
        lighting.clustered.lightIndices =
            m_indexListShaderResourceView.Get();
        lighting.clustered.clusterCounts =
            m_countShaderResourceView.Get();
        lighting.clustered.nearPlane = nearPlane;
        lighting.clustered.farPlane = farPlane;
        lighting.clustered.inverseWidth =
            1.0f / static_cast<float>(
                std::max(width, 1u));
        lighting.clustered.inverseHeight =
            1.0f / static_cast<float>(
                std::max(height, 1u));
        lighting.clustered.lightCount = lightCount;
        lighting.clustered.enabled = true;
    }
}
