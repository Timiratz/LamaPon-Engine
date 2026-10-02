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

    // D3D12の描画サービスを生成する(backend: 寿命が長い描画基盤)。
    [[nodiscard]] std::unique_ptr<GraphicsRenderServices>
        CreateD3D12GraphicsRenderServices(D3D12Backend& backend);
}

namespace LamaPon::Detail
{
    // 公開描画サービスを変えず、D3D12の独自材質処理を取り出すための内部契約。
    class D3D12MaterialShaderServices
    {
    public:
        // 派生した独自材質サービスの資源を解放する。
        virtual ~D3D12MaterialShaderServices() = default;

        // 独自材質で形状を描画する(assets: アセット管理, shader: 元のシェーダー指定, placeholder: 代替シェーダー指定, prepass: 深度プリパスの可否, request: 形状と描画条件, material: 材質固有の描画条件, lighting: 光源設定)。
        [[nodiscard]] virtual MaterialShaderDrawResult DrawMaterialShader(
            AssetManager& assets,
            const MaterialShaderSource& shader,
            const MaterialShaderSource& placeholder,
            bool prepass,
            const PrimitiveDrawRequest& request,
            const MaterialShaderDrawRequest& material,
            const LightingState& lighting) = 0;
        // 次回描画で対象の材質バリアントを再生成させる(shaderPath: 絶対ソースパス)。
        virtual void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) noexcept = 0;
        // 使用可能なキャッシュの描画状態を取得する(cacheKey: パスとキーワードの識別子, state: 取得する描画状態)。
        [[nodiscard]] virtual bool TryGetMaterialShaderRenderState(
            const std::filesystem::path& cacheKey,
            ShaderRenderState& state) const noexcept = 0;
        // 必要ならコンパイルし、追加パスの対応を返す(assets: アセット管理, shader: シェーダー指定)。
        [[nodiscard]] virtual MaterialShaderPasses
            PrepareMaterialShaderPasses(
                AssetManager& assets,
                const MaterialShaderSource& shader) = 0;
        // 独自PSまたは代替表示で粒子を描画する(assets: アセット管理, shader: 元のシェーダー指定, placeholder: 代替シェーダー指定, request: 粒子と描画条件, parameters: 八つの独自定数)。
        // PSMainへb0の八つのfloat4、灯なしのb1、t0とt1の画像、s0の線形循環サンプラーを渡す。
        // 元と代替の両方を使えなければdrawnを偽にし、代替パイプラインの生成失敗は例外を送出する。
        [[nodiscard]] virtual MaterialShaderDrawResult DrawCustomParticles(
            AssetManager& assets,
            const MaterialShaderSource& shader,
            const MaterialShaderSource& placeholder,
            const ParticleDrawRequest& request,
            const std::array<DirectX::XMFLOAT4, 8>& parameters) = 0;

        // 次回描画で対象の粒子PSを再生成させる(shaderPath: キャッシュに使うソースパス)。
        virtual void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) noexcept = 0;

    protected:
        // 独自材質サービスの基底を生成する。
        D3D12MaterialShaderServices() = default;
        // 独自材質サービスの基底を複製する。
        D3D12MaterialShaderServices(
            const D3D12MaterialShaderServices&) = default;
        // 独自材質サービスの基底を複製代入する。
        D3D12MaterialShaderServices& operator=(
            const D3D12MaterialShaderServices&) = default;
    };
}
