#pragma once

#include <string>

namespace LamaPon
{
    struct SimpleMaterialShaderGraph final
    {
        // 基準色の乗算ノードを含めるか
        bool tint{ true };
        // 自己発光ノードを含めるか
        bool emission{};
        // 輪郭発光ノードを含めるか
        bool rimLight{};
        // UVの時間変化を含めるか
        bool uvScroll{};
        // alphaマスクtextureを使うか
        bool maskTexture{};
        // alphaによる描画破棄を使うか
        bool alphaClip{ true };
    };

    // 選択したノードから固定register配置のMaterial Shaderを作る(graph: 有効にするノードの設定)。
    [[nodiscard]] std::string GenerateSimpleMaterialShader(
        const SimpleMaterialShaderGraph& graph);
}
