#pragma once

// DirectX 12 ExperimentalでMesh RendererのMaterial custom shaderを描く
// pipelineです。D3D11のLitEffectと同じb0／b1／b2／b3、t0〜t25、s0／s1の
// 契約で、VSMain／PSMain（とGSMain）を実行します。Runtime内部headerで、
// SDKにはinstallしません。
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/MaterialShaderDrawRequest.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    class D3D12Backend;
    struct LightingState;
}

namespace LamaPon::Detail
{
    // 描画するShaderです。GraphicsDeviceがD3D11のMaterialShaderと同じく
    // 宣言に無いkeywordを落とし、「パス?キーワード」のcache keyを作ります。
    struct MaterialShaderSource final
    {
        std::filesystem::path path;
        std::filesystem::path cacheKey;
        std::vector<std::string> keywords;
        std::function<std::string(const char*)> describeFailure;
    };

    class D3D12MaterialShaderRenderer final
    {
    public:
        explicit D3D12MaterialShaderRenderer(D3D12Backend& backend);
        ~D3D12MaterialShaderRenderer() noexcept;

        D3D12MaterialShaderRenderer(
            const D3D12MaterialShaderRenderer&) = delete;
        D3D12MaterialShaderRenderer& operator=(
            const D3D12MaterialShaderRenderer&) = delete;

        // D3D11と同じく保存を250ミリ秒ごとに確かめて作り直し、失敗した
        // Shaderはplaceholder（マゼンタ）で描きます。prepassはD3D11の
        // 深度プリパスと同じく、深度を書かない宣言のShaderを飛ばします。
        [[nodiscard]] MaterialShaderDrawResult Draw(
            AssetManager& assets,
            const MaterialShaderSource& shader,
            const MaterialShaderSource& placeholder,
            bool prepass,
            const PrimitiveDrawRequest& request,
            std::span<const PrimitiveRenderVertex> vertices,
            std::span<const std::uint32_t> indices,
            const MaterialShaderDrawRequest& material,
            const LightingState& lighting);
        // shaderPath（絶対パス）から作ったすべてのkeyword variantを、次の
        // 描画で保存時刻に関係なく作り直させます。
        void Invalidate(const std::filesystem::path& shaderPath) noexcept;
        // cache済みで使えるShaderの描画状態です。
        [[nodiscard]] bool TryGetRenderState(
            const std::filesystem::path& cacheKey,
            ShaderRenderState& state) const noexcept;
        // D3D11のLitEffect::HasOutline／HasOccludedPassと同じく、Shaderが
        // 輪郭と遮蔽表示の入口を持つかです。まだ用意していないShaderは
        // ここでcompileします。
        [[nodiscard]] MaterialShaderPasses PreparePasses(
            AssetManager& assets,
            const MaterialShaderSource& shader);

    private:
        struct PipelineKey final
        {
            DXGI_FORMAT colorFormat{ DXGI_FORMAT_UNKNOWN };
            DXGI_FORMAT depthFormat{ DXGI_FORMAT_UNKNOWN };
            std::uint8_t blend{};
            std::uint8_t depth{};
            std::uint8_t cull{};
            bool depthOnly{};
            bool pointSampler{};
            bool skinned{};
            // VSInstancedMainとslot 1のworld／colorを使います。
            bool instanced{};
            // HSMain／DSMainを束ね、4制御点パッチで描くpipelineです。
            bool tessellated{};
            // MaterialShaderPassです。
            std::uint8_t pass{};
            // DirectXTKのCommonStates::Wireframeと同じく辺だけを描きます。
            bool wireframe{};

            [[nodiscard]] auto operator<=>(
                const PipelineKey&) const = default;
        };

        struct ShaderEntry final
        {
            Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            Microsoft::WRL::ComPtr<ID3DBlob> instancedVertexShader;
            Microsoft::WRL::ComPtr<ID3DBlob> geometryShader;
            // HSMainとDSMainが両方あるときだけ持ちます。
            Microsoft::WRL::ComPtr<ID3DBlob> hullShader;
            Microsoft::WRL::ComPtr<ID3DBlob> domainShader;
            // VSOutlineとPSOutlineが両方あるときだけ持ちます。
            Microsoft::WRL::ComPtr<ID3DBlob> outlineVertexShader;
            Microsoft::WRL::ComPtr<ID3DBlob> outlinePixelShader;
            Microsoft::WRL::ComPtr<ID3DBlob> occludedPixelShader;
            // PSSkinnedOccludedが無くPSOccludedへ戻った場合は、段間signatureを
            // 合わせるためVSSkinnedMainと組み合わせます。
            bool occludedUsesMaterialVertexShader{};
            // 輪郭／遮蔽表示のpipelineを作れず、そのpassを止めた説明です。
            std::string passError;
            std::map<
                PipelineKey,
                Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines;
            ShaderRenderState renderState;
            // b0〜b3のうち、いずれかのstageが読むconstant bufferです。
            std::array<bool, 4> constantBuffers{};
            bool hasTessellation{};
            std::uint64_t generation{};
            std::string error;
            // D3D11のMaterialShaderEntryと同じ保存監視の状態です。
            std::chrono::steady_clock::time_point nextCheck{};
            std::filesystem::file_time_type writeTime{};
            bool observed{};
            bool forceReload{};
            bool sourceExists{};
        };

        // skinnedはVSSkinnedMain／PSSkinnedMainを別のcacheで用意します。
        [[nodiscard]] ShaderEntry& Prepare(
            AssetManager& assets,
            const MaterialShaderSource& source,
            bool skinned);
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            ShaderEntry& entry,
            const PipelineKey& key);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_linearRootSignature;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pointRootSignature;
        // LitEffectのフラット法線（R=G=0.5, B=1）と同じ1x1です。
        GraphicsViewHandle m_flatNormalView;
        // DirectXTK SkinnedEffectと同じ計算でglTF／FBXの骨を変形する
        // エンジン内蔵の頂点シェーダーです。
        Microsoft::WRL::ComPtr<ID3DBlob> m_skinnedVertexShader;
        // スキニング経路の代替表示です。LamaPonShaderError.hlslの
        // PSSkinnedMainは入力をSV_Positionだけに絞っており、D3D12では内蔵
        // 頂点シェーダーの出力と繋げられないため、同じマゼンタを内蔵します。
        ShaderEntry m_skinnedErrorShader;
        std::unordered_map<std::filesystem::path, ShaderEntry> m_shaders;
        std::unordered_map<std::filesystem::path, ShaderEntry>
            m_skinnedShaders;
        std::uint64_t m_nextGeneration{ 1 };
    };
}
