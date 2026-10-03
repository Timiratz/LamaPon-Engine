#include "LamaPon/Components/ModelRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Graphics/LitTextureRequest.h"
#include "LamaPon/Graphics/MaterialShaderDrawRequest.h"
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
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // DirectXTKの材質合成を使う形式か判定する(path: モデルパス)。
    [[nodiscard]] bool UsesDirectXTKModelMaterial(
        const std::filesystem::path& path)
    {
        // 小文字化した拡張子
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            std::towlower);
        return extension == L".cmo"
            || extension == L".sdkmesh"
            || extension == L".vbo";
    }

    // DirectXTK読込時に反時計回りとなる形式か判定する(path: モデルパス)。
    [[nodiscard]] bool LoadsCounterClockwiseDirectXTKModel(
        const std::filesystem::path& path)
    {
        // 小文字化した拡張子
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            std::towlower);
        return extension == L".cmo";
    }

    // スキニング用材質シェーダーを使う形式か判定する(path: モデルパス)。
    [[nodiscard]] bool UsesSkinnedMaterialShader(
        const std::filesystem::path& path)
    {
        // 小文字化した拡張子
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            std::towlower);
        return extension == L".gltf"
            || extension == L".glb"
            || extension == L".fbx";
    }

    struct ImportedModelVertex final
    {
        // モデル空間の頂点位置
        DirectX::XMFLOAT3 position{};
        // モデル空間の頂点法線
        DirectX::XMFLOAT3 normal{};
        // 接線と従法線の向き
        DirectX::XMFLOAT4 tangent{};
        // 圧縮RGBA頂点色
        std::uint32_t color{};
        // 画像のUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
        // 圧縮ボーン番号
        std::uint32_t blendIndices{};
        // 圧縮ボーン重み
        std::uint32_t blendWeights{};
    };

    // DirectXTKの取込頂点と同じバイト配置を維持する。
    static_assert(
        sizeof(ImportedModelVertex)
            == sizeof(DirectX::
                VertexPositionNormalTangentColorTextureSkinning));

    // 画像の描画用ビューを取得し資源を保持する(asset: 共有画像資産)。
    [[nodiscard]] LamaPon::GraphicsViewHandle AcquireTextureView(
        const std::shared_ptr<
            const LamaPon::TextureAsset>& asset) noexcept
    {
        if (asset == nullptr)
        {
            return {};
        }
        // 描画中に保持する画像資源
        const auto resources = asset->resources.Acquire();
        return resources != nullptr
            ? resources->shaderResourceView
            : LamaPon::GraphicsViewHandle{};
    }

    // 描画要求の上限内で照明と影の設定を転写する(lighting: シーン照明, request: 出力描画要求)。
    void CopyPrimitiveLighting(
        const LamaPon::LightingState& lighting,
        LamaPon::PrimitiveDrawRequest& request) noexcept
    {
        request.ambientColor = lighting.ambientColor;
        request.ambientIntensity = lighting.ambientIntensity;
        request.directionalLightCount = std::min(
            request.directionalLights.size(),
            lighting.directionalLightCount);
        // 転写または描画パスの番号
        for (std::size_t index{};
            index < request.directionalLightCount;
            ++index)
        {
            // 転写元の照明または影
            const auto& source = lighting.directionalLights[index];
            request.directionalLights[index] = {
                source.direction,
                source.color,
                source.intensity,
                source.angularRadius };
        }
        request.pointLightCount = std::min(
            request.pointLights.size(),
            lighting.pointLightCount);
        // 転写または描画パスの番号
        for (std::size_t index{};
            index < request.pointLightCount;
            ++index)
        {
            // 転写元の照明または影
            const auto& source = lighting.pointLights[index];
            request.pointLights[index] = {
                source.position,
                source.range,
                source.color,
                source.intensity };
        }
        request.spotLightCount = std::min(
            request.spotLights.size(),
            lighting.spotLightCount);
        // 転写または描画パスの番号
        for (std::size_t index{};
            index < request.spotLightCount;
            ++index)
        {
            // 転写元の照明または影
            const auto& source = lighting.spotLights[index];
            request.spotLights[index] = {
                source.position,
                source.range,
                source.direction,
                source.innerConeCosine,
                source.color,
                source.intensity,
                source.outerConeCosine };
        }
        // 平行光の影設定
        const auto& shadow = lighting.directionalShadow;
        request.directionalShadow.lightViewProjections =
            shadow.lightViewProjections;
        request.directionalShadow.cascadeSplits =
            shadow.cascadeSplits;
        request.directionalShadow.texture = shadow.texture;
        request.directionalShadow.lightIndex = shadow.lightIndex;
        request.directionalShadow.cascadeCount = shadow.cascadeCount;
        request.directionalShadow.bias = shadow.bias;
        request.directionalShadow.normalBias = shadow.normalBias;
        request.directionalShadow.strength = shadow.strength;
        request.directionalShadow.inverseResolution =
            1.0f / std::max(
                lighting.directionalShadowResolution,
                1.0f);
        request.directionalShadow.enabled = shadow.enabled;
        // 転写または描画パスの番号
        for (std::size_t index{};
            index < request.spotShadows.size();
            ++index)
        {
            // 転写元の照明または影
            const auto& source = lighting.spotShadows[index];
            request.spotShadows[index] = {
                source.lightViewProjection,
                source.lightIndex,
                source.bias,
                source.normalBias,
                source.strength,
                source.enabled };
        }
        request.spotShadowTexture = lighting.spotShadowTexture;
        request.pointShadow = {
            lighting.pointShadow.texture,
            lighting.pointShadow.lightIndex,
            lighting.pointShadow.bias,
            lighting.pointShadow.strength,
            lighting.pointShadow.enabled };
        request.localShadowInverseResolution =
            1.0f / std::max(
                lighting.localShadowResolution,
                1.0f);
        request.screenAmbientOcclusion = {
            lighting.screenAmbientOcclusion.texture,
            lighting.screenAmbientOcclusion.inverseWidth,
            lighting.screenAmbientOcclusion.inverseHeight,
            lighting.screenAmbientOcclusion.enabled };
        // 画面空間反射の設定
        const auto& reflection = lighting.screenSpaceReflection;
        request.screenSpaceReflection = {
            reflection.texture,
            reflection.depth,
            reflection.previousViewProjection,
            reflection.inverseWidth,
            reflection.inverseHeight,
            reflection.intensity,
            reflection.maximumDistance,
            reflection.thickness,
            reflection.roughnessCutoff,
            reflection.stepCount,
            reflection.depthPyramidMaximumMip,
            reflection.enabled };
        // 環境反射の設定
        const auto& environment = lighting.environment;
        request.environment = {
            environment.texture,
            environment.specular,
            environment.irradiance,
            environment.specularMaximumMip,
            environment.intensity,
            environment.enabled };
        request.fog = {
            lighting.fog.color,
            lighting.fog.startDistance,
            lighting.fog.endDistance,
            lighting.fog.density,
            LamaPon::PrimitiveFogModel::LamaPonLit,
            lighting.fog.enabled };
        // クラスタ照明の設定
        const auto& clustered = lighting.clustered;
        request.clustered = {
            clustered.lights,
            clustered.lightIndices,
            clustered.clusterCounts,
            clustered.nearPlane,
            clustered.farPlane,
            clustered.inverseWidth,
            clustered.inverseHeight,
            clustered.lightCount,
            clustered.enabled };
        // 焼き込み間接光の設定
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        request.bakedGlobalIllumination = {
            bakedGi.redCoefficients,
            bakedGi.greenCoefficients,
            bakedGi.blueCoefficients,
            bakedGi.volumeMinimum,
            bakedGi.volumeSize,
            bakedGi.resolution,
            bakedGi.intensity,
            bakedGi.enabled };
    }

    // モデル非対応のテセレーションを代替描画へ差し替える(effect: 選択効果, graphics: 描画機器, skinned: 骨格描画フラグ, shaderError: 出力診断)。
    LamaPon::LitEffect* SubstituteUnsupportedTessellation(
        LamaPon::LitEffect* effect,
        LamaPon::GraphicsDevice& graphics,
        const bool skinned,
        std::string& shaderError)
    {
        if (effect == nullptr)
        {
            return effect;
        }
        // 非対応パスの有無
        bool hasTessellation{};
        // 確認または復帰する描画種別
        const std::array roles{
            skinned
                ? LamaPon::ShaderPassRole::Skinned
                : LamaPon::ShaderPassRole::Forward,
            skinned
                ? LamaPon::ShaderPassRole::SkinnedOutline
                : LamaPon::ShaderPassRole::Outline,
            LamaPon::ShaderPassRole::Instanced
        };
        // 対象の描画種別
        for (const auto role : roles)
        {
            // 対象種別のパス数
            const auto passCount = effect->PassCount(role);
            // 転写または描画パスの番号
            for (std::size_t index = 0;
                // 対象種別のパス数
                index < passCount;
                ++index)
            {
                effect->SelectPass(role, index);
                hasTessellation = hasTessellation
                    || effect->SelectedPassHasTessellation(role);
            }
            if (passCount != 0)
            {
                effect->SelectPass(role, 0);
            }
        }
        if (!hasTessellation)
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
        // 借用する代替描画効果
        if (auto* const placeholder =
                graphics.ShaderErrorPlaceholder(skinned))
        {
            return placeholder;
        }
        return effect;
    }

    // 共有効果の選択パスと一時状態を次のコンポーネントへ持ち越さない。
    class MaterialPassScope final
    {
    public:
        // 共有効果と描画コンテキストを借りて復帰を管理する(effect: 共有描画効果, context: 借用コンテキスト)。
        MaterialPassScope(
            LamaPon::LitEffect& effect,
            ID3D11DeviceContext* context) noexcept
            : m_effect(effect)
            , m_context(context)
        {
        }

        // 復帰処理の重複を防ぐためコピーを禁止する。
        MaterialPassScope(const MaterialPassScope&) = delete;
        // 復帰処理の重複を防ぐためコピー代入を禁止する。
        MaterialPassScope& operator=(const MaterialPassScope&) = delete;

        // 一時フラグを解除し各描画種別の先頭パスへ戻す。
        ~MaterialPassScope()
        {
            m_effect.SetTessellationDrawEnabled(false);
            m_effect.SetDepthOnlyEnabled(false);
            m_effect.SetInstancingEnabled(false);
            // 確認または復帰する描画種別
            constexpr std::array roles{
                LamaPon::ShaderPassRole::Forward,
                LamaPon::ShaderPassRole::Skinned,
                LamaPon::ShaderPassRole::Instanced,
                LamaPon::ShaderPassRole::Outline,
                LamaPon::ShaderPassRole::SkinnedOutline,
                LamaPon::ShaderPassRole::Occluded
            };
            // 対象の描画種別
            for (const auto role : roles)
            {
                if (m_effect.PassCount(role) == 0)
                {
                    continue;
                }
                try
                {
                    m_effect.SelectPass(role, 0);
                }
                catch (...)
                {
                    // 後続描画の状態復帰を優先し、復帰時の例外を破棄する。
                }
            }
            if (m_context != nullptr)
            {
                m_context->HSSetShader(nullptr, nullptr, 0);
                m_context->DSSetShader(nullptr, nullptr, 0);
                m_context->GSSetShader(nullptr, nullptr, 0);
            }
        }

    private:
        // 借用する共有描画効果
        LamaPon::LitEffect& m_effect;
        // 借用するD3D11コンテキスト
        ID3D11DeviceContext* m_context{};
    };

    // 取込モデルの表面方向に合わせて描画状態を適用する(context: 借用コンテキスト, graphics: 描画機器, state: 宣言描画状態, doubleSided: 両面描画フラグ)。
    void ApplyImportedModelRenderState(
        ID3D11DeviceContext* const context,
        LamaPon::GraphicsDevice& graphics,
        const LamaPon::ShaderRenderState& state,
        const bool doubleSided)
    {
        // 借用する標準描画状態
        auto& states =
            LamaPon::Detail::GraphicsDeviceD3D11Access::States(graphics);
        switch (state.blend)
        {
        case LamaPon::ShaderBlendMode::Alpha:
            context->OMSetBlendState(
                states.NonPremultiplied(), nullptr, 0xffffffff);
            break;
        case LamaPon::ShaderBlendMode::Additive:
            context->OMSetBlendState(
                LamaPon::Detail::GraphicsDeviceD3D11Access::
                    AdditiveBlendPreservingAlpha(graphics),
                nullptr,
                0xffffffff);
            break;
        case LamaPon::ShaderBlendMode::Premultiplied:
            context->OMSetBlendState(
                states.AlphaBlend(), nullptr, 0xffffffff);
            break;
        case LamaPon::ShaderBlendMode::Opaque:
        default:
            context->OMSetBlendState(
                states.Opaque(), nullptr, 0xffffffff);
            break;
        }
        context->OMSetDepthStencilState(
            state.depthTest
                ? (state.depthWrite
                    ? states.DepthDefault()
                    : states.DepthRead())
                : states.DepthNone(),
            0);
        switch (state.cull)
        {
        case LamaPon::ShaderCullMode::Front:
            context->RSSetState(states.CullCounterClockwise());
            break;
        case LamaPon::ShaderCullMode::None:
            context->RSSetState(states.CullNone());
            break;
        case LamaPon::ShaderCullMode::Back:
        default:
            context->RSSetState(
                doubleSided
                    ? states.CullNone()
                    : states.CullClockwise());
            break;
        }
    }

    // 位置セマンティクスを交換してレイアウト作成を再試行する(device: 借用描画機器, elements: 頂点要素配列, elementCount: 要素数, byteCode: 頂点シェーダー, layout: 出力レイアウト)。
    HRESULT CreateInputLayoutWithPositionAlias(
        ID3D11Device* const device,
        const D3D11_INPUT_ELEMENT_DESC* const elements,
        const UINT elementCount,
        ID3DBlob* const byteCode,
        ID3D11InputLayout** const layout)
    {
        // 頂点配置の作成結果
        HRESULT result = device->CreateInputLayout(
            elements,
            elementCount,
            byteCode->GetBufferPointer(),
            byteCode->GetBufferSize(),
            layout);
        if (SUCCEEDED(result))
        {
            return result;
        }
        if (*layout != nullptr)
        {
            (*layout)->Release();
            *layout = nullptr;
        }

        // 位置要素名を変更する頂点宣言
        std::vector<D3D11_INPUT_ELEMENT_DESC> aliases(
            elements,
            elements + elementCount);
        // 位置要素名の変更有無
        bool changed{};
        // 確認する頂点要素
        for (auto& element : aliases)
        {
            if (element.SemanticName == nullptr
                || element.SemanticIndex != 0)
            {
                continue;
            }
            if (::_stricmp(element.SemanticName, "POSITION") == 0)
            {
                element.SemanticName = "SV_Position";
                changed = true;
            }
            else if (::_stricmp(
                element.SemanticName,
                "SV_Position") == 0)
            {
                element.SemanticName = "POSITION";
                changed = true;
            }
        }
        return changed
            ? device->CreateInputLayout(
                aliases.data(),
                static_cast<UINT>(aliases.size()),
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                layout)
            : result;
    }

    // 粗さを従来材質の反射指数へ変換する(roughness: 表面の粗さ)。
    float SpecularPowerFromRoughness(const float roughness) noexcept
    {
        // 粗さの二乗
        const float squared = roughness * roughness;
        return std::clamp(
            2.0f / std::max(squared * squared, 0.0001f) - 2.0f,
            1.0f,
            128.0f);
    }

    // 粗さを従来材質の反射色へ変換する(roughness: 表面の粗さ)。
    DirectX::XMVECTOR SpecularColorFromRoughness(
        const float roughness) noexcept
    {
        return DirectX::XMVectorReplicate(
            std::lerp(0.45f, 0.08f, roughness));
    }

    // 頂点宣言に指定のセマンティクス番号0があるか調べる(part: モデル描画部品, semantic: 要素名)。
    bool HasSemantic(
        const DirectX::ModelMeshPart& part,
        const char* semantic) noexcept
    {
        if (!part.vbDecl)
        {
            return false;
        }

        // 番号0の要素名を比較する(element: 確認する頂点要素)。
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

    // 共通照明に必要な位置・法線・UVを持つか調べる(part: モデル描画部品)。
    bool HasCommonLitVertexData(
        const DirectX::ModelMeshPart& part) noexcept
    {
        // 位置要素の有無
        const bool hasPosition =
            HasSemantic(part, "SV_Position")
            || HasSemantic(part, "POSITION");
        return hasPosition
            && HasSemantic(part, "NORMAL")
            && HasSemantic(part, "TEXCOORD");
    }

    // 再生時刻を進めて循環または終端に制限する(time: 再生時刻秒, delta: 進行秒数, duration: クリップ長秒, loop: 循環フラグ)。
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
        // 進行後の再生時刻秒
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
    LitTextureRequest
        ModelRendererComponent::BuildLitTextureRequest() const noexcept
    {
        // 画像と材質の描画要求
        LitTextureRequest request;
        request.albedo = AcquireTextureView(m_albedoTexture);
        request.normal = AcquireTextureView(m_normalTexture);
        request.roughness = AcquireTextureView(m_roughnessTexture);
        request.metallic = AcquireTextureView(m_metallicTexture);
        request.occlusion = AcquireTextureView(m_occlusionTexture);
        request.emissive = AcquireTextureView(m_emissiveTexture);
        // 追加画像または描画パスの番号
        for (std::size_t index{};
            index < request.customTextures.size();
            ++index)
        {
            request.customTextures[index] =
                AcquireTextureView(m_customTextures[index]);
        }
        request.occlusionStrength =
            m_material.OcclusionStrength();
        request.emissiveFactor = m_material.EmissiveColor();
        return request;
    }

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
        // ゼロ固定の合成係数
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
                m_graphics != nullptr
                    ? m_graphics->AdditiveBlendPreservingAlpha()
                    : m_states->Additive(),
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

    struct ModelRendererComponent::CommonLitResources final
    {
        struct Part final
        {
            // 借用するモデルメッシュ
            DirectX::ModelMesh* mesh{};
            // 借用するメッシュ部品
            // 取込モデルの描画部品
            DirectX::ModelMeshPart* part{};
            // 色描画パス順の頂点レイアウト
            std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>> forwardInputLayouts;
            // 輪郭描画パス順の頂点レイアウト
            std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>> outlineInputLayouts;
            // 埋め込み色画像の保持ビュー
            GraphicsViewHandle embeddedAlbedoTexture;
            // 埋め込み法線画像の保持ビュー
            GraphicsViewHandle embeddedNormalTexture;
            // 埋め込み材質のRGBA色
            DirectX::XMFLOAT4 embeddedDiffuseColor{
                1.0f,
                1.0f,
                1.0f,
                1.0f
            };
        };

        // 共通照明で描くモデル部品
        std::vector<Part> parts;
        // モデル内ボーンの変換行列
        std::vector<DirectX::XMMATRIX> boneTransforms;
        // 共通照明への対応診断
        std::string status{
            "モデルの初期化待ち"
        };
        // 共通照明への対応可否
        bool compatible{};
    };

    bool ModelRendererComponent::IsAlphaBlended3D() const
    {
        // D3D12経路の使用有無
        const bool usesD3D12 = m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental;
        if (usesD3D12 && !m_material.Shader().empty())
        {
            // 対象パスの宣言描画状態
            ShaderRenderState state;
            if (m_graphics->TryGetMaterialShaderRenderState(
                    m_material.Shader(),
                    m_material.ShaderKeywords(),
                    state)
                && state.declared)
            {
                return state.blend == ShaderBlendMode::Alpha
                    || state.blend == ShaderBlendMode::Premultiplied;
            }
        }
        // 再読込で共有効果が置き換わるため、生ポインターの参照前にキャッシュを同期する。
        if (!usesD3D12)
        {
            const_cast<ModelRendererComponent*>(this)
                ->RefreshShader(false);
        }
        // 半透明ソートの判定条件は描画側の合成条件と揃える。
        // 有効な描画効果の有無
        bool hasEffect{};
        // 全パスの状態宣言有無
        bool allPassesDeclareState = true;
        // 確認する共有描画効果
        const std::array effects{
            m_effect,
            m_skinnedEffect,
            m_skeletalForwardEffect
        };
        // 共有描画効果の番号
        for (std::size_t effectIndex = 0;
            effectIndex < effects.size();
            ++effectIndex)
        {
            // 確認する共有描画効果
            const auto* const activeEffect = effects[effectIndex];
            if (activeEffect == nullptr)
            {
                continue;
            }
            // 同じfallback Effectが複数roleに返った場合は1回だけ調べます。
            if (std::find(
                    effects.begin(),
                    effects.begin() + effectIndex,
                    activeEffect)
                != effects.begin() + effectIndex)
            {
                continue;
            }
            hasEffect = true;
            // 色描画パスの数
            const auto colorPassCount =
                activeEffect->ColorPassCount();
            allPassesDeclareState = allPassesDeclareState
                && colorPassCount != 0;
            // 追加画像または描画パスの番号
            for (std::size_t index = 0;
                // 色描画パスの数
                index < colorPassCount;
                ++index)
            {
                // 対象パスの宣言描画状態
                const auto& state =
                    activeEffect->ColorPassRenderState(index);
                allPassesDeclareState =
                    allPassesDeclareState && state.declared;
                // 順序依存の合成を含むオブジェクトは全体を半透明ソートへ送る。
                if (state.declared
                    && (state.blend == ShaderBlendMode::Alpha
                        || state.blend
                            == ShaderBlendMode::Premultiplied))
                {
                    return true;
                }
            }
        }
        if (hasEffect && allPassesDeclareState)
        {
            return false;
        }
        if (m_material.BaseColor().w < 0.999f)
        {
            return true;
        }
        // 半透明部品があればオブジェクト全体を並べ替える。
        if (m_commonLitResources)
        {
            // 取込モデルの描画部品
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
        if (m_model && m_model->skeletalModel)
        {
            // 部品に半透明の材質があるか調べる(primitive: 骨格モデルの部品)。
            return std::ranges::any_of(
                m_model->skeletalModel->primitives,
                [](const SkeletalPrimitive& primitive)
                {
                    return primitive.alpha
                        || primitive.baseColor.w < 0.999f;
                });
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
        // 差し替え後のモデル資産
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
            // 参照する制御状態
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
        // 初回バッチ判定より先にシェーダーと描画種別を確定する。
        RefreshShader(false);
        // シェーダーが同じでもモデル部品の借用参照は必ず作り直す。
        RebuildCommonLitResources();
    }

    void ModelRendererComponent::SetAnimationIndex(
        const std::size_t index) noexcept
    {
        m_animationIndex = AnimationCount() > 0
            ? std::min(index, AnimationCount() - 1)
            : 0;
        m_animationTime = 0.0f;
        m_cachedPoseFrame = ~std::uint64_t{};
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
        // 取得した名前の一覧
        std::vector<std::string> result;
        if (!m_model || !m_model->skeletalModel)
        {
            return result;
        }
        result.reserve(
            m_model->skeletalModel->nodes.size());
        // モデル内の骨格ノード
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
            // 参照する制御状態
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
        // 取得した名前の一覧
        std::vector<std::string> result;
        if (!m_animationController)
        {
            return result;
        }
        // 制御器の遷移定義
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
        // 浮動小数点パラメーターの位置
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
        // 浮動小数点パラメーターの位置
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
                // 重み付けするクリップ
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

        // 状態遷移先の混合割合
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
        // 状態内のクリップを重み付けして追加する(state: 対象制御状態, stateTime: 状態の再生時刻秒, outerWeight: 状態の混合重み)。
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
                // 有効なクリップの姿勢標本を追加する(name: クリップ名, weight: 子クリップの混合重み)。
                const auto appendClip =
                    [this,
                        &samples,
                        state,
                        stateTime,
                        outerWeight](
                        const std::string_view name,
                        const float weight)
                    {
                        // クリップまたはノードの番号
                        const auto index =
                            ResolveAnimationIndex(name);
                        if (index >= AnimationCount()
                            || weight <= 0.0f)
                        {
                            return;
                        }
                        // 重み付けするクリップ
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
                // 子クリップの混合重み
                std::vector<float> weights;
                CalculateBlendWeights(
                    *state,
                    weights);
                // クリップまたはノードの番号
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
        // モデル内の骨格ノード一覧
        const auto& nodes =
            m_model->skeletalModel->nodes;
        if (!m_rootMotionNode.empty())
        {
            // クリップまたはノードの番号
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
        // クリップまたはノードの番号
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
        // 移動を抽出するノード番号
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
            // モデル空間のルート位置
            // 分解したモデル空間位置
            DirectX::XMFLOAT3 translation{};
            // ルート回転の四元数
            // 分解した姿勢の四元数
            DirectX::XMFLOAT4 rotation{
                0.0f,
                0.0f,
                0.0f,
                1.0f
            };
        };
        // 重み付き姿勢からルートの位置と回転を抽出する(samples: 評価する姿勢標本)。
        const auto sampleRoot =
            [this, node](
                const std::vector<
                    SkeletalPoseSample>& samples)
            {
                using namespace DirectX;
                // ノードごとのローカル姿勢
                std::vector<
                    SkeletalPoseTransform>
                    localPose;
                // モデル空間のノード姿勢
                std::vector<XMFLOAT4X4>
                    globalPose;
                SkeletalModel::SampleWeightedPose(
                    m_model->skeletalModel->nodes,
                    samples,
                    localPose,
                    globalPose);
                // 抽出した移動と回転
                RootPose result;
                // 分解したモデル空間倍率
                XMVECTOR scale{};
                // 分解した姿勢の四元数
                XMVECTOR rotation{};
                // 分解したモデル空間位置
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

        // 進行前のルート姿勢
        const RootPose beforePose =
            sampleRoot(before);
        // 進行後のルート姿勢
        const RootPose afterPose =
            sampleRoot(after);
        // モデル空間の移動差分
        DirectX::XMFLOAT3 delta{
            afterPose.translation.x
                - beforePose.translation.x,
            afterPose.translation.y
                - beforePose.translation.y,
            afterPose.translation.z
                - beforePose.translation.z
        };
        // Y軸回転の差分ラジアン
        float yawDelta{};
        // 四元数からY軸の向きを求める(rotation: 回転の四元数)。
        const auto yawFrom =
            [](const DirectX::XMFLOAT4& rotation)
            {
                using namespace DirectX;
                // 回転後のモデル前方向
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

        // 同じクリップの時刻が巻き戻ったか調べる(previous: 進行前の姿勢標本)。
        // 時刻の巻戻り検出結果
        const bool looped =
            std::ranges::any_of(
                before,
                [&after](
                    const SkeletalPoseSample&
                        previous)
                {
                    // 同じクリップの標本を探す(current: 進行後の姿勢標本)。
                    // 同じクリップの進行後標本
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
        // 巻戻りは一回の循環として両端の移動を合算する。
        if (looped)
        {
            // 終端時刻に変更する標本
            auto endSamples = before;
            // 先頭時刻に変更する標本
            auto startSamples = after;
            // 端点を評価する姿勢標本
            for (auto& sample : endSamples)
            {
                sample.time = sample.clip
                    ? sample.clip->duration
                    : 0.0f;
            }
            // 端点を評価する姿勢標本
            for (auto& sample : startSamples)
            {
                sample.time = 0.0f;
            }
            // 循環前の終端姿勢
            const RootPose endPose =
                sampleRoot(endSamples);
            // 循環後の先頭姿勢
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

        // 所有物のローカル変換
        auto& transform = GetTransform();
        using namespace DirectX;
        // 所有物の回転を適用した移動
        const XMVECTOR worldDelta =
            XMVector3Rotate(
                XMLoadFloat3(&delta),
                transform.RotationVector());
        // 回転済みの移動成分
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
        // 進行前の正規化時刻
        const float previous =
            std::clamp(
                previousTime / duration,
                0.0f,
                1.0f);
        // 進行後の正規化時刻
        const float current =
            std::clamp(
                currentTime / duration,
                0.0f,
                1.0f);
        // 制御状態の通知定義
        for (const auto& event : state.events)
        {
            // 通知時刻の通過有無
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
            // 通知は最大256件とし、満杯なら最古の通知を破棄する。
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
        m_cachedPoseFrame = ~std::uint64_t{};
    }

    void ModelRendererComponent::AdvanceAnimation(
        const float deltaTime,
        const bool allowRootMotion)
    {
        // 移動抽出用の進行前姿勢標本
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
                // 移動抽出用の進行後姿勢標本
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
        // 再生区間の長さ秒
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
            // 移動抽出用の進行後姿勢標本
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
        // 描画方式の切替に合わせてスキニング効果を選び直す。
        RefreshShader(false);
    }

    void ModelRendererComponent::SetAlbedoTexturePath(
        std::filesystem::path texturePath)
    {
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 変更後に保持する画像資産
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
        // 読込後の色画像資産
        std::shared_ptr<const TextureAsset> albedo;
        // 読込後の法線画像資産
        std::shared_ptr<const TextureAsset> normal;
        // 読込後の粗さ画像資産
        std::shared_ptr<const TextureAsset> roughness;
        // 読込後の金属度画像資産
        std::shared_ptr<const TextureAsset> metallic;
        // 読込後の遮蔽画像資産
        std::shared_ptr<const TextureAsset> occlusion;
        // 読込後の発光画像資産
        std::shared_ptr<const TextureAsset> emissive;
        if (m_assets != nullptr)
        {
            // 画像用途に合わせて読込形式を選ぶ。
            using Usage = TextureLoader::TextureUsage;
            // 未指定の画像は空資産として返す(path: 画像パス, usage: 読込用途)。
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

        // 色画像の解除有無
        const bool removedAlbedo =
            !m_material.AlbedoTexture().empty()
            && material.AlbedoTexture().empty();
        // 法線画像の解除有無
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
        if (m_graphics == nullptr
            || m_graphics->ActiveRenderingApi()
                != RenderingApi::DirectX11)
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
            // 骨格付き部品があるか調べる(primitive: 骨格モデルの部品)。
            const bool hasSkinnedPrimitive = std::ranges::any_of(
                m_model->skeletalModel->primitives,
                [](const SkeletalPrimitive& primitive)
                {
                    return primitive.skin >= 0;
                });
            // 骨格なし部品があるか調べる(primitive: 骨格モデルの部品)。
            const bool hasForwardPrimitive = std::ranges::any_of(
                m_model->skeletalModel->primitives,
                [](const SkeletalPrimitive& primitive)
                {
                    return primitive.skin < 0;
                });
            // 主描画効果の世代
            std::uint64_t generation{};
            // 主描画効果の診断
            std::string primaryError;
            // 借用する主描画効果
            LitEffect* effect{};
            // マニフェストの指定有無
            const bool manifestRequested =
                IsShaderManifestPath(m_material.Shader());
            if (hasSkinnedPrimitive || !manifestRequested)
            {
                effect = m_graphics->SkinnedMaterialShader(
                    m_material.Shader(),
                    generation,
                    primaryError,
                    m_material.ShaderKeywords());
            }
            else if (!m_useLegacyShading
                || !m_material.Shader().empty())
            {
                effect = &m_graphics->MaterialShader(
                    m_material.Shader(),
                    generation,
                    primaryError,
                    m_material.ShaderKeywords());
            }
            // コンパイル失敗の代替表示を既定照明で上書きしない。
            if (effect == nullptr
                && !m_useLegacyShading
                && m_material.Shader().empty())
            {
                effect = &m_graphics->SkinnedLit();
            }
            effect = SubstituteUnsupportedTessellation(
                effect,
                *m_graphics,
                effect != nullptr && effect->IsSkinned(),
                primaryError);
            if (manifestRequested
                && effect != nullptr
                && effect == &m_graphics->Lit())
            {
                // 非同期コンパイル中も既存頂点出力に合うスキニング効果を使う。
                effect = &m_graphics->SkinnedLit();
            }

            // 骨格の有無が混在するマニフェストモデルでは両方の描画効果を保持する。
            // 骨格なし部品の共有描画効果
            LitEffect* forwardEffect{};
            // 骨格なし描画効果の世代
            std::uint64_t forwardGeneration{};
            // 骨格なし描画効果の診断
            std::string forwardError;
            if (manifestRequested
                && hasSkinnedPrimitive
                && hasForwardPrimitive)
            {
                forwardEffect = &m_graphics->MaterialShader(
                    m_material.Shader(),
                    forwardGeneration,
                    forwardError,
                    m_material.ShaderKeywords());
                forwardEffect = SubstituteUnsupportedTessellation(
                    forwardEffect,
                    *m_graphics,
                    false,
                    forwardError);
                if (forwardEffect == &m_graphics->Lit())
                {
                    forwardEffect = &m_graphics->SkinnedLit();
                }
            }

            // 統合したシェーダー診断
            std::string compileError;
            // 空でない診断を種別付きで追記する(destination: 診断の追記先, label: 診断種別の接頭辞, message: 追記する診断)。
            const auto appendError = [](
                std::string& destination,
                const char* const label,
                const std::string& message)
            {
                if (message.empty())
                {
                    return;
                }
                if (!destination.empty())
                {
                    destination += "; ";
                }
                destination += label;
                destination += message;
            };
            if (forwardEffect != nullptr || !forwardError.empty())
            {
                appendError(compileError, "Skinned: ", primaryError);
                appendError(compileError, "Forward: ", forwardError);
            }
            else
            {
                compileError = std::move(primaryError);
            }
            if (m_skinnedEffect == effect
                && m_skeletalForwardEffect == forwardEffect
                && m_activeShaderPath == m_material.Shader()
                && m_shaderGeneration == generation
                && m_skeletalForwardShaderGeneration
                    == forwardGeneration)
            {
                // 頂点レイアウトの診断は描画効果の世代が変わるまで保持する。
                if (!compileError.empty() || m_shaderError.empty())
                {
                    m_shaderError = std::move(compileError);
                }
                return;
            }

            m_skinnedEffect = effect;
            m_skeletalForwardEffect = forwardEffect;
            m_effect = nullptr;
            m_activeShaderPath = m_material.Shader();
            m_shaderGeneration = generation;
            m_skeletalForwardShaderGeneration = forwardGeneration;
            m_shaderError = std::move(compileError);
            m_skinnedInputLayout.Reset();
            m_skeletalColorInputLayouts.clear();
            m_skeletalOutlineInputLayouts.clear();
            m_skeletalForwardColorInputLayouts.clear();
            m_skeletalForwardOutlineInputLayouts.clear();
            m_instancedInputLayouts.clear();

            // 各パスの頂点レイアウトを作成する(manifestEffect: 共有描画効果, colorLayouts: 出力色描画レイアウト, outlineLayouts: 出力輪郭レイアウト, label: 診断種別名)。
            const auto buildManifestLayouts =
                [this, &appendError](
                    LitEffect* const manifestEffect,
                    std::vector<Microsoft::WRL::ComPtr<
                        ID3D11InputLayout>>& colorLayouts,
                    std::vector<Microsoft::WRL::ComPtr<
                        ID3D11InputLayout>>& outlineLayouts,
                    const char* const label)
                {
                    if (manifestEffect == nullptr
                        || !manifestEffect->IsManifestEffect())
                    {
                        return;
                    }
                    // 頂点レイアウトの互換性診断
                    std::string layoutError;
                    // 頂点レイアウトを作成し互換性診断を保持する(byteCode: 頂点シェーダー列, layout: 出力レイアウト, passLabel: 診断用のパス名)。
                    const auto createLayout =
                        [this, &layoutError](
                            ID3DBlob* const byteCode,
                            Microsoft::WRL::ComPtr<
                                ID3D11InputLayout>& layout,
                            const std::string& passLabel)
                        {
                            if (byteCode == nullptr)
                            {
                                layoutError = passLabel
                                    + " has no vertex shader bytecode.";
                                return false;
                            }
                            // 頂点レイアウトの作成結果
                            const HRESULT result =
                                CreateInputLayoutWithPositionAlias(
                                    Detail::GraphicsDeviceD3D11Access::
                                        Device(*m_graphics),
                                    DirectX::
                                        VertexPositionNormalTangentColorTextureSkinning::
                                            InputElements,
                                    DirectX::
                                        VertexPositionNormalTangentColorTextureSkinning::
                                            InputElementCount,
                                    byteCode,
                                    layout.ReleaseAndGetAddressOf());
                            if (FAILED(result))
                            {
                                layoutError = passLabel
                                    + " input layout is incompatible.";
                                return false;
                            }
                            return true;
                        };

                    // 全頂点レイアウトの互換性
                    bool layoutsValid = true;
                    // 色描画パスの数
                    const auto colorPassCount =
                        manifestEffect->ColorPassCount();
                    colorLayouts.reserve(colorPassCount);
                    // 対象描画パスの番号
                    for (std::size_t index = 0;
                        // 色描画パスの数
                        index < colorPassCount;
                        ++index)
                    {
                        // 対象パスの頂点レイアウト
                        Microsoft::WRL::ComPtr<
                            ID3D11InputLayout> layout;
                        layoutsValid = createLayout(
                            manifestEffect
                                ->ColorPassVertexShaderByteCode(index),
                            layout,
                            std::string(label)
                                + " color pass "
                                + std::to_string(index))
                            && layoutsValid;
                        if (!layoutsValid)
                        {
                            break;
                        }
                        colorLayouts.push_back(std::move(layout));
                    }

                    // 輪郭描画の種別
                    const auto outlineRole =
                        manifestEffect->IsSkinned()
                        ? ShaderPassRole::SkinnedOutline
                        : ShaderPassRole::Outline;
                    // 輪郭描画パスの数
                    const auto outlinePassCount =
                        manifestEffect->PassCount(outlineRole);
                    outlineLayouts.reserve(outlinePassCount);
                    // 対象描画パスの番号
                    for (std::size_t index = 0;
                        // 輪郭描画パスの数
                        layoutsValid && index < outlinePassCount;
                        ++index)
                    {
                        manifestEffect->SelectPass(outlineRole, index);
                        // 対象パスの頂点レイアウト
                        Microsoft::WRL::ComPtr<
                            ID3D11InputLayout> layout;
                        layoutsValid = createLayout(
                            manifestEffect
                                ->SelectedPassVertexShaderByteCode(
                                    outlineRole),
                            layout,
                            std::string(label)
                                + " outline pass "
                                + std::to_string(index));
                        if (layoutsValid)
                        {
                            outlineLayouts.push_back(std::move(layout));
                        }
                    }
                    if (outlinePassCount != 0)
                    {
                        manifestEffect->SelectPass(outlineRole, 0);
                    }
                    if (colorPassCount != 0)
                    {
                        manifestEffect->SelectColorPass(0);
                    }
                    if (!layoutsValid
                        || colorLayouts.size() != colorPassCount)
                    {
                        colorLayouts.clear();
                        outlineLayouts.clear();
                        appendError(
                            m_shaderError,
                            "",
                            layoutError);
                    }
                };

            if (effect != nullptr)
            {
                if (effect->IsManifestEffect())
                {
                    buildManifestLayouts(
                        effect,
                        m_skeletalColorInputLayouts,
                        m_skeletalOutlineInputLayouts,
                        effect->IsSkinned()
                            ? "Skinned material"
                            : "Forward material");
                }
                else
                {
                    // 借用する頂点シェーダー列
                    const void* shaderByteCode{};
                    // 頂点シェーダーのバイト数
                    std::size_t shaderByteCodeSize{};
                    effect->GetVertexShaderBytecode(
                        &shaderByteCode,
                        &shaderByteCodeSize);
                    // 頂点レイアウトの作成結果
                    const HRESULT result =
                        Detail::GraphicsDeviceD3D11Access::Device(
                            *m_graphics)->CreateInputLayout(
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
                        // 頂点レイアウトの互換性診断
                        const std::string layoutError =
                            hasSkinnedPrimitive
                            ? "Skinned shader input layout is incompatible."
                            : "Model shader input layout is incompatible.";
                        appendError(m_shaderError, "", layoutError);
                        m_skinnedEffect = nullptr;
                    }
                }
            }
            buildManifestLayouts(
                forwardEffect,
                m_skeletalForwardColorInputLayouts,
                m_skeletalForwardOutlineInputLayouts,
                "Forward material");
            RebuildCommonLitResources();
            return;
        }

        m_skinnedEffect = nullptr;
        m_skeletalForwardEffect = nullptr;
        m_skinnedInputLayout.Reset();
        m_skeletalColorInputLayouts.clear();
        m_skeletalOutlineInputLayouts.clear();
        m_skeletalForwardColorInputLayouts.clear();
        m_skeletalForwardOutlineInputLayouts.clear();
        m_instancedInputLayouts.clear();
        m_skeletalForwardShaderGeneration = 0;
        // 主描画効果の世代
        std::uint64_t generation{};
        // 統合したシェーダー診断
        std::string compileError;
        // 借用する主描画効果
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
        // D3D11経路の使用有無
        const bool usesD3D11 = graphics.ActiveRenderingApi()
            == RenderingApi::DirectX11;
        if (usesD3D11)
        {
            m_context = Detail::GraphicsDeviceD3D11Access::Context(graphics);
            m_states = &Detail::GraphicsDeviceD3D11Access::States(graphics);
        }

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
            // 小文字化したモデル拡張子
            auto extension = m_modelPath.extension().wstring();
            std::ranges::transform(
                extension,
                extension.begin(),
                std::towlower);
            // CMO/SDKMESH/VBO/glTF/GLB/FBXはD3D12でもCPU幾何を共通経路で読み込みます。
            if (usesD3D11
                || extension == L".cmo"
                || extension == L".sdkmesh"
                || extension == L".vbo"
                || extension == L".gltf"
                || extension == L".glb"
                || extension == L".fbx")
            {
                m_model = m_assets->CreateModelInstance(m_modelPath);
            }
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
        // 追加画像をt7〜t10に割り当てる。
        // 追加画像または頂点の番号
        for (std::size_t index = 0;
            index < LitMaterial::CustomTextureCount;
            ++index)
        {
            // 追加画像のパス
            const auto& path =
                m_material.CustomTexture(index);
            m_customTextures[index] = path.empty()
                ? nullptr
                : m_assets->LoadTexture(path);
        }
        // 初回のバッチ収集より先にシェーダーの描画種別を確定する。
        if (usesD3D11)
        {
            RefreshShader(false);
        }
    }

    void ModelRendererComponent::OnUpdate(const float deltaTime)
    {
        AdvanceAnimation(deltaTime);
    }

    bool ModelRendererComponent::HasPreRender3DPass()
    {
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            // 事前の遮蔽表示は材質を上書きするCMO・SDKMESH・VBOに限る。
            return !m_wireframe
                && m_materialOverrideEnabled
                && !m_material.Shader().empty()
                && m_model
                && m_model->skeletalModel
                && UsesDirectXTKModelMaterial(m_modelPath)
                && m_material.CustomParameter(4).w > 0.0f
                && m_graphics->PrepareMaterialShaderPasses(
                    m_material.Shader(),
                    m_material.ShaderKeywords()).occluded;
        }
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                != RenderingApi::DirectX11)
        {
            return false;
        }
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
        // モデル全体の通常描画に先行して遮蔽部分を描く。
        if (!HasPreRender3DPass())
        {
            return;
        }
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            DrawD3D12Model(view, projection, true);
            return;
        }
        DrawCommonLit(view, projection, true);
    }

    void ModelRendererComponent::DrawD3D12Model(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const bool occludedOnly)
    {
        if (m_graphics == nullptr
            || !m_model
            || !m_model->skeletalModel)
        {
            return;
        }
        // 描画する骨格モデル
        auto& model = *m_model->skeletalModel;
        // 現在の再生クリップ
        const auto* clip = m_animationIndex < model.animations.size()
            ? &model.animations[m_animationIndex]
            : nullptr;
        // 遷移先の再生クリップ
        const auto* blendClip = !m_nextAnimationState.empty()
                && m_nextAnimationIndex < model.animations.size()
            ? &model.animations[m_nextAnimationIndex]
            : nullptr;
        // 遷移先の混合割合
        const float blendAmount = blendClip != nullptr
                && m_animationTransitionDuration > 0.0f
            ? std::clamp(
                m_animationTransitionTime
                    / m_animationTransitionDuration,
                0.0f,
                1.0f)
            : 0.0f;
        // 姿勢を評価するフレーム番号
        const auto poseFrame = m_graphics->FrameStats().totalFrames;
        if (m_cachedPoseFrame != poseFrame
            || m_cachedPoseModel != &model)
        {
            // ノードのローカル姿勢
            std::vector<SkeletalPoseTransform> localPose;
            // 重み付きの姿勢標本
            std::vector<SkeletalPoseSample> weightedSamples;
            CollectAnimationPoseSamples(weightedSamples);
            if (m_applyRootMotion
                && weightedSamples.empty()
                && clip != nullptr)
            {
                weightedSamples.push_back({
                    clip,
                    m_animationTime,
                    1.0f });
            }
            if (!weightedSamples.empty())
            {
                SkeletalModel::SampleWeightedPose(
                    model.nodes,
                    weightedSamples,
                    localPose,
                    m_cachedGlobalPose,
                    m_applyRootMotion
                        ? ResolveRootMotionNode()
                        : std::numeric_limits<std::size_t>::max());
            }
            else if (blendClip != nullptr && blendAmount > 0.0f)
            {
                SkeletalModel::SampleBlendedPose(
                    model.nodes,
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
                    model.nodes,
                    clip,
                    m_animationTime,
                    localPose,
                    m_cachedGlobalPose);
            }
            m_cachedPoseFrame = poseFrame;
            m_cachedPoseModel = &model;
        }

        // 上書き画像の保持ビュー
        const auto overrideTextures = BuildLitTextureRequest();
        // 所有物のワールド変換
        const auto ownerWorld = Owner().WorldMatrix();
        // 深度のみを描くフラグ
        const bool depthOnly = m_graphics->IsDepthOnlyPass();
        // 選択した詳細度の段階
        const auto lodLevel = model.SelectAutomaticLod(
            ownerWorld,
            view,
            projection,
            m_graphics->Settings().automaticLodQuality);
        // 従来形式の埋込色は保持設定が有効なときだけ上書き色に乗算する。
        // 従来形式の材質上書き有無
        const bool directXTKMaterialOverride =
            m_materialOverrideEnabled
            && UsesDirectXTKModelMaterial(m_modelPath);
        // glTF・FBXは材質上書きの有無に関係なく自作シェーダーを選ぶ。
        // 骨格用自作シェーダーの有無
        const bool skinnedMaterialShader =
            !m_material.Shader().empty()
            && UsesSkinnedMaterialShader(m_modelPath);
        // 従来形式の自作シェーダーは材質上書き中に限る。
        // 従来形式の自作シェーダー有無
        const bool directXTKMaterialShader =
            directXTKMaterialOverride
            && !m_material.Shader().empty();
        // 遮蔽表示は通常の色描画で従来形式の自作シェーダーを使う場合に限る。
        if (occludedOnly && (depthOnly || !directXTKMaterialShader))
        {
            return;
        }
        // 従来形式の輪郭描画有無
        const bool drawOutline = directXTKMaterialShader
            && !occludedOnly
            && !m_wireframe
            && !depthOnly
            && m_material.CustomParameter(3).x > 0.0f;
        // 骨格形式の輪郭描画有無
        const bool drawSkinnedOutline = skinnedMaterialShader
            && !occludedOnly
            && !m_wireframe
            && !depthOnly
            && m_material.CustomParameter(3).x > 0.0f;
        // 骨格形式の遮蔽描画有無
        const bool drawSkinnedOccluded = skinnedMaterialShader
            && !occludedOnly
            && !m_wireframe
            && !depthOnly
            && m_material.CustomParameter(4).w > 0.0f;
        // 上書きなしの骨格形式には部品の色・粗さ・金属度を渡す。
        // 自作シェーダーの追加値は上書きの有無によらずRendererの設定を渡す。
        // 部品材質を転写する描画材質
        LitMaterial primitiveMaterial;
        // 写す追加値の番号
        for (std::size_t index{};
             index < LitMaterial::CustomParameterCount;
             ++index)
        {
            primitiveMaterial.SetCustomParameter(
                index,
                m_material.CustomParameter(index));
        }
        // 写す追加ベクトルの番号
        for (std::size_t index{};
             index < LitMaterial::CustomVectorCount;
             ++index)
        {
            primitiveMaterial.SetCustomVector(
                index,
                m_material.CustomVector(index));
        }
        // 半透明部品を描くフラグ
        for (const bool alphaPass : { false, true })
        {
            if (depthOnly && alphaPass)
            {
                continue;
            }
            // 描画対象のモデル部品
            for (const auto& primitive : model.primitives)
            {
                if (primitive.cpuVertexStride
                        < sizeof(ImportedModelVertex)
                    || primitive.cpuVertexData.empty()
                    || primitive.cpuVertexData.size()
                        % primitive.cpuVertexStride != 0
                    || primitive.cpuIndices.empty()
                    || primitive.meshNode
                        >= m_cachedGlobalPose.size())
                {
                    continue;
                }
                // 上書き規則を適用したRGBA色
                auto baseColor = m_materialOverrideEnabled
                    ? m_material.BaseColor()
                    : primitive.baseColor;
                if (directXTKMaterialOverride
                    && m_preserveEmbeddedMaterialColor)
                {
                    baseColor = {
                        baseColor.x * primitive.baseColor.x,
                        baseColor.y * primitive.baseColor.y,
                        baseColor.z * primitive.baseColor.z,
                        baseColor.w * primitive.baseColor.w };
                }
                // 形式ごとの半透明条件をD3D11経路と揃える。
                // 部品の半透明描画有無
                const bool alpha = skinnedMaterialShader
                    ? primitive.alpha || baseColor.w < 0.999f
                    : directXTKMaterialOverride
                    ? m_material.BaseColor().w < 0.999f || primitive.alpha
                    : primitive.alpha
                        || primitive.textureHasTransparency
                        || baseColor.w < 0.999f;
                if (alpha != alphaPass)
                {
                    continue;
                }

                // モデル空間の部品変換
                const auto meshGlobal = DirectX::XMLoadFloat4x4(
                    &m_cachedGlobalPose[primitive.meshNode]);
                // 部品空間のボーン変換行列
                std::vector<DirectX::XMMATRIX> palette;
                if (primitive.skin >= 0)
                {
                    // スキンの番号
                    const auto skinIndex =
                        static_cast<std::size_t>(primitive.skin);
                    if (skinIndex >= model.skins.size())
                    {
                        continue;
                    }
                    // 使用するスキンの定義
                    const auto& skin = model.skins[skinIndex];
                    // モデル空間の部品逆変換
                    const auto inverseMesh =
                        DirectX::XMMatrixInverse(nullptr, meshGlobal);
                    palette.reserve(skin.joints.size());
                    // スキン内の関節番号
                    for (std::size_t jointIndex{};
                        jointIndex < skin.joints.size();
                        ++jointIndex)
                    {
                        // 関節のノード番号
                        const auto nodeIndex = skin.joints[jointIndex];
                        if (nodeIndex >= m_cachedGlobalPose.size())
                        {
                            palette.push_back(
                                DirectX::XMMatrixIdentity());
                            continue;
                        }
                        // 初期関節姿勢の逆変換
                        const auto inverseBind =
                            jointIndex < skin.inverseBindMatrices.size()
                            ? DirectX::XMLoadFloat4x4(
                                &skin.inverseBindMatrices[jointIndex])
                            : DirectX::XMMatrixIdentity();
                        palette.push_back(
                            inverseBind
                            * DirectX::XMLoadFloat4x4(
                                &m_cachedGlobalPose[nodeIndex])
                            * inverseMesh);
                    }
                }

                // CPU側の頂点数
                const auto vertexCount = primitive.cpuVertexData.size()
                    / primitive.cpuVertexStride;
                // CPUで変形した描画頂点
                std::vector<PrimitiveRenderVertex> vertices;
                // 骨格用の自作シェーダーではCPU側の骨格変形を省く。
                if (!skinnedMaterialShader)
                {
                    vertices.reserve(vertexCount);
                }
                // 追加画像または頂点の番号
                for (std::size_t index{};
                    !skinnedMaterialShader && index < vertexCount;
                    ++index)
                {
                    // 変換元の取込頂点
                    ImportedModelVertex source{};
                    std::memcpy(
                        &source,
                        primitive.cpuVertexData.data()
                            + index * primitive.cpuVertexStride,
                        sizeof(source));
                    // 変形中の部品空間位置
                    auto position = DirectX::XMLoadFloat3(&source.position);
                    // 変形中の部品空間法線
                    auto normal = DirectX::XMLoadFloat3(&source.normal);
                    if (!palette.empty())
                    {
                        // 重み付きの変形後位置
                        DirectX::XMVECTOR skinnedPosition =
                            DirectX::XMVectorZero();
                        // 重み付きの変形後法線
                        DirectX::XMVECTOR skinnedNormal =
                            DirectX::XMVectorZero();
                        // 有効なボーン重みの合計
                        float totalWeight{};
                        // 頂点に影響するボーン番号
                        for (std::size_t influence{};
                            influence < 4u;
                            ++influence)
                        {
                            // ボーンパレットの番号
                            const auto bone = static_cast<std::uint8_t>(
                                source.blendIndices
                                    >> (influence * 8u));
                            // 頂点に掛かるボーン重み
                            const float weight = static_cast<float>(
                                static_cast<std::uint8_t>(
                                    source.blendWeights
                                        >> (influence * 8u)))
                                / 255.0f;
                            if (bone >= palette.size() || weight <= 0.0f)
                            {
                                continue;
                            }
                            skinnedPosition = DirectX::XMVectorAdd(
                                skinnedPosition,
                                DirectX::XMVectorScale(
                                    DirectX::XMVector3TransformCoord(
                                        position,
                                        palette[bone]),
                                    weight));
                            skinnedNormal = DirectX::XMVectorAdd(
                                skinnedNormal,
                                DirectX::XMVectorScale(
                                    DirectX::XMVector3TransformNormal(
                                        normal,
                                        palette[bone]),
                                    weight));
                            totalWeight += weight;
                        }
                        if (totalWeight > 0.0001f)
                        {
                            position = DirectX::XMVectorScale(
                                skinnedPosition,
                                1.0f / totalWeight);
                            normal = DirectX::XMVector3Normalize(
                                skinnedNormal);
                        }
                    }
                    // 描画用に変換した頂点
                    PrimitiveRenderVertex converted;
                    DirectX::XMStoreFloat3(&converted.position, position);
                    DirectX::XMStoreFloat3(&converted.normal, normal);
                    converted.textureCoordinate =
                        source.textureCoordinate;
                    vertices.push_back(converted);
                }

                // 詳細度を適用した頂点索引
                std::span<const std::uint32_t> indices =
                    primitive.cpuIndices;
                // 選択候補の詳細度段階
                for (std::size_t level = std::min<std::size_t>(
                        lodLevel,
                        primitive.cpuLodIndices.size());
                    level > 0;
                    --level)
                {
                    if (!primitive.cpuLodIndices[level - 1u].empty())
                    {
                        indices = primitive.cpuLodIndices[level - 1u];
                        break;
                    }
                }

                // 部品の描画要求
                PrimitiveDrawRequest request;
                request.shape = PrimitiveRenderShape::Procedural;
                request.vertices = vertices;
                request.indices = indices;
                DirectX::XMStoreFloat4x4(
                    &request.world,
                    meshGlobal * ownerWorld);
                DirectX::XMStoreFloat4x4(&request.view, view);
                DirectX::XMStoreFloat4x4(
                    &request.projection,
                    projection);
                request.baseColor = baseColor;
                request.roughness = m_materialOverrideEnabled
                    ? m_material.Roughness()
                    : primitive.roughness;
                request.metallic = m_materialOverrideEnabled
                    ? m_material.Metallic()
                    : primitive.metallic;
                request.normalStrength = m_materialOverrideEnabled
                    ? m_material.NormalStrength()
                    : 1.0f;
                request.occlusionStrength = m_materialOverrideEnabled
                    ? m_material.OcclusionStrength()
                    : primitive.occlusionStrength;
                request.emissiveFactor = m_materialOverrideEnabled
                    ? m_material.EmissiveColor()
                    : primitive.emissiveFactor;
                // 上書き中も空の色・法線画像は内蔵を継承し、PBR画像は空を含めて上書きする。
                // 部品内蔵画像の保持ビュー
                const auto& embeddedTextures = primitive.embeddedTextures;
                // 採用するPBR画像のビュー
                const auto& pbrTextures = m_materialOverrideEnabled
                    ? overrideTextures
                    : embeddedTextures;
                request.albedo = m_materialOverrideEnabled
                        && overrideTextures.albedo
                    ? overrideTextures.albedo
                    : embeddedTextures.albedo;
                request.normalTexture = m_materialOverrideEnabled
                        && overrideTextures.normal
                    ? overrideTextures.normal
                    : embeddedTextures.normal;
                request.roughnessTexture = pbrTextures.roughness;
                request.metallicTexture = pbrTextures.metallic;
                request.occlusionTexture = pbrTextures.occlusion;
                request.emissiveTexture = pbrTextures.emissive;
                request.fallbackTexture =
                    m_graphics->WhiteTextureViewHandle();
                request.alphaBlend = alpha;
                request.depthWrite = !alpha;
                request.depthOnly = depthOnly;
                // 影・深度の描画にも線表示設定を適用する。
                request.wireframe = m_wireframe;
                CopyPrimitiveLighting(
                    m_graphics->Lighting(),
                    request);
                if (!depthOnly)
                {
                    // 所有物のワールド位置で反射プローブを選ぶ。
                    // プローブ選択用のワールド位置
                    DirectX::XMFLOAT3 ownerPosition{};
                    DirectX::XMStoreFloat3(
                        &ownerPosition,
                        Owner().WorldMatrix().r[3]);
                    request.reflectionProbe = Owner().GetScene()
                        .ReflectionProbeEnvironmentAt(ownerPosition);
                }
                // 上書きなしの従来形式にはDirectXTKの線形霧を適用する。
                if (UsesDirectXTKModelMaterial(m_modelPath)
                    && !m_materialOverrideEnabled)
                {
                    request.fog.model = PrimitiveFogModel::DirectXTK;
                }
                if (!request.directionalShadow.texture
                    && m_graphics->Shadows().IsValid())
                {
                    request.directionalShadow.texture =
                        m_graphics->Shadows().ViewHandle();
                }
                if (!request.spotShadowTexture
                    && m_graphics->SpotShadows().IsValid())
                {
                    request.spotShadowTexture =
                        m_graphics->SpotShadows().ViewHandle();
                }
                if (!request.pointShadow.texture
                    && m_graphics->PointShadows().IsValid())
                {
                    request.pointShadow.texture =
                        m_graphics->PointShadows().ViewHandle();
                }
                if (directXTKMaterialShader)
                {
                    // 埋込色の合成結果だけを部品材質に反映する。
                    // 部品色を反映した描画材質
                    LitMaterial partMaterial = m_material;
                    partMaterial.SetBaseColor(baseColor);
                    // 従来形式の頂点の向きをD3D11の読込規則と揃える。
                    // 従来形式の部品描画状態
                    Detail::DirectXTKModelPartState partState;
                    partState.alphaPass = alpha;
                    partState.counterClockwise =
                        LoadsCounterClockwiseDirectXTKModel(m_modelPath);
                    // 自作シェーダーの描画要求
                    Detail::MaterialShaderDrawRequest material;
                    material.material = &partMaterial;
                    material.shaderMaterial = &m_material;
                    material.directXTKPart = &partState;
                    material.customTextures = pbrTextures.customTextures;
                    // 選択したシェーダーの世代
                    std::uint64_t generation{};
                    // 自作シェーダーの描画診断
                    std::string shaderError;
                    // 従来形式は遮蔽を事前描画し、輪郭を各部品の通常描画直前に重ねる。
                    if (occludedOnly || drawOutline)
                    {
                        material.pass = occludedOnly
                            ? Detail::MaterialShaderPass::Occluded
                            : Detail::MaterialShaderPass::Outline;
                        static_cast<void>(
                            m_graphics->DrawMaterialShaderPrimitive(
                                request,
                                material,
                                generation,
                                shaderError));
                        if (occludedOnly)
                        {
                            continue;
                        }
                        material.pass = Detail::MaterialShaderPass::Main;
                    }
                    static_cast<void>(m_graphics->DrawMaterialShaderPrimitive(
                        request,
                        material,
                        generation,
                        shaderError));
                    m_shaderError = std::move(shaderError);
                    m_shaderGeneration = generation;
                    m_activeShaderPath = m_material.Shader();
                    continue;
                }
                if (skinnedMaterialShader)
                {
                    // 部品描画に使う材質
                    const LitMaterial* drawMaterial = &m_material;
                    if (!m_materialOverrideEnabled)
                    {
                        primitiveMaterial.SetBaseColor(baseColor);
                        primitiveMaterial.SetRoughness(primitive.roughness);
                        primitiveMaterial.SetMetallic(primitive.metallic);
                        drawMaterial = &primitiveMaterial;
                    }
                    // GPUへ渡すボーン変換行列
                    std::vector<DirectX::XMFLOAT4X4> bones(palette.size());
                    // ボーンパレットの番号
                    for (std::size_t bone{}; bone < palette.size(); ++bone)
                    {
                        DirectX::XMStoreFloat4x4(&bones[bone], palette[bone]);
                    }
                    // GPU骨格描画用の幾何要求
                    Detail::SkinnedMaterialShaderGeometry geometry;
                    geometry.vertices = primitive.cpuVertexData;
                    geometry.vertexStride = primitive.cpuVertexStride;
                    geometry.indices = indices;
                    geometry.bones = bones;
                    geometry.alphaPass = alpha;
                    geometry.doubleSided = primitive.doubleSided;
                    // 自作シェーダーの描画要求
                    Detail::MaterialShaderDrawRequest material;
                    material.material = drawMaterial;
                    material.shaderMaterial = &m_material;
                    material.skinned = &geometry;
                    material.customTextures = pbrTextures.customTextures;
                    // 選択したシェーダーの世代
                    std::uint64_t generation{};
                    // 自作シェーダーの描画診断
                    std::string shaderError;
                    // 骨格形式は部品ごとに遮蔽・輪郭・通常描画の順に重ねる。
                    if (drawSkinnedOccluded)
                    {
                        material.pass = Detail::MaterialShaderPass::Occluded;
                        static_cast<void>(
                            m_graphics->DrawMaterialShaderPrimitive(
                                request,
                                material,
                                generation,
                                shaderError));
                    }
                    if (drawSkinnedOutline)
                    {
                        material.pass = Detail::MaterialShaderPass::Outline;
                        static_cast<void>(
                            m_graphics->DrawMaterialShaderPrimitive(
                                request,
                                material,
                                generation,
                                shaderError));
                    }
                    material.pass = Detail::MaterialShaderPass::Main;
                    static_cast<void>(m_graphics->DrawMaterialShaderPrimitive(
                        request,
                        material,
                        generation,
                        shaderError));
                    m_shaderError = std::move(shaderError);
                    m_shaderGeneration = generation;
                    m_activeShaderPath = m_material.Shader();
                    continue;
                }
                static_cast<void>(m_graphics->DrawPrimitive(request));
            }
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
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            DrawD3D12Model(view, projection);
            return;
        }
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                != RenderingApi::DirectX11)
        {
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

        // ジオメトリシェーダーを後続描画へ持ち越さない。
        // 描画後のGS解除を管理する寿命
        const GeometryShaderScope geometryScope{
            m_effect != nullptr
                && m_effect->HasGeometryShader()
                ? m_context
                : nullptr
        };

        if (m_model->skeletalModel)
        {
            // 現在の再生クリップ
            const auto* clip =
                m_animationIndex
                    < m_model->skeletalModel->animations.size()
                ? &m_model->skeletalModel
                    ->animations[m_animationIndex]
                : nullptr;
            // 遷移先の再生クリップ
            const auto* blendClip =
                !m_nextAnimationState.empty()
                    && m_nextAnimationIndex
                        < m_model->skeletalModel->animations.size()
                ? &m_model->skeletalModel
                    ->animations[m_nextAnimationIndex]
                : nullptr;
            // 遷移先の混合割合
            const float blendAmount =
                blendClip != nullptr
                    && m_animationTransitionDuration > 0.0f
                ? std::clamp(
                    m_animationTransitionTime
                        / m_animationTransitionDuration,
                    0.0f,
                    1.0f)
                : 0.0f;
            // 姿勢を評価するフレーム番号
            const auto poseFrame =
                m_graphics->FrameStats().totalFrames;
            // 姿勢を評価するモデル
            const auto* poseModel =
                m_model->skeletalModel.get();
            if (m_cachedPoseFrame != poseFrame
                || m_cachedPoseModel != poseModel)
            {
                LAMAPON_PROFILE_SCOPE("SkeletalModel.Pose");
                // ノードのローカル姿勢
                std::vector<SkeletalPoseTransform> localPose;
                // 重み付きの姿勢標本
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
            // 上書き中は空の色・法線画像を内蔵で補い、PBR・追加画像は空を含めて上書きする。
            // 上書き画像の保持ビュー
            const auto textureOverride = BuildLitTextureRequest();
            // 借用する追加画像のビュー配列
            std::array<
                ID3D11ShaderResourceView*,
                LitMaterial::CustomTextureCount> customTextureViews{};
            // 追加画像の番号
            for (std::size_t index{};
                index < customTextureViews.size();
                ++index)
            {
                // 追加画像の保持ビュー
                const auto& handle =
                    textureOverride.customTextures[index];
                if (!handle)
                {
                    continue;
                }
                customTextureViews[index] =
                    Detail::GraphicsDeviceD3D11Access::
                        TryResolveD3D11ShaderResourceView(
                            *m_graphics,
                            handle);
                if (customTextureViews[index] == nullptr)
                {
                    return;
                }
            }
            // 深度のみを描くフラグ
            const bool depthOnly =
                m_graphics->IsDepthOnlyPass();
            // 材質上書きの有無に関係なく選択済みの照明方式を使う。
            m_model->skeletalModel->Draw(
                *m_graphics,
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
                m_materialOverrideEnabled
                    ? &textureOverride
                    : nullptr,
                blendClip,
                m_nextAnimationTime,
                blendAmount,
                nullptr,
                m_applyRootMotion
                    ? ResolveRootMotionNode()
                    : std::numeric_limits<
                        std::size_t>::max(),
                m_skinnedEffect,
                m_skinnedInputLayout.Get(),
                depthOnly,
                &m_cachedGlobalPose,
                m_graphics->Settings().automaticLodQuality,
                m_skinnedEffect != nullptr
                        && m_skinnedEffect->IsManifestEffect()
                    ? &m_skeletalColorInputLayouts
                    : nullptr,
                m_skinnedEffect != nullptr
                        && m_skinnedEffect->IsManifestEffect()
                    ? &m_skeletalOutlineInputLayouts
                    : nullptr,
                &customTextureViews,
                m_skeletalForwardEffect,
                m_skeletalForwardEffect != nullptr
                        && m_skeletalForwardEffect->IsManifestEffect()
                    ? &m_skeletalForwardColorInputLayouts
                    : nullptr,
                m_skeletalForwardEffect != nullptr
                        && m_skeletalForwardEffect->IsManifestEffect()
                    ? &m_skeletalForwardOutlineInputLayouts
                    : nullptr,
                depthOnly
                    && m_graphics->DepthPass()
                        == DepthPassKind::Prepass,
                &m_material);
            return;
        }

        if (UsesCommonLit())
        {
            DrawCommonLit(view, projection);
            return;
        }

        // 描画中に保持する色画像資源
        const auto albedoResources = m_albedoTexture
            ? m_albedoTexture->resources.Acquire()
            : nullptr;
        // 描画中に保持する法線画像資源
        const auto normalResources = m_normalTexture
            ? m_normalTexture->resources.Acquire()
            : nullptr;
        // 借用する色画像ビュー
        auto* const albedoView = albedoResources
            ? Detail::GraphicsDeviceD3D11Access::
                TryResolveD3D11ShaderResourceView(
                    *m_graphics,
                    *albedoResources)
            : nullptr;
        // 借用する法線画像ビュー
        auto* const normalView = normalResources
            ? Detail::GraphicsDeviceD3D11Access::
                TryResolveD3D11ShaderResourceView(
                    *m_graphics,
                    *normalResources)
            : nullptr;
        // 従来描画の効果へ上書き材質と照明を反映する(effect: 借用するモデル描画効果)。
        m_model->model->UpdateEffects(
            [this, albedoView, normalView](DirectX::IEffect* effect)
            {
                if (m_materialOverrideEnabled)
                {
                    // 従来描画に適用する上書き色
                    const auto color = DirectX::XMLoadFloat4(
                        &m_material.BaseColor());
                    // 上書き色の不透明度
                    const float alpha = m_material.BaseColor().w;
                    // 従来材質の反射指数
                    const float specularPower =
                        SpecularPowerFromRoughness(
                            m_material.Roughness());
                    // 従来材質の反射色
                    const auto specularColor =
                        SpecularColorFromRoughness(
                            m_material.Roughness());
                    // 借用する法線マップ描画効果
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
                        if (albedoView != nullptr)
                        {
                            normalMap->SetTexture(albedoView);
                        }
                        if (normalView != nullptr)
                        {
                            normalMap->SetNormalTexture(normalView);
                        }
                    }
                    // 借用する標準描画効果
                    else if (auto* basic =
                        dynamic_cast<DirectX::BasicEffect*>(effect))
                    {
                        basic->SetDiffuseColor(color);
                        basic->SetAlpha(alpha);
                        basic->SetSpecularColor(specularColor);
                        basic->SetSpecularPower(specularPower);
                        if (albedoView != nullptr)
                        {
                            basic->SetTextureEnabled(true);
                            basic->SetTexture(albedoView);
                        }
                    }
                    // 借用する骨格描画効果
                    else if (auto* skinned =
                        dynamic_cast<DirectX::SkinnedEffect*>(effect))
                    {
                        skinned->SetDiffuseColor(color);
                        skinned->SetAlpha(alpha);
                        skinned->SetSpecularColor(
                            specularColor);
                        skinned->SetSpecularPower(
                            specularPower);
                        if (albedoView != nullptr)
                        {
                            skinned->SetTexture(albedoView);
                        }
                    }
                    // 借用するDGSL描画効果
                    else if (auto* dgsl =
                        dynamic_cast<DirectX::DGSLEffect*>(effect))
                    {
                        dgsl->SetDiffuseColor(color);
                        dgsl->SetAlpha(alpha);
                        dgsl->SetSpecularColor(specularColor);
                        dgsl->SetSpecularPower(specularPower);
                        if (albedoView != nullptr)
                        {
                            dgsl->SetTextureEnabled(true);
                            dgsl->SetTexture(albedoView);
                        }
                    }
                    // 借用する切抜き描画効果
                    else if (auto* alphaTest =
                        dynamic_cast<DirectX::AlphaTestEffect*>(
                            effect))
                    {
                        alphaTest->SetDiffuseColor(color);
                        alphaTest->SetAlpha(alpha);
                        if (albedoView != nullptr)
                        {
                            alphaTest->SetTexture(albedoView);
                        }
                    }
                    // 借用する二画像描画効果
                    else if (auto* dualTexture =
                        dynamic_cast<DirectX::DualTextureEffect*>(
                            effect))
                    {
                        dualTexture->SetDiffuseColor(color);
                        dualTexture->SetAlpha(alpha);
                        if (albedoView != nullptr)
                        {
                            dualTexture->SetTexture(albedoView);
                        }
                    }
                    // 借用する環境反射描画効果
                    else if (auto* environment =
                        dynamic_cast<
                            DirectX::EnvironmentMapEffect*>(
                            effect))
                    {
                        environment->SetDiffuseColor(color);
                        environment->SetAlpha(alpha);
                        if (albedoView != nullptr)
                        {
                            environment->SetTexture(albedoView);
                        }
                    }
                }

                // 借用する照明対応描画効果
                if (auto* lights =
                    dynamic_cast<DirectX::IEffectLights*>(effect))
                {
                    // 照明選択用のワールド位置
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

        // 従来描画の深度パスでは切抜き効果がなければピクセルシェーダーを外す。
        // 深度のみを描くフラグ
        const bool depthOnly = m_graphics->IsDepthOnlyPass();
        // モデル内の切抜き効果の有無
        bool hasAlphaTest = false;
        if (depthOnly)
        {
            // 切抜き用の効果があるか調べる(effect: 借用するモデル描画効果)。
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
        // 切抜きなしの深度描画では効果適用後にピクセルシェーダーを解除する。
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
        // 制御器のパラメーター定義
        for (const auto& parameter :
            m_animationController->FloatParameters())
        {
            m_animationFloatValues.emplace(
                parameter.name,
                parameter.defaultValue);
        }
        // 制御器の状態定義
        for (const auto& state :
            m_animationController->States())
        {
            if (!state.blendChildren.empty())
            {
                // 混合する子クリップ定義
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
                // 状態が参照するクリップ名
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
        // 制御器の初期状態
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
        // 状態が参照するクリップ名
        const std::string_view clipName =
            state.modelClip.empty()
            ? std::string_view{ state.name }
            : std::string_view{ state.modelClip };
        return ResolveAnimationIndex(clipName);
    }

    std::size_t ModelRendererComponent::ResolveAnimationIndex(
        const std::string_view modelClip) const noexcept
    {
        // 子クリップまたは再生の番号
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
        // 状態内の子クリップ定義
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

        // 混合に使うパラメーター値
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
        // 子クリップまたは再生の番号
        for (std::size_t index = 1;
            index < children.size();
            ++index)
        {
            if (value > children[index].threshold)
            {
                continue;
            }
            // 混合区間の左端クリップ
            const auto& left = children[index - 1];
            // 混合区間の右端クリップ
            const auto& right = children[index];
            // 右端クリップの混合割合
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
        // クリップの長さを取得し、未解決なら0を返す(clip: 再生クリップ名)。
        const auto durationFor =
            [this](const std::string_view clip)
            {
                // 子クリップまたは再生の番号
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
        // 子クリップの混合重み
        std::vector<float> weights;
        CalculateBlendWeights(state, weights);
        // 重み付きの再生区間長秒
        float duration{};
        // 子クリップまたは再生の番号
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
        // 子クリップまたは再生の番号
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
        // 遷移先の制御状態
        const auto* target =
            m_animationController->FindState(
                transition.to);
        if (target == nullptr)
        {
            return;
        }
        // 遷移先の再生クリップ番号
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
        // 遷移元の制御状態
        const auto* current =
            m_animationController->FindState(
                m_currentAnimationState);
        if (current == nullptr)
        {
            return;
        }
        // 遷移元の再生区間長秒
        const float currentDuration =
            StateAnimationDuration(*current);
        // 遷移元の進行前時刻秒
        const float previousTime =
            m_animationTime;
        // 遷移元の符号付き進行秒数
        const float currentDelta =
            deltaTime * m_animationSpeed
                * current->speed;
        m_animationTime = AdvanceClipTime(
            previousTime,
            currentDelta,
            currentDuration,
            current->loop);
        // 遷移元の順再生フラグ
        const bool currentForward =
            currentDelta >= 0.0f;
        // 遷移元の循環検出結果
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
            // 遷移元の正規化時刻
            const float normalizedTime =
                currentDuration > 0.0f
                ? m_animationTime / currentDuration
                : 1.0f;
            // 条件を満たした遷移定義
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

        // 遷移先の制御状態
        const auto* next =
            m_animationController->FindState(
                m_nextAnimationState);
        if (next == nullptr
            || m_nextAnimationIndex >= AnimationCount())
        {
            m_nextAnimationState.clear();
            return;
        }
        // 遷移先の再生区間長秒
        const float nextDuration =
            StateAnimationDuration(*next);
        // 遷移先の進行前時刻秒
        const float previousNextTime =
            m_nextAnimationTime;
        // 遷移先の符号付き進行秒数
        const float nextDelta =
            deltaTime * m_animationSpeed
                * next->speed;
        m_nextAnimationTime = AdvanceClipTime(
            previousNextTime,
            nextDelta,
            nextDuration,
            next->loop);
        // 遷移先の順再生フラグ
        const bool nextForward =
            nextDelta >= 0.0f;
        // 遷移先の循環検出結果
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
        // 遷移の経過時間には再生速度倍率を掛けない。
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
            // 骨格形式では選択済みの描画効果の有無を返す。
            return m_skinnedEffect != nullptr;
        }
        return UsesCommonLit();
    }

    bool ModelRendererComponent::CanBeInstanced() const
    {
        // D3D12経路の使用有無
        const bool usesD3D12 = m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental;
        // 共有効果の能力を参照する前に世代と借用ポインターを同期する。
        if (!usesD3D12)
        {
            const_cast<ModelRendererComponent*>(this)
                ->RefreshShader(false);
        }
        // 従来形式・線表示・深度描画はまとめ描画の対象外とする。
        if (m_wireframe
            || m_useLegacyShading
            || m_graphics == nullptr
            || (!usesD3D12
                && m_graphics->ActiveRenderingApi()
                    != RenderingApi::DirectX11)
            || m_graphics->IsDepthOnlyPass()
            || !m_model
            || !m_model->skeletalModel
            || (usesD3D12 && UsesDirectXTKModelMaterial(m_modelPath)))
        {
            return false;
        }
        if (m_materialOverrideEnabled
            && m_material.BaseColor().w < 0.999f)
        {
            return false;
        }
        // まとめて描く骨格モデル
        const auto& model = *m_model->skeletalModel;
        if (!model.skins.empty()
            || !model.animations.empty()
            || model.primitives.empty())
        {
            return false;
        }
        if (usesD3D12)
        {
            if (!m_material.Shader().empty())
            {
                try
                {
                    if (!m_graphics->PrepareMaterialShaderPasses(
                            m_material.Shader(),
                            m_material.ShaderKeywords()).instanced)
                    {
                        return false;
                    }
                }
                catch (...)
                {
                    return false;
                }
            }
        }
        else
        {
            // 借用するまとめ描画効果
            auto* const effect = m_material.Shader().empty()
                ? &m_graphics->Lit()
                : m_skinnedEffect;
            if (effect == nullptr
            || effect->IsSkinned()
            || !effect->SupportsInstancing())
            {
                return false;
            }
            // 順序依存の合成パスの有無
            bool hasOrderDependentInstancedPass{};
            // 描画パスまたはバイトの番号
            for (std::size_t index = 0;
                index < effect->PassCount(ShaderPassRole::Instanced);
                ++index)
            {
                effect->SelectPass(ShaderPassRole::Instanced, index);
                // 対象パスの宣言描画状態
                const auto& state = effect->SelectedPassRenderState(
                    ShaderPassRole::Instanced);
                if (state.declared
                    && (state.blend == ShaderBlendMode::Alpha
                        || state.blend
                            == ShaderBlendMode::Premultiplied))
                {
                    hasOrderDependentInstancedPass = true;
                    break;
                }
            }
            effect->SelectPass(ShaderPassRole::Instanced, 0);
            if (hasOrderDependentInstancedPass
                || (effect->HasOutline()
                && m_material.CustomParameter(3).x > 0.0f)
                || (effect->HasOccludedPass()
                    && m_material.CustomParameter(4).w > 0.0f))
            {
                return false;
            }
        }
        // 全部品が骨格なしの不透明な描画幾何か調べる(primitive: モデルの部品)。
        return std::ranges::all_of(
            model.primitives,
            [usesD3D12](const SkeletalPrimitive& primitive)
            {
                // 使用する描画APIに対応した幾何資源を確認する。
                // 有効な描画幾何の有無
                const bool hasGeometry = usesD3D12
                    ? primitive.cpuVertexStride
                            >= sizeof(ImportedModelVertex)
                        && !primitive.cpuVertexData.empty()
                        && primitive.cpuVertexData.size()
                            % primitive.cpuVertexStride == 0u
                        && !primitive.cpuIndices.empty()
                    : primitive.vertexBuffer && primitive.indexBuffer;
                return primitive.skin < 0
                    && hasGeometry
                    && !primitive.alpha
                    && !primitive.textureHasTransparency
                    && primitive.baseColor.w >= 0.999f;
            });
    }

    std::uint64_t
        ModelRendererComponent::InstanceBatchKey()
            const noexcept
    {
        // プロセス内のバッチ識別値
        std::uint64_t hash = 14695981039346656037ull;
        // バイト列をバッチ識別値へ混合する(data: 入力バイト列, byteCount: 入力バイト数)。
        const auto hashBytes = [&hash](
            const void* const data,
            const std::size_t byteCount) noexcept
        {
            // 識別に使うバイト列
            const auto* const bytes = static_cast<
                const unsigned char*>(data);
            // 描画パスまたはバイトの番号
            for (std::size_t index = 0;
                // 識別に使うバイト数
                index < byteCount;
                ++index)
            {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        // パスの長さとOS固有文字列を識別値へ混合する(path: 対象パス)。
        const auto hashPath = [&hashBytes](
            const std::filesystem::path& path) noexcept
        {
            // OS固有形式のパス文字列
            const auto& native = path.native();
            // 識別に使うバイト数
            const auto byteCount = native.size()
                * sizeof(std::filesystem::path::value_type);
            hashBytes(&byteCount, sizeof(byteCount));
            hashBytes(
                native.data(),
                byteCount);
        };
        // 文字列の長さと内容を識別値へ混合する(value: 対象文字列)。
        const auto hashString = [&hashBytes](
            const std::string& value) noexcept
        {
            // 識別に使うバイト数
            const auto byteCount = value.size();
            hashBytes(&byteCount, sizeof(byteCount));
            hashBytes(value.data(), value.size());
        };

        hashPath(m_modelPath);
        hashPath(m_material.Shader());
        // 有効なシェーダーキーワード
        for (const auto& keyword :
            m_material.ShaderKeywords().Keywords())
        {
            hashString(keyword);
            // 識別値に加える区切り文字
            constexpr unsigned char separator = 0xff;
            hashBytes(&separator, sizeof(separator));
        }
        // 追加画像のパス
        for (const auto& texture : m_material.CustomTextures())
        {
            hashPath(texture);
            // 識別値に加える区切り文字
            constexpr unsigned char separator = 0xfe;
            hashBytes(&separator, sizeof(separator));
        }
        // 材質上書きの識別フラグ
        const std::uint8_t materialOverride =
            m_materialOverrideEnabled ? 1u : 0u;
        hashBytes(&materialOverride, sizeof(materialOverride));
        if (m_materialOverrideEnabled)
        {
            // 上書き材質のRGBA色
            const auto& baseColor = m_material.BaseColor();
            // 上書き材質の粗さ
            const auto roughness = m_material.Roughness();
            // 上書き材質の法線強度
            const auto normalStrength = m_material.NormalStrength();
            // 上書き材質の金属度
            const auto metallic = m_material.Metallic();
            // 上書き材質の遮蔽強度
            const auto occlusionStrength =
                m_material.OcclusionStrength();
            // 上書き材質の発光色
            const auto& emissive = m_material.EmissiveColor();
            hashBytes(&baseColor, sizeof(baseColor));
            hashBytes(&roughness, sizeof(roughness));
            hashBytes(&normalStrength, sizeof(normalStrength));
            hashBytes(&metallic, sizeof(metallic));
            hashBytes(
                &occlusionStrength,
                sizeof(occlusionStrength));
            hashBytes(&emissive, sizeof(emissive));
            hashBytes(
                m_material.CustomParameters().data(),
                sizeof(m_material.CustomParameters()));
            hashBytes(
                m_material.CustomVectors().data(),
                sizeof(m_material.CustomVectors()));
            hashPath(m_material.AlbedoTexture());
            hashPath(m_material.NormalTexture());
            hashPath(m_material.RoughnessTexture());
            hashPath(m_material.MetallicTexture());
            hashPath(m_material.OcclusionTexture());
            hashPath(m_material.EmissiveTexture());
        }
        // 再読込中に異なる共有効果を同じバッチへ混ぜない。
        // バッチの共有描画効果
        const auto* const batchEffect = m_graphics != nullptr
                && m_graphics->ActiveRenderingApi()
                    == RenderingApi::DirectX11
                && m_material.Shader().empty()
                ? &m_graphics->Lit()
                : m_skinnedEffect;
        // 描画効果のアドレス識別値
        const auto effectIdentity = reinterpret_cast<
            std::uintptr_t>(batchEffect);
        hashBytes(&effectIdentity, sizeof(effectIdentity));
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
        if (batch.size() < 2)
        {
            return false;
        }
        // 借用効果を参照する前にバッチ全体を同期する。
        // 代表描画器の同期済みフラグ
        bool refreshedThis{};
        // 対象のモデル描画器
        for (auto* const component : batch)
        {
            if (component == nullptr)
            {
                return false;
            }
            component->RefreshShader(false);
            refreshedThis = refreshedThis || component == this;
        }
        if (!refreshedThis)
        {
            RefreshShader(false);
        }
        if (!CanBeInstanced())
        {
            return false;
        }
        // 同期後の代表バッチ識別値
        const auto refreshedBatchKey = InstanceBatchKey();
        // 同期後の描画能力とバッチ識別値を再確認する(component: 対象のモデル描画器)。
        if (std::ranges::any_of(
            batch,
            [refreshedBatchKey](
                const ModelRendererComponent* const component)
            {
                return !component->CanBeInstanced()
                    || component->InstanceBatchKey()
                        != refreshedBatchKey;
            }))
        {
            return false;
        }
        if (m_graphics->ActiveRenderingApi()
            == RenderingApi::DirectX12Experimental)
        {
            return RenderD3D12InstancedBatch(batch, view, projection);
        }

        using Vertex = DirectX::
            VertexPositionNormalTangentColorTextureSkinning;
        // 頂点とインスタンスの要素宣言
        std::array<
            D3D11_INPUT_ELEMENT_DESC,
            Vertex::InputElementCount + 5> elements{};
        std::copy_n(
            Vertex::InputElements,
            Vertex::InputElementCount,
            elements.begin());
        elements[Vertex::InputElementCount + 0] =
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 0,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
        elements[Vertex::InputElementCount + 1] =
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 1,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 16,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
        elements[Vertex::InputElementCount + 2] =
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 2,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 32,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
        elements[Vertex::InputElementCount + 3] =
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_TRANSFORM", 3,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 48,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
        elements[Vertex::InputElementCount + 4] =
            D3D11_INPUT_ELEMENT_DESC{
                "INSTANCE_COLOR", 0,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 64,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };

        // まとめて描く骨格モデル
        auto& model = *m_model->skeletalModel;
        // 使用する共有描画効果
        auto* const selectedEffect = m_material.Shader().empty()
            ? &m_graphics->Lit()
            : m_skinnedEffect;
        if (selectedEffect == nullptr)
        {
            return false;
        }
        if (selectedEffect->IsSkinned()
            || !selectedEffect->SupportsInstancing())
        {
            return false;
        }
        // 借用するまとめ描画効果
        auto& effect = *selectedEffect;
        // 描画パスの状態復帰を管理
        const MaterialPassScope passScope{ effect, m_context };
        // まとめ描画パスの数
        const auto instancedPassCount = effect.PassCount(
            ShaderPassRole::Instanced);
        if (m_instancedInputLayouts.size()
            != instancedPassCount)
        {
            m_instancedInputLayouts.clear();
            m_instancedInputLayouts.reserve(instancedPassCount);
            // まとめ描画パスの番号
            for (std::size_t passIndex = 0;
                // まとめ描画パスの数
                passIndex < instancedPassCount;
                ++passIndex)
            {
                effect.SelectPass(
                    ShaderPassRole::Instanced,
                    passIndex);
                // 借用する頂点シェーダー列
                auto* const byteCode =
                    effect.SelectedPassVertexShaderByteCode(
                        ShaderPassRole::Instanced);
                if (byteCode == nullptr)
                {
                    m_instancedInputLayouts.clear();
                    return false;
                }
                // 対象パスの頂点レイアウト
                Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
                if (FAILED(
                    CreateInputLayoutWithPositionAlias(
                        Detail::GraphicsDeviceD3D11Access::Device(
                            *m_graphics),
                        elements.data(),
                        static_cast<UINT>(elements.size()),
                        byteCode,
                        layout.ReleaseAndGetAddressOf())))
                {
                    m_instancedInputLayouts.clear();
                    return false;
                }
                m_instancedInputLayouts.push_back(std::move(layout));
            }
            effect.SelectPass(ShaderPassRole::Instanced, 0);
        }

        // 初期ローカル姿勢
        std::vector<SkeletalPoseTransform> localPose;
        // 初期モデル空間姿勢
        std::vector<DirectX::XMFLOAT4X4> bindPose;
        SkeletalModel::SamplePose(
            model.nodes,
            nullptr,
            0.0f,
            localPose,
            bindPose);

        // 詳細度別のモデル描画器一覧
        std::array<
            std::vector<ModelRendererComponent*>,
            3> lodBatches;
        // 対象のモデル描画器
        for (auto* component : batch)
        {
            lodBatches[std::min<std::size_t>(
                component->AutomaticLodLevel(
                    view,
                    projection),
                2u)].push_back(component);
        }

        struct InstanceData final
        {
            // 部品のワールド変換行列
            DirectX::XMFLOAT4X4 world;
            // 描画するRGBA色
            DirectX::XMFLOAT4 color;
        };
        // スロット1の行列4行と色を80バイトに維持する。
        static_assert(sizeof(InstanceData) == 80);
        static_assert(sizeof(Vertex) >= 52);

        // 借用する描画コンテキスト
        auto* const context = Detail::GraphicsDeviceD3D11Access::Context(
            *m_graphics);
        // 頂点ごとのバイト数
        constexpr UINT vertexStride = sizeof(Vertex);
        effect.SetMatrices(
            DirectX::XMMatrixIdentity(),
            view,
            projection);
        if (!m_graphics->TrySetLitEffectLighting(
                effect,
                m_graphics->Lighting()))
        {
            return false;
        }
        effect.SetInstancingEnabled(true);
        effect.SetTessellationDrawEnabled(false);

        // 部品を描画したかの結果
        bool drewAny{};
        // まとめる詳細度の段階
        for (std::size_t lodLevel = 0;
            lodLevel < lodBatches.size();
            ++lodLevel)
        {
            // 同じ詳細度のモデル描画器
            const auto& components = lodBatches[lodLevel];
            if (components.empty())
            {
                continue;
            }
            // 描画するモデル部品
            for (const auto& primitive : model.primitives)
            {
                if (primitive.meshNode >= bindPose.size())
                {
                    continue;
                }
                // 部品ごとのインスタンス列
                std::vector<InstanceData> instances;
                instances.reserve(components.size());
                // 部品の初期モデル空間変換
                const auto meshGlobal = DirectX::XMLoadFloat4x4(
                    &bindPose[primitive.meshNode]);
                // 対象のモデル描画器
                for (const auto* component : components)
                {
                    // インスタンスの変換と色
                    InstanceData data{};
                    DirectX::XMStoreFloat4x4(
                        &data.world,
                        meshGlobal
                            * component->Owner().WorldMatrix());
                    data.color = component->m_materialOverrideEnabled
                        ? component->m_material.BaseColor()
                        : primitive.baseColor;
                    instances.push_back(data);
                }
                // 保持するインスタンスバッファ
                const auto instanceBuffer =
                    m_graphics->AcquireInstanceBufferHandle(
                        std::as_bytes(std::span{ instances }));
                if (!instanceBuffer)
                {
                    effect.SetInstancingEnabled(false);
                    return false;
                }

                // 詳細度に応じた索引バッファ
                ID3D11Buffer* indexBuffer =
                    primitive.indexBuffer.Get();
                // 詳細度に応じた索引数
                std::uint32_t indexCount = primitive.indexCount;
                // 選択候補の詳細度段階
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

                // 代表材質または部品の材質
                LitMaterial material;
                if (m_materialOverrideEnabled)
                {
                    material = m_material;
                }
                else
                {
                    material.SetBaseColor(primitive.baseColor);
                    material.SetRoughness(primitive.roughness);
                    material.SetMetallic(primitive.metallic);
                }
                effect.SetMaterial(material);
                // 部品のPBR画像と係数
                PbrTextures pbr{};
                if (!m_materialOverrideEnabled)
                {
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
                }
                if (m_materialOverrideEnabled)
                {
                    // 上書き画像の保持ビュー
                    const auto textureRequest = BuildLitTextureRequest();
                    if (!m_graphics->TrySetLitEffectTextures(
                            effect,
                            textureRequest))
                    {
                        effect.SetInstancingEnabled(false);
                        return false;
                    }
                }
                else
                {
                    effect.SetTextures(
                        primitive.texture.Get(),
                        primitive.normalTexture.Get(),
                        pbr);
                    effect.SetCustomTextures({});
                }

                // 部品の頂点バッファ配列
                ID3D11Buffer* vertexBuffers[]{
                    primitive.vertexBuffer.Get()
                };
                // 頂点ごとのバイト間隔
                const UINT strides[]{ vertexStride };
                // 頂点列の先頭バイト位置
                constexpr UINT offsets[]{ 0 };
                context->IASetVertexBuffers(
                    0,
                    1,
                    vertexBuffers,
                    strides,
                    offsets);
                m_graphics->BindVertexBuffer(
                    instanceBuffer,
                    1,
                    static_cast<std::uint32_t>(
                        sizeof(InstanceData)));
                context->IASetIndexBuffer(
                    indexBuffer,
                    DXGI_FORMAT_R32_UINT,
                    0);
                context->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                // まとめ描画パスの番号
                for (std::size_t passIndex = 0;
                    // まとめ描画パスの数
                    passIndex < instancedPassCount;
                    ++passIndex)
                {
                    if (passIndex
                        >= m_instancedInputLayouts.size()
                        || !m_instancedInputLayouts[passIndex])
                    {
                        break;
                    }
                    effect.SelectPass(
                        ShaderPassRole::Instanced,
                        passIndex);
                    context->IASetInputLayout(
                        m_instancedInputLayouts[passIndex].Get());
                    // 対象パスの宣言描画状態
                    const auto& renderState =
                        effect.SelectedPassRenderState(
                            ShaderPassRole::Instanced);
                    if (renderState.declared)
                    {
                        ApplyImportedModelRenderState(
                            context,
                            *m_graphics,
                            renderState,
                            primitive.doubleSided);
                    }
                    else
                    {
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
                    }
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
        }
        if (!drewAny)
        {
            return false;
        }
        // 対象のモデル描画器
        for (auto* component : batch)
        {
            if (component != nullptr)
            {
                component->m_instancedThisPass = true;
            }
        }
        return true;
    }

    bool ModelRendererComponent::RenderD3D12InstancedBatch(
        const std::vector<ModelRendererComponent*>& batch,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (batch.size() < 2 || !CanBeInstanced())
        {
            return false;
        }
        // まとめて描く骨格モデル
        const auto& model = *m_model->skeletalModel;
        // 初期姿勢のモデルを詳細度の段階ごとにまとめる。
        // 初期ローカル姿勢
        std::vector<SkeletalPoseTransform> localPose;
        // 初期モデル空間姿勢
        std::vector<DirectX::XMFLOAT4X4> bindPose;
        SkeletalModel::SamplePose(
            model.nodes,
            nullptr,
            0.0f,
            localPose,
            bindPose);
        // 詳細度別のモデル描画器一覧
        std::array<std::vector<ModelRendererComponent*>, 3> lodBatches;
        // 対象のモデル描画器
        for (auto* const component : batch)
        {
            if (component != nullptr && component->CanBeInstanced())
            {
                lodBatches[std::min<std::size_t>(
                    component->AutomaticLodLevel(view, projection),
                    2u)].push_back(component);
            }
        }

        // 描画済みのモデル描画器一覧
        std::vector<ModelRendererComponent*> drawnComponents;
        // まとめる詳細度の段階
        for (std::size_t lodLevel{}; lodLevel < lodBatches.size(); ++lodLevel)
        {
            // 同じ詳細度のモデル描画器
            const auto& components = lodBatches[lodLevel];
            if (components.empty())
            {
                continue;
            }
            // 対象詳細度の描画済みフラグ
            bool drewLevel{};
            // 描画するモデル部品
            for (const auto& primitive : model.primitives)
            {
                if (primitive.meshNode >= bindPose.size())
                {
                    continue;
                }
                // 部品の初期モデル空間変換
                const auto meshGlobal = DirectX::XMLoadFloat4x4(
                    &bindPose[primitive.meshNode]);
                // インスタンス色には部品の埋込色を使う。
                // 部品ごとのインスタンス列
                std::vector<PrimitiveInstanceData> instances;
                instances.reserve(components.size());
                // 対象のモデル描画器
                for (const auto* const component : components)
                {
                    // インスタンスの変換と色
                    PrimitiveInstanceData instance;
                    DirectX::XMStoreFloat4x4(
                        &instance.world,
                        meshGlobal * component->Owner().WorldMatrix());
                    instance.color = primitive.baseColor;
                    instances.push_back(instance);
                }

                // CPU側の頂点数
                const auto vertexCount = primitive.cpuVertexData.size()
                    / primitive.cpuVertexStride;
                // 描画用に変換する頂点列
                std::vector<PrimitiveRenderVertex> vertices;
                vertices.reserve(vertexCount);
                // 変換する頂点の番号
                for (std::size_t index{}; index < vertexCount; ++index)
                {
                    // 変換元の取込頂点
                    ImportedModelVertex source{};
                    std::memcpy(
                        &source,
                        primitive.cpuVertexData.data()
                            + index * primitive.cpuVertexStride,
                        sizeof(source));
                    vertices.push_back({
                        source.position,
                        source.normal,
                        source.textureCoordinate });
                }
                // 詳細度を適用した頂点索引
                std::span<const std::uint32_t> indices =
                    primitive.cpuIndices;
                // 選択候補の詳細度段階
                for (std::size_t level = std::min<std::size_t>(
                        lodLevel,
                        primitive.cpuLodIndices.size());
                    level > 0;
                    --level)
                {
                    if (!primitive.cpuLodIndices[level - 1u].empty())
                    {
                        indices = primitive.cpuLodIndices[level - 1u];
                        break;
                    }
                }

                // 部品のまとめ描画要求
                PrimitiveDrawRequest request;
                request.shape = PrimitiveRenderShape::Procedural;
                request.vertices = vertices;
                request.indices = indices;
                DirectX::XMStoreFloat4x4(
                    &request.world,
                    DirectX::XMMatrixIdentity());
                DirectX::XMStoreFloat4x4(&request.view, view);
                DirectX::XMStoreFloat4x4(&request.projection, projection);
                // 部品の埋込材質を描画要求へ転写する。
                request.baseColor = primitive.baseColor;
                request.roughness = primitive.roughness;
                request.metallic = primitive.metallic;
                request.occlusionStrength = primitive.occlusionStrength;
                request.emissiveFactor = primitive.emissiveFactor;
                // 部品内蔵画像の保持ビュー
                const auto& textures = primitive.embeddedTextures;
                request.albedo = textures.albedo;
                request.normalTexture = textures.normal;
                request.roughnessTexture = textures.roughness;
                request.metallicTexture = textures.metallic;
                request.occlusionTexture = textures.occlusion;
                request.emissiveTexture = textures.emissive;
                request.fallbackTexture =
                    m_graphics->WhiteTextureViewHandle();
                // まとめ描画には反射プローブを追加せずシーンの環境反射を使う。
                CopyPrimitiveLighting(m_graphics->Lighting(), request);
                if (!request.directionalShadow.texture
                    && m_graphics->Shadows().IsValid())
                {
                    request.directionalShadow.texture =
                        m_graphics->Shadows().ViewHandle();
                }
                if (!request.spotShadowTexture
                    && m_graphics->SpotShadows().IsValid())
                {
                    request.spotShadowTexture =
                        m_graphics->SpotShadows().ViewHandle();
                }
                if (!request.pointShadow.texture
                    && m_graphics->PointShadows().IsValid())
                {
                    request.pointShadow.texture =
                        m_graphics->PointShadows().ViewHandle();
                }
                request.instances = instances;
                if (m_graphics->DrawPrimitive(request))
                {
                    drewLevel = true;
                }
            }
            if (drewLevel)
            {
                drawnComponents.insert(
                    drawnComponents.end(),
                    components.begin(),
                    components.end());
            }
        }
        if (drawnComponents.empty())
        {
            return false;
        }
        // まとめ描画に成功した描画器だけ個別描画を省く。
        // 対象のモデル描画器
        for (auto* const component : drawnComponents)
        {
            component->m_instancedThisPass = true;
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
        if (m_graphics == nullptr
            || m_graphics->ActiveRenderingApi()
                != RenderingApi::DirectX11)
        {
            m_commonLitResources.reset();
            return;
        }
        // 作成中の共通照明描画資源
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

        // 従来形式のモデル
        auto& model = *m_model->model;
        // 選択したシェーダーの世代
        std::uint64_t generation{};
        // 再構築時も非対応テセレーションを代替効果へ差し替える。
        // 借用する共有描画効果
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
        // 描画パスの状態復帰を管理
        const MaterialPassScope passScope{ effect, m_context };
        // 借用するモデルメッシュ
        for (const auto& mesh : model.meshes)
        {
            // 借用するメッシュ部品
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

                // 作成中の共通照明用部品
                CommonLitResources::Part commonPart;
                commonPart.mesh = mesh.get();
                commonPart.part = part.get();
                // 借用する描画コンテキスト
                auto* const context =
                    Detail::GraphicsDeviceD3D11Access::Context(
                        *m_graphics);
                try
                {
                    if (effect.IsManifestEffect())
                    {
                        // 部品の頂点宣言でパスのレイアウトを作成する(byteCode: 頂点シェーダー列, layout: 出力レイアウト, label: 診断用のパス名)。
                        const auto createLayout =
                            [this, &part](
                                ID3DBlob* const byteCode,
                                Microsoft::WRL::ComPtr<
                                    ID3D11InputLayout>& layout,
                                const std::string& label)
                            {
                                if (byteCode == nullptr
                                    || !part->vbDecl)
                                {
                                    throw std::runtime_error(
                                        label
                                        + " has no compatible vertex "
                                          "signature.");
                                }
                                // 頂点レイアウトの作成結果
                                const HRESULT result =
                                    CreateInputLayoutWithPositionAlias(
                                        Detail::GraphicsDeviceD3D11Access::
                                            Device(*m_graphics),
                                        part->vbDecl->data(),
                                        static_cast<UINT>(
                                            part->vbDecl->size()),
                                        byteCode,
                                        layout.ReleaseAndGetAddressOf());
                                if (FAILED(result))
                                {
                                    throw std::runtime_error(
                                        label
                                        + " input layout is incompatible.");
                                }
                            };

                        // 色描画パスの数
                        const auto colorPassCount =
                            effect.ColorPassCount();
                        commonPart.forwardInputLayouts.reserve(
                            colorPassCount);
                        // 描画パスまたは画像の番号
                        for (std::size_t index = 0;
                            // 色描画パスの数
                            index < colorPassCount;
                            ++index)
                        {
                            // 対象パスの頂点レイアウト
                            Microsoft::WRL::ComPtr<
                                ID3D11InputLayout> layout;
                            createLayout(
                                effect.ColorPassVertexShaderByteCode(
                                    index),
                                layout,
                                "Material color pass "
                                    + std::to_string(index));
                            commonPart.forwardInputLayouts.push_back(
                                std::move(layout));
                        }

                        // 輪郭描画の種別
                        const auto outlineRole =
                            ShaderPassRole::Outline;
                        // 輪郭描画パスの数
                        const auto outlinePassCount =
                            effect.PassCount(outlineRole);
                        commonPart.outlineInputLayouts.reserve(
                            outlinePassCount);
                        // 描画パスまたは画像の番号
                        for (std::size_t index = 0;
                            // 輪郭描画パスの数
                            index < outlinePassCount;
                            ++index)
                        {
                            effect.SelectPass(outlineRole, index);
                            // 対象パスの頂点レイアウト
                            Microsoft::WRL::ComPtr<
                                ID3D11InputLayout> layout;
                            createLayout(
                                effect.SelectedPassVertexShaderByteCode(
                                    outlineRole),
                                layout,
                                "Material outline pass "
                                    + std::to_string(index));
                            commonPart.outlineInputLayouts.push_back(
                                std::move(layout));
                        }
                        if (outlinePassCount != 0)
                        {
                            effect.SelectPass(outlineRole, 0);
                        }
                        effect.SelectColorPass(0);
                    }
                    else
                    {
                        // 通常と輪郭で共有する頂点配置
                        Microsoft::WRL::ComPtr<ID3D11InputLayout>
                            inputLayout;
                        part->CreateInputLayout(
                            Detail::GraphicsDeviceD3D11Access::Device(
                                *m_graphics),
                            &effect,
                            inputLayout.ReleaseAndGetAddressOf());
                        commonPart.forwardInputLayouts.push_back(
                            inputLayout);
                        if (effect.HasOutline())
                        {
                            // 直接HLSLの輪郭には通常描画と同じ頂点レイアウトを使う。
                            commonPart.outlineInputLayouts.push_back(
                                std::move(inputLayout));
                        }
                    }

                    // 上書きなしの画像を補うため部品の効果から埋込画像を取得する。
                    // 色と法線の画像解除用ビュー
                    ID3D11ShaderResourceView* emptyViews[2]{};
                    context->PSSetShaderResources(
                        0,
                        2,
                        emptyViews);
                    part->effect->Apply(context);
                    // 参照数を加算して取得する画像
                    ID3D11ShaderResourceView*
                        embeddedViews[2]{};
                    context->PSGetShaderResources(
                        0,
                        2,
                        embeddedViews);
                    // 取得した両ビューの参照を先に所有し、片方の取込失敗時にも解放する。
                    // 取得した画像ビューの所有参照
                    std::array<Microsoft::WRL::ComPtr<
                        ID3D11ShaderResourceView>, 2>
                        ownedEmbeddedViews;
                    // 描画パスまたは画像の番号
                    for (std::size_t index{};
                        index < ownedEmbeddedViews.size();
                        ++index)
                    {
                        ownedEmbeddedViews[index].Attach(
                            embeddedViews[index]);
                    }
                    commonPart.embeddedAlbedoTexture =
                        m_graphics->ImportD3D11ShaderResourceView(
                            ownedEmbeddedViews[0].Get());
                    commonPart.embeddedNormalTexture =
                        m_graphics->ImportD3D11ShaderResourceView(
                            ownedEmbeddedViews[1].Get());
                    // 保存された埋込色の位置
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
            // 埋込色画像を持つ部品数を数える(part: 共通照明用のモデル部品)。
            const auto embeddedCount =
                std::ranges::count_if(
                    resources->parts,
                    [](const CommonLitResources::Part& part)
                    {
                        return static_cast<bool>(
                            part.embeddedAlbedoTexture);
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

        // 従来形式のモデル
        auto& model = *m_model->model;
        // 共通照明の描画資源
        auto& resources = *m_commonLitResources;
        if (!resources.boneTransforms.empty())
        {
            model.CopyAbsoluteBoneTransformsTo(
                resources.boneTransforms.size(),
                resources.boneTransforms.data());
        }

        // 借用する共有描画効果
        auto& effect = m_effect != nullptr
            ? *m_effect
            : m_graphics->Lit();
        effect.SetInstancingEnabled(false);
        effect.SetTessellationDrawEnabled(false);
        effect.SelectColorPass(0);
        // 早期終了時にも共有効果の選択状態とHS・DS・GSを解除する。
        // 描画パスの状態復帰を管理
        const MaterialPassScope passScope{ effect, m_context };
        // 深度描画では照明の設定を省く。
        // 深度のみを描くフラグ
        const bool depthOnly =
            m_graphics->IsDepthOnlyPass();
        effect.SetDepthOnlyEnabled(depthOnly);
        if (!depthOnly)
        {
            effect.SetMaterial(m_material);
            if (!m_graphics->TrySetLitEffectLighting(
                    effect,
                    m_graphics->Lighting()))
            {
                return;
            }
            // 所有物の位置に応じて反射プローブを選ぶ。
            // プローブ選択用のワールド変換
            const auto ownerWorldMatrix =
                Owner().WorldMatrix();
            // プローブ選択用のワールド位置
            DirectX::XMFLOAT3 ownerPosition{};
            DirectX::XMStoreFloat3(
                &ownerPosition,
                ownerWorldMatrix.r[3]);
            // 位置で選択した環境反射
            const auto probe = Owner().GetScene()
                .ReflectionProbeEnvironmentAt(ownerPosition);
            // 無効なプローブなら直前に設定した環境反射を維持する。
            static_cast<void>(
                m_graphics->TrySetLitEffectReflectionProbe(
                    effect,
                    probe));
        }

        // 所有物のワールド変換
        const auto ownerWorld = Owner().WorldMatrix();
        // 上書き色による半透明指定
        const bool forceAlpha =
            m_material.BaseColor().w < 0.999f;
        if (depthOnly && forceAlpha)
        {
            // 上書き色が半透明なら深度描画を省く。
            effect.SetDepthOnlyEnabled(false);
            return;
        }
        // 深度プリパスでは不透明かつ深度書込ありの先頭パスに限る。
        if (depthOnly
            && m_graphics->DepthPass()
                == DepthPassKind::Prepass
            && effect.ColorPassRenderState(0).declared
            && (effect.ColorPassRenderState(0).blend
                    != ShaderBlendMode::Opaque
                || !effect.ColorPassRenderState(0).depthWrite))
        {
            effect.SetDepthOnlyEnabled(false);
            return;
        }
        // 輪郭と遮蔽表示は線表示と深度描画では実行しない。
        // 輪郭描画を行うフラグ
        const bool drawOutline =
            !occludedOnly
            && !m_wireframe
            && !depthOnly
            && effect.HasOutline()
            && m_material.CustomParameter(3).x > 0.0f;
        // 遮蔽描画を行うフラグ
        const bool drawOccluded =
            occludedOnly
            && !m_wireframe
            && !depthOnly
            && effect.HasOccludedPass()
            && m_material.CustomParameter(4).w > 0.0f;

        // 上書き画像の保持ビュー
        const auto baseTextureRequest = BuildLitTextureRequest();

        // 半透明部品を描くフラグ
        for (const bool alphaPass : { false, true })
        {
            // 借用するモデルメッシュ
            for (const auto& mesh : model.meshes)
            {
                // 対象の透明度区分に部品があるか調べる(part: 共通照明用のモデル部品)。
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

                // 部品のワールド変換
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

                // 共通照明用のモデル部品
                for (const auto& part : resources.parts)
                {
                    if (part.mesh != mesh.get()
                        || (forceAlpha
                                || part.part->isAlpha)
                            != alphaPass)
                    {
                        continue;
                    }
                    // 色・法線の未指定を内蔵で補い、部品の全描画が戻るまで要求を保持する。
                    // 内蔵画像で補完した保持要求
                    auto partTextureRequest = baseTextureRequest;
                    if (!partTextureRequest.albedo)
                    {
                        partTextureRequest.albedo =
                            part.embeddedAlbedoTexture;
                    }
                    if (!partTextureRequest.normal)
                    {
                        partTextureRequest.normal =
                            part.embeddedNormalTexture;
                    }
                    if (!m_graphics->TrySetLitEffectTextures(
                            effect,
                            partTextureRequest))
                    {
                        // 無効な要求では前の部品の画像設定を再利用せず描画を省く。
                        continue;
                    }
                    if (m_preserveEmbeddedMaterialColor)
                    {
                        // 保持設定が有効なら埋込色を上書き色へ乗算する。
                        // 埋込色を反映する部品材質
                        auto partMaterial = m_material;
                        // 上書き材質のRGBA色
                        const auto& baseColor =
                            m_material.BaseColor();
                        // 部品の埋込RGBA色
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
                        // 対象種別の描画パス数
                        const auto passCount = effect.PassCount(
                            ShaderPassRole::Occluded);
                        // 対象の描画パス番号
                        for (std::size_t passIndex = 0;
                            // 対象種別の描画パス数
                            passIndex < passCount;
                            ++passIndex)
                        {
                            if (part.forwardInputLayouts.empty())
                            {
                                break;
                            }
                            effect.SelectPass(
                                ShaderPassRole::Occluded,
                                passIndex);
                            // 対象パスの宣言描画状態
                            const auto& renderState =
                                effect.SelectedPassRenderState(
                                    ShaderPassRole::Occluded);
                            // 効果適用後に遮蔽用の状態とシェーダーを設定する。
                            part.part->Draw(
                                m_context,
                                &effect,
                                part.forwardInputLayouts.front().Get(),
                                [this,
                                    &effect,
                                    &renderState]()
                                {
                                    if (effect.IsManifestEffect()
                                        && renderState.declared)
                                    {
                                        ApplyShaderRenderState(
                                            renderState);
                                    }
                                    else
                                    {
                                        m_context->OMSetBlendState(
                                            m_states->
                                                NonPremultiplied(),
                                            nullptr,
                                            0xffffffff);
                                        m_context->RSSetState(
                                            m_states->
                                                CullCounterClockwise());
                                    }
                                    // 遮蔽描画は先頭の頂点シェーダーを使い、選択中のPSへ差し替える。
                                    effect.ApplyOccluded(m_context);
                                });
                        }
                    }
                    if (occludedOnly)
                    {
                        continue;
                    }
                    if (drawOutline)
                    {
                        // 輪郭描画の種別
                        const auto outlineRole =
                            ShaderPassRole::Outline;
                        // 対象種別の描画パス数
                        const auto passCount =
                            effect.PassCount(outlineRole);
                        // 対象の描画パス番号
                        for (std::size_t passIndex = 0;
                            // 対象種別の描画パス数
                            passIndex < passCount;
                            ++passIndex)
                        {
                            if (passIndex
                                >= part.outlineInputLayouts.size())
                            {
                                break;
                            }
                            effect.SelectPass(
                                outlineRole,
                                passIndex);
                            // 対象パスの宣言描画状態
                            const auto& renderState =
                                effect.SelectedPassRenderState(
                                    outlineRole);
                            // 効果適用後に輪郭用の状態とシェーダーを設定する。
                            part.part->Draw(
                                m_context,
                                &effect,
                                part.outlineInputLayouts[
                                    passIndex].Get(),
                                [this,
                                    &effect,
                                    &renderState]()
                                {
                                    if (effect.IsManifestEffect()
                                        && renderState.declared)
                                    {
                                        ApplyShaderRenderState(
                                            renderState);
                                    }
                                    else
                                    {
                                        m_context->OMSetBlendState(
                                            m_states->
                                                NonPremultiplied(),
                                            nullptr,
                                            0xffffffff);
                                        m_context->
                                            OMSetDepthStencilState(
                                                m_states->DepthRead(),
                                                0);
                                        m_context->RSSetState(
                                            m_states->CullClockwise());
                                    }
                                    effect.ApplyOutline(m_context);
                                });
                        }
                    }

                    // 深度描画は先頭パスのみ、通常の色描画は宣言順の全パスを実行する。
                    // 実行する色描画パス数
                    const auto colorPassCount = depthOnly
                        ? std::size_t{ 1 }
                        : effect.ColorPassCount();
                    // 対象の描画パス番号
                    for (std::size_t passIndex = 0;
                        // 実行する色描画パス数
                        passIndex < colorPassCount;
                        ++passIndex)
                    {
                        if (passIndex
                            >= part.forwardInputLayouts.size())
                        {
                            break;
                        }
                        effect.SelectColorPass(passIndex);
                        mesh->PrepareForRendering(
                            m_context,
                            *m_states,
                            alphaPass,
                            m_wireframe);
                        // 対象パスの宣言描画状態
                        const auto& renderState =
                            effect.ColorPassRenderState(passIndex);
                        if (renderState.declared)
                        {
                            ApplyShaderRenderState(renderState);
                        }
                        part.part->Draw(
                            m_context,
                            &effect,
                            part.forwardInputLayouts[
                                passIndex].Get());
                    }
                }
            }
        }
        effect.SetDepthOnlyEnabled(false);
    }

    bool ModelRendererComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {
        // まとめ描画後の個別呼出しは描画イベントへ計上しない。
        if (m_instancedThisPass)
        {
            return false;
        }
        description.geometry = Detail::FrameDebugPathLabel(m_modelPath);
        description.triangleCount = TriangleCount(0);
        description.material = Detail::FrameDebugMaterialLabel(
            MaterialAssetPath(),
            m_material.Shader(),
            m_material.AlbedoTexture());
        Detail::AppendFrameDebugItem(
            description.state,
            IsAlphaBlended3D() ? "アルファ合成" : "不透明");
        if (m_wireframe)
        {
            Detail::AppendFrameDebugItem(
                description.state,
                "ワイヤーフレーム");
        }
        return true;
    }
}
