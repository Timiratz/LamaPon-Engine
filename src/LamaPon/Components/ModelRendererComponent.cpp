#include "LamaPon/Components/ModelRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Graphics/SkeletalModel.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <CommonStates.h>
#include <Effects.h>
#include <Model.h>
#include <VertexTypes.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // テセレーションが使えるのは、四角パッチに割れる形状（Plane・
    // Cube）のMesh Rendererだけです。
    // モデルにはパッチで描く経路が無いので、ハル／ドメインは束ねられず、
    // テセレーション前提の頂点シェーダーだけが刺さります。この頂点
    // シェーダーはSV_Positionを出さない（出すのはドメインの仕事）ため、
    // ラスタライザーへ位置が届かず**何も描かれません**。黙って消えるより
    // 理由を出してマゼンタの代役を描きます。
    //
    // 差し替えを1箇所にまとめてあるのは、Effectを選ぶ入口が
    // RefreshShaderとRebuildCommonLitResourcesの**2つ**あるからです。
    // 片方だけ直すと、もう片方が生のEffectでm_effectを上書きして
    // 元へ戻してしまいます。
    LamaPon::LitEffect* SubstituteUnsupportedTessellation(
        LamaPon::LitEffect* effect,
        LamaPon::GraphicsDevice& graphics,
        const bool skinned,
        std::string& shaderError)
    {
        if (effect == nullptr || !effect->HasTessellation())
        {
            return effect;
        }
        if (shaderError.empty())
        {
            shaderError =
                "This shader uses tessellation (HSMain/DSMain),"
                " which only works on a Mesh Renderer whose"
                " shape can be split into quad patches"
                " (Plane and Cube).";
        }
        if (auto* const placeholder =
                graphics.ShaderErrorPlaceholder(skinned))
        {
            return placeholder;
        }
        return effect;
    }

    // 読み込み済みテクスチャとマテリアル値から、Effectへ渡す
    // PBRマップ一式を組み立てます。未設定はnullptrのままにして、
    // シェーダー側では「マップなし」として扱わせます。
    LamaPon::LitEffect::PbrTextures BuildPbrTextures(
        const std::shared_ptr<
            const LamaPon::TextureAsset>& roughness,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& metallic,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& occlusion,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& emissive,
        const LamaPon::LitMaterial& material) noexcept
    {
        LamaPon::LitEffect::PbrTextures textures{};
        textures.roughness = roughness
            ? roughness->view.Get()
            : nullptr;
        textures.metallic = metallic
            ? metallic->view.Get()
            : nullptr;
        textures.occlusion = occlusion
            ? occlusion->view.Get()
            : nullptr;
        textures.emissive = emissive
            ? emissive->view.Get()
            : nullptr;
        textures.occlusionStrength =
            material.OcclusionStrength();
        textures.emissiveFactor = material.EmissiveColor();
        return textures;
    }

    float SpecularPowerFromRoughness(const float roughness) noexcept
    {
        const float squared = roughness * roughness;
        return std::clamp(
            2.0f / std::max(squared * squared, 0.0001f) - 2.0f,
            1.0f,
            128.0f);
    }

    DirectX::XMVECTOR SpecularColorFromRoughness(
        const float roughness) noexcept
    {
        return DirectX::XMVectorReplicate(
            std::lerp(0.45f, 0.08f, roughness));
    }

    bool HasSemantic(
        const DirectX::ModelMeshPart& part,
        const char* semantic) noexcept
    {
        if (!part.vbDecl)
        {
            return false;
        }

        return std::ranges::any_of(
            *part.vbDecl,
            [semantic](
                const D3D11_INPUT_ELEMENT_DESC& element)
            {
                return element.SemanticName != nullptr
                    && element.SemanticIndex == 0
                    && ::_stricmp(
                        element.SemanticName,
                        semantic) == 0;
            });
    }

    bool HasCommonLitVertexData(
        const DirectX::ModelMeshPart& part) noexcept
    {
        const bool hasPosition =
            HasSemantic(part, "SV_Position")
            || HasSemantic(part, "POSITION");
        return hasPosition
            && HasSemantic(part, "NORMAL")
            && HasSemantic(part, "TEXCOORD");
    }

    float AdvanceClipTime(
        const float time,
        const float delta,
        const float duration,
        const bool loop) noexcept
    {
        if (duration <= 0.0f)
        {
            return 0.0f;
        }
        float result = time + delta;
        if (loop)
        {
            result = std::fmod(result, duration);
            if (result < 0.0f)
            {
                result += duration;
            }
            return result;
        }
        return std::clamp(result, 0.0f, duration);
    }
}

namespace LamaPon
{
    bool ModelRendererComponent::TryGetLocalBounds(
        Bounds3D& bounds) const noexcept
    {
        if (!m_model || !m_model->hasLocalBounds)
        {
            return false;
        }
        bounds = m_model->localBounds;
        return true;
    }

    void ModelRendererComponent::ApplyShaderRenderState(
        const ShaderRenderState& state) const
    {
        if (m_context == nullptr || m_states == nullptr)
        {
            return;
        }
        constexpr float blendFactor[4]{};
        switch (state.blend)
        {
        case ShaderBlendMode::Alpha:
            m_context->OMSetBlendState(
                m_states->NonPremultiplied(),
                blendFactor,
                0xffffffffu);
            break;
        case ShaderBlendMode::Additive:
            m_context->OMSetBlendState(
                m_states->Additive(),
                blendFactor,
                0xffffffffu);
            break;
        case ShaderBlendMode::Premultiplied:
            m_context->OMSetBlendState(
                m_states->AlphaBlend(),
                blendFactor,
                0xffffffffu);
            break;
        case ShaderBlendMode::Opaque:
        default:
            m_context->OMSetBlendState(
                m_states->Opaque(),
                blendFactor,
                0xffffffffu);
            break;
        }
        m_context->OMSetDepthStencilState(
            state.depthTest
                ? (state.depthWrite
                    ? m_states->DepthDefault()
                    : m_states->DepthRead())
                : m_states->DepthNone(),
            0);
        switch (state.cull)
        {
        case ShaderCullMode::Front:
            m_context->RSSetState(
                m_states->CullClockwise());
            break;
        case ShaderCullMode::None:
            m_context->RSSetState(
                m_states->CullNone());
            break;
        case ShaderCullMode::Back:
        default:
            m_context->RSSetState(
                m_states->CullCounterClockwise());
            break;
        }
    }

    std::array<
        ID3D11ShaderResourceView*,
        LitMaterial::CustomTextureCount>
        ModelRendererComponent::ResolveCustomTextureViews() const noexcept
    {
        // 未設定の枠はnullptrにします（LitEffect側で白へ差し替え）。
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount> views{};
        for (std::size_t index = 0;
            index < views.size();
            ++index)
        {
            views[index] = m_customTextures[index]
                ? m_customTextures[index]->view.Get()
                : nullptr;
        }
        return views;
    }

    struct ModelRendererComponent::CommonLitResources final
    {
        struct Part final
        {
            DirectX::ModelMesh* mesh{};
            DirectX::ModelMeshPart* part{};
            Microsoft::WRL::ComPtr<ID3D11InputLayout>
                inputLayout;
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>
                embeddedAlbedoTexture;
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>
                embeddedNormalTexture;
            DirectX::XMFLOAT4 embeddedDiffuseColor{
                1.0f,
                1.0f,
                1.0f,
                1.0f
            };
        };

        std::vector<Part> parts;
        std::vector<DirectX::XMMATRIX> boneTransforms;
        std::string status{
            "モデルの初期化待ち"
        };
        bool compatible{};
    };

    bool ModelRendererComponent::IsAlphaBlended3D() const
    {
        // 判定の条件は描画側（forceAlphaとpart->isAlpha）と同じ
        // ものです。片方だけ直すと「並べ替えの対象から外れたのに
        // 半透明で描かれる」パーツができるので、変えるときは両方。
        if (m_material.BaseColor().w < 0.999f)
        {
            return true;
        }
        if (m_effect != nullptr
            && m_effect->RenderState().declared)
        {
            const auto blend = m_effect->RenderState().blend;
            // 加算は順番によらないので並べ替えません。
            return blend == ShaderBlendMode::Alpha
                || blend == ShaderBlendMode::Premultiplied;
        }
        // モデルの中に半透明パーツが1つでもあれば対象にします。
        // 粒度はGameObject単位なので、不透明パーツも一緒に後回しに
        // なりますが、遠い順に描く中では手前の不透明が奥の半透明を
        // 正しく上書きするので破綻しません。
        if (m_commonLitResources)
        {
            for (const auto& part :
                m_commonLitResources->parts)
            {
                if (part.part != nullptr
                    && part.part->isAlpha)
                {
                    return true;
                }
            }
        }
        return false;
    }

    ModelRendererComponent::ModelRendererComponent(
        std::filesystem::path modelPath,
        const bool wireframe,
        const bool materialOverrideEnabled,
        const DirectX::XMFLOAT4 color,
        std::filesystem::path albedoTexture,
        std::filesystem::path normalTexture,
        const float roughness,
        const float normalStrength,
        std::filesystem::path materialAsset,
        const std::size_t animationIndex,
        const float animationSpeed,
        const bool animationLoop,
        const bool animationPlayOnStart,
        std::filesystem::path animationController,
        const bool applyRootMotion,
        std::string rootMotionNode,
        const bool preserveEmbeddedMaterialColor)
        : m_modelPath(std::move(modelPath))
        , m_material(
            color,
            std::move(albedoTexture),
            std::move(normalTexture),
            roughness,
            normalStrength)
        , m_materialAssetPath(std::move(materialAsset))
        , m_wireframe(wireframe)
        , m_materialOverrideEnabled(materialOverrideEnabled)
        , m_preserveEmbeddedMaterialColor(
            preserveEmbeddedMaterialColor)
        , m_animationIndex(animationIndex)
        , m_animationSpeed(
            std::isfinite(animationSpeed)
                ? animationSpeed
                : 1.0f)
        , m_animationLoop(animationLoop)
        , m_animationPlayOnStart(animationPlayOnStart)
        , m_animationControllerPath(
            std::move(animationController))
        , m_applyRootMotion(applyRootMotion)
        , m_rootMotionNode(
            std::move(rootMotionNode))
    {
    }

    ModelRendererComponent::~ModelRendererComponent() =
        default;

    void ModelRendererComponent::SetModelPath(std::filesystem::path modelPath)
    {
        std::shared_ptr<const ModelAsset> model;
        if (m_assets != nullptr && !modelPath.empty())
        {
            model = m_assets->CreateModelInstance(modelPath);
        }

        m_modelPath = std::move(modelPath);
        m_model = std::move(model);
        m_animationIndex = std::min(
            m_animationIndex,
            AnimationCount() > 0
                ? AnimationCount() - 1
                : std::size_t{});
        m_animationTime = 0.0f;
        m_animationPlaying =
            m_animationPlayOnStart && AnimationCount() > 0;
        if (m_animationController)
        {
            const auto* state =
                m_animationController->FindState(
                    m_currentAnimationState);
            if (state == nullptr)
            {
                state = m_animationController->FindState(
                    m_animationController->EntryState());
            }
            if (state != nullptr)
            {
                EnterAnimationState(*state);
            }
        }
        RebuildCommonLitResources();
    }

    void ModelRendererComponent::SetAnimationIndex(
        const std::size_t index) noexcept
    {
        m_animationIndex = AnimationCount() > 0
            ? std::min(index, AnimationCount() - 1)
            : 0;
        m_animationTime = 0.0f;
    }

    std::size_t
        ModelRendererComponent::AnimationCount() const noexcept
    {
        return m_model && m_model->skeletalModel
            ? m_model->skeletalModel->animations.size()
            : 0;
    }

    std::string_view ModelRendererComponent::AnimationName(
        const std::size_t index) const noexcept
    {
        if (!m_model
            || !m_model->skeletalModel
            || index >= m_model->skeletalModel->animations.size())
        {
            return {};
        }
        return m_model->skeletalModel->animations[index].name;
    }

    std::vector<std::string>
        ModelRendererComponent::SkeletonNodeNames() const
    {
        std::vector<std::string> result;
        if (!m_model || !m_model->skeletalModel)
        {
            return result;
        }
        result.reserve(
            m_model->skeletalModel->nodes.size());
        for (const auto& node :
            m_model->skeletalModel->nodes)
        {
            result.push_back(node.name);
        }
        return result;
    }

    float ModelRendererComponent::AnimationDuration() const
    {
        if (m_animationController)
        {
            if (const auto* state =
                    m_animationController->FindState(
                        m_currentAnimationState))
            {
                return StateAnimationDuration(*state);
            }
        }
        if (!m_model
            || !m_model->skeletalModel
            || m_animationIndex
                >= m_model->skeletalModel->animations.size())
        {
            return 0.0f;
        }
        return m_model->skeletalModel
            ->animations[m_animationIndex].duration;
    }

    void ModelRendererComponent::SetAnimationSpeed(
        const float speed) noexcept
    {
        m_animationSpeed =
            std::isfinite(speed) ? speed : 1.0f;
    }

    void ModelRendererComponent::PlayAnimation() noexcept
    {
        if (AnimationCount() == 0)
        {
            return;
        }
        if (m_animationTime >= AnimationDuration())
        {
            m_animationTime = 0.0f;
        }
        m_animationPlaying = true;
    }

    void ModelRendererComponent::PauseAnimation() noexcept
    {
        m_animationPlaying = false;
    }

    void ModelRendererComponent::StopAnimation() noexcept
    {
        m_animationPlaying = false;
        m_animationTime = 0.0f;
        m_nextAnimationState.clear();
        m_nextAnimationTime = 0.0f;
        m_animationTransitionTime = 0.0f;
        m_animationTransitionDuration = 0.0f;
        m_activeAnimationTriggers.clear();
        m_animationEventQueue.clear();
    }

    void ModelRendererComponent::SetAnimationControllerPath(
        std::filesystem::path path)
    {
        if (path == m_animationControllerPath)
        {
            return;
        }
        m_animationControllerPath = std::move(path);
        m_animationController.reset();
        LoadAnimationController();
    }

    void ModelRendererComponent::ReloadAnimationController()
    {
        m_animationController.reset();
        if (m_assets != nullptr
            && !m_animationControllerPath.empty())
        {
            m_animationController =
                m_assets->ReloadAnimatorController(
                    m_animationControllerPath);
        }
        LoadAnimationController();
    }

    void ModelRendererComponent::SetAnimationTrigger(
        std::string trigger)
    {
        if (trigger.empty() || trigger.size() > 96)
        {
            throw std::invalid_argument(
                "Animator trigger must contain between 1 and 96 characters.");
        }
        m_activeAnimationTriggers.insert(
            std::move(trigger));
    }

    std::vector<std::string>
        ModelRendererComponent::AnimationTriggers() const
    {
        std::vector<std::string> result;
        if (!m_animationController)
        {
            return result;
        }
        for (const auto& transition :
            m_animationController->Transitions())
        {
            if (!transition.trigger.empty()
                && std::ranges::find(
                    result,
                    transition.trigger) == result.end())
            {
                result.push_back(transition.trigger);
            }
        }
        return result;
    }

    void ModelRendererComponent::SetAnimationFloat(
        std::string parameter,
        const float value)
    {
        if (!std::isfinite(value))
        {
            throw std::invalid_argument(
                "Animator float parameter must be finite.");
        }
        const auto found =
            m_animationFloatValues.find(parameter);
        if (found == m_animationFloatValues.end())
        {
            throw std::invalid_argument(
                "Animator float parameter does not exist: "
                + parameter);
        }
        found->second = value;
    }

    float ModelRendererComponent::AnimationFloat(
        const std::string_view parameter) const noexcept
    {
        const auto found =
            m_animationFloatValues.find(
                std::string{ parameter });
        return found != m_animationFloatValues.end()
            ? found->second
            : 0.0f;
    }

    std::vector<AnimatorFloatParameter>
        ModelRendererComponent::AnimationFloatParameters() const
    {
        return m_animationController
            ? m_animationController->FloatParameters()
            : std::vector<AnimatorFloatParameter>{};
    }

    bool ModelRendererComponent::PollAnimationEvent(
        AnimationEventNotification& event)
    {
        if (m_animationEventQueue.empty())
        {
            return false;
        }
        event = std::move(
            m_animationEventQueue.front());
        m_animationEventQueue.pop_front();
        return true;
    }

    void ModelRendererComponent::CollectAnimationPoseSamples(
        std::vector<SkeletalPoseSample>& samples) const
    {
        samples.clear();
        if (!m_model
            || !m_model->skeletalModel
            || AnimationCount() == 0)
        {
            return;
        }
        if (!m_animationController)
        {
            if (m_animationIndex < AnimationCount())
            {
                const auto& animation =
                    m_model->skeletalModel
                        ->animations[m_animationIndex];
                samples.push_back({
                    &animation,
                    m_animationTime,
                    1.0f
                });
            }
            return;
        }

        const float transitionAmount =
            !m_nextAnimationState.empty()
                && m_animationTransitionDuration
                    > 0.0f
            ? std::clamp(
                m_animationTransitionTime
                    / m_animationTransitionDuration,
                0.0f,
                1.0f)
            : 0.0f;
        const auto appendState =
            [this, &samples](
                const AnimatorState* state,
                const float stateTime,
                const float outerWeight)
            {
                if (state == nullptr
                    || outerWeight <= 0.0f)
                {
                    return;
                }
                const auto appendClip =
                    [this,
                        &samples,
                        state,
                        stateTime,
                        outerWeight](
                        const std::string_view name,
                        const float weight)
                    {
                        const auto index =
                            ResolveAnimationIndex(name);
                        if (index >= AnimationCount()
                            || weight <= 0.0f)
                        {
                            return;
                        }
                        const auto& animation =
                            m_model->skeletalModel
                                ->animations[index];
                        samples.push_back({
                            &animation,
                            AdvanceClipTime(
                                0.0f,
                                stateTime,
                                animation.duration,
                                state->loop),
                            outerWeight * weight
                        });
                    };

                if (state->blendChildren.empty())
                {
                    appendClip(
                        state->modelClip.empty()
                            ? std::string_view{
                                state->name }
                            : std::string_view{
                                state->modelClip },
                        1.0f);
                    return;
                }
                std::vector<float> weights;
                CalculateBlendWeights(
                    *state,
                    weights);
                for (std::size_t index = 0;
                    index < state->
                        blendChildren.size();
                    ++index)
                {
                    appendClip(
                        state->blendChildren[
                            index].modelClip,
                        index < weights.size()
                            ? weights[index]
                            : 0.0f);
                }
            };
        appendState(
            m_animationController->FindState(
                m_currentAnimationState),
            m_animationTime,
            1.0f - transitionAmount);
        appendState(
            m_nextAnimationState.empty()
                ? nullptr
                : m_animationController->FindState(
                    m_nextAnimationState),
            m_nextAnimationTime,
            transitionAmount);
    }

    std::size_t
        ModelRendererComponent::ResolveRootMotionNode() const noexcept
    {
        if (!m_model
            || !m_model->skeletalModel)
        {
            return std::numeric_limits<
                std::size_t>::max();
        }
        const auto& nodes =
            m_model->skeletalModel->nodes;
        if (!m_rootMotionNode.empty())
        {
            for (std::size_t index = 0;
                index < nodes.size();
                ++index)
            {
                if (nodes[index].name
                    == m_rootMotionNode)
                {
                    return index;
                }
            }
            return std::numeric_limits<
                std::size_t>::max();
        }
        for (std::size_t index = 0;
            index < nodes.size();
            ++index)
        {
            if (nodes[index].parent < 0)
            {
                return index;
            }
        }
        return nodes.empty()
            ? std::numeric_limits<
                std::size_t>::max()
            : 0;
    }

    void ModelRendererComponent::ApplyRootMotionDelta(
        const std::vector<SkeletalPoseSample>& before,
        const std::vector<SkeletalPoseSample>& after)
    {
        const auto node = ResolveRootMotionNode();
        if (!m_applyRootMotion
            || !m_model
            || !m_model->skeletalModel
            || node >= m_model->skeletalModel
                ->nodes.size()
            || before.empty()
            || after.empty())
        {
            return;
        }

        struct RootPose final
        {
            DirectX::XMFLOAT3 translation{};
            DirectX::XMFLOAT4 rotation{
                0.0f,
                0.0f,
                0.0f,
                1.0f
            };
        };
        const auto sampleRoot =
            [this, node](
                const std::vector<
                    SkeletalPoseSample>& samples)
            {
                using namespace DirectX;
                std::vector<
                    SkeletalPoseTransform>
                    localPose;
                std::vector<XMFLOAT4X4>
                    globalPose;
                SkeletalModel::SampleWeightedPose(
                    m_model->skeletalModel->nodes,
                    samples,
                    localPose,
                    globalPose);
                RootPose result;
                XMVECTOR scale{};
                XMVECTOR rotation{};
                XMVECTOR translation{};
                if (XMMatrixDecompose(
                        &scale,
                        &rotation,
                        &translation,
                        XMLoadFloat4x4(
                            &globalPose[node])))
                {
                    XMStoreFloat3(
                        &result.translation,
                        translation);
                    XMStoreFloat4(
                        &result.rotation,
                        XMQuaternionNormalize(
                            rotation));
                }
                return result;
            };

        const RootPose beforePose =
            sampleRoot(before);
        const RootPose afterPose =
            sampleRoot(after);
        DirectX::XMFLOAT3 delta{
            afterPose.translation.x
                - beforePose.translation.x,
            afterPose.translation.y
                - beforePose.translation.y,
            afterPose.translation.z
                - beforePose.translation.z
        };
        float yawDelta{};
        const auto yawFrom =
            [](const DirectX::XMFLOAT4& rotation)
            {
                using namespace DirectX;
                const XMVECTOR forward =
                    XMVector3Rotate(
                        XMVectorSet(
                            0.0f,
                            0.0f,
                            1.0f,
                            0.0f),
                        XMLoadFloat4(&rotation));
                return std::atan2(
                    XMVectorGetX(forward),
                    XMVectorGetZ(forward));
            };
        yawDelta = std::remainder(
            yawFrom(afterPose.rotation)
                - yawFrom(beforePose.rotation),
            std::numbers::pi_v<float>
                * 2.0f);

        const bool looped =
            std::ranges::any_of(
                before,
                [&after](
                    const SkeletalPoseSample&
                        previous)
                {
                    const auto found =
                        std::ranges::find_if(
                            after,
                            [&previous](
                                const SkeletalPoseSample&
                                    current)
                            {
                                return current.clip
                                    == previous.clip;
                            });
                    return found != after.end()
                        && found->time
                            < previous.time;
                });
        if (looped)
        {
            auto endSamples = before;
            auto startSamples = after;
            for (auto& sample : endSamples)
            {
                sample.time = sample.clip
                    ? sample.clip->duration
                    : 0.0f;
            }
            for (auto& sample : startSamples)
            {
                sample.time = 0.0f;
            }
            const RootPose endPose =
                sampleRoot(endSamples);
            const RootPose startPose =
                sampleRoot(startSamples);
            delta = {
                (endPose.translation.x
                    - beforePose.translation.x)
                    + (afterPose.translation.x
                        - startPose.translation.x),
                (endPose.translation.y
                    - beforePose.translation.y)
                    + (afterPose.translation.y
                        - startPose.translation.y),
                (endPose.translation.z
                    - beforePose.translation.z)
                    + (afterPose.translation.z
                        - startPose.translation.z)
            };
            yawDelta =
                std::remainder(
                    yawFrom(endPose.rotation)
                        - yawFrom(
                            beforePose.rotation),
                    std::numbers::pi_v<float>
                        * 2.0f)
                + std::remainder(
                    yawFrom(afterPose.rotation)
                        - yawFrom(
                            startPose.rotation),
                    std::numbers::pi_v<float>
                        * 2.0f);
        }

        auto& transform = GetTransform();
        using namespace DirectX;
        const XMVECTOR worldDelta =
            XMVector3Rotate(
                XMLoadFloat3(&delta),
                transform.RotationVector());
        XMFLOAT3 rotatedDelta{};
        XMStoreFloat3(
            &rotatedDelta,
            worldDelta);
        transform.position.x +=
            rotatedDelta.x;
        transform.position.y +=
            rotatedDelta.y;
        transform.position.z +=
            rotatedDelta.z;
        transform.Rotate({ 0.0f, 1.0f, 0.0f }, yawDelta);
    }

    void ModelRendererComponent::DispatchAnimationEvents(
        const AnimatorState& state,
        const float previousTime,
        const float currentTime,
        const float duration,
        const bool looped,
        const bool forward)
    {
        if (duration <= 0.0f
            || state.events.empty())
        {
            return;
        }
        const float previous =
            std::clamp(
                previousTime / duration,
                0.0f,
                1.0f);
        const float current =
            std::clamp(
                currentTime / duration,
                0.0f,
                1.0f);
        for (const auto& event : state.events)
        {
            bool crossed{};
            if (forward)
            {
                crossed = looped
                    ? event.normalizedTime
                            > previous
                        || event.normalizedTime
                            <= current
                    : (event.normalizedTime
                            > previous
                        || (previous <= 0.0f
                            && event.normalizedTime
                                <= 0.0f))
                        && event.normalizedTime
                            <= current;
            }
            else
            {
                crossed = looped
                    ? event.normalizedTime
                            < previous
                        || event.normalizedTime
                            >= current
                    : event.normalizedTime
                            < previous
                        && event.normalizedTime
                            >= current;
            }
            if (!crossed)
            {
                continue;
            }
            if (m_animationEventQueue.size()
                >= 256)
            {
                m_animationEventQueue.pop_front();
            }
            m_animationEventQueue.push_back({
                state.name,
                event.name,
                event.payload,
                event.normalizedTime
            });
        }
    }

    void ModelRendererComponent::SetAnimationTime(
        const float time) noexcept
    {
        m_animationTime = std::clamp(
            std::isfinite(time) ? time : 0.0f,
            0.0f,
            AnimationDuration());
    }

    void ModelRendererComponent::AdvanceAnimation(
        const float deltaTime,
        const bool allowRootMotion)
    {
        std::vector<SkeletalPoseSample>
            rootMotionBefore;
        if (allowRootMotion
            && m_applyRootMotion
            && m_animationPlaying
            && deltaTime > 0.0f)
        {
            CollectAnimationPoseSamples(
                rootMotionBefore);
        }
        if (m_animationController)
        {
            AdvanceControllerAnimation(deltaTime);
            if (!rootMotionBefore.empty())
            {
                std::vector<SkeletalPoseSample>
                    rootMotionAfter;
                CollectAnimationPoseSamples(
                    rootMotionAfter);
                ApplyRootMotionDelta(
                    rootMotionBefore,
                    rootMotionAfter);
            }
            return;
        }
        const float duration = AnimationDuration();
        if (!m_animationPlaying
            || duration <= 0.0f
            || deltaTime <= 0.0f)
        {
            return;
        }

        m_animationTime += deltaTime * m_animationSpeed;
        if (m_animationLoop)
        {
            m_animationTime = std::fmod(
                m_animationTime,
                duration);
            if (m_animationTime < 0.0f)
            {
                m_animationTime += duration;
            }
        }
        else if (m_animationTime >= duration)
        {
            m_animationTime = duration;
            m_animationPlaying = false;
        }
        else if (m_animationTime <= 0.0f)
        {
            m_animationTime = 0.0f;
            m_animationPlaying = false;
        }
        if (!rootMotionBefore.empty())
        {
            std::vector<SkeletalPoseSample>
                rootMotionAfter;
            CollectAnimationPoseSamples(
                rootMotionAfter);
            ApplyRootMotionDelta(
                rootMotionBefore,
                rootMotionAfter);
        }
    }

    void ModelRendererComponent::SetMaterialOverrideEnabled(
        const bool enabled)
    {
        if (m_materialOverrideEnabled == enabled)
        {
            return;
        }

        if (!enabled)
        {
            ReloadModel();
        }
        m_materialOverrideEnabled = enabled;
    }

    void ModelRendererComponent::SetUseLegacyShading(
        const bool enabled)
    {
        if (m_useLegacyShading == enabled)
        {
            return;
        }

        m_useLegacyShading = enabled;
        // 既定のLitと従来のDirectXTK描画を切り替えるので、
        // スキニング用Effectを選び直します。
        RefreshShader(false);
    }

    void ModelRendererComponent::SetAlbedoTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }

        if (texturePath.empty()
            && !m_material.AlbedoTexture().empty())
        {
            ReloadModel();
        }
        m_material.SetAlbedoTexture(std::move(texturePath));
        m_albedoTexture = std::move(texture);
    }

    void ModelRendererComponent::SetRoughnessTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }
        m_material.SetRoughnessTexture(
            std::move(texturePath));
        m_roughnessTexture = std::move(texture);
    }

    void ModelRendererComponent::SetMetallicTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }
        m_material.SetMetallicTexture(
            std::move(texturePath));
        m_metallicTexture = std::move(texture);
    }

    void ModelRendererComponent::SetOcclusionTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }
        m_material.SetOcclusionTexture(
            std::move(texturePath));
        m_occlusionTexture = std::move(texture);
    }

    void ModelRendererComponent::SetEmissiveTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }
        m_material.SetEmissiveTexture(
            std::move(texturePath));
        m_emissiveTexture = std::move(texture);
    }

    void ModelRendererComponent::SetCustomTexturePath(
        const std::size_t index,
        std::filesystem::path path)
    {
        if (index >= LitMaterial::CustomTextureCount)
        {
            return;
        }
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetCustomTexture(index, std::move(path));
        m_customTextures[index] = std::move(texture);
    }

    void ModelRendererComponent::SetNormalTexturePath(
        std::filesystem::path texturePath)
    {
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !texturePath.empty())
        {
            texture = m_assets->LoadTexture(texturePath);
        }

        if (texturePath.empty()
            && !m_material.NormalTexture().empty())
        {
            ReloadModel();
        }
        m_material.SetNormalTexture(std::move(texturePath));
        m_normalTexture = std::move(texture);
    }

    void ModelRendererComponent::SetShaderPath(
        std::filesystem::path path)
    {
        m_material.SetShader(std::move(path));
        RefreshShader(false);
    }

    void ModelRendererComponent::ReloadShader()
    {
        RefreshShader(true);
    }

    void ModelRendererComponent::SetMaterialAssetPath(
        std::filesystem::path path)
    {
        if (!path.empty() && m_assets != nullptr)
        {
            ApplyMaterial(LoadLitMaterialAsset(
                m_assets->ResolvePath(path),
                &m_assets->Database(),
                m_assets));
        }
        m_materialAssetPath = std::move(path);
        if (!m_materialAssetPath.empty())
        {
            m_materialOverrideEnabled = true;
        }
    }

    void ModelRendererComponent::ReloadMaterialAsset()
    {
        if (m_materialAssetPath.empty())
        {
            return;
        }
        if (m_assets == nullptr)
        {
            throw std::runtime_error(
                "ModelRenderer is not initialized.");
        }
        ApplyMaterial(LoadLitMaterialAsset(
            m_assets->ResolvePath(m_materialAssetPath),
            &m_assets->Database(),
            m_assets));
        m_materialOverrideEnabled = true;
    }

    void ModelRendererComponent::ApplyMaterial(
        const LitMaterial& material)
    {
        std::shared_ptr<const TextureAsset> albedo;
        std::shared_ptr<const TextureAsset> normal;
        std::shared_ptr<const TextureAsset> roughness;
        std::shared_ptr<const TextureAsset> metallic;
        std::shared_ptr<const TextureAsset> occlusion;
        std::shared_ptr<const TextureAsset> emissive;
        if (m_assets != nullptr)
        {
            // スロットごとに用途を渡します。法線はBC5、粗さ・
            // 金属度・遮蔽はBC1、色はBC1/BC3です。
            using Usage = TextureLoader::TextureUsage;
            const auto load =
                [this](
                    const std::filesystem::path& path,
                    const Usage usage)
                -> std::shared_ptr<const TextureAsset>
                {
                    if (path.empty())
                    {
                        return {};
                    }
                    return m_assets->LoadTexture(path, usage);
                };
            albedo = load(material.AlbedoTexture(), Usage::Color);
            normal = load(
                material.NormalTexture(),
                Usage::NormalMap);
            roughness = load(
                material.RoughnessTexture(),
                Usage::DataMap);
            metallic = load(
                material.MetallicTexture(),
                Usage::DataMap);
            occlusion = load(
                material.OcclusionTexture(),
                Usage::DataMap);
            emissive = load(
                material.EmissiveTexture(),
                Usage::Color);
        }

        const bool removedAlbedo =
            !m_material.AlbedoTexture().empty()
            && material.AlbedoTexture().empty();
        const bool removedNormal =
            !m_material.NormalTexture().empty()
            && material.NormalTexture().empty();
        m_material = material;
        m_albedoTexture = std::move(albedo);
        m_normalTexture = std::move(normal);
        m_roughnessTexture = std::move(roughness);
        m_metallicTexture = std::move(metallic);
        m_occlusionTexture = std::move(occlusion);
        m_emissiveTexture = std::move(emissive);
        if (removedAlbedo || removedNormal)
        {
            ReloadModel();
        }
        RefreshShader(false);
    }

    void ModelRendererComponent::RefreshShader(
        const bool forceReload)
    {
        if (m_graphics == nullptr)
        {
            return;
        }
        if (forceReload)
        {
            m_graphics->InvalidateMaterialShader(
                m_material.Shader());
        }

        if (m_model && m_model->skeletalModel)
        {
            std::uint64_t generation{};
            std::string compileError;
            auto* effect = m_graphics->SkinnedMaterialShader(
                m_material.Shader(),
                generation,
                compileError,
                m_material.ShaderKeywords());
            // カスタムShaderが指定されていなければ、既定の
            // LamaPon Lit（PBR）で描きます。互換トグルが有効な
            // ときだけ従来のDirectXTK描画へ戻します。
            // 指定があってコンパイルに失敗した場合はマゼンタの
            // 代役が返るので、ここで標準Litへ倒すとエラーを
            // 隠してしまいます。
            if (effect == nullptr
                && !m_useLegacyShading
                && m_material.Shader().empty())
            {
                effect = &m_graphics->SkinnedLit();
            }
            effect = SubstituteUnsupportedTessellation(
                effect,
                *m_graphics,
                true,
                compileError);
            if (m_skinnedEffect == effect
                && m_activeShaderPath == m_material.Shader()
                && m_shaderGeneration == generation)
            {
                m_shaderError = std::move(compileError);
                return;
            }

            m_skinnedEffect = effect;
            m_effect = nullptr;
            m_activeShaderPath = m_material.Shader();
            m_shaderGeneration = generation;
            m_shaderError = std::move(compileError);
            m_skinnedInputLayout.Reset();
            if (effect != nullptr)
            {
                const void* shaderByteCode{};
                std::size_t shaderByteCodeSize{};
                effect->GetVertexShaderBytecode(
                    &shaderByteCode,
                    &shaderByteCodeSize);
                const HRESULT result =
                    m_graphics->Device()->CreateInputLayout(
                        DirectX::
                            VertexPositionNormalTangentColorTextureSkinning::
                                InputElements,
                        DirectX::
                            VertexPositionNormalTangentColorTextureSkinning::
                                InputElementCount,
                        shaderByteCode,
                        shaderByteCodeSize,
                        m_skinnedInputLayout.
                            ReleaseAndGetAddressOf());
                if (FAILED(result))
                {
                    m_shaderError =
                        "Skinned shader input layout is incompatible.";
                    m_skinnedEffect = nullptr;
                }
            }
            RebuildCommonLitResources();
            return;
        }

        m_skinnedEffect = nullptr;
        m_skinnedInputLayout.Reset();
        std::uint64_t generation{};
        std::string compileError;
        auto* effect = &m_graphics->MaterialShader(
            m_material.Shader(),
            generation,
            compileError,
            m_material.ShaderKeywords());
        m_shaderError = std::move(compileError);
        effect = SubstituteUnsupportedTessellation(
            effect,
            *m_graphics,
            false,
            m_shaderError);
        if (m_effect == effect
            && m_activeShaderPath == m_material.Shader()
            && m_shaderGeneration == generation)
        {
            return;
        }
        m_effect = effect;
        m_activeShaderPath = m_material.Shader();
        m_shaderGeneration = generation;
        RebuildCommonLitResources();
    }

    void ModelRendererComponent::OnInitialize(GraphicsDevice& graphics)
    {
        m_assets = &graphics.Assets();
        m_graphics = &graphics;
        m_context = graphics.Context();
        m_states = &graphics.States();

        if (!m_materialAssetPath.empty())
        {
            m_material = LoadLitMaterialAsset(
                m_assets->ResolvePath(m_materialAssetPath),
                &m_assets->Database(),
                m_assets);
            m_materialOverrideEnabled = true;
        }

        if (!m_modelPath.empty())
        {
            m_model = m_assets->CreateModelInstance(m_modelPath);
        }
        SetAnimationIndex(m_animationIndex);
        LoadAnimationController();
        m_animationPlaying =
            m_animationPlayOnStart && AnimationCount() > 0;
        if (!m_material.AlbedoTexture().empty())
        {
            m_albedoTexture = m_assets->LoadTexture(
                m_material.AlbedoTexture());
        }
        if (!m_material.NormalTexture().empty())
        {
            m_normalTexture = m_assets->LoadTexture(
                m_material.NormalTexture(),
                TextureLoader::TextureUsage::NormalMap);
        }
        if (!m_material.RoughnessTexture().empty())
        {
            m_roughnessTexture = m_assets->LoadTexture(
                m_material.RoughnessTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.MetallicTexture().empty())
        {
            m_metallicTexture = m_assets->LoadTexture(
                m_material.MetallicTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.OcclusionTexture().empty())
        {
            m_occlusionTexture = m_assets->LoadTexture(
                m_material.OcclusionTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.EmissiveTexture().empty())
        {
            m_emissiveTexture = m_assets->LoadTexture(
                m_material.EmissiveTexture());
        }
        // カスタムShaderが宣言した追加テクスチャ（t7以降）。
        for (std::size_t index = 0;
            index < LitMaterial::CustomTextureCount;
            ++index)
        {
            const auto& path =
                m_material.CustomTexture(index);
            m_customTextures[index] = path.empty()
                ? nullptr
                : m_assets->LoadTexture(path);
        }
        RebuildCommonLitResources();
    }

    void ModelRendererComponent::OnUpdate(const float deltaTime)
    {
        AdvanceAnimation(deltaTime);
    }

    bool ModelRendererComponent::HasPreRender3DPass()
    {
        RefreshShader(false);
        return !m_wireframe
            && UsesCommonLit()
            && m_effect != nullptr
            && m_effect->HasOccludedPass()
            && m_material.CustomParameter(4).w > 0.0f;
    }

    void ModelRendererComponent::OnPreRender3D(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        // 原作と同様、キャラクター全パーツの通常描画より先に遮蔽部分だけを描きます。
        if (HasPreRender3DPass())
        {
            DrawCommonLit(view, projection, true);
        }
    }

    void ModelRendererComponent::OnRender3D(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (m_instancedThisPass)
        {
            m_instancedThisPass = false;
            return;
        }
        RefreshShader(false);
        if (!m_model
            || (!m_model->model
                && !m_model->skeletalModel)
            || m_graphics == nullptr
            || m_context == nullptr
            || m_states == nullptr)
        {
            return;
        }

        // MeshRendererと同じ理由です（LitEffect.hのコメント参照）。
        const GeometryShaderScope geometryScope{
            m_effect != nullptr
                && m_effect->HasGeometryShader()
                ? m_context
                : nullptr
        };

        if (m_model->skeletalModel)
        {
            const auto* clip =
                m_animationIndex
                    < m_model->skeletalModel->animations.size()
                ? &m_model->skeletalModel
                    ->animations[m_animationIndex]
                : nullptr;
            const auto* blendClip =
                !m_nextAnimationState.empty()
                    && m_nextAnimationIndex
                        < m_model->skeletalModel->animations.size()
                ? &m_model->skeletalModel
                    ->animations[m_nextAnimationIndex]
                : nullptr;
            const float blendAmount =
                blendClip != nullptr
                    && m_animationTransitionDuration > 0.0f
                ? std::clamp(
                    m_animationTransitionTime
                        / m_animationTransitionDuration,
                    0.0f,
                    1.0f)
                : 0.0f;
            const auto poseFrame =
                m_graphics->FrameStats().totalFrames;
            const auto* poseModel =
                m_model->skeletalModel.get();
            if (m_cachedPoseFrame != poseFrame
                || m_cachedPoseModel != poseModel)
            {
                LAMAPON_PROFILE_SCOPE("SkeletalModel.Pose");
                std::vector<SkeletalPoseTransform> localPose;
                std::vector<SkeletalPoseSample>
                    weightedSamples;
                CollectAnimationPoseSamples(
                    weightedSamples);
                if (m_applyRootMotion
                    && weightedSamples.empty()
                    && clip != nullptr)
                {
                    weightedSamples.push_back({
                        clip,
                        m_animationTime,
                        1.0f
                    });
                }
                if (!weightedSamples.empty())
                {
                    SkeletalModel::SampleWeightedPose(
                        m_model->skeletalModel->nodes,
                        weightedSamples,
                        localPose,
                        m_cachedGlobalPose,
                        m_applyRootMotion
                            ? ResolveRootMotionNode()
                            : std::numeric_limits<
                                std::size_t>::max());
                }
                else if (blendClip != nullptr
                    && blendAmount > 0.0f)
                {
                    SkeletalModel::SampleBlendedPose(
                        m_model->skeletalModel->nodes,
                        clip,
                        m_animationTime,
                        blendClip,
                        m_nextAnimationTime,
                        blendAmount,
                        localPose,
                        m_cachedGlobalPose);
                }
                else
                {
                    SkeletalModel::SamplePose(
                        m_model->skeletalModel->nodes,
                        clip,
                        m_animationTime,
                        localPose,
                        m_cachedGlobalPose);
                }
                m_cachedPoseFrame = poseFrame;
                m_cachedPoseModel = poseModel;
            }
            // マテリアル上書き時にモデル自身のPBRマップより優先させる
            // 一式。上書きが無ければDraw側で無視されます。
            const auto overridePbrTextures = BuildPbrTextures(
                m_roughnessTexture,
                m_metallicTexture,
                m_occlusionTexture,
                m_emissiveTexture,
                m_material);
            m_model->skeletalModel->Draw(
                m_context,
                *m_states,
                m_graphics->Lighting(),
                Owner().WorldMatrix(),
                view,
                projection,
                clip,
                m_animationTime,
                m_wireframe,
                m_materialOverrideEnabled
                    ? &m_material
                    : nullptr,
                m_albedoTexture
                    ? m_albedoTexture->view.Get()
                    : nullptr,
                m_normalTexture
                    ? m_normalTexture->view.Get()
                    : nullptr,
                &overridePbrTextures,
                blendClip,
                m_nextAnimationTime,
                blendAmount,
                nullptr,
                m_applyRootMotion
                    ? ResolveRootMotionNode()
                    : std::numeric_limits<
                        std::size_t>::max(),
                // マテリアル上書きの有無に関係なくLitで描きます。
                // 上書きが無ければモデル自身の材質が使われます
                // （互換トグル時はm_skinnedEffectがnullptrなので
                // 従来のDirectXTK描画になります）。
                m_skinnedEffect,
                m_skinnedInputLayout.Get(),
                m_graphics->IsDepthOnlyPass(),
                &m_cachedGlobalPose,
                m_graphics->Settings().automaticLodQuality);
            return;
        }

        if (UsesCommonLit())
        {
            DrawCommonLit(view, projection);
            return;
        }

        m_model->model->UpdateEffects(
            [this](DirectX::IEffect* effect)
            {
                if (m_materialOverrideEnabled)
                {
                    const auto color = DirectX::XMLoadFloat4(
                        &m_material.BaseColor());
                    const float alpha = m_material.BaseColor().w;
                    const float specularPower =
                        SpecularPowerFromRoughness(
                            m_material.Roughness());
                    const auto specularColor =
                        SpecularColorFromRoughness(
                            m_material.Roughness());
                    auto* const albedo = m_albedoTexture
                        ? m_albedoTexture->view.Get()
                        : nullptr;
                    auto* const normal = m_normalTexture
                        ? m_normalTexture->view.Get()
                        : nullptr;

                    if (auto* normalMap =
                        dynamic_cast<DirectX::NormalMapEffect*>(
                            effect))
                    {
                        normalMap->SetDiffuseColor(color);
                        normalMap->SetAlpha(alpha);
                        normalMap->SetSpecularColor(
                            specularColor);
                        normalMap->SetSpecularPower(
                            specularPower);
                        if (albedo != nullptr)
                        {
                            normalMap->SetTexture(albedo);
                        }
                        if (normal != nullptr)
                        {
                            normalMap->SetNormalTexture(normal);
                        }
                    }
                    else if (auto* basic =
                        dynamic_cast<DirectX::BasicEffect*>(effect))
                    {
                        basic->SetDiffuseColor(color);
                        basic->SetAlpha(alpha);
                        basic->SetSpecularColor(specularColor);
                        basic->SetSpecularPower(specularPower);
                        if (albedo != nullptr)
                        {
                            basic->SetTextureEnabled(true);
                            basic->SetTexture(albedo);
                        }
                    }
                    else if (auto* skinned =
                        dynamic_cast<DirectX::SkinnedEffect*>(effect))
                    {
                        skinned->SetDiffuseColor(color);
                        skinned->SetAlpha(alpha);
                        skinned->SetSpecularColor(
                            specularColor);
                        skinned->SetSpecularPower(
                            specularPower);
                        if (albedo != nullptr)
                        {
                            skinned->SetTexture(albedo);
                        }
                    }
                    else if (auto* dgsl =
                        dynamic_cast<DirectX::DGSLEffect*>(effect))
                    {
                        dgsl->SetDiffuseColor(color);
                        dgsl->SetAlpha(alpha);
                        dgsl->SetSpecularColor(specularColor);
                        dgsl->SetSpecularPower(specularPower);
                        if (albedo != nullptr)
                        {
                            dgsl->SetTextureEnabled(true);
                            dgsl->SetTexture(albedo);
                        }
                    }
                    else if (auto* alphaTest =
                        dynamic_cast<DirectX::AlphaTestEffect*>(
                            effect))
                    {
                        alphaTest->SetDiffuseColor(color);
                        alphaTest->SetAlpha(alpha);
                        if (albedo != nullptr)
                        {
                            alphaTest->SetTexture(albedo);
                        }
                    }
                    else if (auto* dualTexture =
                        dynamic_cast<DirectX::DualTextureEffect*>(
                            effect))
                    {
                        dualTexture->SetDiffuseColor(color);
                        dualTexture->SetAlpha(alpha);
                        if (albedo != nullptr)
                        {
                            dualTexture->SetTexture(albedo);
                        }
                    }
                    else if (auto* environment =
                        dynamic_cast<
                            DirectX::EnvironmentMapEffect*>(
                            effect))
                    {
                        environment->SetDiffuseColor(color);
                        environment->SetAlpha(alpha);
                        if (albedo != nullptr)
                        {
                            environment->SetTexture(albedo);
                        }
                    }
                }

                if (auto* lights =
                    dynamic_cast<DirectX::IEffectLights*>(effect))
                {
                    DirectX::XMFLOAT3 position{};
                    DirectX::XMStoreFloat3(
                        &position,
                        Owner().WorldMatrix().r[3]);
                    ApplyLighting(
                        *lights,
                        m_graphics->Lighting(),
                        position);
                }
            });

        // DirectXTK描画（CMO/SDKMESHなど）。深度パスにはレンダー
        // ターゲットが無いのに、DirectXTKのEffectは必ずピクセル
        // シェーダーを設定します。Applyの直後に呼ばれるこの手続きで
        // 外して、捨てられるだけの計算が影マップの解像度ぶん走るのを
        // 止めます。切り抜き（アルファテスト）はdiscardで影の形を
        // 決めているので、そのときだけ残します。
        const bool depthOnly = m_graphics->IsDepthOnlyPass();
        bool hasAlphaTest = false;
        if (depthOnly)
        {
            m_model->model->UpdateEffects(
                [&hasAlphaTest](DirectX::IEffect* effect)
                {
                    if (dynamic_cast<
                            DirectX::AlphaTestEffect*>(effect)
                        != nullptr)
                    {
                        hasAlphaTest = true;
                    }
                });
        }
        m_model->model->Draw(
            m_context,
            *m_states,
            Owner().WorldMatrix(),
            view,
            projection,
            m_wireframe,
            depthOnly && !hasAlphaTest
                ? std::function<void()>{
                    [this]
                    {
                        m_context->PSSetShader(
                            nullptr,
                            nullptr,
                            0);
                    } }
                : std::function<void()>{});
    }

    void ModelRendererComponent::LoadAnimationController()
    {
        m_currentAnimationState.clear();
        m_nextAnimationState.clear();
        m_activeAnimationTriggers.clear();
        m_animationFloatValues.clear();
        m_animationEventQueue.clear();
        m_animationTime = 0.0f;
        m_nextAnimationTime = 0.0f;
        m_animationTransitionTime = 0.0f;
        m_animationTransitionDuration = 0.0f;

        if (m_animationControllerPath.empty())
        {
            m_animationController.reset();
            return;
        }
        if (m_assets == nullptr || AnimationCount() == 0)
        {
            return;
        }
        if (!m_animationController)
        {
            m_animationController =
                m_assets->LoadAnimatorController(
                    m_animationControllerPath);
        }
        for (const auto& parameter :
            m_animationController->FloatParameters())
        {
            m_animationFloatValues.emplace(
                parameter.name,
                parameter.defaultValue);
        }
        for (const auto& state :
            m_animationController->States())
        {
            if (!state.blendChildren.empty())
            {
                for (const auto& child :
                    state.blendChildren)
                {
                    if (child.modelClip.empty()
                        || ResolveAnimationIndex(
                            child.modelClip)
                            == std::numeric_limits<
                                std::size_t>::max())
                    {
                        throw std::runtime_error(
                            "Animator Blend Tree state '"
                            + state.name
                            + "' references missing model clip '"
                            + child.modelClip + "'.");
                    }
                }
            }
            else if (ResolveAnimationIndex(state)
                == std::numeric_limits<std::size_t>::max())
            {
                const std::string clipName =
                    state.modelClip.empty()
                    ? state.name
                    : state.modelClip;
                throw std::runtime_error(
                    "Animator state '" + state.name
                    + "' references missing model clip '"
                    + clipName + "'.");
            }
        }
        const auto* entry =
            m_animationController->FindState(
                m_animationController->EntryState());
        if (entry == nullptr)
        {
            throw std::runtime_error(
                "Animator entry state is missing.");
        }
        EnterAnimationState(*entry);
    }

    std::size_t ModelRendererComponent::ResolveAnimationIndex(
        const AnimatorState& state) const
    {
        if (!state.blendChildren.empty())
        {
            return ResolveAnimationIndex(
                state.blendChildren.front().modelClip);
        }
        const std::string_view clipName =
            state.modelClip.empty()
            ? std::string_view{ state.name }
            : std::string_view{ state.modelClip };
        return ResolveAnimationIndex(clipName);
    }

    std::size_t ModelRendererComponent::ResolveAnimationIndex(
        const std::string_view modelClip) const noexcept
    {
        for (std::size_t index = 0;
            index < AnimationCount();
            ++index)
        {
            if (AnimationName(index) == modelClip)
            {
                return index;
            }
        }
        return std::numeric_limits<std::size_t>::max();
    }

    void ModelRendererComponent::CalculateBlendWeights(
        const AnimatorState& state,
        std::vector<float>& weights) const
    {
        const auto& children =
            state.blendChildren;
        weights.assign(children.size(), 0.0f);
        if (children.empty())
        {
            return;
        }
        if (state.blendTreeType
            == AnimatorBlendTreeType::TwoDimensional)
        {
            weights =
                AnimatorController::
                    Calculate2DBlendWeights(
                        children,
                        AnimationFloat(
                            state.blendParameter),
                        AnimationFloat(
                            state.blendParameterY));
            return;
        }

        const float value =
            AnimationFloat(
                state.blendParameter);
        if (value <= children.front().threshold)
        {
            weights.front() = 1.0f;
            return;
        }
        if (value >= children.back().threshold)
        {
            weights.back() = 1.0f;
            return;
        }
        for (std::size_t index = 1;
            index < children.size();
            ++index)
        {
            if (value > children[index].threshold)
            {
                continue;
            }
            const auto& left = children[index - 1];
            const auto& right = children[index];
            const float amount = std::clamp(
                (value - left.threshold)
                    / std::max(
                        right.threshold
                            - left.threshold,
                        0.000001f),
                0.0f,
                1.0f);
            weights[index - 1] =
                1.0f - amount;
            weights[index] = amount;
            return;
        }
    }

    float ModelRendererComponent::StateAnimationDuration(
        const AnimatorState& state) const
    {
        const auto durationFor =
            [this](const std::string_view clip)
            {
                const auto index =
                    ResolveAnimationIndex(clip);
                return m_model
                    && m_model->skeletalModel
                    && index < AnimationCount()
                    ? m_model->skeletalModel
                        ->animations[index].duration
                    : 0.0f;
            };
        if (state.blendChildren.empty())
        {
            return durationFor(
                state.modelClip.empty()
                    ? std::string_view{ state.name }
                    : std::string_view{
                        state.modelClip });
        }
        std::vector<float> weights;
        CalculateBlendWeights(state, weights);
        float duration{};
        for (std::size_t index = 0;
            index < state.blendChildren.size();
            ++index)
        {
            duration += durationFor(
                state.blendChildren[
                    index].modelClip)
                * (index < weights.size()
                    ? weights[index]
                    : 0.0f);
        }
        return duration;
    }

    void ModelRendererComponent::EnterAnimationState(
        const AnimatorState& state)
    {
        const auto index = ResolveAnimationIndex(state);
        if (index == std::numeric_limits<std::size_t>::max())
        {
            return;
        }
        m_animationIndex = index;
        m_currentAnimationState = state.name;
        m_animationTime = 0.0f;
        m_nextAnimationState.clear();
        m_nextAnimationTime = 0.0f;
        m_animationTransitionTime = 0.0f;
        m_animationTransitionDuration = 0.0f;
    }

    void ModelRendererComponent::StartAnimationTransition(
        const AnimatorTransition& transition)
    {
        if (!m_animationController)
        {
            return;
        }
        const auto* target =
            m_animationController->FindState(
                transition.to);
        if (target == nullptr)
        {
            return;
        }
        const auto targetIndex =
            ResolveAnimationIndex(*target);
        if (targetIndex
            == std::numeric_limits<std::size_t>::max())
        {
            return;
        }
        if (!transition.trigger.empty())
        {
            m_activeAnimationTriggers.erase(
                transition.trigger);
        }
        if (transition.duration <= 0.0f)
        {
            EnterAnimationState(*target);
            return;
        }
        m_nextAnimationState = target->name;
        m_nextAnimationIndex = targetIndex;
        m_nextAnimationTime = 0.0f;
        m_animationTransitionTime = 0.0f;
        m_animationTransitionDuration =
            transition.duration;
    }

    void ModelRendererComponent::AdvanceControllerAnimation(
        const float deltaTime)
    {
        if (!m_animationPlaying
            || !m_animationController
            || deltaTime <= 0.0f
            || AnimationCount() == 0)
        {
            return;
        }
        const auto* current =
            m_animationController->FindState(
                m_currentAnimationState);
        if (current == nullptr)
        {
            return;
        }
        const float currentDuration =
            StateAnimationDuration(*current);
        const float previousTime =
            m_animationTime;
        const float currentDelta =
            deltaTime * m_animationSpeed
                * current->speed;
        m_animationTime = AdvanceClipTime(
            previousTime,
            currentDelta,
            currentDuration,
            current->loop);
        const bool currentForward =
            currentDelta >= 0.0f;
        const bool currentLooped =
            current->loop
            && (currentForward
                ? m_animationTime < previousTime
                : m_animationTime > previousTime);
        DispatchAnimationEvents(
            *current,
            previousTime,
            m_animationTime,
            currentDuration,
            currentLooped,
            currentForward);

        if (m_nextAnimationState.empty())
        {
            const float normalizedTime =
                currentDuration > 0.0f
                ? m_animationTime / currentDuration
                : 1.0f;
            if (const auto* transition =
                    m_animationController->FindTransition(
                        m_currentAnimationState,
                        normalizedTime,
                        m_activeAnimationTriggers))
            {
                StartAnimationTransition(*transition);
            }
        }
        if (m_nextAnimationState.empty())
        {
            return;
        }

        const auto* next =
            m_animationController->FindState(
                m_nextAnimationState);
        if (next == nullptr
            || m_nextAnimationIndex >= AnimationCount())
        {
            m_nextAnimationState.clear();
            return;
        }
        const float nextDuration =
            StateAnimationDuration(*next);
        const float previousNextTime =
            m_nextAnimationTime;
        const float nextDelta =
            deltaTime * m_animationSpeed
                * next->speed;
        m_nextAnimationTime = AdvanceClipTime(
            previousNextTime,
            nextDelta,
            nextDuration,
            next->loop);
        const bool nextForward =
            nextDelta >= 0.0f;
        const bool nextLooped =
            next->loop
            && (nextForward
                ? m_nextAnimationTime
                    < previousNextTime
                : m_nextAnimationTime
                    > previousNextTime);
        DispatchAnimationEvents(
            *next,
            previousNextTime,
            m_nextAnimationTime,
            nextDuration,
            nextLooped,
            nextForward);
        m_animationTransitionTime += deltaTime;
        if (m_animationTransitionTime
            < m_animationTransitionDuration)
        {
            return;
        }

        m_animationIndex = m_nextAnimationIndex;
        m_currentAnimationState =
            std::move(m_nextAnimationState);
        m_animationTime = m_nextAnimationTime;
        m_nextAnimationTime = 0.0f;
        m_animationTransitionTime = 0.0f;
        m_animationTransitionDuration = 0.0f;
    }

    void ModelRendererComponent::ReloadModel()
    {
        if (m_assets == nullptr || m_modelPath.empty())
        {
            return;
        }
        m_model = m_assets->CreateModelInstance(m_modelPath);
        SetAnimationIndex(m_animationIndex);
        RebuildCommonLitResources();
    }

    bool ModelRendererComponent::UsesCommonLit() const noexcept
    {
        return m_materialOverrideEnabled
            && m_commonLitResources != nullptr
            && m_commonLitResources->compatible;
    }

    bool ModelRendererComponent::UsesLamaPonLit() const noexcept
    {
        if (m_model && m_model->skeletalModel)
        {
            // スキニングは既定でLit。互換トグルやカスタムShaderの
            // コンパイル失敗時だけEffectがnullptrになります。
            return m_skinnedEffect != nullptr;
        }
        return UsesCommonLit();
    }

    bool ModelRendererComponent::CanBeInstanced() const
    {
        if (m_wireframe
            || m_materialOverrideEnabled
            || m_useLegacyShading
            || !m_material.Shader().empty()
            || m_graphics == nullptr
            || m_graphics->IsDepthOnlyPass()
            || !m_model
            || !m_model->skeletalModel)
        {
            return false;
        }
        const auto& model = *m_model->skeletalModel;
        if (!model.skins.empty()
            || !model.animations.empty()
            || model.primitives.empty()
            || !m_graphics->Lit().SupportsInstancing())
        {
            return false;
        }
        return std::ranges::all_of(
            model.primitives,
            [](const SkeletalPrimitive& primitive)
            {
                return primitive.skin < 0
                    && primitive.vertexBuffer
                    && primitive.indexBuffer
                    && !primitive.alpha
                    && !primitive.textureHasTransparency
                    && primitive.baseColor.w >= 0.999f;
            });
    }

    std::uint64_t
        ModelRendererComponent::InstanceBatchKey()
            const noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;
        const auto& native = m_modelPath.native();
        const auto* bytes = reinterpret_cast<
            const unsigned char*>(native.data());
        const std::size_t byteCount = native.size()
            * sizeof(std::filesystem::path::value_type);
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::size_t ModelRendererComponent::AutomaticLodLevel(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection) const noexcept
    {
        return m_model && m_model->skeletalModel
            ? m_model->skeletalModel->SelectAutomaticLod(
                Owner().WorldMatrix(),
                view,
                projection,
                m_graphics != nullptr
                    ? m_graphics->Settings().automaticLodQuality
                    : 1.0f)
            : 0;
    }

    std::uint64_t ModelRendererComponent::TriangleCount(
        const std::size_t lodLevel) const noexcept
    {
        return m_model && m_model->skeletalModel
            ? m_model->skeletalModel->TriangleCount(lodLevel)
            : 0;
    }

    bool ModelRendererComponent::RenderInstancedBatch(
        const std::vector<ModelRendererComponent*>& batch,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (batch.size() < 2 || !CanBeInstanced())
        {
            return false;
        }

        using Vertex = DirectX::
            VertexPositionNormalTangentColorTextureSkinning;
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 8> elements{
            D3D11_INPUT_ELEMENT_DESC{
                "SV_Position", 0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0, 0,
                D3D11_INPUT_PER_VERTEX_DATA, 0 },
            D3D11_INPUT_ELEMENT_DESC{
                "NORMAL", 0,
                DXGI_FORMAT_R32G32B32_FLOAT,
                0, 12,
                D3D11_INPUT_PER_VERTEX_DATA, 0 },
            D3D11_INPUT_ELEMENT_DESC{
                "TEXCOORD", 0,
                DXGI_FORMAT_R32G32_FLOAT,
                0, 44,
                D3D11_INPUT_PER_VERTEX_DATA, 0 },
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 0,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 1,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 16,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 2,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 32,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 3,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 48,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_COLOR", 0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 64,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 }
        };

        auto& model = *m_model->skeletalModel;
        auto& effect = m_graphics->Lit();
        auto* const byteCode =
            effect.InstancedVertexShaderByteCode();
        if (byteCode == nullptr)
        {
            return false;
        }
        for (const auto& primitive : model.primitives)
        {
            if (primitive.instancedInputLayout)
            {
                continue;
            }
            if (FAILED(m_graphics->Device()->CreateInputLayout(
                    elements.data(),
                    static_cast<UINT>(elements.size()),
                    byteCode->GetBufferPointer(),
                    byteCode->GetBufferSize(),
                    primitive.instancedInputLayout
                        .ReleaseAndGetAddressOf())))
            {
                return false;
            }
        }

        std::vector<SkeletalPoseTransform> localPose;
        std::vector<DirectX::XMFLOAT4X4> bindPose;
        SkeletalModel::SamplePose(
            model.nodes,
            nullptr,
            0.0f,
            localPose,
            bindPose);

        std::array<
            std::vector<ModelRendererComponent*>,
            3> lodBatches;
        for (auto* component : batch)
        {
            if (component != nullptr
                && component->CanBeInstanced())
            {
                lodBatches[std::min<std::size_t>(
                    component->AutomaticLodLevel(
                        view,
                        projection),
                    2u)].push_back(component);
            }
        }

        struct InstanceData final
        {
            DirectX::XMFLOAT4X4 world;
            DirectX::XMFLOAT4 color;
        };
        static_assert(sizeof(InstanceData) == 80);
        static_assert(sizeof(Vertex) >= 52);

        auto* const context = m_graphics->Context();
        constexpr UINT vertexStride = sizeof(Vertex);
        constexpr UINT vertexOffset = 0;
        effect.SetMatrices(
            DirectX::XMMatrixIdentity(),
            view,
            projection);
        effect.SetLighting(m_graphics->Lighting());
        effect.SetInstancingEnabled(true);
        effect.SetTessellationDrawEnabled(false);

        bool drewAny{};
        for (std::size_t lodLevel = 0;
            lodLevel < lodBatches.size();
            ++lodLevel)
        {
            const auto& components = lodBatches[lodLevel];
            if (components.empty())
            {
                continue;
            }
            for (const auto& primitive : model.primitives)
            {
                if (primitive.meshNode >= bindPose.size())
                {
                    continue;
                }
                std::vector<InstanceData> instances;
                instances.reserve(components.size());
                const auto meshGlobal = DirectX::XMLoadFloat4x4(
                    &bindPose[primitive.meshNode]);
                for (const auto* component : components)
                {
                    InstanceData data{};
                    DirectX::XMStoreFloat4x4(
                        &data.world,
                        meshGlobal
                            * component->Owner().WorldMatrix());
                    data.color = primitive.baseColor;
                    instances.push_back(data);
                }
                auto* instanceBuffer =
                    m_graphics->AcquireInstanceBuffer(
                        instances.data(),
                        instances.size() * sizeof(InstanceData));
                if (instanceBuffer == nullptr)
                {
                    effect.SetInstancingEnabled(false);
                    return false;
                }

                ID3D11Buffer* indexBuffer =
                    primitive.indexBuffer.Get();
                std::uint32_t indexCount = primitive.indexCount;
                for (std::size_t level = std::min<std::size_t>(
                        lodLevel,
                        primitive.lodIndexBuffers.size());
                    level > 0;
                    --level)
                {
                    if (primitive.lodIndexBuffers[level - 1]
                        && primitive.lodIndexCounts[level - 1] > 0)
                    {
                        indexBuffer = primitive
                            .lodIndexBuffers[level - 1].Get();
                        indexCount = primitive
                            .lodIndexCounts[level - 1];
                        break;
                    }
                }

                LitMaterial material;
                material.SetBaseColor(primitive.baseColor);
                material.SetRoughness(primitive.roughness);
                material.SetMetallic(primitive.metallic);
                effect.SetMaterial(material);
                PbrTextures pbr{};
                pbr.roughness =
                    primitive.roughnessTexture.Get();
                pbr.metallic =
                    primitive.metallicTexture.Get();
                pbr.occlusion =
                    primitive.occlusionTexture.Get();
                pbr.emissive =
                    primitive.emissiveTexture.Get();
                pbr.occlusionStrength =
                    primitive.occlusionStrength;
                pbr.emissiveFactor =
                    primitive.emissiveFactor;
                effect.SetTextures(
                    primitive.texture
                        ? primitive.texture.Get()
                        : m_graphics->WhiteTexture(),
                    primitive.normalTexture.Get(),
                    pbr);
                effect.SetCustomTextures({});

                context->OMSetBlendState(
                    m_states->Opaque(),
                    nullptr,
                    0xffffffff);
                context->OMSetDepthStencilState(
                    m_states->DepthDefault(),
                    0);
                context->RSSetState(
                    primitive.doubleSided
                        ? m_states->CullNone()
                        : m_states->CullClockwise());
                ID3D11Buffer* vertexBuffers[]{
                    primitive.vertexBuffer.Get(),
                    instanceBuffer
                };
                const UINT strides[]{
                    vertexStride,
                    sizeof(InstanceData)
                };
                constexpr UINT offsets[]{ 0, 0 };
                context->IASetVertexBuffers(
                    0,
                    2,
                    vertexBuffers,
                    strides,
                    offsets);
                context->IASetIndexBuffer(
                    indexBuffer,
                    DXGI_FORMAT_R32_UINT,
                    0);
                context->IASetInputLayout(
                    primitive.instancedInputLayout.Get());
                context->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                effect.Apply(context);
                context->DrawIndexedInstanced(
                    indexCount,
                    static_cast<UINT>(instances.size()),
                    0,
                    0,
                    0);
                drewAny = true;
            }
        }
        effect.SetInstancingEnabled(false);
        if (!drewAny)
        {
            return false;
        }
        for (auto* component : batch)
        {
            if (component != nullptr)
            {
                component->m_instancedThisPass = true;
            }
        }
        return true;
    }

    std::string_view
        ModelRendererComponent::CommonLitStatus() const noexcept
    {
        if (m_commonLitResources == nullptr)
        {
            return "モデルの初期化待ち";
        }
        return m_commonLitResources->status;
    }

    void ModelRendererComponent::RebuildCommonLitResources()
    {
        auto resources =
            std::make_unique<CommonLitResources>();
        if (m_model && m_model->skeletalModel)
        {
            resources->status = m_skinnedEffect != nullptr
                ? "glTF/FBX GPUスキニング（LamaPon Lit）"
                : "glTF/FBX GPUスキニング（DirectXTK）";
            m_commonLitResources = std::move(resources);
            return;
        }
        if (!m_model || !m_model->model)
        {
            resources->status = m_modelPath.empty()
                ? "モデル未設定"
                : "モデルを読み込めません";
            m_commonLitResources = std::move(resources);
            return;
        }
        if (m_graphics == nullptr)
        {
            m_commonLitResources = std::move(resources);
            return;
        }

        auto& model = *m_model->model;
        std::uint64_t generation{};
        // ここもShaderを選ぶ入口です。RefreshShaderで差し替えても、
        // この後に呼ばれるここが生のEffectで上書きしてしまうため、
        // 同じ差し替えを通します。
        auto& effect = *SubstituteUnsupportedTessellation(
            &m_graphics->MaterialShader(
                m_material.Shader(),
                generation,
                m_shaderError,
                m_material.ShaderKeywords()),
            *m_graphics,
            false,
            m_shaderError);
        m_effect = &effect;
        m_activeShaderPath = m_material.Shader();
        m_shaderGeneration = generation;
        for (const auto& mesh : model.meshes)
        {
            for (const auto& part : mesh->meshParts)
            {
                if (dynamic_cast<DirectX::IEffectSkinning*>(
                    part->effect.get()) != nullptr)
                {
                    resources->parts.clear();
                    resources->status =
                        "スキニングモデルはDirectXTK描画を使用";
                    m_commonLitResources =
                        std::move(resources);
                    return;
                }
                if (!HasCommonLitVertexData(*part))
                {
                    resources->parts.clear();
                    resources->status =
                        "法線またはUVのないモデルはDirectXTK描画を使用";
                    m_commonLitResources =
                        std::move(resources);
                    return;
                }

                CommonLitResources::Part commonPart;
                commonPart.mesh = mesh.get();
                commonPart.part = part.get();
                try
                {
                    part->CreateInputLayout(
                        m_graphics->Device(),
                        &effect,
                        commonPart.inputLayout.
                            ReleaseAndGetAddressOf());

                    // CMO内の各マテリアルが持つテクスチャを保持する。
                    // 明示的な上書き画像がない場合、カスタムShaderでも
                    // 元モデルの複数マテリアルを失わないようにする。
                    ID3D11ShaderResourceView* emptyViews[2]{};
                    m_graphics->Context()->
                        PSSetShaderResources(
                            0,
                            2,
                            emptyViews);
                    part->effect->Apply(
                        m_graphics->Context());
                    ID3D11ShaderResourceView*
                        embeddedViews[2]{};
                    m_graphics->Context()->
                        PSGetShaderResources(
                            0,
                            2,
                            embeddedViews);
                    commonPart.embeddedAlbedoTexture.
                        Attach(embeddedViews[0]);
                    commonPart.embeddedNormalTexture.
                        Attach(embeddedViews[1]);
                    if (const auto embeddedColor =
                            m_model->embeddedDiffuseColors.find(
                                part->effect.get());
                        embeddedColor
                            != m_model->embeddedDiffuseColors.end())
                    {
                        commonPart.embeddedDiffuseColor =
                            embeddedColor->second;
                    }
                }
                catch (const std::exception&)
                {
                    resources->parts.clear();
                    resources->status =
                        "頂点形式が共通Litシェーダーと互換ではありません";
                    m_commonLitResources =
                        std::move(resources);
                    return;
                }
                resources->parts.emplace_back(
                    std::move(commonPart));
            }
        }

        if (resources->parts.empty())
        {
            resources->status =
                "描画可能なメッシュがありません";
            m_commonLitResources = std::move(resources);
            return;
        }

        resources->boneTransforms.resize(
            model.bones.size());
        if (m_materialOverrideEnabled
            && !m_material.Shader().empty()
            && !m_albedoTexture)
        {
            const auto embeddedCount =
                std::ranges::count_if(
                    resources->parts,
                    [](const CommonLitResources::Part& part)
                    {
                        return part.embeddedAlbedoTexture
                            != nullptr;
                    });
            if (embeddedCount == 0)
            {
                Logger::Instance().Warning(
                    "Custom model shader could not inherit an "
                    "embedded albedo texture: "
                    + PathToUtf8(m_modelPath),
                    Owner().Id());
            }
        }
        resources->compatible = true;
        resources->status = m_material.Shader().empty()
            ? "LamaPon Lit（影・Point・Spot対応）"
            : "カスタムShader（影・Point・Spot定数対応）";
        m_commonLitResources = std::move(resources);
    }

    void ModelRendererComponent::DrawCommonLit(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const bool occludedOnly)
    {
        if (!m_commonLitResources
            || !m_commonLitResources->compatible
            || !m_model
            || !m_model->model
            || m_graphics == nullptr
            || m_context == nullptr
            || m_states == nullptr)
        {
            return;
        }

        // OnPreRender3D（遮蔽表示）からも呼ばれるので、ここにも
        // 見張りを置きます。
        const GeometryShaderScope geometryScope{
            m_effect != nullptr
                && m_effect->HasGeometryShader()
                ? m_context
                : nullptr
        };

        auto& model = *m_model->model;
        auto& resources = *m_commonLitResources;
        if (!resources.boneTransforms.empty())
        {
            model.CopyAbsoluteBoneTransformsTo(
                resources.boneTransforms.size(),
                resources.boneTransforms.data());
        }

        auto& effect = m_effect != nullptr
            ? *m_effect
            : m_graphics->Lit();
        // シャドウパスは深度のみ書き込み、ライティングの
        // セットアップを省略します（テクスチャはパーツ単位で
        // 設定されます）。
        const bool depthOnly =
            m_graphics->IsDepthOnlyPass();
        effect.SetDepthOnlyEnabled(depthOnly);
        if (!depthOnly)
        {
            effect.SetMaterial(m_material);
            effect.SetLighting(m_graphics->Lighting());
            // 範囲に入っているリフレクションプローブがあれば、
            // 環境反射をその結果へ差し替えます（2個あれば混ぜます）。
            const auto ownerWorldMatrix =
                Owner().WorldMatrix();
            DirectX::XMFLOAT3 ownerPosition{};
            DirectX::XMStoreFloat3(
                &ownerPosition,
                ownerWorldMatrix.r[3]);
            effect.SetEnvironmentOverride(
                Owner().GetScene()
                    .ReflectionProbeEnvironmentAt(
                        ownerPosition));
        }

        const auto ownerWorld = Owner().WorldMatrix();
        const bool forceAlpha =
            m_material.BaseColor().w < 0.999f;
        if (depthOnly && forceAlpha)
        {
            // 半透明モデルは従来どおり影を落としません。
            effect.SetDepthOnlyEnabled(false);
            return;
        }
        // 深度プリパスは、メインパスとまったく同じ深度を書けるもの
        // だけに限ります。宣言で半透明にしたShaderはメインパスで
        // 深度を書かないので、プリパスに残すとメインパスの描画が
        // 消えます（シャドウパスは従来どおり描きます）。
        if (depthOnly
            && m_graphics->DepthPass()
                == DepthPassKind::Prepass
            && effect.RenderState().declared
            && (effect.RenderState().blend
                    != ShaderBlendMode::Opaque
                || !effect.RenderState().depthWrite))
        {
            effect.SetDepthOnlyEnabled(false);
            return;
        }
        // アウトライン・遮蔽表示は通常パスのみ実行します。
        const bool drawOutline =
            !occludedOnly
            && !m_wireframe
            && !depthOnly
            && effect.HasOutline()
            && m_material.CustomParameter(3).x > 0.0f;
        const bool drawOccluded =
            occludedOnly
            && !m_wireframe
            && !depthOnly
            && effect.HasOccludedPass()
            && m_material.CustomParameter(4).w > 0.0f;

        for (const bool alphaPass : { false, true })
        {
            for (const auto& mesh : model.meshes)
            {
                const bool hasPartsForPass =
                    std::ranges::any_of(
                        resources.parts,
                        [mesh = mesh.get(),
                            alphaPass,
                            forceAlpha](
                            const CommonLitResources::Part&
                                part)
                        {
                            return part.mesh == mesh
                                && (forceAlpha
                                        || part.part->isAlpha)
                                    == alphaPass;
                        });
                if (!hasPartsForPass)
                {
                    continue;
                }

                mesh->PrepareForRendering(
                    m_context,
                    *m_states,
                    alphaPass,
                    m_wireframe);

                DirectX::XMMATRIX meshWorld = ownerWorld;
                if (mesh->boneIndex
                    < resources.boneTransforms.size())
                {
                    meshWorld =
                        resources.boneTransforms[
                            mesh->boneIndex]
                        * ownerWorld;
                }
                effect.SetMatrices(
                    meshWorld,
                    view,
                    projection);

                for (const auto& part : resources.parts)
                {
                    if (part.mesh != mesh.get()
                        || (forceAlpha
                                || part.part->isAlpha)
                            != alphaPass)
                    {
                        continue;
                    }
                    effect.SetTextures(
                        m_albedoTexture
                            ? m_albedoTexture->view.Get()
                            : (part.embeddedAlbedoTexture
                                ? part.embeddedAlbedoTexture.Get()
                                : m_graphics->WhiteTexture()),
                        m_normalTexture
                            ? m_normalTexture->view.Get()
                            : part.embeddedNormalTexture.Get(),
                        BuildPbrTextures(
                            m_roughnessTexture,
                            m_metallicTexture,
                            m_occlusionTexture,
                            m_emissiveTexture,
                            m_material));
                    effect.SetCustomTextures(
                        ResolveCustomTextureViews());
                    if (m_preserveEmbeddedMaterialColor)
                    {
                        // 上書き色をTintとして扱い、CMO/SDKMESH内の
                        // パーツ固有DiffuseColorを失わないようにします。
                        auto partMaterial = m_material;
                        const auto& baseColor =
                            m_material.BaseColor();
                        const auto& embeddedColor =
                            part.embeddedDiffuseColor;
                        partMaterial.SetBaseColor({
                            baseColor.x * embeddedColor.x,
                            baseColor.y * embeddedColor.y,
                            baseColor.z * embeddedColor.z,
                            baseColor.w * embeddedColor.w
                        });
                        effect.SetMaterial(partMaterial);
                    }
                    else
                    {
                        effect.SetMaterial(m_material);
                    }
                    if (drawOccluded)
                    {
                        part.part->Draw(
                            m_context,
                            &effect,
                            part.inputLayout.Get(),
                            [this, &effect]()
                            {
                                m_context->OMSetBlendState(
                                    m_states->NonPremultiplied(),
                                    nullptr,
                                    0xffffffff);
                                m_context->RSSetState(
                                    m_states->
                                        CullCounterClockwise());
                                effect.ApplyOccluded(m_context);
                            });
                        mesh->PrepareForRendering(
                            m_context,
                            *m_states,
                            alphaPass,
                            m_wireframe);
                    }
                    if (occludedOnly)
                    {
                        continue;
                    }
                    if (drawOutline)
                    {
                        part.part->Draw(
                            m_context,
                            &effect,
                            part.inputLayout.Get(),
                            [this, &effect]()
                            {
                                m_context->OMSetBlendState(
                                    m_states->NonPremultiplied(),
                                    nullptr,
                                    0xffffffff);
                                m_context->OMSetDepthStencilState(
                                    m_states->DepthRead(),
                                    0);
                                m_context->RSSetState(
                                    m_states->CullClockwise());
                                effect.ApplyOutline(m_context);
                            });
                        mesh->PrepareForRendering(
                            m_context,
                            *m_states,
                            alphaPass,
                            m_wireframe);
                    }
                    // Shaderが描画状態を宣言している場合は、
                    // PrepareForRenderingの設定を上書きします。
                    if (effect.RenderState().declared)
                    {
                        ApplyShaderRenderState(
                            effect.RenderState());
                    }
                    part.part->Draw(
                        m_context,
                        &effect,
                        part.inputLayout.Get());
                }
            }
        }
        effect.SetDepthOnlyEnabled(false);
    }
}
