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
    struct TextureAsset;

    enum class PrimitiveShape
    {
        Cube,
        Sphere,
        Cylinder,
        Plane
    };

    // 実行時に道路・地形・チューブなどを組み立てるための頂点です。
    // indexは三角形3個単位で、前面は時計回りです。
    struct ProceduralMeshVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    class MeshRendererComponent final : public Component
    {
    public:
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
        ~MeshRendererComponent() override;

        void SetColor(const DirectX::XMFLOAT4& color) noexcept
        {
            m_material.SetBaseColor(color);
        }
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept
        {
            return m_material.BaseColor();
        }
        void SetAlbedoTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexturePath() const noexcept
        {
            return m_material.AlbedoTexture();
        }
        // カスタムShaderが宣言した追加テクスチャ（t7以降）の割り当て。
        void SetCustomTexturePath(
            std::size_t index,
            std::filesystem::path path);
        // PBRマップと発光マップの割り当て。
        void SetRoughnessTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexturePath() const noexcept
        {
            return m_material.RoughnessTexture();
        }
        void SetMetallicTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            MetallicTexturePath() const noexcept
        {
            return m_material.MetallicTexture();
        }
        void SetOcclusionTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexturePath() const noexcept
        {
            return m_material.OcclusionTexture();
        }
        void SetEmissiveTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            EmissiveTexturePath() const noexcept
        {
            return m_material.EmissiveTexture();
        }
        void SetOcclusionStrength(
            const float strength) noexcept
        {
            m_material.SetOcclusionStrength(strength);
        }
        [[nodiscard]] float OcclusionStrength() const noexcept
        {
            return m_material.OcclusionStrength();
        }
        void SetEmissiveColor(
            const DirectX::XMFLOAT3& color) noexcept
        {
            m_material.SetEmissiveColor(color);
        }
        [[nodiscard]] const DirectX::XMFLOAT3&
            EmissiveColor() const noexcept
        {
            return m_material.EmissiveColor();
        }

        void SetNormalTexturePath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            NormalTexturePath() const noexcept
        {
            return m_material.NormalTexture();
        }
        void SetRoughness(const float roughness) noexcept
        {
            m_material.SetRoughness(roughness);
        }
        [[nodiscard]] float Roughness() const noexcept
        {
            return m_material.Roughness();
        }
        void SetNormalStrength(const float strength) noexcept
        {
            m_material.SetNormalStrength(strength);
        }
        [[nodiscard]] float NormalStrength() const noexcept
        {
            return m_material.NormalStrength();
        }
        void SetMetallic(const float metallic) noexcept
        {
            m_material.SetMetallic(metallic);
        }
        [[nodiscard]] float Metallic() const noexcept
        {
            return m_material.Metallic();
        }
        void SetShaderPath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_material.Shader();
        }
        // 裏面カリングの設定。Shader宣言より優先する明示的なRenderer設定。
        // Back=裏面を除外、Front=表面を除外、None=両面描画。
        void SetCullMode(ShaderCullMode mode) noexcept
        {
            m_cullMode = mode;
            m_cullModeOverride = true;
        }
        void ClearCullModeOverride() noexcept
        {
            m_cullModeOverride = false;
        }
        [[nodiscard]] ShaderCullMode CullMode() const noexcept
        {
            return m_cullMode;
        }
        [[nodiscard]] bool IsCullModeOverridden() const noexcept
        {
            return m_cullModeOverride;
        }
        // バリアントのキーワード（#pragma multi_compileで宣言した
        // もの）。立てるとそのキーワード付きでコンパイルされた
        // シェーダーが使われます。宣言に無いキーワードは無視されます。
        void EnableShaderKeyword(std::string keyword)
        {
            m_material.EnableShaderKeyword(std::move(keyword));
        }
        void DisableShaderKeyword(
            const std::string_view keyword)
        {
            m_material.DisableShaderKeyword(keyword);
        }
        [[nodiscard]] bool IsShaderKeywordEnabled(
            const std::string_view keyword) const noexcept
        {
            return m_material.IsShaderKeywordEnabled(keyword);
        }
        void SetShaderKeywords(ShaderKeywordSet keywords)
        {
            m_material.SetShaderKeywords(std::move(keywords));
        }
        [[nodiscard]] const ShaderKeywordSet&
            ShaderKeywords() const noexcept
        {
            return m_material.ShaderKeywords();
        }

        void SetCustomParameter(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            m_material.SetCustomParameter(index, value);
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(std::size_t index) const noexcept
        {
            return m_material.CustomParameter(index);
        }
        void SetCustomVector(
            std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            m_material.SetCustomVector(index, value);
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomVector(std::size_t index) const noexcept
        {
            return m_material.CustomVector(index);
        }
        void ReloadShader();
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        [[nodiscard]] const LitMaterial& Material() const noexcept
        {
            return m_material;
        }
        void SetMaterialAssetPath(std::filesystem::path path);
        void ReloadMaterialAsset();
        [[nodiscard]] const std::filesystem::path&
            MaterialAssetPath() const noexcept
        {
            return m_materialAssetPath;
        }
        [[nodiscard]] PrimitiveShape Shape() const noexcept { return m_shape; }
        // Scriptが生成する一時メッシュ。Scene JSONへは埋め込まず、
        // ScriptのStart等で再生成する用途です。頂点は最大65536個です
        // （DirectXTKの16-bit index描画に合わせています）。
        void SetProceduralMesh(
            std::vector<ProceduralMeshVertex> vertices,
            std::vector<std::uint32_t> indices,
            bool recalculateNormals = false);
        void ClearProceduralMesh();
        [[nodiscard]] bool HasProceduralMesh() const noexcept
        {
            return !m_proceduralVertices.empty();
        }
        [[nodiscard]] const std::vector<ProceduralMeshVertex>&
            ProceduralVertices() const noexcept
        {
            return m_proceduralVertices;
        }
        [[nodiscard]] const std::vector<std::uint32_t>&
            ProceduralIndices() const noexcept
        {
            return m_proceduralIndices;
        }
        [[nodiscard]] bool TryGetLocalBounds(
            Bounds3D& bounds) const noexcept;
        // 3D空間内のUIや攻撃予兆向けに、深度を無効化して
        // 非プレマルチプライド透過で描画します。
        void SetWorldOverlay(const bool enabled) noexcept
        {
            m_worldOverlay = enabled;
        }
        [[nodiscard]] bool IsWorldOverlay() const noexcept
        {
            return m_worldOverlay;
        }
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "MeshRenderer"; }

        // インスタンス描画：同一キーのMeshRendererはシーンが
        // まとめて1ドローコールで描きます。
        [[nodiscard]] bool CanBeInstanced() const noexcept;
        // 並べ替えの振り分けにSceneが直接使うので、Componentの
        // 仮想関数（protected）と違ってこちらは公開します。
        [[nodiscard]] bool IsAlphaBlended3D() const override;
        [[nodiscard]] std::uint64_t
            InstanceBatchKey() const noexcept;
        void RenderInstancedBatch(
            const std::vector<MeshRendererComponent*>&
                batch,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // Shaderが宣言した描画状態（合成・深度・カリング）を適用します。
        void ApplyShaderRenderState(
            const ShaderRenderState& state) const;
        void ApplyCullModeOverride() const;
        // 追加テクスチャのSRVを、未設定はnullptrで並べて返します。
        [[nodiscard]] std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            ResolveCustomTextureViews() const noexcept;
        void ApplyMaterial(const LitMaterial& material);
        void RefreshShader(bool forceReload);
        // 自分の位置を含む一番近いリフレクションプローブがあれば、
        // 環境反射（IBL）をその結果へ差し替えます。SetLightingの
        // 直後に呼びます。
        void ApplyReflectionProbe() const;
        // 今このRendererをパッチ（制御点）で描けるか。
        // 「テセレーションShaderを持っているか」とは別物です。
        // 持っているのに描けない形のときは、位置を出す段（ドメイン）が
        // 無いまま頂点シェーダーだけが刺さり、ラスタライザーへ位置が
        // 届かない不正な描画になります。
        [[nodiscard]] bool CanDrawTessellatedPatch() const noexcept;
        // パッチを描きます。**描画状態は呼ぶ側の責任**です
        // （深度パスは影用の設定で描くので、ここで触ると壊れます）。
        void DrawTessellatedPatch() const;
        // 形状に応じた制御点を作ります。四角パッチに割れない形状では
        // 何も作りません（m_tessellationPatchesがnullのままになり、
        // テセレーションShaderは代役へ倒れます）。
        void BuildTessellationPatches(GraphicsDevice& graphics);
        void BuildActivePrimitive(GraphicsDevice& graphics);

        PrimitiveShape m_shape;
        std::vector<ProceduralMeshVertex> m_proceduralVertices;
        std::vector<std::uint32_t> m_proceduralIndices;
        DirectX::XMFLOAT3 m_proceduralBoundsMinimum{};
        DirectX::XMFLOAT3 m_proceduralBoundsMaximum{};
        LitMaterial m_material;
        std::filesystem::path m_materialAssetPath;
        std::shared_ptr<const TextureAsset> m_albedoTexture;
        std::shared_ptr<const TextureAsset> m_normalTexture;
        // PBRマップ（粗さ・金属度・遮蔽）と発光マップ。
        std::shared_ptr<const TextureAsset> m_roughnessTexture;
        std::shared_ptr<const TextureAsset> m_metallicTexture;
        std::shared_ptr<const TextureAsset> m_occlusionTexture;
        std::shared_ptr<const TextureAsset> m_emissiveTexture;
        // カスタムShaderが宣言した追加テクスチャ（t7以降）。
        std::array<
            std::shared_ptr<const TextureAsset>,
            LitMaterial::CustomTextureCount>
            m_customTextures{};
        AssetManager* m_assets{};
        std::unique_ptr<DirectX::GeometricPrimitive> m_primitive;
        LitEffect* m_effect{};
        struct InputLayoutHolder;
        std::unique_ptr<InputLayoutHolder> m_inputLayout;
        // テセレーション用の制御点。四角パッチに割れる形状
        // （Plane・Cube）でだけ作ります。Sphere／Cylinderは
        // 四角パッチにならないので作りません。
        struct TessellationPatchHolder;
        std::unique_ptr<TessellationPatchHolder>
            m_tessellationPatches;
        std::unique_ptr<InputLayoutHolder>
            m_instancedInputLayout;
        GraphicsDevice* m_graphics{};
        std::filesystem::path m_activeShaderPath;
        std::uint64_t m_shaderGeneration{};
        std::string m_shaderError;
        bool m_worldOverlay{};
        ShaderCullMode m_cullMode{ ShaderCullMode::Back };
        bool m_cullModeOverride{};
        // このパスで既にインスタンス描画済み（OnRender3Dが消費）。
        bool m_instancedThisPass{};
    };
}
