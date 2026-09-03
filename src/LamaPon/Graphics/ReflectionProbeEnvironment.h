#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

namespace LamaPon
{
    // 1つのオブジェクトへ適用するリフレクションプローブの内容。
    //
    // プローブは最大2個まで混ぜます。境界をまたいだ瞬間に映り込みが
    // 飛ぶのを防ぐためで、weightが0なら1個だけを使う従来の挙動です。
    //
    // MeshRendererとModelRendererが同じ手順を踏むので、組み立ては
    // Scene側（ReflectionProbeEnvironmentAt）に置いています。
    struct ReflectionProbeEnvironment final
    {
        // 主のプローブ。specularがnullptrなら「プローブ無し」で、
        // シーン共通のSky IBLがそのまま使われます。
        ID3D11ShaderResourceView* specular{};
        ID3D11ShaderResourceView* irradiance{};
        float specularMaximumMip{};
        float intensity{ 1.0f };
        // ボックス射影の箱（中心＝プローブの位置）。半径が3軸すべて
        // 正のときだけ有効になります。
        DirectX::XMFLOAT3 boxCenter{};
        DirectX::XMFLOAT3 boxExtents{};

        // 混ぜる相手。secondarySpecularがnullptrなら混ぜません。
        ID3D11ShaderResourceView* secondarySpecular{};
        ID3D11ShaderResourceView* secondaryIrradiance{};
        float secondarySpecularMaximumMip{};
        DirectX::XMFLOAT3 secondaryBoxCenter{};
        DirectX::XMFLOAT3 secondaryBoxExtents{};
        // 相手side の比率（0＝主だけ、0.5＝半々、1＝相手だけ）。
        float secondaryWeight{};

        // 2枚そろっていないと使えません。片方だけ渡すと、Shaderが
        // 空のキューブを読んで拡散か反射が真っ黒になります。
        [[nodiscard]] bool IsValid() const noexcept
        {
            return specular != nullptr
                && irradiance != nullptr;
        }
        [[nodiscard]] bool IsBlended() const noexcept
        {
            return secondarySpecular != nullptr
                && secondaryIrradiance != nullptr
                && secondaryWeight > 0.0f;
        }
    };
}
