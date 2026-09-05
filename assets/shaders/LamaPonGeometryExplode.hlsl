// ジオメトリシェーダーの見本。三角形を1枚ずつ、面の向きへ押し出します。
//
// GSMainを書くと有効になります。宣言や追加設定は不要です。
// 入口は VSMain → GSMain → PSMain の順に流れます。
//
// 入力は必ず triangle です（エンジンが流すのは三角形だけなので、
// point や line を宣言するとコンパイルの時点で断られます）。
//
// Mesh Renderer にも Model Renderer にも割り当てられます。
// スキニングモデル（ボーン付き）では、頂点シェーダーがDirectXTKの
// ものになるためGSは使われません。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "none", "depthWrite": true }
*/
// 押し出すと裏面が見えるので、両面描画にしています。

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

cbuffer ObjectBuffer : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProjection;
    row_major float4x4 WorldInverseTranspose;
    float4 MaterialColor;
    float4 CameraPosition;
    float4 CameraForward;
    float4 MaterialParameters;
    float4 CustomParameters[8];
};

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

// 頂点シェーダーからジオメトリシェーダーへ渡す形。
// ここでは射影しません。押し出してからクリップ空間へ移すので、
// 位置はワールド空間のまま持ち回ります。
struct GeometryInput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

GeometryInput VSMain(VertexInput input)
{
    GeometryInput output;
    const float4 worldPosition =
        mul(float4(input.Position, 1.0f), World);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(input.Normal, (float3x3)WorldInverseTranspose));
    output.TexCoord = input.TexCoord;
    // GSで押し出してから射影し直します。ここでの値は使われませんが、
    // SV_Positionは構造体に要るので入れておきます。
    output.Position = mul(worldPosition, ViewProjection);
    return output;
}

// 1枚の三角形から出力する最大3頂点に合わせます。
// （宣言より多く出すと描画が途中で切れます）。
[maxvertexcount(3)]
void GSMain(
    triangle GeometryInput input[3],
    inout TriangleStream<GeometryInput> stream)
{
    const float distance = CustomParameters[1].x;
    const float shrink = saturate(CustomParameters[1].y);

    // 面の法線は3頂点から作ります。頂点法線を平均するのではなく
    // 外積で取るのが要点で、こうすると角の丸め方に左右されません。
    const float3 edge1 =
        input[1].WorldPosition - input[0].WorldPosition;
    const float3 edge2 =
        input[2].WorldPosition - input[0].WorldPosition;
    const float3 faceNormal = normalize(cross(edge1, edge2));

    const float3 center =
        (input[0].WorldPosition
            + input[1].WorldPosition
            + input[2].WorldPosition) / 3.0f;

    for (int index = 0; index < 3; ++index)
    {
        GeometryInput output = input[index];
        const float3 shrunk = lerp(
            input[index].WorldPosition,
            center,
            shrink);
        const float3 moved = shrunk + faceNormal * distance;
        output.WorldPosition = moved;
        output.WorldNormal = faceNormal;
        output.Position =
            mul(float4(moved, 1.0f), ViewProjection);
        stream.Append(output);
    }
    stream.RestartStrip();
}

float4 PSMain(GeometryInput input) : SV_Target
{
    const float3 baseColor =
        CustomParameters[0].rgb * MaterialColor.rgb;
    // 面ごとに向きが違うので、簡単な陰影を付けるだけで形が分かります。
    const float3 lightDirection =
        normalize(float3(0.4f, 0.8f, -0.45f));
    const float diffuse =
        saturate(dot(normalize(input.WorldNormal),
            lightDirection));
    const float3 color = baseColor * (0.35f + 0.65f * diffuse);
    return float4(color, 1.0f);
}
