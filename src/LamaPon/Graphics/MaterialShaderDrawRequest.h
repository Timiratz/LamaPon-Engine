#pragma once

// 3D Materialのcustom shaderでPrimitiveを描く、Runtime内部の要求と結果です。
// 公開のPrimitiveDrawRequestのlayoutを変えずに、D3D11のLitEffectと同じ
// Material値・custom texture・描画状態の入力を渡します。SDKには
// installしません。
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace LamaPon::Detail
{
    // VSInstancedMainのslot 1へ渡す、D3D11のInstanceDataと同じ80 bytesです。
    using MaterialShaderInstanceData = PrimitiveInstanceData;

    // Model RendererのglTF／FBXです。D3D11はDirectXTK SkinnedEffectの
    // 頂点シェーダーで骨を変形し、PSSkinnedMainだけを差し替えて描きます。
    struct SkinnedMaterialShaderGeometry final
    {
        // ImportedModelVertexと同じ60 bytesの頂点列です。
        std::span<const std::uint8_t> vertices;
        std::uint32_t vertexStride{};
        std::span<const std::uint32_t> indices;
        // meshの逆行列まで掛けた、行ベクトル規約のbone行列です（72本まで）。
        std::span<const DirectX::XMFLOAT4X4> bones;
        // D3D11のSkeletalModelと同じく、半透明passと両面描画から既定の
        // 合成・深度・カリングを決めます。
        bool alphaPass{};
        bool doubleSided{};
    };

    // Model RendererのCMO／SDKMESH／VBOのpartです。D3D11のModelMesh::
    // PrepareForRenderingと同じく、半透明passと三角形の向きから既定の
    // 合成・深度・カリングを決めます。
    struct DirectXTKModelPartState final
    {
        bool alphaPass{};
        // DirectXTKのModelLoader_PremultipledAlphaで読んだmeshです。
        bool premultipliedAlpha{};
        // DirectXTKのModelLoader_CounterClockwiseで読んだmeshです。
        bool counterClockwise{};
    };

    // D3D11のLitEffect::Apply／ApplyOutline／ApplyOccludedに当たる、
    // CMO／SDKMESH／VBOのpartを描くpassです。
    enum class MaterialShaderPass : std::uint8_t
    {
        Main,
        // VSOutline／PSOutlineで、法線方向へ広げた形の裏面を先に重ねます。
        Outline,
        // VSMain／PSOccludedで、手前の物に隠れた部分だけを描きます。
        Occluded
    };

    // Shaderが持つ追加passです（D3D11のLitEffect::HasOutline／
    // HasOccludedPass）。
    struct MaterialShaderPasses final
    {
        bool outline{};
        bool occluded{};
        bool instanced{};
    };

    struct MaterialShaderDrawRequest final
    {
        // custom値、custom vector、色などの定数を読むMaterialです。
        // 描画の同期呼び出し中だけ参照します。
        const LitMaterial* material{};
        // Shaderとkeywordを読むMaterialです。nullptrならmaterialを使います。
        const LitMaterial* shaderMaterial{};
        // 指定したときはglTF／FBXのスキニング経路で描きます。
        const SkinnedMaterialShaderGeometry* skinned{};
        // 指定したときはCMO／SDKMESH／VBOのpartの描画状態で描きます。
        const DirectXTKModelPartState* directXTKPart{};
        // directXTKPartを描くpassです。追加のpassは、Shaderにその入口が
        // 無ければ何も描きません。
        MaterialShaderPass pass{ MaterialShaderPass::Main };
        // Mesh RendererのPlane／Cubeの4制御点パッチです（D3D11の
        // BuildTessellationPatchesと同じ並び）。テセレーションShaderは
        // これを索引なしで描き、空ならパッチへ分けられない形として扱います。
        std::span<const PrimitiveRenderVertex> tessellationPatches;
        // 空でなければVSInstancedMainとslot 1を使って一度に描きます。
        std::span<const MaterialShaderInstanceData> instances;
        // t7〜t10です。emptyの枠は白になります。
        std::array<GraphicsViewHandle, LitMaterial::CustomTextureCount>
            customTextures{};
        // 深度を無視して非プレマルチプライド透過で重ねる描画です。
        bool worldOverlay{};
        // Componentが明示したcull modeです。Shader宣言より優先します。
        std::optional<ShaderCullMode> cullOverride;
    };

    struct MaterialShaderDrawResult final
    {
        std::uint64_t generation{};
        // compile失敗などの説明です。空なら正常です。
        std::string error;
        // 輪郭／遮蔽表示のpipelineを作れず、そのpassを止めた説明です。
        // 通常の描画は続けます。
        std::string passError;
        // 実際に使ったShaderの描画状態です。
        ShaderRenderState renderState;
        bool drawn{};
        // マゼンタの代替表示で描いたときtrueです。
        bool placeholder{};
    };
}
