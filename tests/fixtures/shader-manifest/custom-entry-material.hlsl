#pragma multi_compile _ MATERIAL_VARIANT

#ifndef MATERIAL_VARIANT
#error MATERIAL_VARIANT must be defined for this fixture.
#endif

struct MaterialVertexInput
{
    float3 Position : POSITION;
};

struct MaterialVertexOutput
{
    float4 Position : SV_Position;
};

MaterialVertexOutput CustomMaterialVertex(
    MaterialVertexInput input)
{
    MaterialVertexOutput output;
    output.Position = float4(input.Position, 1.0f);
    return output;
}

float4 CustomMaterialPixel(
    MaterialVertexOutput input) : SV_Target
{
    return float4(
        input.Position.xy * 0.0f,
        0.25f,
        1.0f);
}

MaterialVertexOutput CustomMaterialVertexAlt(
    MaterialVertexInput input)
{
    MaterialVertexOutput output;
    output.Position = float4(
        input.Position.x + 0.001f,
        input.Position.yz,
        1.0f);
    return output;
}

float4 CustomMaterialPixelAlt(
    MaterialVertexOutput input) : SV_Target
{
    return float4(
        input.Position.xy * 0.0f,
        0.5f,
        1.0f);
}

[maxvertexcount(3)]
void CustomMaterialGeometry(
    triangle MaterialVertexOutput input[3],
    inout TriangleStream<MaterialVertexOutput> stream)
{
    [unroll]
    for (uint index = 0; index < 3; ++index)
    {
        stream.Append(input[index]);
    }
}

[maxvertexcount(1)]
void CustomMaterialPointGeometry(
    point MaterialVertexOutput input[1],
    inout PointStream<MaterialVertexOutput> stream)
{
    stream.Append(input[0]);
}

struct MaterialPatchConstants
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

MaterialPatchConstants CustomMaterialPatchConstants(
    InputPatch<MaterialVertexOutput, 3> input,
    uint patchId : SV_PrimitiveID)
{
    MaterialPatchConstants output;
    output.edge[0] = 1.0f;
    output.edge[1] = 1.0f;
    output.edge[2] = 1.0f;
    output.inside = 1.0f;
    return output;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("CustomMaterialPatchConstants")]
MaterialVertexOutput CustomMaterialHull(
    InputPatch<MaterialVertexOutput, 3> input,
    uint pointId : SV_OutputControlPointID,
    uint patchId : SV_PrimitiveID)
{
    return input[pointId];
}

[domain("tri")]
MaterialVertexOutput CustomMaterialDomain(
    MaterialPatchConstants constants,
    float3 barycentric : SV_DomainLocation,
    const OutputPatch<MaterialVertexOutput, 3> patch)
{
    MaterialVertexOutput output;
    output.Position = patch[0].Position * barycentric.x
        + patch[1].Position * barycentric.y
        + patch[2].Position * barycentric.z;
    return output;
}
