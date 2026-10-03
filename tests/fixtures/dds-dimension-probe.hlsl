// 材質のt7配列2枚目・t8立体奥側・t9配列Cube2個目の正Y面を、画面の左・中・右へ描いて検査する。
// CPUと配置を揃える物体定数
cbuffer ObjectBuffer : register(b0)
{
    // 行優先のWorld変換行列
    row_major float4x4 World;
    // 行優先のビュー透視合成行列
    row_major float4x4 ViewProjection;
};

// t7のDDS配列画像
Texture2DArray SliceTexture : register(t7);
// t8のDDS立体画像
Texture3D VolumeTexture : register(t8);
// t9のDDS Cube配列画像
TextureCubeArray CubeArrayTexture : register(t9);
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
    // 補間する画像UV
    float2 TexCoord : TEXCOORD0;
};

// 通常頂点をWorld変換して検査用のPixel入力を作る(input: 通常頂点)。
PixelInput VSMain(VertexInput input)
{
    // 検査用のPixel入力
    PixelInput output;
    output.Position = mul(
        mul(float4(input.Position, 1.0f), World),
        ViewProjection);
    output.TexCoord = input.TexCoord;
    return output;
}

// DDSの配列・立体・Cube配列を3帯へ描く(input: 透視位置と画像UV)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 画像採取UV
    const float2 uv = input.TexCoord;
    // 2枚目の配列画像の採取RGB
    const float3 slice =
        SliceTexture.SampleLevel(MaterialSampler, float3(uv, 1.0f), 0.0f).rgb;
    // 奥側層の中心の採取RGB
    const float3 volume =
        VolumeTexture.SampleLevel(MaterialSampler, float3(uv, 0.75f), 0.0f).rgb;
    // 2個目Cubeの正Y面のRGB
    const float3 cube = CubeArrayTexture.SampleLevel(
        MaterialSampler,
        float4(0.0f, 1.0f, 0.0f, 1.0f),
        0.0f).rgb;
    // 検査結果の出力RGB
    const float3 color = uv.x < 1.0f / 3.0f
        ? slice
        : (uv.x < 2.0f / 3.0f ? volume : cube);
    return float4(color, 1.0f);
}
