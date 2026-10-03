#pragma once

#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/PbrTextures.h"
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"
#include "LamaPon/Graphics/ShaderManifest.h"
#include "LamaPon/Graphics/ShaderProgram.h"
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
    // 後続の描画へGSを持ち越さないよう、終了時に設定を解除する。
    struct GeometryShaderScope final
    {
        // 借用するGS解除先
        ID3D11DeviceContext* context{};

        // 終了時にGSを解除する(deviceContext: 借用する解除先、空なら解除不要)。
        explicit GeometryShaderScope(
            ID3D11DeviceContext* const deviceContext) noexcept
            : context(deviceContext)
        {
        }
        // GS解除を二重に行わないようコピーを禁止する。
        GeometryShaderScope(const GeometryShaderScope&) = delete;
        // GS解除を二重に行わないようコピー代入を禁止する。
        GeometryShaderScope& operator=(
            const GeometryShaderScope&) = delete;

        // 借用したコンテキストのGSを解除する。
        ~GeometryShaderScope()
        {
            if (context != nullptr)
            {
                context->GSSetShader(nullptr, nullptr, 0);
            }
        }
    };

    class AssetManager;
    class GraphicsDevice;

    class LitEffect final : public DirectX::IEffect
    {
        // 借用するコンテキストは効果の寿命まで、設定する全SRVは対応する描画の完了まで保持する。
    public:
        // 材質シェーダーと定数資源を生成する(device: 生成する機器, context: 効果の寿命まで借用する描画先, assets: 読み込み元, shaderPath: HLSLまたは材質Manifest, skinned: スキニング指定, keywords: defineとして渡すキーワード)。
        LitEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath,
            bool skinned = false,
            const std::vector<std::string>& keywords = {});

        // 物体・視点・時間の定数を更新する(world: ワールド行列, view: ビュー行列, projection: 射影行列)。
        void SetMatrices(
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection) noexcept;
        // 材質の色・粗さ・金属度と追加定数をコピーする(material: 設定する材質)。
        void SetMaterial(const LitMaterial& material) noexcept;

        // 材質用のPBRテクスチャ指定。
        using PbrTextures = LamaPon::PbrTextures;

        // 色・法線・PBR画像を借用し、PBR省略時は前の指定を解除する(albedoTexture: 未指定は白, normalTexture: 未指定は平坦法線, pbrTextures: PBR画像と強度)。
        void SetTextures(
            ID3D11ShaderResourceView* albedoTexture,
            ID3D11ShaderResourceView* normalTexture,
            const PbrTextures& pbrTextures = {}) noexcept;
        // t7以降の追加画像を借用する(textures: 未指定枠は描画時に白へ置換)。
        void SetCustomTextures(
            const std::array<
                ID3D11ShaderResourceView*,
                LitMaterial::CustomTextureCount>&
                textures) noexcept;
        // 先頭72本まで変換をコピーし、残りは維持する(transforms: 空なら更新しない行列列, count: 行列数)。
        void SetBoneTransforms(
            const DirectX::XMMATRIX* transforms,
            std::size_t count) noexcept;
        // スキニング用に生成した効果か返す。
        [[nodiscard]] bool IsSkinned() const noexcept
        {
            return m_skinned;
        }
        // Manifestから生成した効果か返す。
        [[nodiscard]] bool IsManifestEffect() const noexcept
        {
            return m_manifestEffect;
        }
        // モデルの種類に対応する輪郭パスを持つか返す。
        [[nodiscard]] bool HasOutline() const noexcept;
        // 遮蔽パスを持つか返す。
        [[nodiscard]] bool HasOccludedPass() const noexcept;

        // JSON順のパス数を返し、直書きHLSLは対応役割を各1件とする(role: 描画の役割)。
        [[nodiscard]] std::size_t PassCount(
            ShaderPassRole role) const noexcept;
        // 役割内の使用パスを選び、範囲外なら例外を送出する(role: 描画の役割, index: 0から始まるパス番号)。
        void SelectPass(ShaderPassRole role, std::size_t index);
        // 選択中の状態を借用し、役割がなければ例外を送出する(role: 描画の役割)。
        [[nodiscard]] const ShaderRenderState&
            SelectedPassRenderState(ShaderPassRole role) const;
        // 選択中の頂点バイトコードを借用し、遮蔽は通常パスの先頭を代用する(role: 描画の役割)。
        [[nodiscard]] ID3DBlob* SelectedPassVertexShaderByteCode(
            ShaderPassRole role) const noexcept;
        // 選択中のハル・ドメイン処理の有無を返す(role: 遮蔽では通常パスの先頭を参照)。
        [[nodiscard]] bool SelectedPassHasTessellation(
            ShaderPassRole role) const noexcept;
        // 選択中のGSの有無を返す(role: 遮蔽では通常パスの先頭を参照)。
        [[nodiscard]] bool SelectedPassHasGeometryShader(
            ShaderPassRole role) const noexcept;

        // 静的またはスキニングの通常色パス数を返す。
        [[nodiscard]] std::size_t ColorPassCount() const noexcept;
        // 通常色パスを選び、範囲外なら例外を送出する(index: 0から始まるパス番号)。
        void SelectColorPass(std::size_t index);
        // 通常色パスの状態を借用し、範囲外なら例外を送出する(index: 0から始まるパス番号)。
        [[nodiscard]] const ShaderRenderState& ColorPassRenderState(
            std::size_t index) const;
        // 通常色パスの頂点バイトコードを借用し、範囲外なら空を返す(index: 0から始まるパス番号)。
        [[nodiscard]] ID3DBlob* ColorPassVertexShaderByteCode(
            std::size_t index) const noexcept;
        // 通常またはインスタンスの選択中状態を借用し、宣言がなければ既定値を返す。
        [[nodiscard]] const ShaderRenderState&
            RenderState() const noexcept;

        // 現在のパスにハル・ドメイン処理が両方あるか返す。
        [[nodiscard]] bool HasTessellation() const noexcept;

        // 現在のパスにGSがあるか返す。
        [[nodiscard]] bool HasGeometryShader() const noexcept;
        // 輪郭のシェーダーと定数を設定する(deviceContext: 空なら生成時の描画先)。
        void ApplyOutline(ID3D11DeviceContext* deviceContext);
        // 通常の頂点処理・遮蔽PS・Greater深度比較を設定する(deviceContext: 空なら生成時の描画先)。
        void ApplyOccluded(ID3D11DeviceContext* deviceContext);
        // PS側の材質・照明を設定してHS・DS・GSを解除する(deviceContext: 空なら生成時の描画先)。
        void ApplyPixelOnly(ID3D11DeviceContext* deviceContext);

        // インスタンス用の頂点処理を持つか返す。
        [[nodiscard]] bool SupportsInstancing() const noexcept;
        // 対応時だけインスタンス描画を有効にする(enabled: 有効化の指定)。
        void SetInstancingEnabled(const bool enabled) noexcept
        {
            m_instancingEnabled =
                enabled && SupportsInstancing();
        }
        // インスタンス用入力レイアウトの頂点バイトコードを借用して返す。
        [[nodiscard]] ID3DBlob*
            InstancedVertexShaderByteCode() const noexcept;
        // 先頭パスの頂点処理だけを使い、PSと照明を省く(enabled: 深度専用描画の指定)。
        void SetDepthOnlyEnabled(
            const bool enabled) noexcept
        {
            m_depthOnly = enabled;
        }
        // 制御点パッチで描く場合だけハル・ドメイン処理を使う(enabled: 今回のパッチ描画指定)。
        void SetTessellationDrawEnabled(
            const bool enabled) noexcept
        {
            m_tessellationDraw = enabled;
        }

        // 選択中のシェーダーと定数を設定する(deviceContext: 空なら生成時の描画先)。
        void __cdecl Apply(
            ID3D11DeviceContext* deviceContext) override;
        // 有効な頂点バイトコードを借用して返す(shaderByteCode: 非空のポインター出力先, byteCodeLength: 非空のバイト数出力先)。
        void __cdecl GetVertexShaderBytecode(
            const void** shaderByteCode,
            std::size_t* byteCodeLength) override;

    private:
        // 旧Game Moduleを読み込んでAPI不一致を案内するため、互換入口のシンボルを保持する。
        struct ManifestPass final
        {
            // Manifest内のパス名
            std::string name;
            // 保持するパスのシェーダー
            ShaderProgram program;
            // 宣言したパスの描画状態
            ShaderRenderState renderState;
        };

        // 役割を固定配列の添字へ変換する(role: 有効な描画役割)。
        [[nodiscard]] static std::size_t RoleIndex(
            ShaderPassRole role) noexcept;
        // モデルの種類に対応する通常色の役割を返す。
        [[nodiscard]] ShaderPassRole PrimaryRole() const noexcept;
        // モデルの種類に対応する輪郭の役割を返す。
        [[nodiscard]] ShaderPassRole OutlineRole() const noexcept;
        // 役割のパスを借用し、未生成・範囲外なら空を返す(role: 描画の役割, index: パス番号)。
        [[nodiscard]] const ManifestPass* ManifestPassAt(
            ShaderPassRole role,
            std::size_t index) const noexcept;
        // 選択中のManifestパスを借用し、該当なしなら空を返す(role: 描画の役割)。
        [[nodiscard]] const ManifestPass* SelectedManifestPass(
            ShaderPassRole role) const noexcept;
        // 現在のシェーダーを借用し、直書きHLSLなら空を返す(primaryOnly: 通常パスの先頭を強制するか)。
        [[nodiscard]] const ShaderProgram* ActiveManifestProgram(
            bool primaryOnly = false) const noexcept;

        friend class GraphicsDevice;
        friend class SkeletalModel;


        // 互換用に照明定数を更新し、中立ハンドルの画像は解決せず無効とする(lighting: 照明の設定)。
        void SetLighting(const LightingState& lighting) noexcept;
        // 全ビューの検証後にまとめて渡し、照明を部分更新しない。
        struct D3D11LightingViews final
        {
            // 借用する平行光の影SRV
            ID3D11ShaderResourceView* directionalShadow{};
            // 借用するスポット影SRV
            ID3D11ShaderResourceView* spotShadow{};
            // 借用する点光源の影SRV
            ID3D11ShaderResourceView* pointShadow{};
            // 環境の原画像・鏡面・拡散SRV
            std::array<ID3D11ShaderResourceView*, 3> environment{};
            // 借用する画面空間AOのSRV
            ID3D11ShaderResourceView* screenAmbientOcclusion{};
            // 反射の色履歴・深度SRV
            std::array<ID3D11ShaderResourceView*, 2>
                screenSpaceReflection{};
            // ライト一覧・番号表・個数SRV
            std::array<ID3D11ShaderResourceView*, 3> clustered{};
            // 間接光のRGB別SH係数SRV
            std::array<ID3D11ShaderResourceView*, 3>
                bakedGlobalIllumination{};
        };
        // 照明と検証済みの借用ビューをまとめて設定する(lighting: 照明の設定, views: 全件を検証済みのD3D11参照)。
        void SetLightingD3D11(
            const LightingState& lighting,
            const D3D11LightingViews& views) noexcept;

        // 互換シンボルを維持し、中立ハンドルのプローブは解決せず更新を省く(probe: 互換用のプローブ指定)。
        void SetEnvironmentOverride(
            const ReflectionProbeEnvironment& probe) noexcept;
        struct D3D11ReflectionProbeViews final
        {
            // 借用する主プローブの鏡面SRV
            ID3D11ShaderResourceView* specular{};
            // 借用する主プローブの拡散SRV
            ID3D11ShaderResourceView* irradiance{};
            // 借用する副プローブの鏡面SRV
            ID3D11ShaderResourceView* secondarySpecular{};
            // 借用する副プローブの拡散SRV
            ID3D11ShaderResourceView* secondaryIrradiance{};
        };
        // 照明設定後にプローブを借用し、主画像不足なら更新を省く(probe: プローブの設定, views: 検証済みのD3D11参照)。
        void SetEnvironmentOverrideD3D11(
            const ReflectionProbeEnvironment& probe,
            const D3D11ReflectionProbeViews& views) noexcept;

        // 追加定数7.wが0.5以上なら点補間、その他は線形補間を返す。
        [[nodiscard]] ID3D11SamplerState*
            ActiveMaterialSampler() const noexcept;
        // ファイルパスではなく実際の画像参照から法線・PBR・発光の有効定数を設定する。
        void ResolveTextureFlags() noexcept;
        // t11～t15へPBRとAOを設定し、未指定枠は白を使う(context: 非空の描画先)。
        void BindPbrTextures(
            ID3D11DeviceContext* context) const noexcept;
        // t0～t6へ材質・環境・影を設定し、色と法線の未指定は代替画像を使う(context: 非空の描画先)。
        void BindMaterialAndShadowTextures(
            ID3D11DeviceContext* context) const noexcept;

        // 既存の自作HLSLとの配置互換を保ち、定数の追加はバッファ末尾に行う。
        struct ObjectConstants final
        {
            // ワールド行列
            DirectX::XMFLOAT4X4 world{};
            // ビューと射影の合成行列
            DirectX::XMFLOAT4X4 viewProjection{};
            // 法線変換用の逆転置行列
            DirectX::XMFLOAT4X4 worldInverseTranspose{};
            // 材質のRGBA色
            DirectX::XMFLOAT4 materialColor{
                1.0f,
                1.0f,
                1.0f,
                1.0f
            };
            // ワールド座標でのカメラ位置
            DirectX::XMFLOAT4 cameraPosition{};
            // ワールド座標でのカメラ前方
            DirectX::XMFLOAT4 cameraForward{};
            // 粗さ・法線強度・法線有無・金属度
            DirectX::XMFLOAT4 materialParameters{
                0.5f,
                1.0f,
                0.0f,
                0.0f
            };
            // 材質の追加float4定数
            std::array<
                DirectX::XMFLOAT4,
                LitMaterial::CustomParameterCount>
                customParameters{};
            // 粗さ・金属度・AOの有無とAO強度
            DirectX::XMFLOAT4 materialTextureParameters{
                0.0f,
                0.0f,
                0.0f,
                1.0f
            };
            // rgbは発光色、wは画像有無
            DirectX::XMFLOAT4 emissiveParameters{};
            // 秒・フレーム秒数・番号・予約
            DirectX::XMFLOAT4 timeParameters{};
        };

        struct DirectionalConstants final
        {
            // xyzは光の方向、wは強度
            DirectX::XMFLOAT4 directionIntensity{};
            // rgbは光の色、wは太陽角半径
            DirectX::XMFLOAT4 color{};
        };

        struct PointConstants final
        {
            // xyzは光の位置、wは範囲
            DirectX::XMFLOAT4 positionRange{};
            // rgbは光の色、wは強度
            DirectX::XMFLOAT4 colorIntensity{};
        };

        struct SpotConstants final
        {
            // xyzは光の位置、wは範囲
            DirectX::XMFLOAT4 positionRange{};
            // xyzは光の方向、wは内角余弦
            DirectX::XMFLOAT4 directionInnerCosine{};
            // rgbは光の色、wは強度
            DirectX::XMFLOAT4 colorIntensity{};
            // 外角余弦・影枠番号+1・予約
            DirectX::XMFLOAT4 outerCosinePadding{};
        };

        // HLSLのLightingBufferと同じ配置を保ち、定数の追加は末尾に行う。
        struct LightingConstants final
        {
            // 環境光のRGB色と予約値
            DirectX::XMFLOAT4 ambient{};
            // 平行光・点光・スポット・影段数
            std::array<std::uint32_t, 4> lightCounts{};
            // 平行光の定数配列
            std::array<
                DirectionalConstants,
                MaximumDirectionalLights> directionalLights{};
            // 点光源の定数配列
            std::array<
                PointConstants,
                MaximumPointLights> pointLights{};
            // スポットライトの定数配列
            std::array<
                SpotConstants,
                MaximumSpotLights> spotLights{};
            // カスケード影の変換行列
            std::array<
                DirectX::XMFLOAT4X4,
                MaximumShadowCascades>
                shadowViewProjections{};
            // カスケードの分割距離
            DirectX::XMFLOAT4 shadowCascadeSplits{};
            // 光番号+1・バイアス2種・強度
            DirectX::XMFLOAT4 shadowParameters{};
            // フォグのRGB色と予約値
            DirectX::XMFLOAT4 fogColor{};
            // 開始距離・終了距離・密度・有効
            DirectX::XMFLOAT4 fogParameters{};
            // IBL強度・有効・最終ミップ・予約
            DirectX::XMFLOAT4 environmentParameters{};
            // スポット影の変換行列
            std::array<
                DirectX::XMFLOAT4X4,
                MaximumSpotShadows>
                spotShadowViewProjections{};
            // バイアス・法線バイアス・強度・有効
            std::array<
                DirectX::XMFLOAT4,
                MaximumSpotShadows>
                spotShadowParameters{};
            // 光番号+1・バイアス・強度・予約
            DirectX::XMFLOAT4 pointShadowParameters{};
            // カスケード・スポット・点光の画素幅
            DirectX::XMFLOAT4 shadowTexelSizes{};
            // 逆幅・逆高さ・AO有効・予約
            DirectX::XMFLOAT4
                screenAmbientOcclusionParameters{};
            // クラスタのxyz個数と有効値
            DirectX::XMFLOAT4 clusteredParameters{};
            // 近面・遠面・対数比・最大灯数
            DirectX::XMFLOAT4 clusteredDepthParameters{};
            // 逆幅・逆高さ・ライト数・予約
            DirectX::XMFLOAT4 clusteredScreenParameters{};
            // 主プローブの箱中心と予約
            DirectX::XMFLOAT4 reflectionBoxCenter{};
            // 主プローブのxyz半径と有効値
            DirectX::XMFLOAT4 reflectionBoxParameters{};
            // 副プローブの箱中心と予約
            DirectX::XMFLOAT4
                reflectionSecondaryBoxCenter{};
            // 副プローブのxyz半径と有効値
            DirectX::XMFLOAT4
                reflectionSecondaryBoxParameters{};
            // 副プローブ比率・最終ミップ・予約
            DirectX::XMFLOAT4 reflectionBlendParameters{};
            // SSR強度・有効・距離・採取数
            DirectX::XMFLOAT4
                screenReflectionParameters{};
            // 逆幅・逆高さ・射影33・射影43
            DirectX::XMFLOAT4 screenReflectionScreen{};
            // 厚み・最大粗さ・最終ミップ・予約
            DirectX::XMFLOAT4 screenReflectionQuality{};
            // SSR履歴のビュー射影行列
            DirectX::XMFLOAT4X4
                screenReflectionPreviousViewProjection{};
            // 間接光領域のxyz最小点と有効値
            DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
            // 間接光領域のxyz逆寸法と強度
            DirectX::XMFLOAT4 bakedGiInverseSize{};
            // 間接光のxyzプローブ数と予約
            DirectX::XMFLOAT4 bakedGiResolution{};
        };

        // GPUへ渡せるボーンの最大数
        static constexpr std::size_t MaximumBones = 72;
        struct BoneConstants final
        {
            // ボーンの3×4変換行列
            std::array<
                DirectX::XMFLOAT3X4,
                MaximumBones> transforms{};
        };

        struct CustomVectorConstants final
        {
            // 材質の追加ベクトル定数
            std::array<
                DirectX::XMFLOAT4,
                LitMaterial::CustomVectorCount> vectors{};
        };

        // 借用する既定の描画コンテキスト
        ID3D11DeviceContext* m_context{};
        // 保持する通常の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        // 保持する通常のピクセル処理
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
        // 保持するハルシェーダー
        Microsoft::WRL::ComPtr<ID3D11HullShader> m_hullShader;
        // 保持するドメインシェーダー
        Microsoft::WRL::ComPtr<ID3D11DomainShader> m_domainShader;
        // 保持するジオメトリシェーダー
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            m_geometryShader;
        // 輪郭用の頂点シェーダー
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_outlineVertexShader;
        // 輪郭用のピクセルシェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_outlinePixelShader;
        // 遮蔽用のピクセルシェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_occludedPixelShader;
        // 遮蔽用の深度Greater状態
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
            m_occludedDepthState;
        // インスタンス用の頂点処理
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_instancedVertexShader;
        // 通常の頂点バイトコード
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShaderByteCode;
        // インスタンス頂点バイトコード
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_instancedVertexShaderByteCode;
        // 役割別にJSON順のパス列
        std::array<
            std::vector<ManifestPass>,
            ShaderPassRoleCount> m_manifestPasses;
        // 役割別の選択中パス番号
        std::array<std::size_t, ShaderPassRoleCount>
            m_selectedManifestPasses{};
        // b0へ渡す物体の定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_objectBuffer;
        // b1へ渡す照明の定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_lightingBuffer;
        // b2へ渡すボーン定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneBuffer;
        // b3へ渡す追加定数バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_customVectorBuffer;
        // 保持する1画素の平坦法線SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_flatNormalTexture;

        // 保持する代替用の白1画素SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_whiteTexture;
        // 線形補間の材質サンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        // 点補間の材質サンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_pointSampler;
        // 影の比較サンプラー
        Microsoft::WRL::ComPtr<ID3D11SamplerState>
            m_shadowSampler;
        // 借用する色テクスチャ
        ID3D11ShaderResourceView* m_albedoTexture{};
        // 借用するt7以降の追加画像
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            m_customTextures{};
        // 直書きHLSLの描画状態
        ShaderRenderState m_renderState;
        // 借用する法線テクスチャ
        ID3D11ShaderResourceView* m_normalTexture{};
        // 借用する粗さテクスチャ
        ID3D11ShaderResourceView* m_roughnessTexture{};
        // 借用する金属度テクスチャ
        ID3D11ShaderResourceView* m_metallicTexture{};
        // 借用する遮蔽テクスチャ
        ID3D11ShaderResourceView* m_occlusionTexture{};
        // 借用する発光テクスチャ
        ID3D11ShaderResourceView* m_emissiveTexture{};
        // 遮蔽テクスチャの強度
        float m_occlusionStrength{ 1.0f };
        // 強度を含む発光色
        DirectX::XMFLOAT3 m_emissiveFactor{};
        // 借用する平行光の影画像
        ID3D11ShaderResourceView* m_shadowTexture{};
        // 借用する鏡面環境画像
        ID3D11ShaderResourceView* m_environmentTexture{};
        // 借用する拡散環境画像
        ID3D11ShaderResourceView* m_irradianceTexture{};
        // 借用する副プローブ鏡面画像
        ID3D11ShaderResourceView*
            m_secondaryEnvironmentTexture{};
        // 借用する副プローブ拡散画像
        ID3D11ShaderResourceView*
            m_secondaryIrradianceTexture{};
        // 借用するSSR色履歴画像
        ID3D11ShaderResourceView*
            m_screenReflectionColorTexture{};
        // 借用するSSR深度画像
        ID3D11ShaderResourceView*
            m_screenReflectionDepthTexture{};

        // 借用する間接光の赤SH係数
        ID3D11ShaderResourceView* m_bakedGiRedTexture{};
        // 借用する間接光の緑SH係数
        ID3D11ShaderResourceView* m_bakedGiGreenTexture{};
        // 借用する間接光の青SH係数
        ID3D11ShaderResourceView* m_bakedGiBlueTexture{};
        // 借用するスポット影画像
        ID3D11ShaderResourceView* m_spotShadowTexture{};
        // 借用する点光源の影画像
        ID3D11ShaderResourceView* m_pointShadowTexture{};
        // 借用する画面空間AO画像
        ID3D11ShaderResourceView*
            m_screenAmbientOcclusionTexture{};

        // 借用するクラスタのライト一覧
        ID3D11ShaderResourceView* m_clusterLights{};
        // 借用するクラスタの番号表
        ID3D11ShaderResourceView* m_clusterIndexList{};
        // 借用するクラスタのライト数
        ID3D11ShaderResourceView* m_clusterCounts{};
        // 物体のCPU側定数
        ObjectConstants m_objectConstants;
        // 照明のCPU側定数
        LightingConstants m_lightingConstants;
        // ボーンのCPU側定数
        BoneConstants m_boneConstants;
        // 追加ベクトルのCPU側定数
        CustomVectorConstants m_customVectorConstants;
        // スキニング用の効果か
        bool m_skinned{};
        // Manifestから作った効果か
        bool m_manifestEffect{};
        // 追加パスの役割を強制するか
        bool m_manifestRoleOverride{};
        // 追加パス用に強制する役割
        ShaderPassRole m_manifestOverrideRole{
            ShaderPassRole::Forward
        };
        // インスタンス描画を行うか
        bool m_instancingEnabled{};
        // 深度だけを描くか
        bool m_depthOnly{};
        // 制御点パッチで描くか
        bool m_tessellationDraw{};
    };
}
