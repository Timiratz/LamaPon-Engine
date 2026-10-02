// キーワードのコンパイル伝達と材質バリアント選択を、緑赤と明暗の出力色で検査する。
#pragma multi_compile _ VARIANT_PROBE_GREEN
#pragma shader_feature _ VARIANT_PROBE_BRIGHT

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
    // 検査ごとの8本の追加設定
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
    // 補間するWorld位置
    float3 WorldPosition : TEXCOORD0;
    // 補間するWorld法線
    float3 WorldNormal : TEXCOORD1;
    // 補間する画像UV
    float2 TexCoord : TEXCOORD2;
};

// 通常頂点をWorld変換して検査用のPixel入力を作る(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 検査用のPixel入力
    PixelInput output;
    // World空間の同次位置
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f),
            WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    return output;
}

// GREENとBRIGHTの定義から検査色を静的に選ぶ(input: 互換用の通常Pixel入力)。
float4 PSMain(PixelInput input) : SV_Target
{
#if defined(VARIANT_PROBE_GREEN)
    // 検査結果の出力RGB
    float3 color = float3(0.0f, 1.0f, 0.0f);
#else
    // 検査結果の出力RGB
    float3 color = float3(1.0f, 0.0f, 0.0f);
#endif
#if defined(VARIANT_PROBE_BRIGHT)
    color *= 1.0f;
#else
    color *= 0.25f;
#endif
    return float4(color, 1.0f);
}
