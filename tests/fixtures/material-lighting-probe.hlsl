// 材質のb1光源・b3追加値・t0表面・t7マスク・s0の採取設定を半透明の検査色へ合成する。
/* LAMAPON_RENDER_STATE
{ "blend": "alpha", "cull": "none" }
*/

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
    // 7.w点採取設定・他互換値
    float4 CustomParameters[8];
};

struct DirectionalLight
{
    // 光進行方向XYZ・W強度
    float4 DirectionIntensity;
    // 光RGB・W太陽角半径rad
    float4 Color;
};

struct PointLight
{
    // World位置XYZ・W到達距離
    float4 PositionRange;
    // 光RGB・W強度
    float4 ColorIntensity;
};

// CPUと配置を揃える光源定数
cbuffer LightingBuffer : register(b1)
{
    // 間接光の基本RGB
    float4 Ambient;
    // 平行・点・スポット・Cascade数
    uint4 LightCounts;
    // 最大4本の平行光
    DirectionalLight DirectionalLights[4];
    // 通常経路の最大16本の点光
    PointLight PointLights[16];
};

// 材質検査用の最大64本の値
cbuffer CustomVectorBuffer : register(b3)
{
    // b3の最大64本の追加値
    float4 CustomVectors[64];
};

// t0の表面色画像
Texture2D AlbedoTexture : register(t0);
// t7の検査用マスク画像
Texture2D MaskTexture : register(t7);
// 材質画像の採取設定
SamplerState MaterialSampler : register(s0);

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
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TexCoord = input.TexCoord;
    return output;
}

// 環境光と平行・点光源に画像と追加色を合わせ半透明で返す(input: World位置・法線とUV)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 補間後のWorld単位法線
    const float3 normal = normalize(input.WorldNormal);
    // 画像と材質色を掛けたRGBA
    const float4 albedo = AlbedoTexture.Sample(
        MaterialSampler,
        input.TexCoord);
    // 3倍UVで採取するマスクR
    const float mask = MaskTexture.Sample(
        MaterialSampler,
        input.TexCoord * 3.0f).r;

    // 累積する検査用の照明RGB
    float3 light = Ambient.rgb;
    // 検査する平行光番号
    [loop]
    for (uint index = 0u; index < LightCounts.x; ++index)
    {
        // 表面から平行光への単位方向
        const float3 direction =
            -normalize(DirectionalLights[index].DirectionIntensity.xyz);
        light += DirectionalLights[index].Color.rgb
            * DirectionalLights[index].DirectionIntensity.w
            * saturate(dot(normal, direction));
    }
    // 検査する点光源番号
    [loop]
    for (uint pointIndex = 0u; pointIndex < LightCounts.y; ++pointIndex)
    {
        // 表面から点光源への差分
        const float3 offset =
            PointLights[pointIndex].PositionRange.xyz - input.WorldPosition;
        // 表面と点光源のWorld距離
        const float distance = length(offset);
        // 到達距離による一次減衰
        const float attenuation = saturate(
            1.0f - distance / max(PointLights[pointIndex].PositionRange.w, 0.001f));
        light += PointLights[pointIndex].ColorIntensity.rgb
            * PointLights[pointIndex].ColorIntensity.w
            * attenuation
            * saturate(dot(normal, offset / max(distance, 0.001f)));
    }

    // 検査結果の出力RGB
    const float3 color = albedo.rgb
        * MaterialColor.rgb
        * light
        * lerp(0.5f, 1.0f, mask)
        + CustomVectors[0].rgb;
    return float4(color, MaterialColor.a);
}
