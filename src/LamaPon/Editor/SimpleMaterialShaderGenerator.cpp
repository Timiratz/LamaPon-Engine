#include "LamaPon/Editor/SimpleMaterialShaderGenerator.h"

#include <sstream>
#include <string_view>
#include <vector>

namespace
{
    struct Property final
    {
        std::string target;
        std::string type;
        std::string name;
        std::string extra;
    };

    void WriteProperties(
        std::ostringstream& output,
        const std::vector<Property>& properties)
    {
        output << "/* LAMAPON_PROPERTIES\n[\n";
        for (std::size_t index{}; index < properties.size(); ++index)
        {
            const auto& property = properties[index];
            output << "  { \"target\": \"" << property.target
                << "\", \"type\": \"" << property.type
                << "\", \"name\": \"" << property.name << "\"";
            if (!property.extra.empty())
            {
                output << ", " << property.extra;
            }
            output << " }";
            if (index + 1 < properties.size())
            {
                output << ',';
            }
            output << '\n';
        }
        output << "]\n*/\n";
    }
}

namespace LamaPon
{
    std::string GenerateSimpleMaterialShader(
        const SimpleMaterialShaderGraph& graph)
    {
        std::vector<Property> properties;
        if (graph.tint)
        {
            properties.push_back({
                "0.rgb", "color", "Tint",
                "\"default\": [1.0, 1.0, 1.0]" });
            properties.push_back({
                "0.a", "float", "Tint Amount",
                "\"min\": 0.0, \"max\": 1.0, \"default\": 1.0" });
        }
        if (graph.emission)
        {
            properties.push_back({
                "1.x", "float", "Emission",
                "\"min\": 0.0, \"max\": 8.0, \"default\": 0.0" });
        }
        if (graph.rimLight)
        {
            properties.push_back({
                "1.y", "float", "Rim Light",
                "\"min\": 0.0, \"max\": 4.0, \"default\": 0.0" });
        }
        if (graph.uvScroll)
        {
            properties.push_back({
                "2.xy", "vector", "UV Scroll",
                "\"default\": [0.0, 0.0]" });
        }
        if (graph.maskTexture)
        {
            properties.push_back({ "t7", "texture", "Mask", {} });
        }

        std::ostringstream output;
        output << "// LamaPon Simple Shader Graph generated file.\n"
            "// 作成ダイアログで選択したノードから生成されています。\n"
            "/* LAMAPON_RENDER_STATE\n"
            "{ \"blend\": \"opaque\", \"cull\": \"back\", "
            "\"depthWrite\": true }\n"
            "*/\n";
        WriteProperties(output, properties);
        output << "#include \"shaders/LamaPonSimpleMaterialGraph.hlsli\"\n\n"
            "float4 EvaluateSimpleMaterial(PixelInput input)\n"
            "{\n"
            "    float2 uv = input.TexCoord;\n";
        if (graph.uvScroll)
        {
            output << "    uv += CustomParameters[2].xy * TimeParameters.x;\n";
        }
        output << "    float4 surface = AlbedoTexture.Sample(MaterialSampler, uv)\n"
            "        * MaterialColor;\n";
        if (graph.maskTexture)
        {
            output << "    surface.a *= CustomTexture0.Sample(MaterialSampler, uv).r;\n";
        }
        if (graph.alphaClip)
        {
            output << "    clip(surface.a - 0.08f);\n";
        }
        if (graph.tint)
        {
            output << "    surface.rgb *= lerp(float3(1.0f, 1.0f, 1.0f),\n"
                "        CustomParameters[0].rgb, saturate(CustomParameters[0].a));\n";
        }
        if (graph.rimLight)
        {
            output << "    const float3 viewDirection = normalize(\n"
                "        CameraPosition.xyz - input.WorldPosition);\n"
                "    const float rim = pow(1.0f - saturate(dot(\n"
                "        normalize(input.WorldNormal), viewDirection)), 3.0f);\n"
                "    surface.rgb += surface.rgb * rim * max(CustomParameters[1].y, 0.0f);\n";
        }
        if (graph.emission)
        {
            output << "    surface.rgb *= 1.0f + max(CustomParameters[1].x, 0.0f);\n";
        }
        output << "    return surface;\n"
            "}\n\n"
            "float4 PSMain(PixelInput input) : SV_Target\n"
            "{\n"
            "    return EvaluateSimpleMaterial(input);\n"
            "}\n\n"
            "float4 PSSkinnedMain(SkinnedPixelInput input) : SV_Target\n"
            "{\n"
            "    return EvaluateSimpleMaterial(ToPixelInput(input));\n"
            "}\n";
        return output.str();
    }
}
