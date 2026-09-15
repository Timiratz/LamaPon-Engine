#pragma once

#include "LamaPon/Graphics/D3D12MaterialShaderRenderer.h"
#include "LamaPon/Graphics/MaterialShaderDrawRequest.h"

#include <filesystem>
#include <memory>

namespace LamaPon
{
    class AssetManager;
    class D3D12Backend;
    class GraphicsRenderServices;
    struct LightingState;
    struct PrimitiveDrawRequest;
    struct ShaderRenderState;

    [[nodiscard]] std::unique_ptr<GraphicsRenderServices>
        CreateD3D12GraphicsRenderServices(D3D12Backend& backend);
}

namespace LamaPon::Detail
{
    // D3D12のrender serviceだけが持つ、3D Material custom shaderの入口です。
    // GraphicsDeviceがdynamic_castで取り出すため、公開の
    // GraphicsRenderServices契約は変わりません。
    class D3D12MaterialShaderServices
    {
    public:
        virtual ~D3D12MaterialShaderServices() = default;

        [[nodiscard]] virtual MaterialShaderDrawResult DrawMaterialShader(
            AssetManager& assets,
            const MaterialShaderSource& shader,
            const MaterialShaderSource& placeholder,
            bool prepass,
            const PrimitiveDrawRequest& request,
            const MaterialShaderDrawRequest& material,
            const LightingState& lighting) = 0;
        virtual void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) noexcept = 0;
        [[nodiscard]] virtual bool TryGetMaterialShaderRenderState(
            const std::filesystem::path& cacheKey,
            ShaderRenderState& state) const noexcept = 0;
        [[nodiscard]] virtual MaterialShaderPasses
            PrepareMaterialShaderPasses(
                AssetManager& assets,
                const MaterialShaderSource& shader) = 0;
        // D3D11のApplyCustomPixelShaderと同じく、ParticleSystemの`PSMain`を
        // b0の8本のfloat4、b1のLight2D、t0のparticle texture、t1の補助
        // texture、s0の線形wrapで描きます。compileに失敗したShaderは
        // placeholderで描き、どちらも使えないときはdrawnをfalseで返します。
        [[nodiscard]] virtual MaterialShaderDrawResult DrawCustomParticles(
            AssetManager& assets,
            const MaterialShaderSource& shader,
            const MaterialShaderSource& placeholder,
            const ParticleDrawRequest& request,
            const std::array<DirectX::XMFLOAT4, 8>& parameters) = 0;
        // 次の描画で、保存時刻に関係なくParticleSystemのShaderを作り直させます。
        virtual void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) noexcept = 0;

    protected:
        D3D12MaterialShaderServices() = default;
        D3D12MaterialShaderServices(
            const D3D12MaterialShaderServices&) = default;
        D3D12MaterialShaderServices& operator=(
            const D3D12MaterialShaderServices&) = default;
    };
}
