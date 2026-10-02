#pragma once

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

    // D3D11ではSkinnedEffectの頂点変形と独自のPSSkinnedMainを組み合わせる。
    struct SkinnedMaterialShaderGeometry final
    {
        // 60バイト形式の頂点列
        std::span<const std::uint8_t> vertices;
        // 一頂点のバイト数
        std::uint32_t vertexStride{};
        // 三角形の頂点番号列
        std::span<const std::uint32_t> indices;
        // メッシュ逆行列適用済み骨行列
        // 骨行列は行ベクトル規約とし、72本以下を渡す。
        std::span<const DirectX::XMFLOAT4X4> bones;
        // 半透明の描画パス
        bool alphaPass{};
        // 両面を描画する
        bool doubleSided{};
    };

    // DirectXTKのモデル部分と同じく、透過と三角形の向きで描画状態を決める。
    struct DirectXTKModelPartState final
    {
        // 半透明の描画パス
        bool alphaPass{};
        // 乗算済みアルファの材質
        bool premultipliedAlpha{};
        // 反時計回りの三角形
        bool counterClockwise{};
    };

    // DirectXTKのモデル部分に適用する主描画・輪郭・遮蔽表示のパス。
    enum class MaterialShaderPass : std::uint8_t
    {
        Main,
        // VSOutline／PSOutlineで、法線方向へ広げた形の裏面を先に重ねます。
        Outline,
        // VSMain／PSOccludedで、手前の物に隠れた部分だけを描きます。
        Occluded
    };

    // シェーダーが持つ追加描画パスを示す。
    struct MaterialShaderPasses final
    {
        // 輪郭描画の入口がある
        bool outline{};
        // 遮蔽表示の入口がある
        bool occluded{};
        // 一括描画の入口がある
        bool instanced{};
    };

    struct MaterialShaderDrawRequest final
    {
        // 定数を読む借用材質
        // 材質・形状情報・各列は描画の同期呼出し中だけ借用する。
        const LitMaterial* material{};
        // シェーダーを読む借用材質
        // 未指定ならmaterialからシェーダーとキーワードを読む。
        const LitMaterial* shaderMaterial{};
        // 任意の骨変形用借用情報
        const SkinnedMaterialShaderGeometry* skinned{};
        // 任意のモデル部分の借用状態
        const DirectXTKModelPartState* directXTKPart{};
        // モデル部分に適用するパス
        // シェーダーに追加パスの入口がなければ、そのパスは描画しない。
        MaterialShaderPass pass{ MaterialShaderPass::Main };
        // 四制御点単位のパッチ列
        // テセレーションでは索引を使わず、空の列はパッチ化できない形状として扱う。
        std::span<const PrimitiveRenderVertex> tessellationPatches;
        // 一括描画する個体情報列
        std::span<const MaterialShaderInstanceData> instances;
        // t7〜t10の追加テクスチャ参照
        // 未指定の参照は白テクスチャへ戻す。
        std::array<GraphicsViewHandle, LitMaterial::CustomTextureCount>
            customTextures{};
        // 深度を無視した透過の重ね描き
        bool worldOverlay{};
        // 宣言より優先する任意の面除去
        std::optional<ShaderCullMode> cullOverride;
    };

    struct MaterialShaderDrawResult final
    {
        // 元シェーダーの世代番号
        std::uint64_t generation{};
        // 元シェーダーの失敗説明
        std::string error;
        // 追加パスの無効化理由
        std::string passError;
        // 実際に使った描画状態
        ShaderRenderState renderState;
        // 描画要求を処理できた
        bool drawn{};
        // 代替表示を使った
        bool placeholder{};
    };
}
