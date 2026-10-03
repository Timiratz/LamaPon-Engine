// 互換名の通常・Instance材質入口を、固定の緑色で検査する。
struct LegacyMaterialVertexOutput
{
    // 互換入口の透視位置
    float4 Position : SV_Position;
};

// 入力位置を透視位置としてそのまま渡す(position: 検査用の頂点位置)。
LegacyMaterialVertexOutput VSMain(float3 position : POSITION)
{
    // 材質入口の透視位置
    LegacyMaterialVertexOutput output;
    output.Position = float4(position, 1.0f);
    return output;
}

// 同じ位置を通常入口へ渡して互換Instance入口を検査する(position: 検査用の頂点位置)。
LegacyMaterialVertexOutput VSInstancedMain(
    float3 position : POSITION)
{
    return VSMain(position);
}

// 固定緑を返して互換材質入口を検査する(input: 検査用の透視位置)。
float4 PSMain(
    LegacyMaterialVertexOutput input) : SV_Target
{
    return float4(input.Position.xy * 0.0f, 1.0f, 1.0f);
}
