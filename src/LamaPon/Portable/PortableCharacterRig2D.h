#pragma once

#include "LamaPon/LamaPon.h"
#include "LamaPon/Web/WebMath.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    // Portable版の2Dキャラクター部品の読み込み・更新・描画をSceneから呼ぶ窓口です。
    class CharacterRig2DRuntime final
    {
    public:
        // シーンJSONの部品を復元し、2Dキャラクター部品ならtrueを返します(object: 追加先, type: 部品の型名, component: 部品のJSON)。
        static bool LoadComponent(
            GameObject& object,
            const std::string& type,
            const nlohmann::json& component);
        // SpriteRendererのJSONからメッシュ分割を復元します(sprite: 対象, component: 部品のJSON)。
        static void LoadSpriteMesh(
            SpriteRendererComponent& sprite,
            const nlohmann::json& component);
        // 保存時のIDで書かれたボーン参照を生成後のIDへ置き換えます(bySourceId: 保存時のIDと生成した物体の対応)。
        static void ResolveReferences(
            const std::unordered_map<std::int64_t, GameObject*>& bySourceId);
        // 全物体の2Dキャラクター部品を進めます(objects: シーンの物体, deltaTime: 経過秒数)。
        // 通常の部品の更新が終わった後に呼び、キーフォーム・瞬き・揺れ物の順に適用します。
        static void Update(
            const std::vector<std::unique_ptr<GameObject>>& objects,
            float deltaTime);
        // メッシュで描くSpriteを三角形のDOMで描き、描いた場合はtrueを返します(object: 対象, sprite: Sprite Renderer, model: ワールド行列, texturePath: 仮想パス)。
        static bool RenderSpriteMesh(
            GameObject& object,
            SpriteRendererComponent& sprite,
            const Web::Mat4& model,
            const std::string& texturePath);

    private:
        // 祖先の揺れ物を先に解いてから、このフレームの揺れを一度だけ計算して適用します(sway: 解く揺れ物)。
        static void SolveSway(Sway2DComponent& sway);
        // 先端の点を経過秒数だけ進めます(sway: 揺れ物, pivot: ワールド回転中心, restTip: ワールド静止先端, deltaTime: 経過秒数)。
        static void SimulateSway(
            Sway2DComponent& sway,
            DirectX::XMFLOAT2 pivot,
            DirectX::XMFLOAT2 restTip,
            float deltaTime);
    };
}
