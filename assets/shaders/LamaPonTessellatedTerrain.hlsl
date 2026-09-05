// テセレーションの見本。Plane形状のMesh Rendererへ割り当てると、
// 1枚の板をGPU側で細かく割って波打たせます。
//
// HSMain（ハル）とDSMain（ドメイン）の両方を書くと有効になります。
// 片方だけだと無視されて、ただの板のまま描かれます。
//
// 不透明な地形として描画する設定を宣言します。

/* LAMAPON_RENDER_STATE
{ "blend": "opaque", "cull": "none", "depthWrite": true }
*/

/* LAMAPON_PROPERTIES
[
  { "target": "0.rgb", "type": "color", "name": "地面の色",
    "default": [0.45, 0.62, 0.35] },
  { "target": "1.x", "type": "float", "name": "起伏の高さ",
    "min": 0.0, "max": 1.0, "default": 0.12 },
  { "target": "1.y", "type": "float", "name": "起伏の細かさ",
    "min": 0.5, "max": 16.0, "default": 4.0 },
  { "target": "1.z", "type": "float", "name": "流れる速さ",
    "min": 0.0, "max": 4.0, "default": 0.0 },
  { "target": "3.x", "type": "float", "name": "分割数",
    "min": 1.0, "max": 64.0, "default": 16.0 }
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
    float4 MaterialTextureParameters;
    float4 EmissiveParameters;
    float4 TimeParameters;
};

struct VertexInput
{
    float3 Position : SV_Position;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

// 頂点シェーダーからハルシェーダーへ渡す形。ここでは射影しません。
// 分割してから位置を動かすので、クリップ空間へ移すのはドメイン
// シェーダーの仕事です。
struct ControlPoint
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
};

struct PixelInput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

struct PatchConstants
{
    float edges[4] : SV_TessFactor;
    float inside[2] : SV_InsideTessFactor;
};

ControlPoint VSMain(VertexInput input)
{
    ControlPoint output;
    output.Position = input.Position;
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    return output;
}

PatchConstants PatchConstantMain(
    InputPatch<ControlPoint, 4> patch)
{
    // 分割数はInspectorから。距離で変えたいときはここで
    // CameraPositionとの距離を見て決めます。
    const float factor =
        clamp(CustomParameters[3].x, 1.0f, 64.0f);
    PatchConstants output;
    output.edges[0] = factor;
    output.edges[1] = factor;
    output.edges[2] = factor;
    output.edges[3] = factor;
    output.inside[0] = factor;
    output.inside[1] = factor;
    return output;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstantMain")]
ControlPoint HSMain(
    InputPatch<ControlPoint, 4> patch,
    uint id : SV_OutputControlPointID)
{
    return patch[id];
}

// 高さ。法線を数値微分で求めるので関数に切り出しています。
float TerrainHeight(float2 uv)
{
    const float amplitude = CustomParameters[1].x;
    const float frequency = max(CustomParameters[1].y, 0.001f);
    const float speed = CustomParameters[1].z;
    const float phase = TimeParameters.x * speed;
    return amplitude
        * sin(uv.x * frequency * 6.2831853f + phase)
        * cos(uv.y * frequency * 6.2831853f + phase);
}

[domain("quad")]
PixelInput DSMain(
    PatchConstants constants,
    float2 domain : SV_DomainLocation,
    const OutputPatch<ControlPoint, 4> patch)
{
    // 制御点の並びは (-x,+z) (+x,+z) (-x,-z) (+x,-z) です。
    const float3 top = lerp(
        patch[0].Position, patch[1].Position, domain.x);
    const float3 bottom = lerp(
        patch[2].Position, patch[3].Position, domain.x);
    float3 position = lerp(top, bottom, domain.y);

    const float2 topUv = lerp(
        patch[0].TexCoord, patch[1].TexCoord, domain.x);
    const float2 bottomUv = lerp(
        patch[2].TexCoord, patch[3].TexCoord, domain.x);
    const float2 texCoord = lerp(topUv, bottomUv, domain.y);

    // パッチ自身の向きを、制御点の並びから作ります。
    // uAxisは(0→1)、vAxisは(0→2)の向きで、engine側が
    // uAxis × vAxis = 面の法線 になるよう制御点を並べています。
    // 法線は固定の+Yにせず、高さの傾きから求めます。固定にすると、
    // Planeでは正しくてもCubeの側面や底面で変位が横倒しになります。
    const float3 uAxis =
        normalize(patch[1].Position - patch[0].Position);
    const float3 vAxis =
        normalize(patch[2].Position - patch[0].Position);
    const float3 faceNormal = normalize(cross(uAxis, vAxis));

    position += faceNormal * TerrainHeight(texCoord);

    // 法線は高さの傾きから作ります。少しずらして差を取るだけ。
    // 傾きはパッチの面内（uAxis／vAxis）へ戻して合成します。
    const float step = 0.01f;
    const float slopeX =
        (TerrainHeight(texCoord + float2(step, 0.0f))
            - TerrainHeight(texCoord - float2(step, 0.0f)))
        / (2.0f * step);
    const float slopeY =
        (TerrainHeight(texCoord + float2(0.0f, step))
            - TerrainHeight(texCoord - float2(0.0f, step)))
        / (2.0f * step);
    const float3 normal = normalize(
        faceNormal
        - uAxis * slopeX
        - vAxis * slopeY);

    PixelInput output;
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(normal, (float3x3)WorldInverseTranspose));
    output.TexCoord = texCoord;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    const float3 baseColor =
        CustomParameters[0].rgb * MaterialColor.rgb;
    // 簡単なランバート。上からの光を仮定した見本なので、
    // 本気で使うときはLamaPonLit.hlsl相当のライティングへ
    // 差し替えてください。
    const float3 toLight = normalize(float3(0.3f, 1.0f, 0.2f));
    const float lambert =
        saturate(dot(normalize(input.WorldNormal), toLight));
    const float3 color =
        baseColor * (0.35f + 0.65f * lambert);
    return float4(color, 1.0f);
}
