// 材質Shaderのt23～t25のL1間接光とb1の格子定数を検査し、縁のフェードを省いて法線で評価する。
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
    // 4枠のWorldスポット影変換
    row_major float4x4 SpotShadowViewProjections[4];
    // 深度・法線補正・強度・有効
    float4 SpotShadowParameters[4];
    // 対象+1・深度補正・強度
    float4 PointShadowParameters;
    // Cascade・spot・point画素幅
    float4 ShadowTexelSizes;
    // 逆画面幅高さ・Z有効
    float4 ScreenAmbientOcclusionParameters;
    // 分割XYZ・W有効
    float4 ClusteredParameters;
    // near・far・対数比・灯数上限
    float4 ClusteredDepthParameters;
    // 逆画面幅高さ・Z総灯数
    float4 ClusteredScreenParameters;
    // 主プローブのWorld箱中心
    float4 ReflectionBoxCenter;
    // 主プローブ箱半径XYZ・W有効
    float4 ReflectionBoxParameters;
    // 副プローブのWorld箱中心
    float4 ReflectionSecondaryBoxCenter;
    // 副プローブ箱半径XYZ・W有効
    float4 ReflectionSecondaryBoxParameters;
    // 副混合比・副最終Mip
    float4 ReflectionBlendParameters;
    // SSR強度・有効・距離・反復数
    float4 ScreenReflectionParameters;
    // 逆画面幅高さ・投影係数ZW
    float4 ScreenReflectionScreen;
    // 厚み・粗さ上限・最終Mip
    float4 ScreenReflectionQuality;
    // 前フレームのビュー透視合成
    row_major float4x4 ScreenReflectionPreviousViewProjection;
    // 格子のWorld最小XYZ・W有効
    float4 BakedGiVolumeMinimum;
    // 逆格子寸法XYZ・W強度
    float4 BakedGiInverseSize;
    // 各軸の間接光プローブ数
    float4 BakedGiResolution;
};

// 間接光RのL1係数画像
Texture3D BakedGiRedTexture : register(t23);
// 間接光GのL1係数画像
Texture3D BakedGiGreenTexture : register(t24);
// 間接光BのL1係数画像
Texture3D BakedGiBlueTexture : register(t25);
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

// 格子のL1間接光を評価し無効時は環境光の半分を返す(input: World位置と法線)。
float4 PSMain(PixelInput input) : SV_Target
{
    if (BakedGiVolumeMinimum.w < 0.5f)
    {
        return float4(Ambient.rgb * 0.5f, 1.0f);
    }
    // 補間後のWorld単位法線
    const float3 normal = normalize(input.WorldNormal);
    // 格子内の正規化World位置
    const float3 volumeUvw = saturate(
        (input.WorldPosition - BakedGiVolumeMinimum.xyz)
        * BakedGiInverseSize.xyz);
    // 各軸の間接光プローブ数
    const float3 resolution = BakedGiResolution.xyz;
    // 格子角を画素中心へ移したUV
    const float3 texelUvw =
        (volumeUvw * (resolution - 1.0f) + 0.5f) / resolution;
    // 法線XYZ・定数1のL1基底
    const float4 basis = float4(normal, 1.0f);
    // ベイク間接光のRGB
    const float3 gi = float3(
        dot(basis, BakedGiRedTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)),
        dot(basis, BakedGiGreenTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)),
        dot(basis, BakedGiBlueTexture.SampleLevel(
            MaterialSampler, texelUvw, 0.0f)));
    return float4(max(gi, 0.0f) * BakedGiInverseSize.w * 0.8f, 1.0f);
}
