#pragma once

#include "LamaPon/Graphics/LitTextureRequest.h"
#include "LamaPon/Graphics/LitMaterial.h"
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
    class GraphicsDevice;
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
        // 親を基準にした移動
        DirectX::XMFLOAT3 translation{};
        // 親を基準にした回転四元数
        DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 親を基準にした各軸の倍率
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct SkeletalVectorKey final
    {
        // キー時刻（秒）
        float time{};
        // 移動・拡縮のキー値
        DirectX::XMFLOAT3 value{};
        // 直前からの補間接線
        DirectX::XMFLOAT3 inTangent{};
        // 直後への補間接線
        DirectX::XMFLOAT3 outTangent{};
    };

    struct SkeletalQuaternionKey final
    {
        // キー時刻（秒）
        float time{};
        // 回転のキー四元数
        DirectX::XMFLOAT4 value{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 直前からの四元数接線
        DirectX::XMFLOAT4 inTangent{};
        // 直後への四元数接線
        DirectX::XMFLOAT4 outTangent{};
    };

    struct SkeletalVectorChannel final
    {
        // 時刻順の移動・拡縮キー
        std::vector<SkeletalVectorKey> keys;
        // キー間の補間方式
        SkeletalInterpolation interpolation{
            SkeletalInterpolation::Linear
        };
    };

    struct SkeletalQuaternionChannel final
    {
        // 時刻順の回転キー
        std::vector<SkeletalQuaternionKey> keys;
        // キー間の補間方式
        SkeletalInterpolation interpolation{
            SkeletalInterpolation::Linear
        };
    };

    struct SkeletalNodeTrack final
    {
        // 対象ノードの番号
        std::size_t node{};
        // 移動の時間変化
        SkeletalVectorChannel translation;
        // 回転の時間変化
        SkeletalQuaternionChannel rotation;
        // 拡縮の時間変化
        SkeletalVectorChannel scale;
    };

    struct SkeletalAnimationClip final
    {
        // アニメーションの識別名
        std::string name;
        // 再生区間の秒数
        float duration{};
        // ノード別の時間変化
        std::vector<SkeletalNodeTrack> tracks;
    };

    struct SkeletalPoseSample final
    {
        // 借用する再生クリップ
        const SkeletalAnimationClip* clip{};
        // 採取する再生時刻（秒）
        float time{};
        // 混合する有限な重み
        float weight{ 1.0f };
    };

    struct SkeletalNode final
    {
        // ノードの識別名
        std::string name;
        // 親ノード番号で-1は根
        std::ptrdiff_t parent{ -1 };
        // 親を基準にした初期姿勢
        SkeletalPoseTransform bindPose;
    };

    struct SkeletalSkin final
    {
        // スキンの識別名
        std::string name;
        // 骨を参照するノード番号
        std::vector<std::size_t> joints;
        // 骨の初期姿勢の逆行列
        std::vector<DirectX::XMFLOAT4X4> inverseBindMatrices;
    };

    // CPU頂点はposition/normal/tangent/color/uv/blend indices/weights順で、他APIはこの保存データから資源を作ります。
    struct SkeletalPrimitive final
    {

        // 他APIでも使うCPU頂点列
        std::vector<std::uint8_t> cpuVertexData;
        // CPU頂点の間隔（バイト）
        std::uint32_t cpuVertexStride{};
        // 通常描画の頂点番号列
        std::vector<std::uint32_t> cpuIndices;
        // 中・遠距離LODの頂点番号列
        std::array<std::vector<std::uint32_t>, 2> cpuLodIndices;
        // D3D11の頂点資源
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        // D3D11の頂点番号資源
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;

        // 中・遠距離LODの番号資源
        std::array<
            Microsoft::WRL::ComPtr<ID3D11Buffer>,
            2> lodIndexBuffers;
        // 中・遠距離LODの番号数
        std::array<std::uint32_t, 2> lodIndexCounts{};
        // DirectXTKの頂点形式
        Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
        // 複数描画用の頂点形式
        mutable Microsoft::WRL::ComPtr<ID3D11InputLayout>
            instancedInputLayout;
        // 切り抜き描画の頂点形式
        Microsoft::WRL::ComPtr<ID3D11InputLayout> cutoutInputLayout;
        // 基本色のSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;

        // 照明用の法線マップSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            normalTexture;

        // G成分の粗さマップSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            roughnessTexture;
        // B成分の金属度マップSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            metallicTexture;
        // R成分の遮蔽マップSRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            occlusionTexture;

        // 発光マップのRGB SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            emissiveTexture;

        // 内蔵画像のAPI共通入力
        // 旧SRVと共通入力は同じ画像を保持し、直接Importerで構築したモデルでは描画時に共通入力を補完します。
        mutable LitTextureRequest embeddedTextures;
        // DirectXTKの骨変形照明
        std::shared_ptr<DirectX::SkinnedEffect> effect;
        // 骨変形とアルファ切り抜き
        std::shared_ptr<DirectX::SkinnedDGSLEffect> cutoutEffect;
        // RGBAの基本色係数
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 表面の粗さ
        float roughness{ 0.5f };

        // 表面の金属度
        float metallic{};
        // 遮蔽マップの強度
        float occlusionStrength{ 1.0f };

        // 発光画像に掛けるRGB色
        DirectX::XMFLOAT3 emissiveFactor{};
        // 通常描画の頂点番号数
        std::uint32_t indexCount{};
        // メッシュの姿勢ノード番号
        std::size_t meshNode{};
        // スキン番号で-1は骨なし
        std::ptrdiff_t skin{ -1 };
        // メッシュのローカル境界
        Bounds3D localBounds{};
        // ローカル境界の有無
        bool hasLocalBounds{};
        // 半透明の描画指定
        bool alpha{};
        // 基本色画像に透明部分あり
        bool textureHasTransparency{};
        // 両面を描く指定
        bool doubleSided{};
    };

    class SkeletalModel final
    {
    public:
        // モデルのノード階層
        std::vector<SkeletalNode> nodes;
        // モデルのスキン一覧
        std::vector<SkeletalSkin> skins;
        // モデルの再生クリップ一覧
        std::vector<SkeletalAnimationClip> animations;
        // モデルのメッシュ一覧
        std::vector<SkeletalPrimitive> primitives;
        // モデル全体のローカル境界
        Bounds3D localBounds{};
        // 全体のローカル境界の有無
        bool hasLocalBounds{};

        // 投影された境界の大きさからLODを選びます(ownerWorld: 所有者の世界行列, view: ビュー行列, projection: 射影行列, quality: 0.25～2へ補正する品質)。
        [[nodiscard]] std::size_t SelectAutomaticLod(
            DirectX::FXMMATRIX ownerWorld,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            float quality = 1.0f) const noexcept;
        // 指定LODの三角形数を返します(lodLevel: 0=元、1=中、2=遠距離)。
        [[nodiscard]] std::uint64_t TriangleCount(
            std::size_t lodLevel) const noexcept;

        // 拡縮・回転・移動の順で行列を作ります(transform: 親を基準にした姿勢)。
        [[nodiscard]] static DirectX::XMMATRIX LocalMatrix(
            const SkeletalPoseTransform& transform) noexcept;
        // 単一クリップの局所・全体姿勢を採取します(nodes: ノード階層と初期姿勢, clip: 再生クリップで空なら初期姿勢, time: 採取する秒数, localPose: 局所姿勢の出力, globalPose: モデル内の全体行列出力)。
        // 親の循環・範囲外は例外になり、キー範囲外の時刻は端の値に制限します。
        static void SamplePose(
            const std::vector<SkeletalNode>& nodes,
            const SkeletalAnimationClip* clip,
            float time,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose);
        // 2つの局所姿勢を混ぜて全体行列を作ります(nodes: ノード階層と初期姿勢, fromClip: 元クリップで空なら初期姿勢, fromTime: 元の採取秒数, toClip: 合成先で空なら初期姿勢, toTime: 合成先の採取秒数, amount: 0～1へ補正する合成率, localPose: 局所姿勢の出力, globalPose: モデル内の全体行列出力)。
        static void SampleBlendedPose(
            const std::vector<SkeletalNode>& nodes,
            const SkeletalAnimationClip* fromClip,
            float fromTime,
            const SkeletalAnimationClip* toClip,
            float toTime,
            float amount,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose);
        // 重み付きの姿勢を順に混ぜます(nodes: ノード階層と初期姿勢, samples: クリップ・秒数・有限の重み, localPose: 局所姿勢の出力, globalPose: モデル内の全体行列出力, removeRootMotionNode: 初期姿勢へ戻すノード)。
        // 重み0以下の入力を除き、全て除けば初期姿勢で、指定ノードの移動と回転を初期値へ戻します。
        static void SampleWeightedPose(
            const std::vector<SkeletalNode>& nodes,
            const std::vector<SkeletalPoseSample>& samples,
            std::vector<SkeletalPoseTransform>& localPose,
            std::vector<DirectX::XMFLOAT4X4>& globalPose,
            std::size_t removeRootMotionNode =
                std::numeric_limits<std::size_t>::max());

        // D3D11でモデルを描きます(graphics: 描画デバイス, lighting: 照明入力, ownerWorld: 所有者の世界行列, view: ビュー行列, projection: 射影行列, clip: 再生クリップ, time: 再生秒数, wireframe: 辺だけの描画, materialOverride: 上書き材質, textureOverride: 上書き画像入力, blendClip: 合成先クリップ, blendTime: 合成先の再生秒数, blendAmount: 合成率, weightedSamples: 優先する重み付き姿勢群, removeRootMotionNode: 初期姿勢へ戻すノード, customEffect: 自作シェーダー, customInputLayout: 互換用の未使用レイアウト, depthOnly: 深度専用描画, globalPoseOverride: 計算済みの全体行列群, automaticLodQuality: 自動LODの品質, customColorInputLayouts: 自作の色パス別頂点形式, customOutlineInputLayouts: 自作の輪郭パス別頂点形式, customTextureViews: 追加画像の借用SRV列, staticManifestEffect: 骨なし素材のシェーダー, staticManifestColorInputLayouts: 骨なしの色パス別頂点形式, staticManifestOutlineInputLayouts: 骨なしの輪郭パス別頂点形式, depthPrepass: 主描画と一致する深度描画)。
        // textureOverrideは空の基本色・法線を内蔵画像から継承し、空のPBRは明示的なマップなしとして扱います。
        // 深度描画は輪郭・遮蔽表示を省き、PSも切り抜きに必要な場合だけ使用します。
        // globalPoseOverrideが空なら姿勢を採取し、指定時は影・深度・通常描画で共有します。
        // Manifestの頂点形式はVS別のJSON順で渡し、直接HLSLでは空を指定してDirectXTKの頂点形式を使います。
        // staticManifest系は骨なしのforward素材用で、深度プリパスは非Opaqueや深度を書かない素材を個別に除外します。
        void Draw(
            GraphicsDevice& graphics,
            const LightingState& lighting,
            DirectX::FXMMATRIX ownerWorld,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkeletalAnimationClip* clip,
            float time,
            bool wireframe,
            const LitMaterial* materialOverride = nullptr,
            const LitTextureRequest* textureOverride = nullptr,
            const SkeletalAnimationClip* blendClip = nullptr,
            float blendTime = 0.0f,
            float blendAmount = 0.0f,
            const std::vector<SkeletalPoseSample>*
                weightedSamples = nullptr,
            std::size_t removeRootMotionNode =
                std::numeric_limits<std::size_t>::max(),
            LitEffect* customEffect = nullptr,
            ID3D11InputLayout* customInputLayout = nullptr,
            bool depthOnly = false,
            const std::vector<DirectX::XMFLOAT4X4>*
                globalPoseOverride = nullptr,
            float automaticLodQuality = 1.0f,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* customColorInputLayouts = nullptr,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* customOutlineInputLayouts = nullptr,
            const std::array<
                ID3D11ShaderResourceView*,
                LitMaterial::CustomTextureCount>*
                customTextureViews = nullptr,
            LitEffect* staticManifestEffect = nullptr,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>*
                staticManifestColorInputLayouts = nullptr,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>*
                staticManifestOutlineInputLayouts = nullptr,
            bool depthPrepass = false)
            const;

    private:
        struct TextureInputs;
        // 素材・姿勢・LODを選んで描きます(context: 即時命令のコンテキスト, states: 共通の描画状態, lighting: 照明入力, ownerWorld: 所有者の世界行列, view: ビュー行列, projection: 射影行列, clip: 再生クリップ, time: 再生秒数, wireframe: 辺だけの描画, materialOverride: 上書き材質, textures: 共通または互換用の画像入力, blendClip: 合成先クリップ, blendTime: 合成先の再生秒数, blendAmount: 合成率, weightedSamples: 優先する重み付き姿勢群, removeRootMotionNode: 初期姿勢へ戻すノード, customEffect: 自作シェーダー, customInputLayout: 互換用の未使用レイアウト, depthOnly: 深度専用描画, globalPoseOverride: 計算済みの全体行列群, automaticLodQuality: 自動LODの品質, customColorInputLayouts: 自作の色パス別頂点形式, customOutlineInputLayouts: 自作の輪郭パス別頂点形式, customTextureViews: 追加画像の借用SRV列, staticManifestEffect: 骨なし素材のシェーダー, staticManifestColorInputLayouts: 骨なしの色パス別頂点形式, staticManifestOutlineInputLayouts: 骨なしの輪郭パス別頂点形式, depthPrepass: 主描画と一致する深度描画)。
        void DrawD3D11(
            ID3D11DeviceContext* context,
            DirectX::CommonStates& states,
            const LightingState& lighting,
            DirectX::FXMMATRIX ownerWorld,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkeletalAnimationClip* clip,
            float time,
            bool wireframe,
            const LitMaterial* materialOverride,
            const TextureInputs& textures,
            const SkeletalAnimationClip* blendClip,
            float blendTime,
            float blendAmount,
            const std::vector<SkeletalPoseSample>* weightedSamples,
            std::size_t removeRootMotionNode,
            LitEffect* customEffect,
            ID3D11InputLayout* customInputLayout,
            bool depthOnly,
            const std::vector<DirectX::XMFLOAT4X4>*
                globalPoseOverride,
            float automaticLodQuality,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* customColorInputLayouts,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* customOutlineInputLayouts,
            const std::array<
                ID3D11ShaderResourceView*,
                LitMaterial::CustomTextureCount>* customTextureViews,
            LitEffect* staticManifestEffect,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* staticManifestColorInputLayouts,
            const std::vector<Microsoft::WRL::ComPtr<
                ID3D11InputLayout>>* staticManifestOutlineInputLayouts,
            bool depthPrepass) const;

        // 互換用のD3D11描画入口です(context: 即時命令のコンテキスト, states: 共通の描画状態, lighting: 照明入力, ownerWorld: 所有者の世界行列, view: ビュー行列, projection: 射影行列, clip: 再生クリップ, time: 再生秒数, wireframe: 辺だけの描画, materialOverride: 上書き材質, albedoOverride: 上書き基本色の借用SRV, normalOverride: 上書き法線の借用SRV, pbrOverride: 上書きPBRの借用入力, blendClip: 合成先クリップ, blendTime: 合成先の再生秒数, blendAmount: 合成率, weightedSamples: 優先する重み付き姿勢群, removeRootMotionNode: 初期姿勢へ戻すノード, customEffect: 自作シェーダー, customInputLayout: 互換用の未使用レイアウト, depthOnly: 深度専用描画, globalPoseOverride: 計算済みの全体行列群, automaticLodQuality: 自動LODの品質)。
        // 旧Game ModuleへのAPI不一致案内のため、公開していた署名のシンボルを維持します。
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
            const LitMaterial* materialOverride,
            ID3D11ShaderResourceView* albedoOverride,
            ID3D11ShaderResourceView* normalOverride,
            const PbrTextures* pbrOverride,
            const SkeletalAnimationClip* blendClip,
            float blendTime,
            float blendAmount,
            const std::vector<SkeletalPoseSample>* weightedSamples,
            std::size_t removeRootMotionNode,
            LitEffect* customEffect,
            ID3D11InputLayout* customInputLayout,
            bool depthOnly,
            const std::vector<DirectX::XMFLOAT4X4>*
                globalPoseOverride,
            float automaticLodQuality) const;

        // 初回描画で作るアルファ保持加算状態
        mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
            m_additiveBlendPreservingAlpha;
    };
}
