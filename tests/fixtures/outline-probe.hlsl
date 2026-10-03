// 材質の通常・輪郭・遮蔽Passを、灰色・設定輪郭RGB・設定遮蔽RGBで検査する。
// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
    // 行優先の法線変換用逆転置
    row_major float4x4 WorldInverseTranspose;
    // 通常・スキンの材質RGBA
    float4 MaterialColor;
    // World視点XYZ・W予約
    float4 CameraPosition;
    // World視線XYZ・W予約
    float4 CameraForward;
    // 粗さ・法線強度有無・金属度
    float4 MaterialParameters;
    // 3.x輪郭幅・3.yzw色・4遮蔽色
    float4 CustomParameters[8];
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
    // 補間するWorld法線
    float3 WorldNormal : TEXCOORD0;
    // 補間する画像UV
    float2 TexCoord : TEXCOORD1;
};

struct OutlinePixelInput
{
    // 輪郭頂点の画面位置
    float4 Position : SV_Position;
    // 輪郭の画像UV
    float2 TexCoord : TEXCOORD0;
};

// 通常頂点をWorld変換して検査用のPixel入力を作る(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 検査用のPixel入力
    PixelInput output;
    output.Position = mul(mul(float4(input.Position, 1.0f), World), ViewProjection);
    output.WorldNormal = mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz;
    output.TexCoord = input.TexCoord;
    return output;
}

// 固定光源の灰色の検査面を返す(input: World法線とUV)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 固定光源による明るさ倍率
    const float light = 0.35f + 0.65f * saturate(dot(
        normalize(input.WorldNormal),
        normalize(float3(0.3f, 0.6f, 0.75f))));
    return float4(0.8f * light, 0.8f * light, 0.8f * light, 1.0f);
}

// 3.xの幅だけローカル法線方向へ頂点を広げる(input: 通常頂点)。
OutlinePixelInput VSOutline(VertexInput input)
{
    // 輪郭幅を加えたローカル位置
    const float3 position = input.Position
        + input.Normal * max(CustomParameters[3].x, 0.0f);
    // 輪郭Passへ渡す透視位置とUV
    OutlinePixelInput output;
    output.Position = mul(mul(float4(position, 1.0f), World), ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

// 3.yzwの輪郭色を不透明で返す(input: 互換用の輪郭頂点)。
float4 PSOutline(OutlinePixelInput input) : SV_Target
{
    return float4(saturate(CustomParameters[3].yzw), 1.0f);
}

// 4.rgbの遮蔽色をAlpha0.6で返し非プレマルチ合成を検査する(input: 互換用の通常Pixel入力)。
float4 PSOccluded(PixelInput input) : SV_Target
{
    return float4(saturate(CustomParameters[4].rgb), 0.6f);
}
