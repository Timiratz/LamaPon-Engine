#pragma once

#include "LamaPon/Graphics/PbrTextures.h"
#include "LamaPon/Physics/CollisionTypes.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <limits>
#include <string>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
        class SkinnedDGSLEffect;
        class SkinnedEffect;
    }
}

namespace LamaPon
{
    class LitMaterial;
    class LitEffect;
    struct LightingState;

    enum class SkeletalInterpolation
    {
        Step,
        Linear,
        CubicSpline
    };

    struct SkeletalPoseTransform final
    {
        DirectX::XMFLOAT3 translation{};
        DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct SkeletalVectorKey final
    {
        float time{};
        DirectX::XMFLOAT3 value{};
        DirectX::XMFLOAT3 inTangent{};
        DirectX::XMFLOAT3 outTangent{};
    };

    struct SkeletalQuaternionKey final
    {
        float time{};
        DirectX::XMFLOAT4 value{ 0.0f, 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT4 inTangent{};
        DirectX::XMFLOAT4 outTangent{};
    };

    struct SkeletalVectorChannel final
    {
        std::vector<SkeletalVectorKey> keys;
        SkeletalInterpolation interpolation{
            SkeletalInterpolation::Linear
        };
    };

    struct SkeletalQuaternionChannel final
    {
        std::vector<SkeletalQuaternionKey> keys;
        SkeletalInterpolation interpolation{
            SkeletalInterpolation::Linear
        };
    };

    struct SkeletalNodeTrack final
    {
        std::size_t node{};
        SkeletalVectorChannel translation;
        SkeletalQuaternionChannel rotation;
        SkeletalVectorChannel scale;
    };

    struct SkeletalAnimationClip final
    {
        std::string name;
        float duration{};
        std::vector<SkeletalNodeTrack> tracks;
    };

    struct SkeletalPoseSample final
    {
        const SkeletalAnimationClip* clip{};
        float time{};
        float weight{ 1.0f };
    };

    struct SkeletalNode final
    {
        std::string name;
        std::ptrdiff_t parent{ -1 };
        SkeletalPoseTransform bindPose;
    };

    struct SkeletalSkin final
    {
        std::string name;
        std::vector<std::size_t> joints;
        std::vector<DirectX::XMFLOAT4X4> inverseBindMatrices;
    };

    struct SkeletalPrimitive final
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        // 自動LODは元の頂点バッファを共有し、軽いインデックスだけを
        // 追加します。0=中距離、1=遠距離です。
        std::array<
            Microsoft::WRL::ComPtr<ID3D11Buffer>,
            2> lodIndexBuffers;
        std::array<std::uint32_t, 2> lodIndexCounts{};
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
        mutable Microsoft::WRL::ComPtr<ID3D11InputLayout>
            instancedInputLayout;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> cutoutInputLayout;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
        // モデルが持つ法線マップ（glTFのnormalTexture、FBXの
        // pbr.normal_map）。DirectXTKのSkinnedEffectは法線マップを
        // 扱えないため、LamaPon Litで描くときだけ使われます。
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            normalTexture;
        // PBRマップ。粗さはG、金属度はB、遮蔽はRを読みます
        // （glTFのmetallicRoughness規約に合わせています）。
        // glTFでは粗さと金属度に同じ画像が入ります。
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            roughnessTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            metallicTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            occlusionTexture;
        // 発光マップ。発光色は emissiveFactor と掛け算されます。
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            emissiveTexture;
        std::shared_ptr<DirectX::SkinnedEffect> effect;
        std::shared_ptr<DirectX::SkinnedDGSLEffect> cutoutEffect;
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        float roughness{ 0.5f };
        // 金属度。glTFのmetallicFactor、FBXのpbr.metalness由来です。
        // 既定0は非金属（誘電体）で、従来の見た目と同じです。
        float metallic{};
        // 遮蔽マップの強さ（glTFのocclusionTexture.strength）。
        float occlusionStrength{ 1.0f };
        // 発光色。glTFのemissiveFactor（KHR_materials_emissive_strength
        // があれば掛け込み済み）、FBXのemission_color×emission_factor
        // 由来です。既定の黒は発光なしで、従来の見た目と同じです。
        DirectX::XMFLOAT3 emissiveFactor{};
        std::uint32_t indexCount{};
        std::size_t meshNode{};
        std::ptrdiff_t skin{ -1 };
        Bounds3D localBounds{};
        bool hasLocalBounds{};
        bool alpha{};
        bool textureHasTransparency{};
        bool doubleSided{};
    };

    class SkeletalModel final
    {
    public:
        std::vector<SkeletalNode> nodes;
        std::vector<SkeletalSkin> skins;
        std::vector<SkeletalAnimationClip> animations;
        std::vector<SkeletalPrimitive> primitives;
        Bounds3D localBounds{};
        bool hasLocalBounds{};

        [[nodiscard]] std::size_t SelectAutomaticLod(
            DirectX::FXMMATRIX ownerWorld,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            float quality = 1.0f) const noexcept;
        [[nodiscard]] std::uint64_t TriangleCount(
            std::size_t lodLevel) const noexcept;

        [[nodiscard]] static DirectX::XMMATRIX LocalMatrix(
            const SkeletalPoseTransform& transform) noexcept;
        static void SamplePose(
            const std::vector<SkeletalNode>& nodes,
            const SkeletalAnimationClip* clip,
            float time,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose);
        static void SampleBlendedPose(
            const std::vector<SkeletalNode>& nodes,
            const SkeletalAnimationClip* fromClip,
            float fromTime,
            const SkeletalAnimationClip* toClip,
            float toTime,
            float amount,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose);
        static void SampleWeightedPose(
            const std::vector<SkeletalNode>& nodes,
            const std::vector<SkeletalPoseSample>& samples,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose,
            std::size_t removeRootMotionNode =
                std::numeric_limits<std::size_t>::max());

        void Draw(
            ID3D11DeviceContext* context,
            DirectX::CommonStates& states,
            const LightingState& lighting,
            DirectX::FXMMATRIX ownerWorld,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkeletalAnimationClip* clip,
            float time,
            bool wireframe,
            const LitMaterial* materialOverride = nullptr,
            ID3D11ShaderResourceView* albedoOverride = nullptr,
            ID3D11ShaderResourceView* normalOverride = nullptr,
            // マテリアル上書き時に、モデル自身のPBRマップの代わりに
            // 使う一式（LitMaterialが正になります）。
            const PbrTextures* pbrOverride = nullptr,
            const SkeletalAnimationClip* blendClip = nullptr,
            float blendTime = 0.0f,
            float blendAmount = 0.0f,
            const std::vector<SkeletalPoseSample>*
                weightedSamples = nullptr,
            std::size_t removeRootMotionNode =
                std::numeric_limits<std::size_t>::max(),
            LitEffect* customEffect = nullptr,
            ID3D11InputLayout* customInputLayout = nullptr,
            // 影／深度プリパスからの呼び出し。深度しか要らないので、
            // ピクセルシェーダーを外し、輪郭と遮蔽表示の追加パスも
            // 描きません（どちらも影へ写り込ませるものではありません）。
            bool depthOnly = false,
            // 同じフレームの影・深度・通常描画で共有する計算済み姿勢。
            // nullptrなら従来どおり、この呼び出し内で計算します。
            const std::vector<DirectX::XMFLOAT4X4>*
                globalPoseOverride = nullptr,
            float automaticLodQuality = 1.0f) const;

    private:
        // 宣言blend:additive用の純加算ブレンド（アルファ保存）。
        // 初回のDrawでcontextのデバイスから作る（詳細は
        // ShaderRenderState.hのCreateAdditiveBlendPreservingAlpha）。
        mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
            m_additiveBlendPreservingAlpha;
    };
}
