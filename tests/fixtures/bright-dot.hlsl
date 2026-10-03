// 中央のHDR四角の滲みで、ScreenEffectをBloomの前後へ挿入した位置を検査する。
// 画面検査の追加値と画面寸法
cbuffer ScreenParameters : register(b0)
{
    // 0.x四角半径・0.yHDR明度
    float4 CustomParameters[8];
    // 画面幅高さXY・逆幅高さZW
    float4 ScreenSize;
};

// 処理前のシーン画像
Texture2D SceneTexture : register(t0);
// シーンと補助画像の採取設定
SamplerState SceneSampler : register(s0);

struct VertexOutput
{
    // 画面三角形の透視位置
    float4 Position : SV_Position;
    // 画面画像のUV
    float2 TexCoord : TEXCOORD0;
};

// 頂点IDだけで画面を覆う三角形とUVを作る(vertexId: 0～2の頂点番号)。
VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    // 画面全体の三角形頂点
    VertexOutput output;
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

// 中央へ指定半径と明るさの四角を描き外は元画像を返す(input: 画面位置とUV)。
float4 PSMain(VertexOutput input) : SV_Target
{
    // 中央の四角の画面半径
    const float radius = max(CustomParameters[0].x, 0.001f);
    // 中央四角のHDR明るさ
    const float intensity = max(CustomParameters[0].y, 0.0f);
    // 四角の外へ返す元RGBA
    const float4 scene =
        SceneTexture.Sample(SceneSampler, input.TexCoord);

    // 画面中心からのUV絶対差
    const float2 offset = abs(input.TexCoord - 0.5f);
    if (offset.x < radius && offset.y < radius)
    {
        return float4(intensity, intensity, intensity, 1.0f);
    }
    return scene;
}
