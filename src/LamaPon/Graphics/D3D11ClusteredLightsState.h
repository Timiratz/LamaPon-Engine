#pragma once

// DirectX 11専用のClusteredLights stateです。共通stateと分けることで、
// 将来のD3D12 BackendはD3D11型をincludeせず実装できます。
#include "LamaPon/Graphics/ClusteredLightsBackendState.h"
#include "LamaPon/Graphics/Lighting.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;
}

namespace LamaPon::Detail
{
    struct D3D11ClusteredLightsState final
        : ClusteredLightsBackendState
    {
        [[nodiscard]] bool HasNativeResources() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            LightShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            IndexListShaderResourceView() const noexcept;
        [[nodiscard]] ID3D11ShaderResourceView*
            CountShaderResourceView() const noexcept;

        void Initialize(
            ID3D11Device* device,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);
        void Update(
            ID3D11DeviceContext* context,
            LightingState& lighting,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            std::uint32_t width,
            std::uint32_t height);

        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_cullingShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_lightShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_indexListBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_indexListUnorderedView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_indexListShaderResourceView;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_countBuffer;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>
            m_countUnorderedView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_countShaderResourceView;
    };
}
