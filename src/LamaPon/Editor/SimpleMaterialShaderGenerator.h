#pragma once

#include <string>

namespace LamaPon
{
    struct SimpleMaterialShaderGraph final
    {
        bool tint{ true };
        bool emission{};
        bool rimLight{};
        bool uvScroll{};
        bool maskTexture{};
        bool alphaClip{ true };
    };

    // レジスタやCustomParametersの配置を利用者に選ばせず、選択した
    // ノードからMaterial Shaderを生成します。
    [[nodiscard]] std::string GenerateSimpleMaterialShader(
        const SimpleMaterialShaderGraph& graph);
}
