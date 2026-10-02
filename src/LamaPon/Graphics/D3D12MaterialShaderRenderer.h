#pragma once

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
    // 宣言にないキーワードを除いた「パス?キーワード」のキーを呼び出し側で渡します。
    struct MaterialShaderSource final
    {
        // シェーダーの絶対パス
        std::filesystem::path path;
        // キーワード別の識別キー
        std::filesystem::path cacheKey;
        // 有効なキーワード
        std::vector<std::string> keywords;
        // 失敗文を整える関数(message: 元の失敗文)。
        std::function<std::string(const char*)> describeFailure;
    };

    class D3D12MaterialShaderRenderer final
    {
    public:
        // 描画器を作ります(backend: 描画器より長く生存するバックエンド)。
        explicit D3D12MaterialShaderRenderer(D3D12Backend& backend);
        // 所有するシェーダーと描画資源を破棄します。
        ~D3D12MaterialShaderRenderer() noexcept;

        // 描画器のコピーを禁止します。
        D3D12MaterialShaderRenderer(
            const D3D12MaterialShaderRenderer&) = delete;
        // 描画器のコピー代入を禁止します。
        D3D12MaterialShaderRenderer& operator=(
            const D3D12MaterialShaderRenderer&) = delete;

        // マテリアルを描きます(assets: シェーダー取得元, shader: 描画するシェーダー, placeholder: 失敗時の代替, prepass: 深度プリパスの除外判定, request: 描画条件, vertices: 頂点列, indices: 頂点番号列, material: 素材・骨・追加パス, lighting: ライト情報)。
        // 深度プリパスは骨変形を除き宣言付き半透明・深度書き込みなしを省き、深度描画には開始済みの深度パスが必要です。
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
        // 全キーワードの次回再読込を要求します(shaderPath: シェーダーの絶対パス)。
        void Invalidate(const std::filesystem::path& shaderPath) noexcept;
        // 準備済み通常シェーダーの描画状態を返します(cacheKey: キーワード別のキー, state: 成功時の出力)。
        [[nodiscard]] bool TryGetRenderState(
            const std::filesystem::path& cacheKey,
            ShaderRenderState& state) const noexcept;
        // 通常シェーダーを準備して追加パスの有無を返します(assets: 取得元, shader: 準備するシェーダー)。
        [[nodiscard]] MaterialShaderPasses PreparePasses(
            AssetManager& assets,
            const MaterialShaderSource& shader);

    private:
        struct PipelineKey final
        {
            // カラー出力形式
            DXGI_FORMAT colorFormat{ DXGI_FORMAT_UNKNOWN };
            // 深度出力形式
            DXGI_FORMAT depthFormat{ DXGI_FORMAT_UNKNOWN };
            // 合成方式
            std::uint8_t blend{};
            // 深度状態
            std::uint8_t depth{};
            // 面の除外方式
            std::uint8_t cull{};
            // 深度のみの描画
            bool depthOnly{};
            // 最近傍の補間
            bool pointSampler{};
            // 骨変形の描画
            bool skinned{};
            // 複数インスタンス描画
            bool instanced{};
            // 4制御点パッチの描画
            bool tessellated{};
            // マテリアルパス番号
            std::uint8_t pass{};
            // 辺だけの描画
            bool wireframe{};

            // 描画状態のキーを比較します。
            [[nodiscard]] auto operator<=>(
                const PipelineKey&) const = default;
        };

        struct ShaderEntry final
        {
            // 通常の頂点シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> vertexShader;
            // 通常の画素シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> pixelShader;
            // 複数描画の頂点シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> instancedVertexShader;
            // 形状シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> geometryShader;
            // パッチ分割シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> hullShader;
            // 分割面の頂点シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> domainShader;
            // 輪郭の頂点シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> outlineVertexShader;
            // 輪郭の画素シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> outlinePixelShader;
            // 遮蔽表示の画素シェーダー
            Microsoft::WRL::ComPtr<ID3DBlob> occludedPixelShader;
            // PSSkinnedOccludedがなくPSOccludedへ戻る場合はVSSkinnedMainと組み合わせます。
            bool occludedUsesMaterialVertexShader{};
            // 追加パスの失敗理由
            std::string passError;
            // 描画状態ごとのPSO
            std::map<
                PipelineKey,
                Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines;
            // 宣言された描画状態
            ShaderRenderState renderState;
            // 使用するb0～b3
            std::array<bool, 4> constantBuffers{};
            // パッチ分割の有無
            bool hasTessellation{};
            // シェーダーの世代番号
            std::uint64_t generation{};
            // 準備の失敗理由
            std::string error;
            // 次の保存時刻確認
            std::chrono::steady_clock::time_point nextCheck{};
            // 前回確認した保存時刻
            std::filesystem::file_time_type writeTime{};
            // 保存状態の確認済み
            bool observed{};
            // 次回の強制再読込
            bool forceReload{};
            // 前回の元ファイル存在
            bool sourceExists{};
        };

        // 世代別シェーダーを準備します(assets: 取得元, source: パスとキーワード, skinned: 骨変形の別キャッシュ指定)。
        // 元ファイルを250msごとに確認し、変更・無効化・コンパイル失敗では旧PSOを外します。
        // HSMain/DSMainと輪郭のVS/PSはそれぞれ両方そろう場合だけ使用します。
        [[nodiscard]] ShaderEntry& Prepare(
            AssetManager& assets,
            const MaterialShaderSource& source,
            bool skinned);
        // 描画状態に合うPSOを取得・生成します(entry: 準備済みシェーダー, key: 出力形式と描画状態)。
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            ShaderEntry& entry,
            const PipelineKey& key);

        // 借用する描画バックエンド
        D3D12Backend* m_backend{};
        // 線形補間のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_linearRootSignature;
        // 最近傍補間のルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_pointRootSignature;
        // 既定の平坦法線ビュー
        GraphicsViewHandle m_flatNormalView;
        // 内蔵の骨変形頂点シェーダー
        Microsoft::WRL::ComPtr<ID3DBlob> m_skinnedVertexShader;
        // 骨変形出力に合う内蔵マゼンタ表示
        ShaderEntry m_skinnedErrorShader;
        // 通常描画のシェーダー群
        std::unordered_map<std::filesystem::path, ShaderEntry> m_shaders;
        // 骨変形のシェーダー群
        std::unordered_map<std::filesystem::path, ShaderEntry>
            m_skinnedShaders;
        // 次の世代番号
        std::uint64_t m_nextGeneration{ 1 };
    };
}
