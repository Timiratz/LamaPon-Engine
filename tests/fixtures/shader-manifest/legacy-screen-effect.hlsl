// 互換名VSMain・PSMainの画面入口を、画面UVのGR色で検査する。
struct LegacyVertexOutput
{
    // 画面三角形の透視位置
    float4 Position : SV_Position;
    // 画面画像のUV
    float2 TexCoord : TEXCOORD0;
};

// 頂点IDだけで画面を覆う三角形とUVを作る(vertexId: 0～2の頂点番号)。
LegacyVertexOutput VSMain(uint vertexId : SV_VertexID)
{
    // 画面全体の三角形頂点
    LegacyVertexOutput output;
    // 三角形頂点の画面UV
    const float2 uv = float2(
        (vertexId << 1) & 2,
        vertexId & 2);
    output.Position = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f,
        1.0f);
    output.TexCoord = uv;
    return output;
}

// 画面UVをGRへ書いて互換入口選択を検査する(input: 画面位置とUV)。
float4 PSMain(LegacyVertexOutput input) : SV_Target
{
    return float4(input.TexCoord.yx, 0.0f, 1.0f);
}
