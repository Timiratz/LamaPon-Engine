// HSMainとDSMainの組で4制御点の面を分割し、波状に変位する見本Shader。

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

// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
    // 行優先の法線変換用逆転置
    row_major float4x4 WorldInverseTranspose;
    // 地面色のRGBA倍率
    float4 MaterialColor;
    // 互換配置の視点位置
    float4 CameraPosition;
    // 互換配置の視線方向
    float4 CameraForward;
    // 互換配置の材質設定
    float4 MaterialParameters;
    // 色・変位・分割数の8本の値
    float4 CustomParameters[8];
    // 互換配置の材質画像設定
    float4 MaterialTextureParameters;
    // 互換配置の発光設定
    float4 EmissiveParameters;
    // 1時間周期秒・差分秒・フレーム数
    float4 TimeParameters;
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

// 制御点はローカル位置を保ち、分割と変位の後にDSで透視投影する。
struct ControlPoint
{
    // 変位前のローカル位置
    float3 Position : POSITION;
    // 互換入力のローカル法線
    float3 Normal : NORMAL;
    // 変位量を採取するUV
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

struct PatchConstants
{
    // 4辺の分割数
    float edges[4] : SV_TessFactor;
    // U・V方向の内部の分割数
    float inside[2] : SV_InsideTessFactor;
};

// ローカル頂点を4点パッチの制御点として渡す(input: 通常頂点)。
ControlPoint VSMain(VertexInput input)
{
    // HSへ渡す制御点
    ControlPoint output;
    output.Position = input.Position;
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    return output;
}

// 全辺と内部を同じ分割数1～64へ設定する(patch: 互換用の4制御点)。
PatchConstants PatchConstantMain(
    InputPatch<ControlPoint, 4> patch)
{

    // 1～64へ制限した分割数
    const float factor =
        clamp(CustomParameters[3].x, 1.0f, 64.0f);
    // 全辺と内部の分割定数
    PatchConstants output;
    output.edges[0] = factor;
    output.edges[1] = factor;
    output.edges[2] = factor;
    output.edges[3] = factor;
    output.inside[0] = factor;
    output.inside[1] = factor;
    return output;
}

// 4点パッチの指定制御点を返す(patch: 4制御点, id: 出力制御点番号)。
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


// UVと循環時刻から面法線方向の変位量を求める(uv: パッチの画像UV)。
float TerrainHeight(float2 uv)
{
    // 面法線方向の変位の高さ
    const float amplitude = CustomParameters[1].x;
    // UV内の波の周期数
    const float frequency = max(CustomParameters[1].y, 0.001f);
    // 毎秒の波の位相変化
    const float speed = CustomParameters[1].z;
    // 循環時刻に応じた波の位相
    const float phase = TimeParameters.x * speed;
    return amplitude
        * sin(uv.x * frequency * 6.2831853f + phase)
        * cos(uv.y * frequency * 6.2831853f + phase);
}

// パッチ面へ波状変位と傾き法線を適用する(constants: 互換用の分割定数, domain: パッチ内の補間UV, patch: 分割後の4制御点)。
[domain("quad")]
PixelInput DSMain(
    PatchConstants constants,
    float2 domain : SV_DomainLocation,
    const OutputPatch<ControlPoint, 4> patch)
{
    // 4制御点の順は(-x,+z)・(+x,+z)・(-x,-z)・(+x,-z)に保つ。
    // U補間後の上辺位置
    const float3 top = lerp(
        patch[0].Position, patch[1].Position, domain.x);
    // U補間後の下辺位置
    const float3 bottom = lerp(
        patch[2].Position, patch[3].Position, domain.x);
    // 変位前後のローカル位置
    float3 position = lerp(top, bottom, domain.y);

    // U補間後の上辺UV
    const float2 topUv = lerp(
        patch[0].TexCoord, patch[1].TexCoord, domain.x);
    // U補間後の下辺UV
    const float2 bottomUv = lerp(
        patch[2].TexCoord, patch[3].TexCoord, domain.x);
    // パッチ内の補間済みUV
    const float2 texCoord = lerp(topUv, bottomUv, domain.y);

    // 0→1をU、0→2をVとし、U×Vが面法線となる制御点順を使う。
    // 面内のU単位方向
    const float3 uAxis =
        normalize(patch[1].Position - patch[0].Position);
    // 面内のV単位方向
    const float3 vAxis =
        normalize(patch[2].Position - patch[0].Position);
    // パッチ面の単位法線
    const float3 faceNormal = normalize(cross(uAxis, vAxis));

    position += faceNormal * TerrainHeight(texCoord);

    // UVの中心差分による高さの傾きを、パッチ面内のU・V方向へ合成する。
    // 傾きを取るUV差分の幅
    const float step = 0.01f;
    // UVのU方向の高さ勾配
    const float slopeX =
        (TerrainHeight(texCoord + float2(step, 0.0f))
            - TerrainHeight(texCoord - float2(step, 0.0f)))
        / (2.0f * step);
    // UVのV方向の高さ勾配
    const float slopeY =
        (TerrainHeight(texCoord + float2(0.0f, step))
            - TerrainHeight(texCoord - float2(0.0f, step)))
        / (2.0f * step);
    // 傾きによるローカル法線
    const float3 normal = normalize(
        faceNormal
        - uAxis * slopeX
        - vAxis * slopeY);

    // 変位後のPixel入力
    PixelInput output;
    // 変位後のWorld同次位置
    const float4 worldPosition =
        mul(float4(position, 1.0f), World);
    output.Position = mul(worldPosition, ViewProjection);
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(
        mul(normal, (float3x3)WorldInverseTranspose));
    output.TexCoord = texCoord;
    return output;
}

// 地面色へ固定光源の簡易照明を掛ける(input: DSの出力頂点)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 地面色と材質色の乗算RGB
    const float3 baseColor =
        CustomParameters[0].rgb * MaterialColor.rgb;

    // 表面から固定光源への単位方向
    const float3 toLight = normalize(float3(0.3f, 1.0f, 0.2f));
    // 法線と光源方向の内積
    const float lambert =
        saturate(dot(normalize(input.WorldNormal), toLight));
    // 簡易照明を掛けた地面RGB
    const float3 color =
        baseColor * (0.35f + 0.65f * lambert);
    return float4(color, 1.0f);
}
