#pragma once

#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/ShaderRenderState.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>
#include <d3d11.h>

#include <array>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class GeometricPrimitive;
    }
}

namespace LamaPon
{
    struct Bounds3D;
    class AssetManager;
    class LitEffect;
    struct LitTextureRequest;
    struct TextureAsset;

    enum class PrimitiveShape
    {
        // 立方体
        Cube,
        // 球
        Sphere,
        // 円柱
        Cylinder,
        // XZ平面の板状形状
        Plane
    };

    // 手続きメッシュのローカル頂点で、添字は三つで一三角形を表し時計回りを前面とする。
    struct ProceduralMeshVertex final
    {
        // ローカル座標の位置
        DirectX::XMFLOAT3 position{};
        // ローカル座標の法線
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        // 画像を読むUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    // 数値設定は有限値を前提とし、借用する描画機器とアセットはこの部品より長寿命とする。
    class MeshRendererComponent final : public Component
    {
    public:
        // 基本形状とLitマテリアルを持つメッシュ描画部品を作る(shape: 基本形状の種類, color: アルファ乗算前のRGBA, albedoTexture: 基本色画像のパス, normalTexture: 法線画像のパス, roughness: 表面の粗さ, normalStrength: 法線補正の強さ, materialAsset: 初期化時に優先する材質パス)。
        explicit MeshRendererComponent(
            PrimitiveShape shape = PrimitiveShape::Cube,
            DirectX::XMFLOAT4 color = {
                0.16f,
                0.65f,
                0.95f,
                1.0f
            },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f,
            std::filesystem::path materialAsset = {}) noexcept;
        // 所有するメッシュと入力レイアウト・制御点資源を破棄する。
        ~MeshRendererComponent() override;

        // 基本色の各成分を0〜1に収めて設定する(color: アルファ乗算前のRGBA)。
        void SetColor(const DirectX::XMFLOAT4& color) noexcept
        {
            m_material.SetBaseColor(color);
        }
        // アルファ乗算前の基本色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept
        {
            return m_material.BaseColor();
        }
        // 基本色画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: 画像パスで空は解除)。
        void SetAlbedoTexturePath(std::filesystem::path path);
        // 基本色画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexturePath() const noexcept
        {
            return m_material.AlbedoTexture();
        }
        // 追加画像を読み込んでt7以降に割り当て、範囲外の番号なら無視する(index: 0〜3の追加画像番号, path: 画像パスで空は解除)。
        void SetCustomTexturePath(
            std::size_t index,
            std::filesystem::path path);
        // 粗さ画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: G成分を使う画像パス)。
        void SetRoughnessTexturePath(std::filesystem::path path);
        // 粗さ画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexturePath() const noexcept
        {
            return m_material.RoughnessTexture();
        }
        // 金属度画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: B成分を使う画像パス)。
        void SetMetallicTexturePath(std::filesystem::path path);
        // 金属度画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            MetallicTexturePath() const noexcept
        {
            return m_material.MetallicTexture();
        }
        // 遮蔽画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: R成分を使う画像パス)。
        void SetOcclusionTexturePath(std::filesystem::path path);
        // 環境光とIBLに適用する遮蔽画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexturePath() const noexcept
        {
            return m_material.OcclusionTexture();
        }
        // 発光画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: 発光画像のパス)。
        void SetEmissiveTexturePath(std::filesystem::path path);
        // 発光画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            EmissiveTexturePath() const noexcept
        {
            return m_material.EmissiveTexture();
        }
        // 遮蔽の強さを0〜1に収める(strength: 遮蔽を適用する強さ)。
        void SetOcclusionStrength(
            const float strength) noexcept
        {
            m_material.SetOcclusionStrength(strength);
        }
        // 遮蔽を適用する強さを取得する。
        [[nodiscard]] float OcclusionStrength() const noexcept
        {
            return m_material.OcclusionStrength();
        }
        // 発光色の各成分を0以上に収める(color: 発光のRGB倍率)。
        void SetEmissiveColor(
            const DirectX::XMFLOAT3& color) noexcept
        {
            m_material.SetEmissiveColor(color);
        }
        // 発光色のRGB倍率を取得する。
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmissiveColor() const noexcept
        {
            return m_material.EmissiveColor();
        }

        // 法線画像を読み込んで差し替え、失敗時は以前の設定を保持する(path: 法線画像のパス)。
        void SetNormalTexturePath(std::filesystem::path path);
        // 法線画像のパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            NormalTexturePath() const noexcept
        {
            return m_material.NormalTexture();
        }
        // 表面の粗さを0.02〜1に収める(roughness: 表面の粗さ)。
        void SetRoughness(const float roughness) noexcept
        {
            m_material.SetRoughness(roughness);
        }
        // 表面の粗さを取得する。
        [[nodiscard]] float Roughness() const noexcept
        {
            return m_material.Roughness();
        }
        // 法線補正の強さを0〜2に収める(strength: 法線画像の補正倍率)。
        void SetNormalStrength(const float strength) noexcept
        {
            m_material.SetNormalStrength(strength);
        }
        // 法線画像の補正倍率を取得する。
        [[nodiscard]] float NormalStrength() const noexcept
        {
            return m_material.NormalStrength();
        }
        // 金属度を0〜1に収める(metallic: 0で非金属から1で金属)。
        void SetMetallic(const float metallic) noexcept
        {
            m_material.SetMetallic(metallic);
        }
        // 金属度を取得する。
        [[nodiscard]] float Metallic() const noexcept
        {
            return m_material.Metallic();
        }
        // マテリアルのシェーダーパスを設定して描画準備を更新する(path: HLSLかManifestのパス)。
        void SetShaderPath(std::filesystem::path path);
        // 設定したマテリアルシェーダーのパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_material.Shader();
        }
        // シェーダー宣言より優先する明示的なカリングを設定する(mode: 除外する面の設定)。
        void SetCullMode(ShaderCullMode mode) noexcept
        {
            m_cullMode = mode;
            m_cullModeOverride = true;
        }
        // 明示的なカリングを解除してシェーダーまたは既定の設定へ戻す。
        void ClearCullModeOverride() noexcept
        {
            m_cullModeOverride = false;
        }
        // 明示設定用に保持するカリング値を取得する。
        [[nodiscard]] ShaderCullMode CullMode() const noexcept
        {
            return m_cullMode;
        }
        // 明示的なカリング設定が有効か確認する。
        [[nodiscard]] bool IsCullModeOverridden() const noexcept
        {
            return m_cullModeOverride;
        }
        // シェーダーバリアントのキーワードを有効にする(keyword: 有効にするキーワード名)。
        void EnableShaderKeyword(std::string keyword)
        {
            m_material.EnableShaderKeyword(std::move(keyword));
        }
        // シェーダーバリアントのキーワードを無効にする(keyword: 無効にするキーワード名)。
        void DisableShaderKeyword(
            const std::string_view keyword)
        {
            m_material.DisableShaderKeyword(keyword);
        }
        // 設定したキーワードが有効か確認する(keyword: 調べるキーワード名)。
        [[nodiscard]] bool IsShaderKeywordEnabled(
            const std::string_view keyword) const noexcept
        {
            return m_material.IsShaderKeywordEnabled(keyword);
        }
        // シェーダーバリアントのキーワード一覧を置き換える(keywords: 有効にするキーワード集合)。
        void SetShaderKeywords(ShaderKeywordSet keywords)
        {
            m_material.SetShaderKeywords(std::move(keywords));
        }
        // 設定されたキーワード集合を取得する。
        [[nodiscard]] const ShaderKeywordSet&
            ShaderKeywords() const noexcept
        {
            return m_material.ShaderKeywords();
        }

        // 範囲内のカスタム定数を設定し、範囲外なら無視する(index: 0〜7の定数番号, value: 設定する四成分の定数)。
        void SetCustomParameter(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            m_material.SetCustomParameter(index, value);
        }
        // カスタム定数を取得し、範囲外なら最後の定数を返す(index: 0〜7の定数番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(std::size_t index) const noexcept
        {
            return m_material.CustomParameter(index);
        }
        // 範囲内のカスタムベクトルを設定し、範囲外なら無視する(index: 0〜63のベクトル番号, value: 設定する四成分の値)。
        void SetCustomVector(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            m_material.SetCustomVector(index, value);
        }
        // カスタムベクトルを取得し、範囲外なら最後の要素を返す(index: 0〜63のベクトル番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomVector(std::size_t index) const noexcept
        {
            return m_material.CustomVector(index);
        }
        // D3D11の準備済みシェーダーを無効化してEffectと入力レイアウトを再取得する。
        void ReloadShader();
        // 最後のシェーダー準備または描画時の診断を取得する。
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        // 現在のLitマテリアル設定を取得する。
        [[nodiscard]] const LitMaterial& Material() const noexcept
        {
            return m_material;
        }
        // 材質アセットの関連を解除し、画像を読んでマテリアルを適用する(material: 適用する材質設定)。
        void SetMaterial(const LitMaterial& material);
        // 指定アセットの材質を読み、パスの関連を設定する(path: 材質アセットのパス)。
        // 空のパスは関連だけを解除し、現在の材質設定は保持する。
        void SetMaterialAssetPath(std::filesystem::path path);
        // 関連する材質アセットを再読み込みし、未初期化なら例外を返す。
        void ReloadMaterialAsset();
        // 関連する材質アセットのパスを取得する。
        [[nodiscard]] const std::filesystem::path&
            MaterialAssetPath() const noexcept
        {
            return m_materialAssetPath;
        }
        // 手続きメッシュ解除後に使う基本形状を取得する。
        [[nodiscard]] PrimitiveShape Shape() const noexcept { return m_shape; }
        // 有限な頂点と三角形添字を検証し、法線を正規化して手続きメッシュへ切り替える(vertices: 最大65536個のローカル頂点, indices: 時計回りの三角形の添字, recalculateNormals: 面積加重で法線を再計算する指定)。
        // Scene JSONへ保存しないためScript等で再生成し、D3D11のGPUメッシュ生成失敗時は旧メッシュを保持する。
        void SetProceduralMesh(
            std::vector<ProceduralMeshVertex> vertices,
            std::vector<std::uint32_t> indices,
            bool recalculateNormals = false);
        // 手続きメッシュを解除して設定された基本形状へ戻す。
        void ClearProceduralMesh();
        // 手続きメッシュの頂点を保持しているか確認する。
        [[nodiscard]] bool HasProceduralMesh() const noexcept
        {
            return !m_proceduralVertices.empty();
        }
        // 手続きメッシュのローカル頂点一覧を取得する。
        [[nodiscard]] const std::vector<ProceduralMeshVertex>&
            ProceduralVertices() const noexcept
        {
            return m_proceduralVertices;
        }
        // 手続きメッシュの三角形添字一覧を取得する。
        [[nodiscard]] const std::vector<std::uint32_t>&
            ProceduralIndices() const noexcept
        {
            return m_proceduralIndices;
        }
        // 手続きメッシュのローカルAABBを出力し、基本形状なら出力を変えずfalseを返す(bounds: AABBの出力先)。
        [[nodiscard]] bool TryGetLocalBounds(
            Bounds3D& bounds) const noexcept;
        // 深度を使わず重ねる表示を設定する(enabled: ワールド上の重ね描き指定)。
        void SetWorldOverlay(const bool enabled) noexcept
        {
            m_worldOverlay = enabled;
        }
        // 深度を使わない重ね描き設定か確認する。
        [[nodiscard]] bool IsWorldOverlay() const noexcept
        {
            return m_worldOverlay;
        }
        // 形状と材質の描画情報を記録し、同パスでバッチ描画済みならfalseを返す(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "MeshRenderer"; }

        // シェーダーを同期し、現在のAPIと形状でインスタンス描画が可能か確認する。
        [[nodiscard]] bool CanBeInstanced() const;
        // Effectを同期し、宣言されたアルファ合成または基本色のアルファから描画整列区分を調べる。
        [[nodiscard]] bool IsAlphaBlended3D() const override;
        // 形状・材質・画像・キーワード・Effectの識別情報を64ビットのバッチキーへまとめる。
        // キー取得前にCanBeInstancedで同期し、基本色はカスタムシェーダー使用時にだけ全成分を含める。
        [[nodiscard]] std::uint64_t
            InstanceBatchKey() const noexcept;
        // この部品を代表とする同種のバッチを描き、成功した部品の同パス個別描画を省く(batch: 同じキーの描画部品一覧, view: ビュー行列, projection: 射影行列)。
        // 代表の材質・光源・リフレクションプローブを共有するため、位置ごとのプローブ差は反映しない。
        void RenderInstancedBatch(
            const std::vector<MeshRendererComponent*>&
                batch,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

    protected:
        // 描画機器とアセットを借用し、APIに応じて形状と材質画像を準備する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // API別にメッシュを色または深度で描き、同パスのバッチ描画済みなら省く(view: ビュー行列, projection: 射影行列)。
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // D3D11へ合成・深度と明示設定優先のカリングを反映する(state: シェーダーの描画状態宣言)。
        void ApplyShaderRenderState(
            const ShaderRenderState& state) const;
        // D3D11へ有効な明示カリング設定を反映する。
        void ApplyCullModeOverride() const;
        // 材質の画像ビューと遮蔽・発光設定をAPI共通の要求へまとめる。
        [[nodiscard]] LitTextureRequest
            BuildLitTextureRequest() const noexcept;
        // 材質画像を用途別に読み、材質と画像を差し替えてシェーダー準備を更新する(material: 適用する材質設定)。
        void ApplyMaterial(const LitMaterial& material);
        // D3D11のEffectと各パスの入力レイアウトを同期し、失敗の診断を記録する(forceReload: キャッシュを無効化する指定)。
        void RefreshShader(bool forceReload);
        // シーンが位置から選んだプローブ環境をLighting設定の直後に適用する。
        void ApplyReflectionProbe() const;
        // Effectのテセレーションと有効な四制御点バッファが揃うか確認する。
        [[nodiscard]] bool CanDrawTessellatedPatch() const noexcept;
        // D3D11の四制御点パッチを描き、HS・DSとトポロジーを戻す(inputLayout: 現在パスの入力レイアウト)。
        // 有効なEffectと制御点を前提とし、合成・深度・カリングの設定は呼び出し側が行う。
        void DrawTessellatedPatch(
            ID3D11InputLayout* inputLayout) const;
        // PlaneかCubeのD3D11四制御点バッファを作り、手続きメッシュなどは解除する(graphics: D3D11の描画機器)。
        void BuildTessellationPatches(GraphicsDevice& graphics);
        // D3D11の手続きメッシュまたは基本形状を作る(graphics: D3D11の描画機器)。
        void BuildActivePrimitive(GraphicsDevice& graphics);

        // 手続きメッシュ解除後の基本形状
        PrimitiveShape m_shape;
        // 手続きメッシュのローカル頂点
        std::vector<ProceduralMeshVertex> m_proceduralVertices;
        // 手続きメッシュの三角形添字
        std::vector<std::uint32_t> m_proceduralIndices;
        // 手続きメッシュの最小端
        DirectX::XMFLOAT3 m_proceduralBoundsMinimum{};
        // 手続きメッシュの最大端
        DirectX::XMFLOAT3 m_proceduralBoundsMaximum{};
        // 現在のLit材質設定
        LitMaterial m_material;
        // 関連する材質アセットのパス
        std::filesystem::path m_materialAssetPath;
        // 共有する基本色画像
        std::shared_ptr<const TextureAsset> m_albedoTexture;
        // 共有する法線画像
        std::shared_ptr<const TextureAsset> m_normalTexture;

        // 共有する粗さ画像
        std::shared_ptr<const TextureAsset> m_roughnessTexture;
        // 共有する金属度画像
        std::shared_ptr<const TextureAsset> m_metallicTexture;
        // 共有する遮蔽画像
        std::shared_ptr<const TextureAsset> m_occlusionTexture;
        // 共有する発光画像
        std::shared_ptr<const TextureAsset> m_emissiveTexture;
        // t7から割り当てる追加画像の共有参照
        std::array<
            std::shared_ptr<const TextureAsset>,
            LitMaterial::CustomTextureCount>
            m_customTextures{};
        // 借用したアセット管理
        AssetManager* m_assets{};
        // 所有するD3D11メッシュ
        std::unique_ptr<DirectX::GeometricPrimitive> m_primitive;
        // 借用した共有LitEffect
        LitEffect* m_effect{};
        struct InputLayoutHolder;
        // 色パス順の入力レイアウト
        std::vector<std::unique_ptr<InputLayoutHolder>>
            m_colorInputLayouts;
        struct TessellationPatchHolder;
        // PlaneかCubeの四制御点資源
        std::unique_ptr<TessellationPatchHolder>
            m_tessellationPatches;
        // バッチパス順の入力レイアウト
        std::vector<std::unique_ptr<InputLayoutHolder>>
            m_instancedInputLayouts;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 最後に取得したシェーダーパス
        std::filesystem::path m_activeShaderPath;
        // 最後に取得したシェーダー世代
        std::uint64_t m_shaderGeneration{};
        // シェーダーの準備・描画診断
        std::string m_shaderError;
        // 深度を使わず重ね描きする指定
        bool m_worldOverlay{};
        // 明示設定するカリング値
        ShaderCullMode m_cullMode{ ShaderCullMode::Back };
        // 明示カリングを優先する指定
        bool m_cullModeOverride{};
        // 同パスの個別描画を一回省く指定
        bool m_instancedThisPass{};
    };
}
