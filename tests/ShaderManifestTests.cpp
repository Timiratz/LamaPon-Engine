#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/ComputeEffect.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/ScreenEffect.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShaderManifest.h"
#include "LamaPon/Graphics/ShaderProgram.h"

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef LAMAPON_SHADER_MANIFEST_FIXTURE_ROOT
#error LAMAPON_SHADER_MANIFEST_FIXTURE_ROOT must name the shader manifest fixture directory.
#endif

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    [[nodiscard]] bool Contains(
        const std::string& value,
        const std::string_view expected)
    {
        return value.find(expected) != std::string::npos;
    }

    template<typename Callback>
    void RequireThrowsContaining(
        Callback&& callback,
        const std::string_view expectedError,
        const char* message)
    {
        try
        {
            callback();
        }
        catch (const std::exception& exception)
        {
            Require(
                Contains(exception.what(), expectedError),
                "exception did not contain the expected diagnostic");
            return;
        }
        throw std::runtime_error(message);
    }

    // Parse failures are transactional so a failed hot reload cannot replace
    // the last valid description with a partially parsed manifest.
    void RequireRejected(
        const std::string_view json,
        const std::string_view expectedError)
    {
        LamaPon::ShaderAssetDesc description;
        description.version = 77;
        description.name = "unchanged";
        description.source = "unchanged.hlsl";
        description.passes.emplace_back();

        std::string error;
        Require(
            !LamaPon::ParseShaderAssetDesc(
                json,
                description,
                error),
            "an invalid shader manifest was accepted");
        Require(
            Contains(error, expectedError),
            "shader manifest validation returned the wrong error");
        Require(
            description.version == 77
                && description.name == "unchanged"
                && description.source == "unchanged.hlsl"
                && description.passes.size() == 1,
            "a failed parse modified the output description");
    }

    [[nodiscard]] std::string ManifestWithProperties(
        const std::string_view properties)
    {
        return std::string{
            R"json({"version":1,"name":"Properties","type":"material","source":"a.hlsl","properties":)json"
        } + std::string{ properties }
            + R"json(,"passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}}]})json";
    }

    class ShaderCacheStateGuard final
    {
    public:
        ShaderCacheStateGuard()
            : m_previous(LamaPon::IsShaderCacheEnabled())
        {
            // These checks must neither read nor write the user's shared
            // shader cache. Every compile therefore uses the fixture source.
            LamaPon::SetShaderCacheEnabled(false);
        }

        ~ShaderCacheStateGuard()
        {
            LamaPon::SetShaderCacheEnabled(m_previous);
        }

        ShaderCacheStateGuard(
            const ShaderCacheStateGuard&) = delete;
        ShaderCacheStateGuard& operator=(
            const ShaderCacheStateGuard&) = delete;

    private:
        bool m_previous{};
    };

    constexpr std::string_view ValidManifest = R"json(
{
  "version": 1,
  "name": "Custom/RetainedFields",
  "type": "screenEffect",
  "source": "custom-entry-screen-effect.hlsl",
  "properties": [
    { "name": "Intensity", "type": "float", "target": "0.x",
      "min": 0.0, "max": 2.0, "default": 1.0 },
    { "name": "Enabled", "type": "bool", "target": "0.y",
      "default": true },
    { "name": "Tint", "type": "color", "target": "1.rgb",
      "default": [1.0, 0.5, 0.25] },
    { "name": "OptionalValue", "type": "float" }
  ],
  "passes": [
    {
      "name": "Main",
      "vertex": { "entry": "ManifestVertex", "target": "vs_5_0" },
      "pixel": {
        "entry": "ManifestPixel",
        "target": "ps_5_0",
        "optional": false
      },
      "geometry": {
        "entry": "OptionalGeometry",
        "target": "gs_5_0",
        "optional": true
      },
      "hull": { "entry": "HullEntry", "target": "hs_5_0" },
      "domain": { "entry": "DomainEntry", "target": "ds_5_0" },
      "compute": { "entry": "ComputeEntry", "target": "cs_5_0" },
      "renderState": {
        "zTest": "Always",
        "zWrite": false,
        "cull": "Off",
        "blend": "Alpha"
      }
    }
  ]
}
)json";

    void TestSuccessfulParseAndRetention()
    {
        LamaPon::ShaderAssetDesc description;
        std::string error = "stale error";
        Require(
            LamaPon::ParseShaderAssetDesc(
                ValidManifest,
                description,
                error),
            "a valid screen-effect manifest must parse");
        Require(error.empty(), "success must clear the error");
        Require(
            description.version == 1
                && description.name == "Custom/RetainedFields"
                && description.type
                    == LamaPon::ShaderAssetType::ScreenEffect,
            "shader manifest identity was not retained");
        Require(
            description.source.generic_string()
                == "custom-entry-screen-effect.hlsl",
            "shader source was not retained");

        Require(
            description.properties.size() == 4,
            "shader property count was not retained");
        Require(
            description.properties[0].name == "Intensity"
                && description.properties[0].type == "float"
                && description.properties[0].target == "0.x"
                && description.properties[0].defaultValue == "1.0"
                && description.properties[0].minimum == 0.0
                && description.properties[0].maximum == 2.0
                && description.properties[1].target == "0.y"
                && description.properties[1].defaultValue == "true"
                && description.properties[2].target == "1.rgb"
                && description.properties[2].defaultValue
                    == "[1.0,0.5,0.25]"
                && description.properties[3].target.empty()
                && description.properties[3].defaultValue.empty()
                && !description.properties[3].minimum.has_value()
                && !description.properties[3].maximum.has_value(),
            "shader property fields or defaults were not retained");

        Require(
            description.passes.size() == 1
                && description.passes.front().name == "Main",
            "shader passes were not retained");
        const auto& pass = description.passes.front();
        const auto* vertex = LamaPon::FindShaderStage(
            pass,
            LamaPon::ShaderStage::Vertex);
        const auto* pixel = LamaPon::FindShaderStage(
            pass,
            LamaPon::ShaderStage::Pixel);
        const auto* geometry = LamaPon::FindShaderStage(
            pass,
            LamaPon::ShaderStage::Geometry);
        Require(
            vertex != nullptr
                && vertex->entryPoint == "ManifestVertex"
                && vertex->target == "vs_5_0"
                && !vertex->optional,
            "vertex fields or the optional=false default changed");
        Require(
            pixel != nullptr && !pixel->optional,
            "an explicit optional=false was not retained");
        Require(
            geometry != nullptr && geometry->optional,
            "an optional stage was not retained as optional");
        Require(
            LamaPon::FindShaderStage(
                pass,
                LamaPon::ShaderStage::Hull) != nullptr
                && LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Domain) != nullptr
                && LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Compute) != nullptr,
            "one or more supported shader stages were not recognized");
        Require(
            pass.renderState.zTest == "Always"
                && !pass.renderState.zWrite
                && pass.renderState.cull == "Off"
                && pass.renderState.blend == "Alpha",
            "render state fields were not retained");
    }

    void TestDefaults()
    {
        constexpr std::string_view manifest = R"json(
{
  "version": 1,
  "name": "Custom/Defaults",
  "type": "material",
  "source": "defaults.hlsl",
  "passes": [
    {
      "vertex": { "entry": "VSMain", "target": "vs_5_0" },
      "pixel": { "entry": "PSMain", "target": "ps_5_0" }
    }
  ]
}
)json";

        LamaPon::ShaderAssetDesc description;
        std::string error;
        Require(
            LamaPon::ParseShaderAssetDesc(
                manifest,
                description,
                error),
            "a manifest using defaults must parse");
        const auto& pass = description.passes.front();
        Require(
            pass.name.empty()
                && pass.role == LamaPon::ShaderPassRole::Forward,
            "pass name or default forward role changed");
        Require(
            pass.renderState.zTest == "LessEqual"
                && pass.renderState.zWrite
                && pass.renderState.cull == "Back"
                && pass.renderState.blend == "Opaque",
            "render state defaults changed");
        const auto* vertex = LamaPon::FindShaderStage(
            pass,
            LamaPon::ShaderStage::Vertex);
        Require(
            vertex != nullptr && !vertex->optional,
            "stage optional must default to false");
        Require(
            LamaPon::FindShaderStage(
                pass,
                LamaPon::ShaderStage::Geometry) == nullptr,
            "an absent optional stage must not be synthesized");
    }

    void TestPropertyValidation()
    {
        LamaPon::ShaderAssetDesc description;
        std::string error;
        Require(
            LamaPon::ParseShaderAssetDesc(
                ManifestWithProperties(R"json([
                  {"name":"Legacy","type":"vector","default":[1,2]},
                  {"name":"Amount","type":"float","target":"0.x","min":0,"max":2,"default":1},
                  {"name":"Enabled","type":"bool","target":"0.y","default":true},
                  {"name":"Tint","type":"color","target":"1.rgb","default":[1,0.5,0.25]},
                  {"name":"Offset","type":"vector","target":"2.zw","default":[3,4]},
                  {"name":"Mask","type":"texture","target":"t7"},
                  {"name":"Detail","type":"texture","target":"T10"}
                ])json"),
                description,
                error),
            "valid manifest properties must parse");
        Require(
            description.properties.size() == 7
                && description.properties[0].target.empty()
                && description.properties[1].minimum == 0.0
                && description.properties[1].maximum == 2.0
                && description.properties[6].target == "T10",
            "valid manifest property fields were not retained");

        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"matrix","target":"0.x"}])"),
            ".type must be one of");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":1}])"),
            ".target must be a string");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":""}])"),
            ".target must not be empty");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"8.x"}])"),
            "between 0 and 7");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"texture","target":"t6"}])"),
            "must be t7, t8, t9, or t10");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"texture","target":"t7x"}])"),
            "must be t7, t8, t9, or t10");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"t7"}])"),
            "is a texture slot");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.xy"}])"),
            "must select exactly 1");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"vector","target":"0.x"}])"),
            "must select 2 to 4");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"color","target":"0.xy"}])"),
            "must select 3 or 4");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"vector","target":"0.xq"}])"),
            "components must use");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"vector","target":"0.xr"}])"),
            "repeats the same component");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"A","type":"float","target":"0.x"},{"name":"B","type":"float","target":"0.r"}])"),
            "overlaps a constant component");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"A","type":"texture","target":"t7"},{"name":"B","type":"texture","target":"T7"}])"),
            "duplicates texture slot");

        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.x","default":true}])"),
            "default for type 'float'");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"bool","target":"0.x","default":1}])"),
            "default for type 'bool'");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"vector","target":"0.xy","default":[1]}])"),
            "must contain 2 number");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"color","target":"0.rgb","default":[1,"green",0]}])"),
            ".default[1] must be a JSON number");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"texture","target":"t7","default":"white"}])"),
            "default is not supported for type 'texture'");

        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.x","min":0}])"),
            ".min and .max must be specified together");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.x","min":"zero","max":1}])"),
            ".min must be a JSON number");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.x","min":1,"max":1}])"),
            ".max must be greater than .min");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"bool","target":"0.x","min":0,"max":1}])"),
            "only supported for type 'float'");
        RequireRejected(
            ManifestWithProperties(
                R"([{"name":"P","type":"float","target":"0.x","min":0,"max":1,"default":2}])"),
            ".default must be within");
    }

    void TestValidationFailures()
    {
        RequireRejected(
            R"({"version":2,"name":"A","type":"material","source":"a.hlsl","passes":[{}]})",
            "version 2");
        RequireRejected(
            R"({"version":1,"name":"","type":"material","source":"a.hlsl","passes":[{}]})",
            "name");
        RequireRejected(
            R"({"version":1,"name":"A","type":"invalid","source":"a.hlsl","passes":[{}]})",
            "type");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"","passes":[{}]})",
            "source");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"C:/outside.hlsl","passes":[{}]})",
            "asset-root-relative");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"C:outside.hlsl","passes":[{}]})",
            "asset-root-relative");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"/outside.hlsl","passes":[{}]})",
            "asset-root-relative");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"shaders/../outside.hlsl","passes":[{}]})",
            "must not contain '..'");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[]})",
            "passes");
        RequireRejected(
            R"({"version":1,"name":"A","type":"screenEffect","source":"a.hlsl","passes":[{"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "vertex stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"screenEffect","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"}}]})",
            "pixel stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"screenEffect","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0","optional":true},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "vertex stage must not be optional");
        RequireRejected(
            R"({"version":1,"name":"A","type":"screenEffect","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0","optional":true}}]})",
            "pixel stage must not be optional");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "material passes[0] requires a vertex stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"}}]})",
            "material passes[0] requires a pixel stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0","optional":true},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "vertex stage must not be optional");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0","optional":true}}]})",
            "pixel stage must not be optional");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"ps_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "'vs_' profile prefix");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"vs_5_0"}}]})",
            "'ps_' profile prefix");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"renderState":{"blend":"Multiply"}}]})",
            "renderState.blend");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"renderState":{"cull":"Sideways"}}]})",
            "renderState.cull");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"renderState":{"zTest":"Greater"}}]})",
            "renderState.zTest");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"renderState":{"zTest":"Always","zWrite":true}}]})",
            "zTest=Always requires zWrite=false");
        RequireRejected(
            R"({"version":1,"name":"A","type":"compute","source":"a.hlsl","passes":[{}]})",
            "compute stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"compute","source":"a.hlsl","passes":[{"compute":{"entry":"CS","target":"cs_5_0","optional":true}}]})",
            "non-optional compute stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"compute","source":"a.hlsl","passes":[{"pixel":{"entry":"PS","target":"ps_5_0"},"compute":{"entry":"CS","target":"cs_5_0"}}]})",
            "unsupported pixel stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"","target":"vs_5_0"}}]})",
            "entry");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":""}}]})",
            "target");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"fragment":{"entry":"PS","target":"ps_5_0"}}]})",
            "unknown stage 'fragment'");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"role":"lighting","vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            ".role must be one of");
        RequireRejected(
            R"({"version":1,"name":"A","type":"screenEffect","source":"a.hlsl","passes":[{"role":"forward","vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "role is only supported for material");
        RequireRejected(
            R"({"version":1,"name":"A","type":"compute","source":"a.hlsl","passes":[{"role":"forward","compute":{"entry":"CS","target":"cs_5_0"}}]})",
            "role is only supported for material");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"role":"skinned","vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}}]})",
            "at least one pass with role 'forward'");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"compute":{"entry":"CS","target":"cs_5_0"}}]})",
            "must not declare a compute stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"hull":{"entry":"HS","target":"hs_5_0"}}]})",
            "hull and domain stages together");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"},"hull":{"entry":"HS","target":"hs_5_0","optional":true},"domain":{"entry":"DS","target":"ds_5_0"}}]})",
            "must use the same optional value");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}},{"role":"occluded","vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"Hidden","target":"ps_5_0"}}]})",
            "role 'occluded' may only declare a pixel stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}},{"role":"occluded"}]})",
            "role 'occluded' requires a pixel stage");
        RequireRejected(
            R"({"version":1,"name":"A","type":"material","source":"a.hlsl","passes":[{"vertex":{"entry":"VS","target":"vs_5_0"},"pixel":{"entry":"PS","target":"ps_5_0"}},{"role":"occluded","pixel":{"entry":"Hidden","target":"ps_5_0","optional":true}}]})",
            "role 'occluded' pixel stage must not be optional");
        RequireRejected("{", "JSON parse failed");
    }

    void TestLoad(
        LamaPon::AssetManager& assets)
    {
        LamaPon::ShaderAssetDesc description;
        std::string error = "stale error";
        Require(
            LamaPon::LoadShaderAssetDesc(
                assets,
                "valid-screen-effect.lamashader.json",
                description,
                error),
            "the checked-in manifest fixture must load");
        Require(error.empty(), "a successful load must clear the error");
        Require(
            description.name == "Custom/FixtureEntryPoints"
                && description.source
                    == "custom-entry-screen-effect.hlsl",
            "the loaded fixture has the wrong fields");

        Require(
            LamaPon::LoadShaderAssetDesc(
                assets,
                "valid-material.lamashader.json",
                description,
                error)
                && description.passes.size() == 8
                && description.passes[0].role
                    == LamaPon::ShaderPassRole::Forward
                && description.passes[1].role
                    == LamaPon::ShaderPassRole::Forward
                && description.passes[2].role
                    == LamaPon::ShaderPassRole::Skinned
                && description.passes[3].role
                    == LamaPon::ShaderPassRole::Instanced
                && description.passes[4].role
                    == LamaPon::ShaderPassRole::Outline
                && description.passes[5].role
                    == LamaPon::ShaderPassRole::SkinnedOutline
                && description.passes[6].role
                    == LamaPon::ShaderPassRole::Occluded,
            "material pass roles or their declaration order were not "
            "retained");

        description.name = "unchanged";
        Require(
            !LamaPon::LoadShaderAssetDesc(
                assets,
                "does-not-exist.lamashader.json",
                description,
                error),
            "a missing manifest was accepted");
        Require(
            Contains(error, "does not exist"),
            "a missing manifest must have a distinct error");
        Require(
            description.name == "unchanged",
            "a missing manifest modified the output description");
    }

    void TestShaderProgram(
        ID3D11Device* device,
        LamaPon::AssetManager& assets)
    {
        LamaPon::ShaderPassDesc optionalPass;
        optionalPass.name = "OptionalFailure";
        optionalPass.stages = {
            {
                LamaPon::ShaderStage::Vertex,
                "ManifestVertex",
                "vs_5_0",
                false
            },
            {
                LamaPon::ShaderStage::Pixel,
                "ManifestPixel",
                "ps_5_0",
                false
            },
            {
                LamaPon::ShaderStage::Geometry,
                "OptionalGeometryDoesNotExist",
                "gs_5_0",
                true
            }
        };

        LamaPon::ShaderProgram program;
        std::string error = "stale error";
        Require(
            program.Compile(
                device,
                assets,
                "custom-entry-screen-effect.hlsl",
                optionalPass,
                error),
            "an optional stage compile failure must be skipped");
        Require(error.empty(), "optional stage failure leaked an error");
        Require(
            program.VertexShader() != nullptr
                && program.PixelShader() != nullptr
                && program.GeometryShader() == nullptr
                && program.VertexShaderByteCode() != nullptr,
            "ShaderProgram created the wrong stage set");

        auto* const previousVertex = program.VertexShader();
        auto* const previousPixel = program.PixelShader();
        auto* const previousVertexByteCode =
            program.VertexShaderByteCode();
        LamaPon::ShaderPassDesc requiredPass;
        requiredPass.name = "RequiredFailure";
        requiredPass.stages = {
            {
                LamaPon::ShaderStage::Vertex,
                "ManifestVertex",
                "vs_5_0",
                false
            },
            {
                LamaPon::ShaderStage::Pixel,
                "RequiredPixelDoesNotExist",
                "ps_5_0",
                false
            }
        };
        Require(
            !program.Compile(
                device,
                assets,
                "custom-entry-screen-effect.hlsl",
                requiredPass,
                error),
            "a required stage compile failure was ignored");
        Require(
            Contains(error, "required pixel")
                && Contains(error, "RequiredPixelDoesNotExist")
                && Contains(error, "ps_5_0"),
            "required stage error lacks actionable context");
        const std::string missingEntryError = error;
        Require(
            program.VertexShader() == previousVertex
                && program.PixelShader() == previousPixel
                && program.VertexShaderByteCode()
                    == previousVertexByteCode,
            "a failed rebuild replaced the last valid program");

        LamaPon::ShaderPassDesc invalidTargetPass;
        invalidTargetPass.name = "InvalidTarget";
        invalidTargetPass.stages = {
            {
                LamaPon::ShaderStage::Vertex,
                "ManifestVertex",
                "not_a_target",
                false
            }
        };
        Require(
            !program.Compile(
                device,
                assets,
                "custom-entry-screen-effect.hlsl",
                invalidTargetPass,
                error),
            "an invalid shader target was accepted");
        Require(
            Contains(error, "required vertex")
                && Contains(error, "ManifestVertex")
                && Contains(error, "not_a_target")
                && error != missingEntryError,
            "an invalid target lacks a distinct actionable error");

        LamaPon::ShaderPassDesc keywordPass;
        keywordPass.name = "KeywordDefines";
        keywordPass.stages = {
            {
                LamaPon::ShaderStage::Vertex,
                "CustomMaterialVertex",
                "vs_5_0",
                false
            },
            {
                LamaPon::ShaderStage::Pixel,
                "CustomMaterialPixel",
                "ps_5_0",
                false
            }
        };
        LamaPon::ShaderProgram keywordProgram;
        Require(
            !keywordProgram.Compile(
                device,
                assets,
                "custom-entry-material.hlsl",
                keywordPass,
                error),
            "a shader requiring a keyword compiled without its define");
        Require(
            keywordProgram.Compile(
                device,
                assets,
                "custom-entry-material.hlsl",
                keywordPass,
                error,
                { "MATERIAL_VARIANT" }),
            "ShaderProgram did not forward keyword defines");
        Require(
            keywordProgram.VertexShaderByteCode() != nullptr,
            "keyword compile did not retain vertex bytecode");
    }

    void TestMaterialManifestEntryPoints(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        LamaPon::AssetManager& assets)
    {
        // The source deliberately requires MATERIAL_VARIANT and has no fixed
        // legacy entries. Construction therefore proves custom entry-point,
        // role and keyword resolution together.
        LamaPon::LitEffect manifestEffect(
            device,
            context,
            assets,
            "valid-material.lamashader.json",
            false,
            { "MATERIAL_VARIANT" });
        Require(
            manifestEffect.IsManifestEffect()
                && manifestEffect.ColorPassCount() == 2
                && manifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Forward) == 2
                && manifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Instanced) == 1
                && manifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Outline) == 1
                && manifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Occluded) == 2
                && manifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Skinned) == 0,
            "static LitEffect compiled the wrong manifest role set");

        auto* const firstVertex =
            manifestEffect.ColorPassVertexShaderByteCode(0);
        const void* vertexByteCode{};
        std::size_t vertexByteCodeSize{};
        manifestEffect.GetVertexShaderBytecode(
            &vertexByteCode,
            &vertexByteCodeSize);
        Require(
            vertexByteCode != nullptr && vertexByteCodeSize != 0,
            "LitEffect did not retain the manifest vertex bytecode");
        Require(
            vertexByteCode == firstVertex->GetBufferPointer()
                && manifestEffect.SupportsInstancing()
                && manifestEffect.HasOutline()
                && manifestEffect.HasOccludedPass()
                && !manifestEffect.HasTessellation()
                && !manifestEffect.HasGeometryShader(),
            "the first manifest pass or special role capability is wrong");

        const auto& state = manifestEffect.RenderState();
        Require(
            state.declared
                && state.blend
                    == LamaPon::ShaderBlendMode::Premultiplied
                && state.cull == LamaPon::ShaderCullMode::None
                && !state.depthTest
                && !state.depthWrite,
            "manifest renderState was not converted case-insensitively");

        manifestEffect.SelectColorPass(1);
        const auto& secondState = manifestEffect.RenderState();
        Require(
            secondState.blend == LamaPon::ShaderBlendMode::Alpha
                && secondState.cull == LamaPon::ShaderCullMode::Front
                && secondState.depthTest
                && secondState.depthWrite
                && manifestEffect.HasGeometryShader()
                && manifestEffect.HasTessellation()
                && manifestEffect.SelectedPassHasGeometryShader(
                    LamaPon::ShaderPassRole::Forward)
                && manifestEffect.SelectedPassHasTessellation(
                    LamaPon::ShaderPassRole::Forward),
            "selecting the second color pass did not activate its program "
            "and render state");
        manifestEffect.GetVertexShaderBytecode(
            &vertexByteCode,
            &vertexByteCodeSize);
        Require(
            vertexByteCode
                == manifestEffect.ColorPassVertexShaderByteCode(1)
                    ->GetBufferPointer(),
            "GetVertexShaderBytecode did not follow the active color pass");

        manifestEffect.SetTessellationDrawEnabled(true);
        manifestEffect.Apply(context);
        Microsoft::WRL::ComPtr<ID3D11HullShader> boundHull;
        Microsoft::WRL::ComPtr<ID3D11DomainShader> boundDomain;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> boundGeometry;
        context->HSGetShader(
            boundHull.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        context->DSGetShader(
            boundDomain.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        context->GSGetShader(
            boundGeometry.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        Require(
            boundHull != nullptr
                && boundDomain != nullptr
                && boundGeometry != nullptr,
            "manifest geometry/tessellation stages were not bound");

        // Selecting a pass without optional programmable stages must also
        // unbind all stages left by the previous pass.
        manifestEffect.SetTessellationDrawEnabled(false);
        manifestEffect.SelectColorPass(0);
        manifestEffect.Apply(context);
        boundHull.Reset();
        boundDomain.Reset();
        boundGeometry.Reset();
        context->HSGetShader(
            boundHull.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        context->DSGetShader(
            boundDomain.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        context->GSGetShader(
            boundGeometry.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        Require(
            boundHull == nullptr
                && boundDomain == nullptr
                && boundGeometry == nullptr,
            "switching manifest passes leaked HS, DS, or GS state");

        manifestEffect.SelectPass(
            LamaPon::ShaderPassRole::Instanced,
            0);
        manifestEffect.SetInstancingEnabled(true);
        Require(
            manifestEffect.RenderState().blend
                == LamaPon::ShaderBlendMode::Additive
                && manifestEffect.InstancedVertexShaderByteCode()
                    != nullptr,
            "instanced role did not expose its program and render state");
        manifestEffect.GetVertexShaderBytecode(
            &vertexByteCode,
            &vertexByteCodeSize);
        Require(
            vertexByteCode
                == manifestEffect.InstancedVertexShaderByteCode()
                    ->GetBufferPointer(),
            "active bytecode did not switch to the instanced role");
        manifestEffect.SetInstancingEnabled(false);

        manifestEffect.SelectPass(
            LamaPon::ShaderPassRole::Outline,
            0);
        Require(
            !manifestEffect.SelectedPassHasGeometryShader(
                LamaPon::ShaderPassRole::Outline)
                && !manifestEffect.SelectedPassHasTessellation(
                    LamaPon::ShaderPassRole::Outline),
            "outline role reported stages that it does not declare");
        manifestEffect.ApplyOutline(context);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> outlineVertex;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> outlinePixel;
        context->VSGetShader(
            outlineVertex.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        context->PSGetShader(
            outlinePixel.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        Require(
            outlineVertex != nullptr && outlinePixel != nullptr,
            "ApplyOutline did not bind the manifest outline role");

        manifestEffect.SelectPass(
            LamaPon::ShaderPassRole::Occluded,
            0);
        manifestEffect.SelectColorPass(0);
        Require(
            manifestEffect.SelectedPassVertexShaderByteCode(
                LamaPon::ShaderPassRole::Occluded)
                == manifestEffect.ColorPassVertexShaderByteCode(0),
            "pixel-only occluded role did not expose the primary VS "
            "signature");
        manifestEffect.Apply(context);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> primaryVertex;
        context->VSGetShader(
            primaryVertex.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        // Occluded must use primary index 0 even if a later color pass was
        // selected immediately before it.
        manifestEffect.SelectColorPass(1);
        manifestEffect.ApplyOccluded(context);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> occludedVertex;
        context->VSGetShader(
            occludedVertex.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> firstOccludedPixel;
        context->PSGetShader(
            firstOccludedPixel.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        manifestEffect.SelectPass(
            LamaPon::ShaderPassRole::Occluded,
            1);
        manifestEffect.ApplyOccluded(context);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> secondOccludedPixel;
        context->PSGetShader(
            secondOccludedPixel.ReleaseAndGetAddressOf(),
            nullptr,
            nullptr);
        Require(
            primaryVertex != nullptr
                && occludedVertex.Get() == primaryVertex.Get()
                && firstOccludedPixel != nullptr
                && secondOccludedPixel != nullptr
                && firstOccludedPixel.Get()
                    != secondOccludedPixel.Get(),
            "multiple occluded role passes were not independently "
            "selectable");

        RequireThrowsContaining(
            [&]
            {
                manifestEffect.SelectColorPass(2);
            },
            "out of range",
            "an out-of-range material pass selection was accepted");

        LamaPon::LitEffect skinnedManifestEffect(
            device,
            context,
            assets,
            "valid-material.lamashader.json",
            true,
            { "MATERIAL_VARIANT" });
        Require(
            skinnedManifestEffect.IsManifestEffect()
                && skinnedManifestEffect.IsSkinned()
                && skinnedManifestEffect.ColorPassCount() == 1
                && skinnedManifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Skinned) == 1
                && skinnedManifestEffect.PassCount(
                    LamaPon::ShaderPassRole::SkinnedOutline) == 1
                && skinnedManifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Occluded) == 2
                && skinnedManifestEffect.PassCount(
                    LamaPon::ShaderPassRole::Forward) == 0
                && !skinnedManifestEffect.SupportsInstancing()
                && skinnedManifestEffect.HasOutline(),
            "skinned LitEffect compiled the wrong manifest role set");
        skinnedManifestEffect.Apply(context);
        skinnedManifestEffect.ApplyOutline(context);
        skinnedManifestEffect.ApplyOccluded(context);

        // A direct HLSL still takes the legacy VSMain/PSMain path and probes
        // its fixed optional entries.
        LamaPon::LitEffect legacyEffect(
            device,
            context,
            assets,
            "legacy-material.hlsl");
        Require(
            !legacyEffect.IsManifestEffect()
                && legacyEffect.ColorPassCount() == 1
                && legacyEffect.PassCount(
                    LamaPon::ShaderPassRole::Forward) == 1
                && legacyEffect.SupportsInstancing(),
            "direct HLSL no longer probes VSInstancedMain");

        RequireThrowsContaining(
            [&]
            {
                LamaPon::LitEffect invalid(
                    device,
                    context,
                    assets,
                    "invalid-material-render-state.lamashader.json",
                    false,
                    { "MATERIAL_VARIANT" });
            },
            "renderState.blend",
            "an unknown manifest blend mode was accepted");
        RequireThrowsContaining(
            [&]
            {
                LamaPon::LitEffect invalid(
                    device,
                    context,
                    assets,
                    "invalid-material-depth-state.lamashader.json",
                    false,
                    { "MATERIAL_VARIANT" });
            },
            "zTest=Always requires zWrite=false",
            "Always with depth writes enabled was accepted");
        RequireThrowsContaining(
            [&]
            {
                LamaPon::LitEffect invalid(
                    device,
                    context,
                    assets,
                    "unsupported-material-stage.lamashader.json",
                    false,
                    { "MATERIAL_VARIANT" });
            },
            "must not declare a compute stage",
            "a material manifest compute stage was accepted");
        bool geometryInputDiagnosed = false;
        try
        {
            LamaPon::LitEffect invalid(
                device,
                context,
                assets,
                "invalid-material-geometry-input.lamashader.json",
                false,
                { "MATERIAL_VARIANT" });
        }
        catch (const std::exception& exception)
        {
            const std::string diagnostic = exception.what();
            geometryInputDiagnosed =
                Contains(diagnostic, "CustomMaterialPointGeometry")
                && Contains(diagnostic, "must take triangle input");
        }
        Require(
            geometryInputDiagnosed,
            "a point-input manifest geometry stage was accepted or lacked "
            "an actionable diagnostic");
        RequireThrowsContaining(
            [&]
            {
                LamaPon::LitEffect invalid(
                    device,
                    context,
                    assets,
                    "valid-screen-effect.lamashader.json");
            },
            "type is not 'material'",
            "LitEffect accepted a screenEffect manifest");
        RequireThrowsContaining(
            [&]
            {
                LamaPon::LitEffect invalid(
                    device,
                    context,
                    assets,
                    "invalid-material-render-state.lamashader.json",
                    true,
                    { "MATERIAL_VARIANT" });
            },
            "renderState.blend",
            "an invalid forward-only manifest was accepted for a skinned "
            "effect");
    }

    void TestScreenEffectEntryPoints(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        LamaPon::AssetManager& assets)
    {
        // The manifest fixture deliberately has no VSMain or PSMain, so
        // construction can only succeed when its custom entries are used.
        LamaPon::ScreenEffect manifestEffect(
            device,
            context,
            assets,
            "valid-screen-effect.lamashader.json");

        // This fixture has only VSMain and PSMain, proving direct .hlsl paths
        // retain the legacy fixed-entry fallback.
        LamaPon::ScreenEffect legacyEffect(
            device,
            context,
            assets,
            "legacy-screen-effect.hlsl");
        static_cast<void>(manifestEffect);
        static_cast<void>(legacyEffect);
    }

    void TestComputeManifestEntryPoints(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        LamaPon::AssetManager& assets)
    {
        constexpr std::uint32_t width = 3;
        constexpr std::uint32_t height = 2;
        D3D11_TEXTURE2D_DESC outputDescription{};
        outputDescription.Width = width;
        outputDescription.Height = height;
        outputDescription.MipLevels = 1;
        outputDescription.ArraySize = 1;
        outputDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        outputDescription.SampleDesc.Count = 1;
        outputDescription.Usage = D3D11_USAGE_DEFAULT;
        outputDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
        Require(
            SUCCEEDED(device->CreateTexture2D(
                &outputDescription,
                nullptr,
                outputTexture.ReleaseAndGetAddressOf())),
            "compute manifest output texture creation failed");
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputView;
        Require(
            SUCCEEDED(device->CreateUnorderedAccessView(
                outputTexture.Get(),
                nullptr,
                outputView.ReleaseAndGetAddressOf())),
            "compute manifest output UAV creation failed");

        auto dispatchAndRequireColor =
            [&](const std::filesystem::path& shaderPath,
                const DirectX::XMFLOAT4& expected)
        {
            LamaPon::ComputeEffect effect(
                device,
                context,
                assets,
                shaderPath);
            LamaPon::ComputeEffect::CustomParameters parameters{};
            parameters[0] = expected;
            effect.Dispatch(
                { nullptr, nullptr },
                outputView.Get(),
                width,
                height,
                parameters);

            auto stagingDescription = outputDescription;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            Require(
                SUCCEEDED(device->CreateTexture2D(
                    &stagingDescription,
                    nullptr,
                    staging.ReleaseAndGetAddressOf())),
                "compute manifest staging texture creation failed");
            context->CopyResource(staging.Get(), outputTexture.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            Require(
                SUCCEEDED(context->Map(
                    staging.Get(),
                    0,
                    D3D11_MAP_READ,
                    0,
                    &mapped)),
                "compute manifest output readback failed");
            const auto* pixel = static_cast<const float*>(mapped.pData);
            const bool matches =
                std::abs(pixel[0] - expected.x) < 0.0001f
                && std::abs(pixel[1] - expected.y) < 0.0001f
                && std::abs(pixel[2] - expected.z) < 0.0001f
                && std::abs(pixel[3] - 1.0f) < 0.0001f;
            context->Unmap(staging.Get(), 0);
            Require(
                matches,
                "compute effect did not write the expected output color");
        };

        // The first manifest pass is only an optional probe. The source has
        // no CSMain, proving selection of the first non-optional compute
        // declaration and use of its custom entry point.
        dispatchAndRequireColor(
            "valid-compute.lamashader.json",
            { 0.25f, 0.5f, 0.75f, 0.0f });

        // Direct HLSL remains on the legacy CSMain/cs_5_0 path.
        dispatchAndRequireColor(
            "legacy-compute.hlsl",
            { 0.75f, 0.5f, 0.25f, 0.0f });

        RequireThrowsContaining(
            [&]
            {
                LamaPon::ComputeEffect invalid(
                    device,
                    context,
                    assets,
                    "valid-screen-effect.lamashader.json");
            },
            "type is 'compute'",
            "ComputeEffect accepted a screenEffect manifest");
        RequireThrowsContaining(
            [&]
            {
                LamaPon::ComputeEffect invalid(
                    device,
                    context,
                    assets,
                    "invalid-compute-mixed-stage.lamashader.json");
            },
            "unsupported pixel stage",
            "ComputeEffect accepted a mixed graphics/compute pass");

        bool compileFailureDiagnosed = false;
        try
        {
            LamaPon::ComputeEffect invalid(
                device,
                context,
                assets,
                "invalid-compute-entry.lamashader.json");
        }
        catch (const std::exception& exception)
        {
            const std::string diagnostic = exception.what();
            compileFailureDiagnosed =
                Contains(diagnostic, "required compute")
                && Contains(
                    diagnostic,
                    "RequiredComputeDoesNotExist")
                && Contains(diagnostic, "cs_5_0");
        }
        Require(
            compileFailureDiagnosed,
            "required compute failure omitted its entry or target");
    }

    void RunTests()
    {
        TestSuccessfulParseAndRetention();
        TestDefaults();
        TestPropertyValidation();
        TestValidationFailures();

        const std::filesystem::path fixtureRoot{
            LAMAPON_SHADER_MANIFEST_FIXTURE_ROOT
        };
        Require(
            std::filesystem::is_directory(fixtureRoot),
            "shader manifest fixture root does not exist");

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel{};
        const HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(),
            &featureLevel,
            context.ReleaseAndGetAddressOf());
        Require(
            SUCCEEDED(result),
            "D3D11CreateDevice(WARP) failed");
        Require(
            featureLevel >= D3D_FEATURE_LEVEL_11_0,
            "the WARP device does not support Shader Model 5");

        LamaPon::AssetManager assets(device.Get(), context.Get());
        assets.SetAssetRoot(fixtureRoot);
        TestLoad(assets);
        TestShaderProgram(device.Get(), assets);
        TestMaterialManifestEntryPoints(
            device.Get(),
            context.Get(),
            assets);
        TestScreenEffectEntryPoints(
            device.Get(),
            context.Get(),
            assets);
        TestComputeManifestEntryPoints(
            device.Get(),
            context.Get(),
            assets);
    }
}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);

    int status = 0;
    try
    {
        ShaderCacheStateGuard cacheGuard;
        Require(
            !LamaPon::IsShaderCacheEnabled(),
            "shader disk caching was not disabled for this test");
        RunTests();
        std::cout << "Shader manifest tests passed.\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Shader manifest test failed: "
                  << exception.what() << '\n';
        status = 1;
    }

    if (uninitialize)
    {
        CoUninitialize();
    }
    return status;
}
