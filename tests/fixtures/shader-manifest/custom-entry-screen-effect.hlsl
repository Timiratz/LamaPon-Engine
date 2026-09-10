struct ManifestVertexOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

ManifestVertexOutput ManifestVertex(uint vertexId : SV_VertexID)
{
    ManifestVertexOutput output;
    const float2 uv = float2(
        (vertexId << 1) & 2,
        vertexId & 2);
    output.Position = float4(
        uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f,
        1.0f);
    output.TexCoord = uv;
    return output;
}

float4 ManifestPixel(
    ManifestVertexOutput input) : SV_Target
{
    return float4(input.TexCoord, 0.0f, 1.0f);
}
