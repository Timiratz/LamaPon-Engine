#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <vector>

namespace LamaPon
{
    class SpriteRendererComponent;

    // 同じGameObjectのSprite Rendererの格子頂点を描画の直前に変形する部品の基底です。
    // 全ての更新と揺れが終わった姿勢で呼ばれ、複数あれば部品の並び順に重ねて適用します。
    class SpriteMeshDeformer : public Component
    {
    public:
        // 格子頂点のローカル位置を変形します(sprite: 描画するSprite Renderer, positions: 上の行から並ぶ頂点位置の入出力)。
        // 頂点数がspriteの格子と一致する場合だけ呼ばれ、変形できなければpositionsを変えずに戻します。
        virtual void DeformSpriteMesh(
            const SpriteRendererComponent& sprite,
            std::vector<DirectX::XMFLOAT2>& positions) = 0;
    };
}
