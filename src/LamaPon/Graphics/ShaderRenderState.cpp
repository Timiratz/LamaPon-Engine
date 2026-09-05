#include "LamaPon/Graphics/ShaderRenderState.h"

#include <nlohmann/json.hpp>

#include <d3d11.h>

#include <string>

namespace
{
    constexpr std::string_view BlockName = "LAMAPON_RENDER_STATE";

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
        ShaderRenderState state;
        const auto blockPosition =
            shaderSource.find(BlockName);
        if (blockPosition == std::string_view::npos)
        {
            return state;
        }

        // ブロック名の後ろの最初の '{' 〜 対応する '}' をJSONとして
        // 読みます（コメント内に書かれる前提です）。
        const auto objectStart =
            shaderSource.find('{', blockPosition);
        if (objectStart == std::string_view::npos)
        {
            return state;
        }

        std::size_t depth = 0;
        std::size_t objectEnd = std::string_view::npos;
        bool inString = false;
        for (std::size_t index = objectStart;
            index < shaderSource.size();
            ++index)
        {
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
        // 半透明で指定がない場合は、重ね合わせを保つため深度へ
        // 書き込まない設定を使います。
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
        Microsoft::WRL::ComPtr<ID3D11BlendState> state;
        if (device == nullptr)
        {
            return state;
        }
        D3D11_BLEND_DESC description{};
        auto& target = description.RenderTarget[0];
        target.BlendEnable = TRUE;
        // RGBは純加算（発光）。SrcAlpha加重にしないのは、宣言側の
        // シェーダーが強さを自分でRGBへ乗せる約束のため。
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_ONE;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        // アルファは書き込み先を保存する。
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
