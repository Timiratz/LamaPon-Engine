#pragma once

#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/PbrTextures.h"
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <Effects.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <array>

namespace LamaPon
{
    // ジオメトリシェーダーを束ねたまま関数を抜けないための見張りです。
    //
    // なぜ要るか: GSを設定するのはLitEffectだけで、スプライト・
    // ポスト処理・ImGuiはGSに触れません。束ねたまま抜けると、それらの
    // 描画まで巻き込みます。ハル／ドメインでまったく同じ形の不具合を
    // 出しているので、抜け道の多い描画関数では必ずこれを置きます。
    // contextがnullptrなら何もしません（GSを持たないShader用）。
    struct GeometryShaderScope final
    {
        ID3D11DeviceContext* context{};

        explicit GeometryShaderScope(
            ID3D11DeviceContext* const deviceContext) noexcept
            : context(deviceContext)
        {
        }
        GeometryShaderScope(const GeometryShaderScope&) = delete;
        GeometryShaderScope& operator=(
            const GeometryShaderScope&) = delete;

        ~GeometryShaderScope()
        {
            if (context != nullptr)
            {
                context->GSSetShader(nullptr, nullptr, 0);
            }
        }
    };

    class AssetManager;

    class LitEffect final : public DirectX::IEffect
    {
    public:
        // keywordsはバリアントのキーワードです（#pragma
        // multi_compileで宣言されたもの）。渡すと `#define` として
        // コンパイルへ入り、分岐そのものが消えます。
        LitEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            bool skinned = false,
            const std::vector<std::string>& keywords = {});

        void SetMatrices(
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection) noexcept;
        void SetMaterial(const LitMaterial& material) noexcept;

        // PBRマップ一式（定義はPbrTextures.h）。既存の
        // LitEffect::PbrTextures という書き方も使えるようにします。
        using PbrTextures = LamaPon::PbrTextures;

        // PBRマップを省略した呼び出しは「マップなし」になります。
        // Effectは描画間で使い回されるので、既定引数で毎回明示的に
        // クリアして、前のオブジェクトのマップが残らないようにします。
        void SetTextures(
            ID3D11ShaderResourceView* albedoTexture,
            ID3D11ShaderResourceView* normalTexture,
            const PbrTextures& pbrTextures = {}) noexcept;
        // カスタムShaderが使う追加テクスチャ（t7以降へ割り当て）。
        // nullptrのスロットは白テクスチャになります。
        void SetCustomTextures(
            const std::array<
                ID3D11ShaderResourceView*,
                LitMaterial::CustomTextureCount>&
                textures) noexcept;
        void SetLighting(const LightingState& lighting) noexcept;
        // リフレクションプローブ用。環境反射（t3/t6）をシーン共通の
        // ものからプローブの結果へ差し替えます。SetLightingの後に
        // 呼び、SetLightingを呼び直せば元へ戻ります。
        //
        // 2個目が入っていればt7/t8へ載せて重みで混ぜます。中身の
        // 組み立てはScene::ReflectionProbeEnvironmentAtが行います。
        void SetEnvironmentOverride(
            const ReflectionProbeEnvironment& probe) noexcept;
        void SetBoneTransforms(
            const DirectX::XMMATRIX* transforms,
            std::size_t count) noexcept;
        [[nodiscard]] bool IsSkinned() const noexcept
        {
            return m_skinned;
        }
        [[nodiscard]] bool HasOutline() const noexcept
        {
            return m_outlineVertexShader != nullptr
                && m_outlinePixelShader != nullptr;
        }
        [[nodiscard]] bool HasOccludedPass() const noexcept
        {
            return m_occludedPixelShader != nullptr
                && m_occludedDepthState != nullptr;
        }
        // Shaderが宣言した描画状態（半透明・カリング・深度）。
        // 宣言が無ければ既定値（不透明・裏面カリング・深度書き込み）
        // です。レンダラーがDraw前に適用します。
        [[nodiscard]] const ShaderRenderState&
            RenderState() const noexcept
        {
            return m_renderState;
        }

        [[nodiscard]] bool HasTessellation() const noexcept
        {
            return m_hullShader != nullptr
                && m_domainShader != nullptr;
        }

        // GSMainを書いたShaderか。テセレーションと違って束ねる条件は
        // 「持っているか」で足ります――エンジンが流すのは常に三角形で、
        // 三角形入力でないGSはコンパイル時に弾いているためです。
        // ただし**束ねたまま抜けない**ことは呼ぶ側の責任です
        // （スプライトやポスト処理まで巻き込みます）。
        [[nodiscard]] bool HasGeometryShader() const noexcept
        {
            return m_geometryShader != nullptr;
        }
        void ApplyOutline(ID3D11DeviceContext* deviceContext);
        void ApplyOccluded(ID3D11DeviceContext* deviceContext);
        void ApplyPixelOnly(ID3D11DeviceContext* deviceContext);

        // インスタンス描画対応（VSInstancedMainを定義するシェーダー
        // のみ）。有効化するとApplyがインスタンス用VSを使います。
        [[nodiscard]] bool SupportsInstancing() const noexcept
        {
            return m_instancedVertexShader != nullptr;
        }
        void SetInstancingEnabled(const bool enabled) noexcept
        {
            m_instancingEnabled =
                enabled && SupportsInstancing();
        }
        // インスタンス用入力レイアウト作成に使うバイトコード。
        [[nodiscard]] ID3DBlob*
            InstancedVertexShaderByteCode() const noexcept
        {
            return m_instancedVertexShaderByteCode.Get();
        }
        // 深度専用描画（シャドウパス用）。有効中のApplyは
        // ピクセルシェーダーとライティングを省略します。
        void SetDepthOnlyEnabled(
            const bool enabled) noexcept
        {
            m_depthOnly = enabled;
        }
        // パッチ（制御点）で描くときだけtrueにします。既定はfalse。
        // ハル／ドメインシェーダーを束ねたまま三角形リストを描くのは
        // D3D11では不正で、環境によってはドライバーごと落ちます。
        // 「テセレーションShaderを持っているか」と「今それで描くか」
        // は別物なので、判断は描く側が持ちます。
        void SetTessellationDrawEnabled(
            const bool enabled) noexcept
        {
            m_tessellationDraw = enabled;
        }

        void __cdecl Apply(
            ID3D11DeviceContext* deviceContext) override;
        void __cdecl GetVertexShaderBytecode(
            const void** shaderByteCode,
            std::size_t* byteCodeLength) override;

    private:
        [[nodiscard]] ID3D11SamplerState*
            ActiveMaterialSampler() const noexcept;
        // 法線マップとPBRマップの有効フラグを、実際にバインドされて
        // いるテクスチャから決めます。読み込みモデルはファイルパスを
        // 持たずSRVだけを渡してくるため、LitMaterialのパス有無では
        // 判定できません。
        void ResolveTextureFlags() noexcept;
        // PBRマップ（t11〜t13）をバインドします。未設定の枠は白
        // テクスチャにして、シェーダー側の未初期化読みを防ぎます。
        void BindPbrTextures(
            ID3D11DeviceContext* context) const noexcept;
        // エンジンが使うt0〜t6をバインドします。
        void BindMaterialAndShadowTextures(
            ID3D11DeviceContext* context) const noexcept;

        struct ObjectConstants final
        {
            DirectX::XMFLOAT4X4 world{};
            DirectX::XMFLOAT4X4 viewProjection{};
            DirectX::XMFLOAT4X4 worldInverseTranspose{};
            DirectX::XMFLOAT4 materialColor{
                1.0f,
                1.0f,
                1.0f,
                1.0f
            };
            DirectX::XMFLOAT4 cameraPosition{};
            DirectX::XMFLOAT4 cameraForward{};
            DirectX::XMFLOAT4 materialParameters{
                0.5f,
                1.0f,
                0.0f,
                0.0f
            };
            std::array<
                DirectX::XMFLOAT4,
                LitMaterial::CustomParameterCount>
                customParameters{};
            // x=粗さマップ, y=金属度マップ, z=遮蔽マップ,
            // w=遮蔽の強さ。ObjectBufferの末尾に足しているので、
            // この行が無い既存の自作Shaderもそのまま動きます。
            DirectX::XMFLOAT4 materialTextureParameters{
                0.0f,
                0.0f,
                0.0f,
                1.0f
            };
            // rgb=発光色（強度込み）, w=発光マップの有無。
            DirectX::XMFLOAT4 emissiveParameters{};
            // 経過時間。x=秒, y=前フレームからの秒数, z=フレーム数,
            // w=予約。自作Shaderが「揺れる・流れる」表現を書くのに
            // スクリプトを1本も書かずに済むよう、エンジンが毎描画
            // 入れます（水面の見本Shaderが使っています）。
            // ObjectBufferの末尾なので、この行を持たない既存の自作
            // Shaderもそのまま動きます。
            DirectX::XMFLOAT4 timeParameters{};
        };

        struct DirectionalConstants final
        {
            DirectX::XMFLOAT4 directionIntensity{};
            DirectX::XMFLOAT4 color{};
        };

        struct PointConstants final
        {
            DirectX::XMFLOAT4 positionRange{};
            DirectX::XMFLOAT4 colorIntensity{};
        };

        struct SpotConstants final
        {
            DirectX::XMFLOAT4 positionRange{};
            DirectX::XMFLOAT4 directionInnerCosine{};
            DirectX::XMFLOAT4 colorIntensity{};
            DirectX::XMFLOAT4 outerCosinePadding{};
        };

        struct LightingConstants final
        {
            DirectX::XMFLOAT4 ambient{};
            std::array<std::uint32_t, 4> lightCounts{};
            std::array<
                DirectionalConstants,
                MaximumDirectionalLights> directionalLights{};
            std::array<
                PointConstants,
                MaximumPointLights> pointLights{};
            std::array<
                SpotConstants,
                MaximumSpotLights> spotLights{};
            std::array<
                DirectX::XMFLOAT4X4,
                MaximumShadowCascades>
                shadowViewProjections{};
            DirectX::XMFLOAT4 shadowCascadeSplits{};
            DirectX::XMFLOAT4 shadowParameters{};
            DirectX::XMFLOAT4 fogColor{};
            DirectX::XMFLOAT4 fogParameters{};
            // x=IBL強度, y=有効,
            // z=事前フィルタ済みスペキュラの最終ミップ番号
            // （0なら事前フィルタなし）, w=予約
            DirectX::XMFLOAT4 environmentParameters{};
            std::array<
                DirectX::XMFLOAT4X4,
                MaximumSpotShadows>
                spotShadowViewProjections{};
            // x=バイアス, y=法線バイアス, z=強さ, w=有効
            std::array<
                DirectX::XMFLOAT4,
                MaximumSpotShadows>
                spotShadowParameters{};
            // x=対象ライト+1, y=バイアス, z=強さ, w=予約
            DirectX::XMFLOAT4 pointShadowParameters{};
            // PCF用テクセルサイズ
            // （x=カスケード, y=スポット, z=ポイント, w=予約）。
            DirectX::XMFLOAT4 shadowTexelSizes{};
            // 画面空間AO。x=1/画面幅, y=1/画面高さ, z=有効, w=予約。
            // LightingBufferの末尾なので、この行を持たない既存の
            // 自作Shaderもそのまま動きます。
            DirectX::XMFLOAT4
                screenAmbientOcclusionParameters{};
            // クラスタライトカリング（並びはLamaPonLit.hlsl参照）。
            DirectX::XMFLOAT4 clusteredParameters{};
            DirectX::XMFLOAT4 clusteredDepthParameters{};
            DirectX::XMFLOAT4 clusteredScreenParameters{};
            // ボックス射影。xyz=箱の中心, w=予約。
            DirectX::XMFLOAT4 reflectionBoxCenter{};
            // xyz=箱の半径, w=有効。
            DirectX::XMFLOAT4 reflectionBoxParameters{};
            // 2個目のプローブのボックス射影（同じ意味）。
            DirectX::XMFLOAT4
                reflectionSecondaryBoxCenter{};
            DirectX::XMFLOAT4
                reflectionSecondaryBoxParameters{};
            // x=2個目の比率(0-1), y=2個目の最終ミップ番号,
            // z/w=予約。
            DirectX::XMFLOAT4 reflectionBlendParameters{};
            // SSR。x=強さ, y=有効, z=最大距離, w=サンプル数。
            DirectX::XMFLOAT4
                screenReflectionParameters{};
            // x=1/幅, y=1/高さ, z=projection._33,
            // w=projection._43。
            DirectX::XMFLOAT4 screenReflectionScreen{};
            // x=物の厚み, y=粗さの上限, z/w=予約。
            DirectX::XMFLOAT4 screenReflectionQuality{};
            DirectX::XMFLOAT4X4
                screenReflectionPreviousViewProjection{};
            // ベイクした間接光（照度ボリューム）。
            // xyz=ボリュームの最小コーナー, w=有効。
            DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
            // xyz=1/大きさ, w=強さ。
            DirectX::XMFLOAT4 bakedGiInverseSize{};
            // xyz=各軸のプローブ数, w=予約。
            DirectX::XMFLOAT4 bakedGiResolution{};
        };

        static constexpr std::size_t MaximumBones = 72;
        struct BoneConstants final
        {
            std::array<
                DirectX::XMFLOAT3X4,
                MaximumBones> transforms{};
        };

        struct CustomVectorConstants final
        {
            std::array<
                DirectX::XMFLOAT4,
                LitMaterial::CustomVectorCount> vectors{};
        };

        ID3D11DeviceContext* m_context{};
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3D11HullShader> m_hullShader;
        Microsoft::WRL::ComPtr<ID3D11DomainShader> m_domainShader;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            m_geometryShader;
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_outlineVertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_outlinePixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_occludedPixelShader;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            m_occludedDepthState;
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_instancedVertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShaderByteCode;
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_instancedVertexShaderByteCode;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_objectBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightingBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_customVectorBuffer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_flatNormalTexture;
        // カスタムShaderの未設定テクスチャ枠に渡す1x1の白。
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_whiteTexture;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_pointSampler;
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_shadowSampler;
        ID3D11ShaderResourceView* m_albedoTexture{};
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            m_customTextures{};
        ShaderRenderState m_renderState;
        ID3D11ShaderResourceView* m_normalTexture{};
        ID3D11ShaderResourceView* m_roughnessTexture{};
        ID3D11ShaderResourceView* m_metallicTexture{};
        ID3D11ShaderResourceView* m_occlusionTexture{};
        ID3D11ShaderResourceView* m_emissiveTexture{};
        float m_occlusionStrength{ 1.0f };
        DirectX::XMFLOAT3 m_emissiveFactor{};
        ID3D11ShaderResourceView* m_shadowTexture{};
        ID3D11ShaderResourceView* m_environmentTexture{};
        ID3D11ShaderResourceView* m_irradianceTexture{};
        ID3D11ShaderResourceView*
            m_secondaryEnvironmentTexture{};
        ID3D11ShaderResourceView*
            m_secondaryIrradianceTexture{};
        ID3D11ShaderResourceView*
            m_screenReflectionColorTexture{};
        ID3D11ShaderResourceView*
            m_screenReflectionDepthTexture{};
        // ベイクした間接光のSH係数（t23〜t25、RGB各チャンネル）。
        ID3D11ShaderResourceView* m_bakedGiRedTexture{};
        ID3D11ShaderResourceView* m_bakedGiGreenTexture{};
        ID3D11ShaderResourceView* m_bakedGiBlueTexture{};
        ID3D11ShaderResourceView* m_spotShadowTexture{};
        ID3D11ShaderResourceView* m_pointShadowTexture{};
        ID3D11ShaderResourceView*
            m_screenAmbientOcclusionTexture{};
        // クラスタライトカリング（t16〜t18）。
        ID3D11ShaderResourceView* m_clusterLights{};
        ID3D11ShaderResourceView* m_clusterIndexList{};
        ID3D11ShaderResourceView* m_clusterCounts{};
        ObjectConstants m_objectConstants;
        LightingConstants m_lightingConstants;
        BoneConstants m_boneConstants;
        CustomVectorConstants m_customVectorConstants;
        bool m_skinned{};
        bool m_instancingEnabled{};
        bool m_depthOnly{};
        bool m_tessellationDraw{};
    };
}
