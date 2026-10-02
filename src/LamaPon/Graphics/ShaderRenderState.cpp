#include "LamaPon/Graphics/ShaderRenderState.h"

#include <nlohmann/json.hpp>

#include <d3d11.h>

#include <string>

namespace
{
    // 描画状態のJSON識別子
    constexpr std::string_view BlockName = "LAMAPON_RENDER_STATE";

    // 合成方式名を変換し、未知なら不透明を返す(name: 合成方式名)。
    [[nodiscard]] LamaPon::ShaderBlendMode BlendFromName(
        const std::string& name)
    {
        if (name == "alpha")
        {
            return LamaPon::ShaderBlendMode::Alpha;
        }
        if (name == "additive" || name == "add")
        {
            return LamaPon::ShaderBlendMode::Additive;
        }
        if (name == "premultiplied")
        {
            return LamaPon::ShaderBlendMode::Premultiplied;
        }
        return LamaPon::ShaderBlendMode::Opaque;
    }

    // カリング名を変換し、未知なら裏面省略を返す(name: カリング方式名)。
    [[nodiscard]] LamaPon::ShaderCullMode CullFromName(
        const std::string& name)
    {
        if (name == "front")
        {
            return LamaPon::ShaderCullMode::Front;
        }
        if (name == "none" || name == "off")
        {
            return LamaPon::ShaderCullMode::None;
        }
        return LamaPon::ShaderCullMode::Back;
    }
}

namespace LamaPon
{
    ShaderRenderState ParseShaderRenderState(
        const std::string_view shaderSource)
    {
        // 解析結果または作成する状態
        ShaderRenderState state;
        // 宣言識別子の出現位置
        const auto blockPosition =
            shaderSource.find(BlockName);
        if (blockPosition == std::string_view::npos)
        {
            return state;
        }

        // 識別子の後ろで最初のJSONオブジェクトを読み取る。
        // JSONオブジェクトの開始位置
        const auto objectStart =
            shaderSource.find('{', blockPosition);
        if (objectStart == std::string_view::npos)
        {
            return state;
        }

        // JSONの波括弧の深さ
        std::size_t depth = 0;
        // JSONオブジェクトの終了位置
        std::size_t objectEnd = std::string_view::npos;
        // JSON文字列の解析中フラグ
        bool inString = false;
        // ソース内の解析位置
        for (std::size_t index = objectStart;
            index < shaderSource.size();
            ++index)
        {
            // 解析中の一文字
            const char character = shaderSource[index];
            if (inString)
            {
                if (character == '\\')
                {
                    ++index;
                }
                else if (character == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (character == '"')
            {
                inString = true;
            }
            else if (character == '{')
            {
                ++depth;
            }
            else if (character == '}')
            {
                --depth;
                if (depth == 0)
                {
                    objectEnd = index;
                    break;
                }
            }
        }
        if (objectEnd == std::string_view::npos)
        {
            return state;
        }

        // 構文解析したJSON文書
        const auto document = nlohmann::json::parse(
            shaderSource.substr(
                objectStart,
                objectEnd - objectStart + 1),
            nullptr,
            false);
        if (document.is_discarded()
            || !document.is_object())
        {
            return state;
        }

        state.declared = true;
        state.blend = BlendFromName(
            document.value("blend", std::string{}));
        state.cull = CullFromName(
            document.value("cull", std::string{}));
        state.depthWrite =
            document.value("depthWrite", true);
        state.depthTest =
            document.value("depthTest", true);
        // 不透明以外で深度書込の指定がなければ書込を無効にする。
        if (state.blend != ShaderBlendMode::Opaque
            && !document.contains("depthWrite"))
        {
            state.depthWrite = false;
        }
        return state;
    }

    Microsoft::WRL::ComPtr<ID3D11BlendState>
        CreateAdditiveBlendPreservingAlpha(ID3D11Device* device)
    {
        // 解析結果または作成する状態
        Microsoft::WRL::ComPtr<ID3D11BlendState> state;
        if (device == nullptr)
        {
            return state;
        }
        // 純加算の合成状態設定
        D3D11_BLEND_DESC description{};
        // 先頭描画先の合成設定
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        // 合成強度はシェーダー側のRGBへ反映し、合成時はアルファで重み付けしない。
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        // 描画先のアルファを保存する。
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(
                &description,
                state.ReleaseAndGetAddressOf())))
        {
            state.Reset();
        }
        return state;
    }
}
