#include "LamaPon/Graphics/ShaderDiagnostics.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool Contains(
        const std::string& text,
        const std::string_view needle)
    {
        return text.find(needle) != std::string::npos;
    }

    constexpr std::string_view SpriteShader = R"(
Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

float4 PSMain(
    float4 color : COLOR0,
    float2 uv : TEXCOORD0,
    float4 position : SV_Position) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, uv) * color;
}
)";

    constexpr std::string_view ComputeShader = R"(
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.xy] = float4(1, 0, 0, 1);
}
)";

    // 雛形にある説明文。コメントを取り除かないと、ここに書かれた
    // 「VSMain and PSMain (...)」を入口として数えてしまいます。
    constexpr std::string_view CommentOnlyMentions = R"(
// Entry points must remain VSMain and PSMain (Shader Model 5.0).
/* HSMain (tessellation) is optional. */
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {}
)";

    // 3D用としては正しく書けている（が、スキニング用の入口は無い）。
    constexpr std::string_view MaterialShader = R"(
struct PixelInput { float4 position : SV_Position; };
PixelInput VSMain(float3 position : SV_Position)
{
    PixelInput output;
    output.position = float4(position, 1);
    return output;
}
float4 PSMain(PixelInput input) : SV_Target { return 1; }
)";

    constexpr std::string_view TessellatedShader = R"(
struct Patch { float4 p : SV_Position; };
Patch VSMain(float3 position : SV_Position) { Patch o; o.p = float4(position, 1); return o; }
void HSMain() {}
void DSMain() {}
float4 PSMain(Patch input) : SV_Target { return 1; }
)";
}

int main()
{
    try
    {
        // 入口の読み取り。
        {
            const auto sprite =
                LamaPon::ParseShaderEntryPoints(SpriteShader);
            Require(
                sprite.pixel && !sprite.vertex
                    && !sprite.compute,
                "a sprite shader has PSMain only");

            const auto compute =
                LamaPon::ParseShaderEntryPoints(ComputeShader);
            Require(
                compute.compute && !compute.pixel
                    && !compute.vertex,
                "a compute shader has CSMain only");

            const auto tessellated =
                LamaPon::ParseShaderEntryPoints(
                    TessellatedShader);
            Require(
                tessellated.vertex && tessellated.pixel
                    && tessellated.hull && tessellated.domain,
                "a tessellated shader has all four");
        }

        // コメントの中の名前を数えないこと。数えると、雛形の説明文
        // だけで「3Dマテリアル用」と誤って案内してしまいます。
        {
            const auto parsed =
                LamaPon::ParseShaderEntryPoints(
                    CommentOnlyMentions);
            Require(
                !parsed.vertex && !parsed.pixel && !parsed.hull,
                "names inside comments must not count");
            Require(
                parsed.compute,
                "the real entry point outside comments counts");
        }

        // 2D用を3Dマテリアルへ割り当てた場合。
        {
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
            // 元のメッセージは必ず残します（行番号が要る場面がある）。
            Require(
                Contains(message, "X3501"),
                "the original compiler message must be kept");
        }

        // Compute Shaderを3Dマテリアルへ割り当てた場合。
        {
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSMain': entrypoint not found",
                ComputeShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "Compute Shader"),
                "the hint must recognise a compute shader");
        }

        // includeの取りこぼし。全プロジェクトを開けなくした穴なので、
        // ここだけはソースが読めなくても説明が出ること。
        {
            const auto message = LamaPon::ExplainShaderError(
                "error X1507: failed to open source file:"
                " 'LamaPonScreenDepth.hlsli'",
                {},
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "#include"),
                "a missing include must be explained");
        }

        // 3D用として正しく書けているShaderを、スキニングモデルへ
        // 割り当てた場合。ここを一般論で済ませると「VSMainなら
        // あるのに」と読まれます。
        {
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSSkinnedMain':"
                " entrypoint not found",
                MaterialShader,
                LamaPon::ShaderUsage::Material);
            Require(
                Contains(message, "VSSkinnedMain")
                    && Contains(
                        message,
                        "3Dマテリアルとしては書けて"),
                "a skinned-only miss must say which entry points"
                " to add");
        }

        // 逆に、入口がまるごと無いものへは従来どおりの説明。
        {
            const auto message = LamaPon::ExplainShaderError(
                "error X3501: 'VSMain': entrypoint not found",
                ComputeShader,
                LamaPon::ShaderUsage::Material);
            Require(
                !Contains(message, "3Dマテリアルとしては書けて"),
                "the skinned hint must not leak into other"
                " entry point failures");
        }

        // セマンティクスの付け忘れ。
        {
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

        // エンジンが渡す名前を、宣言せずに使った場合。
        {
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

        // 見当がつかないものは元のまま返すこと。勝手な説明を足すと
        // 本当の原因から目をそらせます。**上の判定は名前が一致した
        // ときだけ効くこと**を、ここで縛っています。
        {
            const std::string original =
                "error X3004: undeclared identifier 'Foo'";
            const auto message = LamaPon::ExplainShaderError(
                original,
                SpriteShader,
                LamaPon::ShaderUsage::Material);
            Require(
                message == original,
                "an unrecognised error must be passed through");
        }

        std::cout << "Shader diagnostics tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
