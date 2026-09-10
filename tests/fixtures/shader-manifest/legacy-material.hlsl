struct LegacyMaterialVertexOutput
{
    float4 Position : SV_Position;
};

LegacyMaterialVertexOutput VSMain(float3 position : POSITION)
{
    LegacyMaterialVertexOutput output;
    output.Position = float4(position, 1.0f);
    return output;
}

LegacyMaterialVertexOutput VSInstancedMain(
    float3 position : POSITION)
{
    return VSMain(position);
}

float4 PSMain(
    LegacyMaterialVertexOutput input) : SV_Target
{
    return float4(input.Position.xy * 0.0f, 1.0f, 1.0f);
}
