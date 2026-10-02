// 材質のt3鏡面・t6拡散天空画像とb1の環境定数を、固定比の反射色で検査する。
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

struct SpotLight
{
    // World位置XYZ・W到達距離
    float4 PositionRange;
    // 光進行方向XYZ・W内角cos
    float4 DirectionInnerCosine;
    // 光RGB・W強度
    float4 ColorIntensity;
    // X外角cos・Y影枠+1
    float4 OuterCosinePadding;
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
    // 通常経路の最大8本のスポット
    SpotLight SpotLights[8];
    // 4CascadeのWorld影変換
    row_major float4x4 ShadowViewProjections[4];
    // 4Cascadeの終端距離
    float4 ShadowCascadeSplits;
    // 対象+1・深度法線補正・強度
    float4 ShadowParameters;
    // 霧のRGB
    float4 FogColor;
    // 開始・終了距離・密度・有効
    float4 FogParameters;
    // IBL強度・有効・最終Mip
    float4 EnvironmentParameters;
};

// 主プローブの鏡面天空画像
TextureCube EnvironmentMap : register(t3);
// 主プローブの拡散天空画像
TextureCube IrradianceMap : register(t6);
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

// 半粗さMipの鏡面6割と法線方向の拡散4割を合成する(input: World位置と法線)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 補間後のWorld単位法線
    const float3 normal = normalize(input.WorldNormal);
    // 表面から視点への単位方向
    const float3 viewDirection = normalize(
        CameraPosition.xyz - input.WorldPosition);
    // 鏡面反射のRGB寄与
    const float3 specular = EnvironmentMap.SampleLevel(
        MaterialSampler,
        reflect(-viewDirection, normal),
        EnvironmentParameters.z * 0.5f).rgb;
    // 拡散反射のRGB寄与
    const float3 diffuse = IrradianceMap.SampleLevel(
        MaterialSampler,
        normal,
        0.0f).rgb;
    // 検査結果の出力RGB
    const float3 color =
        (specular * 0.6f + diffuse * 0.4f) * EnvironmentParameters.x
        + Ambient.rgb * 0.1f;
    return float4(color, 1.0f);
}
