#include "LamaPon/Graphics/ShaderDiagnostics.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    // Require(condition: 検証条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(
        const bool condition,
        const char* message)
    {
        // 条件違反を検出する
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // Contains(text: 検索対象, needle: 検索語)は部分一致を返す。
    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // Pixel ShaderとTextureを使うSprite用HLSL
    constexpr std::string_view SpriteShader = R"(
// SpriteTexture: Sprite画像
Texture2D SpriteTexture : register(t0);
// SpriteSampler: Sprite画像のSampler
SamplerState SpriteSampler : register(s0);

// PSMain(color: 頂点色, uv: UV座標, position: クリップ座標)はSprite色を出力する。
float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, uv) * color;
}
)";

    // 出力Textureへ書き込むCompute Shader
    constexpr std::string_view ComputeShader = R"(
// Output: Compute結果の出力先
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
// CSMain(id: Dispatch座標)は出力Textureを初期化する。
void CSMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.xy] = float4(1, 0, 0, 1);
}
)";

    // EntryPoint名をコメント内から誤検出しないためのテスト入力
    constexpr std::string_view CommentOnlyMentions = R"(
// Entry points must remain VSMain and PSMain (Shader Model 5.0).
/* GSMain and HSMain (geometry/tessellation) are optional. */
[numthreads(8, 8, 1)]
// id: Dispatch内のスレッド座標
void CSMain(uint3 id : SV_DispatchThreadID) {}
)";

    // 3D Material用VSMainとPSMainを含むHLSL
    constexpr std::string_view MaterialShader = R"(
struct PixelInput {
    // position: クリップ空間の頂点位置
    float4 position : SV_Position;
};
// VSMain(position: 入力頂点座標)はPixelInputへ位置を渡す。
PixelInput VSMain(float3 position : SV_Position)
{
    // output: Pixel Shaderへ渡す頂点出力
    PixelInput output;
    output.position = float4(position, 1);
    return output;
}
// PSMain(input: 補間済みピクセル入力)は単色を出力する。
float4 PSMain(PixelInput input) : SV_Target { return 1; }
)";

    // Tessellationを含む5段階のGraphics Shader
    constexpr std::string_view TessellatedShader = R"(
struct Patch {
    // p: クリップ空間のPatch位置
    float4 p : SV_Position;
};
// VSMain(position: 入力頂点座標)はPatchの初期値を作る。
Patch VSMain(float3 position : SV_Position)
{
    // o: Domain Shaderへ渡すPatch
    Patch o;
    o.p = float4(position, 1);
    return o;
}
// Geometry Shader段階を表すEntryPoint
void GSMain() {}
// Hull Shader段階を表すEntryPoint
void HSMain() {}
// Domain Shader段階を表すEntryPoint
void DSMain() {}
// PSMain(input: Tessellation後のPatch)は単色を出力する。
float4 PSMain(Patch input) : SV_Target { return 1; }
)";
}

// Shader EntryPointと診断位置の説明を検証する
int main()
{
    // テスト失敗を終了コードに変換する
    try
    {
        // Shader種別ごとのEntryPointを検証する。
        {
            // Sprite Shaderから解析したEntryPoint
            const auto sprite =
                LamaPon::ParseShaderEntryPoints(SpriteShader);
            Require(
                sprite.pixel && !sprite.vertex
                    && !sprite.compute,
                "a sprite shader has PSMain only");

            // Compute Shaderから解析したEntryPoint
            const auto compute =
                LamaPon::ParseShaderEntryPoints(ComputeShader);
            Require(
                compute.compute && !compute.pixel
                    && !compute.vertex,
                "a compute shader has CSMain only");

            // Tessellated Shaderから解析したEntryPoint
            const auto tessellated =
                LamaPon::ParseShaderEntryPoints(
                    TessellatedShader);
            Require(
                tessellated.vertex && tessellated.pixel
                    && tessellated.geometry
                    && tessellated.hull && tessellated.domain,
                "a tessellated shader has all five graphics stages");
        }

        // HLSLコメント内のEntryPoint名を誤検出しないこと。
        {
            // コメントを除いて解析したEntryPoint
            const auto parsed =
                LamaPon::ParseShaderEntryPoints(
                    CommentOnlyMentions);
            Require(
                !parsed.vertex && !parsed.pixel && !parsed.geometry
                    && !parsed.hull,
                "names inside comments must not count");
            Require(
                parsed.compute,
                "the real entry point outside comments counts");
        }

        // Sprite Shaderを3D Materialに割り当てた場合の診断
        {
            // 2D Shaderに対する診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSMain': entrypoint not found",
                SpriteShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "2D"),
                "the hint must say the shader looks like a 2D one");
            Require(
                Contains(message, "VSMain"),
                "the hint must name what is required");
            // 行番号の用途のため元のエラー文を保つ
            Require(
                Contains(message, "X3501"),
                "the original compiler message must be kept");
        }

        // Compute ShaderをMaterialへ割り当てた場合の診断
        {
            // Compute Shaderに対する診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSMain': entrypoint not found",
                ComputeShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "Compute Shader"),
                "the hint must recognize a compute shader");
        }

        // HLSL Includeを読み取れない場合の診断
        {
            // Include欠落に対する診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X1507: failed to open source file:"
                " 'LamaPonScreenDepth.hlsli'",
                {},
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "#include"),
                "a missing include must be explained");
        }

        // Skinning専用EntryPointがないMaterial Shaderの診断
        {
            // Skinning EntryPoint不足の診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSSkinnedMain':"
                " entrypoint not found",
                MaterialShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "VSSkinnedMain")
                    && Contains(message, "PSSkinnedMain")
                    && Contains(message, "スキニング"),
                "a skinned-only miss must say which entry points"
                " to add");
        }

        // 必須EntryPointが存在しない場合の診断
        {
            // 必須EntryPoint不足の診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSMain': entrypoint not found",
                ComputeShader,
                LamaPon::ShaderUsage::Material);
            Require(
                !Contains(message, "3Dマテリアルとしては書けて"),
                "the skinned hint must not leak into other"
                " entry point failures");
        }

        // Shader入出力セマンティクス不足の診断
        {
            // Sprite Shaderのセマンティクス診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3506: 'PSMain': function return value"
                " missing semantics",
                SpriteShader,
                LamaPon::ShaderUsage::Sprite);
            Require(
                Contains(message, "SV_Target")
                    && Contains(message, "COLOR0"),
                "missing semantics on a sprite shader must"
                " mention the argument order too");
        }

        // Engine予約名の宣言漏れに対する診断
        {
            // ObjectBuffer宣言漏れの診断文
            const auto message = LamaPon::ExplainShaderError(
                "error X3004: undeclared identifier"
                " 'ViewProjection'",
                MaterialShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "ObjectBuffer")
                    && Contains(message, "b0"),
                "an engine-provided name must point at the"
                " constant buffer declaration");
        }

        // 未知のエラーは原文を保持し、識別子が一致する場合だけヒントを追加する。
        {
            // 未知のコンパイラーエラー
            const std::string original =
                "error X3004: undeclared identifier 'Foo'";
            // 未知エラーに対する診断結果
            const auto message = LamaPon::ExplainShaderError(
                original,
                SpriteShader,
                LamaPon::ShaderUsage::Material);
            Require(
                message == original,
                "an unrecognized error must be passed through");
        }

        // D3DCompiler位置情報から実ファイル・行・列を抽出する。
        {
            // 日本語説明を含む診断位置
            const auto location =
                LamaPon::ParseShaderDiagnosticLocation(
                    "分かりやすい説明\n"
                    "C:\\Game\\assets\\shaders\\broken.hlsl(27,9): "
                    "error X3000: syntax error");
            Require(location.has_value(), "a compiler location is parsed");
            Require(
                location->path.filename() == "broken.hlsl"
                    && location->line == 27
                    && location->column == 9,
                "the compiler file, line and column are preserved");
        }
        {
            // 列番号が省略された診断位置
            const auto location =
                LamaPon::ParseShaderDiagnosticLocation(
                    "included.hlsli(4): error X3004");
            Require(
                location.has_value()
                    && location->line == 4
                    && location->column == 1,
                "a line-only compiler location defaults to column one");
        }
        {
            // Engine出力接頭辞を含む診断位置
            const auto location =
                LamaPon::ParseShaderDiagnosticLocation(
                    "Failed to compile shader "
                    "assets/shaders/broken.hlsl (VSMain): "
                    "assets/shaders/broken.hlsl(12,6): error X3000");
            Require(
                location.has_value()
                    && location->path
                        == "assets/shaders/broken.hlsl"
                    && location->line == 12
                    && location->column == 6,
                "the engine compile prefix is excluded from the path");
        }

        std::cout << "Shader diagnostics tests passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返す
        return 1;
    }
}
