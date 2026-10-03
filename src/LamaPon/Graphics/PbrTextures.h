#pragma once

#include <DirectXMath.h>

struct ID3D11ShaderResourceView;

namespace LamaPon
{
    // 一回の描画で借用するPBRマップと係数。
    // ヌルのマップは不在として扱い、粗さはG、金属度はB、遮蔽はRを読む。
    struct PbrTextures final
    {
        // 粗さをGから読む借用SRV
        ID3D11ShaderResourceView* roughness{};
        // 金属度をBから読む借用SRV
        ID3D11ShaderResourceView* metallic{};
        // 遮蔽をRから読む借用SRV
        ID3D11ShaderResourceView* occlusion{};
        // 発光色を読む借用SRV
        ID3D11ShaderResourceView* emissive{};
        // 遮蔽マップの適用強度
        float occlusionStrength{ 1.0f };
        // 強度込みの発光色、黒は発光なし
        DirectX::XMFLOAT3 emissiveFactor{};
    };
}
