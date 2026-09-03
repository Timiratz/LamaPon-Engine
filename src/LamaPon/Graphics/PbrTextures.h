#pragma once

#include <DirectXMath.h>

struct ID3D11ShaderResourceView;

namespace LamaPon
{
    // 1回の描画で使うPBRマップ一式（粗さ・金属度・遮蔽・発光）。
    // nullptrの枠は白テクスチャになり、シェーダー側では「マップなし」
    // として扱われます。読むチャンネルはglTFのmetallicRoughness規約に
    // 合わせて粗さ=G、金属度=B、遮蔽=Rです。グレースケール画像なら
    // どのチャンネルでも同じ値なので、FBXのように粗さと金属度が
    // 別画像でもそのまま使えます。
    //
    // LitEffectの入れ子ではなく名前空間直下に置いているのは、
    // SkeletalModel.hがLitEffect.hを丸ごと取り込まずに済むように
    // するためです（入れ子型はポインタでも完全な定義が必要）。
    struct PbrTextures final
    {
        ID3D11ShaderResourceView* roughness{};
        ID3D11ShaderResourceView* metallic{};
        ID3D11ShaderResourceView* occlusion{};
        ID3D11ShaderResourceView* emissive{};
        float occlusionStrength{ 1.0f };
        // 発光色（強度を掛け込んだ値）。既定の黒＝発光なしです。
        DirectX::XMFLOAT3 emissiveFactor{};
    };
}
