// VS→GS→PSで三角形を縮小・押し出しする見本で、スキンモデルにはGSを適用しない。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "none", "depthWrite": true }
*/


/* LAMAPON_PROPERTIES
[
  { "target": "0.rgb", "type": "color", "name": "色",
    "default": [0.85, 0.45, 0.2] },
  { "target": "1.x", "type": "float", "name": "押し出す量",
    "min": 0.0, "max": 1.0, "default": 0.15 },
  { "target": "1.y", "type": "float", "name": "縮める量",
    "min": 0.0, "max": 0.9, "default": 0.0 }
]
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
    // 材質のRGBA倍率
    float4 MaterialColor;
    // 互換配置の視点位置
    float4 CameraPosition;
    // 互換配置の視線方向
    float4 CameraForward;
    // 互換配置の材質設定
    float4 MaterialParameters;
    // 色・押出量・縮小比の8本の値
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

// GSで頂点を移動した後に透視投影するため、World位置を渡す。
struct GeometryInput
{
    // VS設定後にGSで置き換える位置
    float4 Position : SV_Position;
    // 移動するWorld位置
    float3 WorldPosition : TEXCOORD0;
    // World空間の頂点・面法線
    float3 WorldNormal : TEXCOORD1;
    // 互換入力の画像UV
    float2 TexCoord : TEXCOORD2;
};

// 頂点のWorld位置と法線をGSへ渡す(input: 通常頂点)。
GeometryInput VSMain(VertexInput input)
{
    // 変換後または移動後の頂点
    GeometryInput output;
    // World空間の同次位置
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), World);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(input.Normal, (float3x3)WorldInverseTranspose));
    output.TexCoord = input.TexCoord;
    // SV_Positionは互換入力として設定し、GSで移動後の位置に置き換える。
    output.Position = mul(worldPosition, ViewProjection);
    return output;
}


// 三角形を中心へ縮小して面法線方向へ押し出す(input: 3頂点の三角形, stream: 最大3頂点の出力列)。
[maxvertexcount(3)]
void GSMain(
    triangle GeometryInput input[3],
    inout TriangleStream<GeometryInput> stream)
{
    // World空間の押し出す距離
    const float distance = CustomParameters[1].x;
    // 中心へ縮める補間比
    const float shrink = saturate(CustomParameters[1].y);


    // 第1頂点から第2頂点への辺
    const float3 edge1 =
        input[1].WorldPosition - input[0].WorldPosition;
    // 第1頂点から第3頂点への辺
    const float3 edge2 =
        input[2].WorldPosition - input[0].WorldPosition;
    // 外積から求めた面の単位法線
    const float3 faceNormal = normalize(cross(edge1, edge2));

    // World空間の三角形中心
    const float3 center =
        (input[0].WorldPosition
            + input[1].WorldPosition
            + input[2].WorldPosition) / 3.0f;

    // 移動して出力する頂点番号
    for (int index = 0; index < 3; ++index)
    {
        // 変換後または移動後の頂点
        GeometryInput output = input[index];
        // 中心へ縮小したWorld位置
        const float3 shrunk = lerp(
            input[index].WorldPosition,
            center,
            shrink);
        // 面法線で押し出したWorld位置
        const float3 moved = shrunk + faceNormal * distance;
        output.WorldPosition = moved;
        output.WorldNormal = faceNormal;
        output.Position =
            mul(float4(moved, 1.0f), ViewProjection);
        stream.Append(output);
    }
    stream.RestartStrip();
}

// 面法線で色に簡易照明を掛ける(input: GSの出力頂点)。
float4 PSMain(GeometryInput input) : SV_Target
{
    // 指定色と材質色の乗算RGB
    const float3 baseColor =
        CustomParameters[0].rgb * MaterialColor.rgb;

    // 表面から簡易光源への単位方向
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, -0.45f));
    // 面法線と光源方向の内積
    const float diffuse =
        saturate(dot(normalize(input.WorldNormal),
            lightDirection));
    // 簡易照明を掛けたRGB
    const float3 color = baseColor * (0.35f + 0.65f * diffuse);
    return float4(color, 1.0f);
}
