// 描画状態を省略した材質の表裏を、視点側法線の色と裏面の暗赤で検査する。
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
    output.WorldNormal =
        mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz;
    return output;
}

// 視点へ向く法線は方向色・逆向きは暗赤を返す(input: World位置と法線)。
float4 PSMain(PixelInput input) : SV_Target
{
    // 補間後のWorld単位法線
    const float3 normal = normalize(input.WorldNormal);
    // 表面法線が視点へ向くか
    const bool front = dot(
        normal,
        normalize(CameraPosition.xyz - input.WorldPosition)) > 0.0f;
    return front
        ? float4(normal * 0.5f + 0.5f, 1.0f)
        : float4(0.3f, 0.02f, 0.02f, 1.0f);
}
