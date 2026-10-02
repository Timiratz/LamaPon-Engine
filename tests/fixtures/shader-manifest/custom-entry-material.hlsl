// Manifest指定のVS・PS・GS・HS・DSと差し替え入口を検査し、MATERIAL_VARIANTの定義を必須にする。
#pragma multi_compile _ MATERIAL_VARIANT

#ifndef MATERIAL_VARIANT
#error MATERIAL_VARIANT must be defined for this fixture.
#endif

struct MaterialVertexInput
{
    // 入口検査用の頂点位置
    float3 Position : POSITION;
};

struct MaterialVertexOutput
{
    // 入口間で渡す透視位置
    float4 Position : SV_Position;
};

// 入力位置をそのまま透視位置へ渡す(input: 検査用の通常頂点)。
MaterialVertexOutput CustomMaterialVertex(
    MaterialVertexInput input)
{
    // 入口間で渡す透視位置
    MaterialVertexOutput output;
    output.Position = float4(input.Position, 1.0f);
    return output;
}

// 固定の青0.25でPS入口を検査する(input: 検査用の透視位置)。
float4 CustomMaterialPixel(
    MaterialVertexOutput input) : SV_Target
{
    return float4(
        input.Position.xy * 0.0f,
        0.25f,
        1.0f);
}

// Xを0.001ずらしてVS差し替えを検査する(input: 検査用の通常頂点)。
MaterialVertexOutput CustomMaterialVertexAlt(
    MaterialVertexInput input)
{
    // 入口間で渡す透視位置
    MaterialVertexOutput output;
    output.Position = float4(
        input.Position.x + 0.001f,
        input.Position.yz,
        1.0f);
    return output;
}

// 固定の青0.5でPS差し替えを検査する(input: 検査用の透視位置)。
float4 CustomMaterialPixelAlt(
    MaterialVertexOutput input) : SV_Target
{
    return float4(
        input.Position.xy * 0.0f,
        0.5f,
        1.0f);
}

// 三角形の3頂点を順序を保って渡す(input: 三角形の入力3頂点, stream: 三角形の出力先)。
[maxvertexcount(3)]
void CustomMaterialGeometry(
    triangle MaterialVertexOutput input[3],
    inout TriangleStream<MaterialVertexOutput> stream)
{
    // 走査する光源または頂点番号
    [unroll]
    for (uint index = 0; index < 3; ++index)
    {
        stream.Append(input[index]);
    }
}

// 入力1点を出力してPoint GS入口を検査する(input: 入力1頂点, stream: 点の出力先)。
[maxvertexcount(1)]
void CustomMaterialPointGeometry(
    point MaterialVertexOutput input[1],
    inout PointStream<MaterialVertexOutput> stream)
{
    stream.Append(input[0]);
}

struct MaterialPatchConstants
{
    // 3辺の分割数
    float edge[3] : SV_TessFactor;
    // 内部の分割数
    float inside : SV_InsideTessFactor;
};

// 3辺と内部の分割数を1に保つ(input: 互換用の3制御点, patchId: 互換用のPatch番号)。
MaterialPatchConstants CustomMaterialPatchConstants(
    InputPatch<MaterialVertexOutput, 3> input,
    uint patchId : SV_PrimitiveID)
{
    // 全辺と内部の分割定数1
    MaterialPatchConstants output;
    output.edge[0] = 1.0f;
    output.edge[1] = 1.0f;
    output.edge[2] = 1.0f;
    output.inside = 1.0f;
    return output;
}

// 指定制御点を返して3点PatchのHS入口を検査する(input: 3制御点, pointId: 出力制御点番号, patchId: 互換用のPatch番号)。
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

// 3制御点を重心座標で補間してDS入口を検査する(constants: 互換用の分割定数, barycentric: Patch内の重心座標, patch: 出力側の3制御点)。
[domain("tri")]
MaterialVertexOutput CustomMaterialDomain(
    MaterialPatchConstants constants,
    float3 barycentric : SV_DomainLocation,
    const OutputPatch<MaterialVertexOutput, 3> patch)
{
    // 入口間で渡す透視位置
    MaterialVertexOutput output;
    output.Position = patch[0].Position * barycentric.x
        + patch[1].Position * barycentric.y
        + patch[2].Position * barycentric.z;
    return output;
}
