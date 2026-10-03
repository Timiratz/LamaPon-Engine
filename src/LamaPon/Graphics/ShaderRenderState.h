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
        // 色とアルファの合成方式
        ShaderBlendMode blend{ ShaderBlendMode::Opaque };
        // 描画を省く面の指定
        ShaderCullMode cull{ ShaderCullMode::Back };
        // 深度書込フラグ
        bool depthWrite{ true };
        // 深度比較フラグ
        bool depthTest{ true };
        // 有効なJSON宣言の有無
        bool declared{};
    };

    // 描画状態のJSONを読み、未宣言や構文不正なら既定値を返す(shaderSource: HLSLソース文字列)。
    // 既知キーの値が想定型と異なる場合はJSONの型例外を送出する。
    [[nodiscard]] ShaderRenderState ParseShaderRenderState(
        std::string_view shaderSource);

    // RGBだけを純加算する描画状態を作り、失敗時は空を返す(device: 借用するD3D11機器)。
    // 後処理がシーンのアルファを読むため、加算描画では保存する。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11BlendState>
        CreateAdditiveBlendPreservingAlpha(ID3D11Device* device);
}
