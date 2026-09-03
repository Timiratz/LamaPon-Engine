#pragma once

#include "LamaPon/Animation/AnimatorController.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Scene/Component.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>

#include <cstdint>
#include <filesystem>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11InputLayout;

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
    }
}

namespace LamaPon
{
    struct Bounds3D;
    struct ShaderRenderState;

    class AssetManager;
    class LitEffect;
    class SkeletalModel;
    struct ModelAsset;
    struct TextureAsset;
    struct SkeletalPoseSample;

    struct AnimationEventNotification final
    {
        std::string state;
        std::string name;
        std::string payload;
        float normalizedTime{};
    };

    class ModelRendererComponent final : public Component
    {
    public:
        explicit ModelRendererComponent(
            std::filesystem::path modelPath = {},
            bool wireframe = false,
            bool materialOverrideEnabled = false,
            DirectX::XMFLOAT4 color = {
                1.0f,
                1.0f,
                1.0f,
                1.0f
            },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f,
            std::filesystem::path materialAsset = {},
            std::size_t animationIndex = 0,
            float animationSpeed = 1.0f,
            bool animationLoop = true,
            bool animationPlayOnStart = true,
            std::filesystem::path animationController = {},
            bool applyRootMotion = false,
            std::string rootMotionNode = {},
            bool preserveEmbeddedMaterialColor = false);
        ~ModelRendererComponent() override;

        void SetModelPath(std::filesystem::path modelPath);
        [[nodiscard]] const std::filesystem::path& ModelPath() const noexcept
        {
            return m_modelPath;
        }
        [[nodiscard]] bool TryGetLocalBounds(
            Bounds3D& bounds) const noexcept;

        void SetWireframe(const bool wireframe) noexcept { m_wireframe = wireframe; }
        [[nodiscard]] bool IsWireframe() const noexcept { return m_wireframe; }

        void SetAnimationIndex(std::size_t index) noexcept;
        [[nodiscard]] std::size_t AnimationIndex() const noexcept
        {
            return m_animationIndex;
        }
        [[nodiscard]] std::size_t AnimationCount() const noexcept;
        [[nodiscard]] std::vector<std::string>
            SkeletonNodeNames() const;
        [[nodiscard]] std::string_view AnimationName(
            std::size_t index) const noexcept;
        [[nodiscard]] float AnimationDuration() const;
        void SetAnimationSpeed(float speed) noexcept;
        [[nodiscard]] float AnimationSpeed() const noexcept
        {
            return m_animationSpeed;
        }
        void SetAnimationLoop(bool loop) noexcept
        {
            m_animationLoop = loop;
        }
        [[nodiscard]] bool AnimationLoop() const noexcept
        {
            return m_animationLoop;
        }
        void SetAnimationPlayOnStart(bool enabled) noexcept
        {
            m_animationPlayOnStart = enabled;
        }
        [[nodiscard]] bool AnimationPlayOnStart() const noexcept
        {
            return m_animationPlayOnStart;
        }
        void PlayAnimation() noexcept;
        void PauseAnimation() noexcept;
        void StopAnimation() noexcept;
        void AdvanceAnimation(
            float deltaTime,
            bool allowRootMotion = true);
        void SetAnimationTime(float time) noexcept;
        [[nodiscard]] float AnimationTime() const noexcept
        {
            return m_animationTime;
        }
        [[nodiscard]] bool IsAnimationPlaying() const noexcept
        {
            return m_animationPlaying;
        }
        void SetAnimationControllerPath(
            std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path&
            AnimationControllerPath() const noexcept
        {
            return m_animationControllerPath;
        }
        void ReloadAnimationController();
        void SetAnimationTrigger(std::string trigger);
        [[nodiscard]] const std::string&
            CurrentAnimationState() const noexcept
        {
            return m_currentAnimationState;
        }
        [[nodiscard]] bool IsAnimationTransitioning() const noexcept
        {
            return !m_nextAnimationState.empty();
        }
        [[nodiscard]] std::vector<std::string>
            AnimationTriggers() const;
        void SetAnimationFloat(
            std::string parameter,
            float value);
        [[nodiscard]] float AnimationFloat(
            std::string_view parameter) const noexcept;
        [[nodiscard]] std::vector<AnimatorFloatParameter>
            AnimationFloatParameters() const;
        void SetApplyRootMotion(bool enabled) noexcept
        {
            m_applyRootMotion = enabled;
        }
        [[nodiscard]] bool ApplyRootMotion() const noexcept
        {
            return m_applyRootMotion;
        }
        void SetRootMotionNode(std::string node)
        {
            m_rootMotionNode = std::move(node);
        }
        [[nodiscard]] const std::string&
            RootMotionNode() const noexcept
        {
            return m_rootMotionNode;
        }
        [[nodiscard]] bool PollAnimationEvent(
            AnimationEventNotification& event);
        [[nodiscard]] std::size_t
            PendingAnimationEventCount() const noexcept
        {
            return m_animationEventQueue.size();
        }

        void SetMaterialOverrideEnabled(bool enabled);
        [[nodiscard]] bool IsMaterialOverrideEnabled() const noexcept
        {
            return m_materialOverrideEnabled;
        }

        // 従来のDirectXTK描画（Blinn-Phong）に戻すかどうか。
        // 既定falseで、glTF/FBXモデルはLamaPon Lit（PBR、法線マップ・
        // metallic・IBL対応）で描かれます。旧バージョンと同じ見た目に
        // したいモデルだけtrueにしてください。
        void SetUseLegacyShading(bool enabled);
        [[nodiscard]] bool UsesLegacyShading() const noexcept
        {
            return m_useLegacyShading;
        }

        void SetPreserveEmbeddedMaterialColor(
            const bool enabled) noexcept
        {
            m_preserveEmbeddedMaterialColor = enabled;
        }
        [[nodiscard]] bool PreserveEmbeddedMaterialColor() const noexcept
        {
            return m_preserveEmbeddedMaterialColor;
        }

        void SetColor(const DirectX::XMFLOAT4& color) noexcept
        {
            m_material.SetBaseColor(color);
        }
        // 半透明を含むモデルか。1つでも半透明パーツがあれば、
        // GameObjectごと「遠い順に描く組」へ回されます。
        [[nodiscard]] bool IsAlphaBlended3D() const override;
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept
        {
            return m_material.BaseColor();
        }

        void SetAlbedoTexturePath(std::filesystem::path texturePath);
        [[nodiscard]] const std::filesystem::path&
            AlbedoTexturePath() const noexcept
        {
            return m_material.AlbedoTexture();
        }

        // カスタムShaderが宣言した追加テクスチャ（t7以降）の割り当て。
        void SetCustomTexturePath(
            std::size_t index,
            std::filesystem::path path);
        void SetNormalTexturePath(std::filesystem::path texturePath);
        [[nodiscard]] const std::filesystem::path&
            NormalTexturePath() const noexcept
        {
            return m_material.NormalTexture();
        }

        // PBRマップと発光マップの割り当て。マテリアル上書きが
        // 有効なとき、モデル自身のマップより優先されます。
        void SetRoughnessTexturePath(
            std::filesystem::path texturePath);
        [[nodiscard]] const std::filesystem::path&
            RoughnessTexturePath() const noexcept
        {
            return m_material.RoughnessTexture();
        }
        void SetMetallicTexturePath(
            std::filesystem::path texturePath);
        [[nodiscard]] const std::filesystem::path&
            MetallicTexturePath() const noexcept
        {
            return m_material.MetallicTexture();
        }
        void SetOcclusionTexturePath(
            std::filesystem::path texturePath);
        [[nodiscard]] const std::filesystem::path&
            OcclusionTexturePath() const noexcept
        {
            return m_material.OcclusionTexture();
        }
        void SetEmissiveTexturePath(
            std::filesystem::path texturePath);
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
        [[nodiscard]] bool UsesCommonLit() const noexcept;
        // 実際にLamaPon Lit（PBR）で描かれているか。glTF/FBXは
        // 既定でtrue、CMO/SDKMESHはマテリアル上書き時のみtrueです。
        [[nodiscard]] bool UsesLamaPonLit() const noexcept;
        [[nodiscard]] std::string_view
            CommonLitStatus() const noexcept;
        [[nodiscard]] bool CanBeInstanced() const;
        [[nodiscard]] std::uint64_t
            InstanceBatchKey() const noexcept;
        bool RenderInstancedBatch(
            const std::vector<ModelRendererComponent*>& batch,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection);
        [[nodiscard]] std::size_t AutomaticLodLevel(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) const noexcept;
        [[nodiscard]] std::uint64_t TriangleCount(
            std::size_t lodLevel) const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "ModelRenderer";
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        [[nodiscard]] bool HasPreRender3DPass() override;
        void OnPreRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;
        void OnRender3D(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection) override;

    private:
        // Shaderが宣言した描画状態（合成・深度・カリング）を適用します。
        void ApplyShaderRenderState(
            const ShaderRenderState& state) const;
        // 追加テクスチャのSRVを、未設定はnullptrで並べて返します。
        [[nodiscard]] std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>
            ResolveCustomTextureViews() const noexcept;
        struct CommonLitResources;

        void ReloadModel();
        void LoadAnimationController();
        [[nodiscard]] std::size_t ResolveAnimationIndex(
            const AnimatorState& state) const;
        [[nodiscard]] std::size_t ResolveAnimationIndex(
            std::string_view modelClip) const noexcept;
        [[nodiscard]] float StateAnimationDuration(
            const AnimatorState& state) const;
        void CalculateBlendWeights(
            const AnimatorState& state,
            std::vector<float>& weights) const;
        void EnterAnimationState(
            const AnimatorState& state);
        void StartAnimationTransition(
            const AnimatorTransition& transition);
        void AdvanceControllerAnimation(
            float deltaTime);
        void CollectAnimationPoseSamples(
            std::vector<SkeletalPoseSample>& samples) const;
        [[nodiscard]] std::size_t
            ResolveRootMotionNode() const noexcept;
        void ApplyRootMotionDelta(
            const std::vector<SkeletalPoseSample>& before,
            const std::vector<SkeletalPoseSample>& after);
        void DispatchAnimationEvents(
            const AnimatorState& state,
            float previousTime,
            float currentTime,
            float duration,
            bool looped,
            bool forward);
        void RebuildCommonLitResources();
        void RefreshShader(bool forceReload);
        void ApplyMaterial(const LitMaterial& material);
        void DrawCommonLit(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            bool occludedOnly = false);

        std::filesystem::path m_modelPath;
        std::shared_ptr<const ModelAsset> m_model;
        LitMaterial m_material;
        std::filesystem::path m_materialAssetPath;
        std::shared_ptr<const TextureAsset> m_albedoTexture;
        std::shared_ptr<const TextureAsset> m_normalTexture;
        // PBRマップ（粗さ・金属度・遮蔽）と発光マップ。
        // マテリアル上書き時にモデル自身のマップより優先されます。
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
        GraphicsDevice* m_graphics{};
        ID3D11DeviceContext* m_context{};
        DirectX::CommonStates* m_states{};
        std::unique_ptr<CommonLitResources>
            m_commonLitResources;
        LitEffect* m_effect{};
        LitEffect* m_skinnedEffect{};
        Microsoft::WRL::ComPtr<ID3D11InputLayout>
            m_skinnedInputLayout;
        std::filesystem::path m_activeShaderPath;
        std::uint64_t m_shaderGeneration{};
        std::string m_shaderError;
        bool m_wireframe{};
        bool m_materialOverrideEnabled{};
        bool m_useLegacyShading{};
        bool m_preserveEmbeddedMaterialColor{};
        std::size_t m_animationIndex{};
        float m_animationSpeed{ 1.0f };
        float m_animationTime{};
        bool m_animationLoop{ true };
        bool m_animationPlayOnStart{ true };
        bool m_animationPlaying{};
        std::filesystem::path m_animationControllerPath;
        std::shared_ptr<const AnimatorController>
            m_animationController;
        std::string m_currentAnimationState;
        std::string m_nextAnimationState;
        std::size_t m_nextAnimationIndex{};
        float m_nextAnimationTime{};
        float m_animationTransitionTime{};
        float m_animationTransitionDuration{};
        std::unordered_set<std::string>
            m_activeAnimationTriggers;
        std::unordered_map<std::string, float>
            m_animationFloatValues;
        bool m_applyRootMotion{};
        std::string m_rootMotionNode;
        std::deque<AnimationEventNotification>
            m_animationEventQueue;
        // 1フレーム内では影・深度プリパス・通常描画で同じ姿勢を
        // 使うため、ノード行列を一度だけ評価します。
        std::vector<DirectX::XMFLOAT4X4> m_cachedGlobalPose;
        std::uint64_t m_cachedPoseFrame{ ~std::uint64_t{} };
        const SkeletalModel* m_cachedPoseModel{};
        bool m_instancedThisPass{};
    };
}
