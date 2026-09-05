#pragma once

#include <string_view>

#include <wrl/client.h>

struct ID3D11BlendState;
struct ID3D11Device;

namespace LamaPon
{
    // カスタムShaderが宣言する描画状態です。半透明や加算合成、
    // 片面カリング、深度書き込みの有無をShader側から指定できます。
    //
    //   /* LAMAPON_RENDER_STATE
    //   { "blend": "additive", "cull": "none", "depthWrite": false }
    //   */
    //
    // パラメーターの名前付け（LAMAPON_PROPERTIES）はエディターだけが
    // 読みますが、描画状態はゲーム実行時にも必要なので、こちらは
    // ランタイム側（Shaderのコンパイル時）で読み取ります。
    enum class ShaderBlendMode
    {
        // 半透明なし（既定）。
        Opaque,
        // アルファ合成（ガラス、フェード）。
        Alpha,
        // 加算合成（発光、エフェクト）。
        Additive,
        // 乗算済みアルファ。
        Premultiplied
    };

    enum class ShaderCullMode
    {
        // 裏面を描かない（既定）。
        Back,
        // 表面を描かない（内側から見せる箱など）。
        Front,
        // 両面を描く（板ポリゴンの草、旗）。
        None
    };

    struct ShaderRenderState final
    {
        ShaderBlendMode blend{ ShaderBlendMode::Opaque };
        ShaderCullMode cull{ ShaderCullMode::Back };
        // falseにすると深度バッファへ書き込みません（半透明の
        // 重ね合わせで前後の抜けを防ぎたいとき）。
        bool depthWrite{ true };
        // falseにすると深度テストをしません（常に手前に描く）。
        bool depthTest{ true };
        // 宣言ブロックがあったか。無ければ既定のまま使います。
        bool declared{};
    };

    // HLSLのテキストから宣言を読み取ります。例外は投げません。
    // 宣言が無ければ declared=false の既定値を返します。
    [[nodiscard]] ShaderRenderState ParseShaderRenderState(
        std::string_view shaderSource);

    // 宣言blend:additive用の純加算ブレンド（RGB: One+One）を作ります。
    // DirectXTKのAdditive（SrcAlpha加重・dstA += srcA）と違い、
    // 書き込み先のアルファを一切汚しません（Alpha: Zero+One）。
    // シーンバッファのアルファは後段（被写界深度のCoC等）が意味を
    // 持って読むため、加算描画がアルファを積み上げると後段の
    // フレームが暗くなります。
    // 失敗時はnullptrを返します。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11BlendState>
        CreateAdditiveBlendPreservingAlpha(ID3D11Device* device);
}
