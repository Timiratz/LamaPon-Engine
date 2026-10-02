// 代替表示を検査するため、存在しない関数呼び出しによるコンパイル失敗を維持する。
// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
};

struct VertexInput
{
    // ローカル頂点位置
    float3 Position : SV_Position;
    // ローカル頂点法線
    float3 Normal : NORMAL;
    // 画像UV
    float2 TexCoord : TEXCOORD0;
};

struct PixelInput
{
    // 透視投影後の画面位置
    float4 Position : SV_Position;
};

// 通常頂点をWorld変換して検査用のPixel入力を作る(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 検査用のPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    return output;
}

// 未定義関数を呼んで検査用のコンパイル失敗を起こす(input: 検査用の透視位置)。
float4 PSMain(PixelInput input) : SV_Target
{
    return ThisFunctionDoesNotExist(input.Position);
}
