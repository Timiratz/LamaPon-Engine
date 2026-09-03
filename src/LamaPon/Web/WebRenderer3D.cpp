#include "LamaPon/Web/WebRenderer3D.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LamaPon::Web
{
    namespace
    {
        constexpr char VertexShaderSourceWebGL2[] = R"glsl(
#version 300 es
precision highp float;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vWorldNormal;
out vec2 vUv;
out vec3 vWorldPosition;

void main()
{
    mat3 model3 = mat3(uModel);
    vec3 cofactor0 = cross(model3[1], model3[2]);
    vec3 cofactor1 = cross(model3[2], model3[0]);
    vec3 cofactor2 = cross(model3[0], model3[1]);
    float determinant = dot(model3[0], cofactor0);
    mat3 normalMatrix = abs(determinant) > 0.000001
        ? mat3(cofactor0, cofactor1, cofactor2) / determinant
        : mat3(1.0);
    vWorldNormal = normalMatrix * aNormal;
    vUv = aUv;
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    gl_Position = uProjection * uView * worldPosition;
}
)glsl";

        constexpr char FragmentShaderSourceWebGL2[] = R"glsl(
#version 300 es
precision mediump float;

in vec3 vWorldNormal;
in vec3 vWorldPosition;
uniform vec4 uColor;
uniform vec3 uLightDirection;
uniform vec3 uAmbientColor;
uniform float uAmbientIntensity;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform float uRoughness;
uniform float uMetallic;
uniform vec3 uDielectricSpecular;
uniform sampler2D uTexture;
uniform float uUseTexture;
uniform float uAlphaCutoff;
uniform sampler2D uNormalTexture;
uniform float uUseNormalTexture;
uniform float uNormalStrength;
uniform sampler2D uMetallicRoughnessTexture;
uniform float uUseMetallicRoughnessTexture;
uniform sampler2D uRoughnessTexture;
uniform float uUseRoughnessTexture;
uniform sampler2D uMetallicTexture;
uniform float uUseMetallicTexture;
uniform sampler2D uOcclusionTexture;
uniform float uUseOcclusionTexture;
uniform float uOcclusionStrength;
uniform sampler2D uEmissiveTexture;
uniform float uUseEmissiveTexture;
uniform vec3 uEmissiveColor;
uniform float uUnlit;
uniform vec3 uCameraPosition;
uniform vec4 uFogColor;
uniform vec2 uFogRange;
uniform float uFogEnabled;
out vec4 outColor;

void main()
{
    vec3 normal = normalize(vWorldNormal);
    if (uUseNormalTexture > 0.5)
    {
        vec3 positionDx = dFdx(vWorldPosition);
        vec3 positionDy = dFdy(vWorldPosition);
        vec2 uvDx = dFdx(vUv);
        vec2 uvDy = dFdy(vUv);
        vec3 tangent = positionDx * uvDy.y - positionDy * uvDx.y;
        vec3 bitangent = -positionDx * uvDy.x + positionDy * uvDx.x;
        float basisLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
        if (basisLength > 0.000001)
        {
            float inverseBasisLength = inversesqrt(basisLength);
            tangent *= inverseBasisLength;
            bitangent *= inverseBasisLength;
            vec3 mapped = texture(uNormalTexture, vUv).xyz * 2.0 - 1.0;
            mapped.xy *= uNormalStrength;
            normal = normalize(
                tangent * mapped.x + bitangent * mapped.y + normal * mapped.z);
        }
    }
    vec4 sampled = mix(vec4(1.0), texture(uTexture, vUv), uUseTexture);
    vec4 materialSample = mix(
        vec4(1.0),
        texture(uMetallicRoughnessTexture, vUv),
        uUseMetallicRoughnessTexture);
    float roughness = clamp(uRoughness * materialSample.g, 0.04, 1.0);
    float metallic = clamp(uMetallic * materialSample.b, 0.0, 1.0);
    roughness = clamp(roughness * mix(
        1.0, texture(uRoughnessTexture, vUv).g, uUseRoughnessTexture),
        0.04, 1.0);
    metallic = clamp(metallic * mix(
        1.0, texture(uMetallicTexture, vUv).b, uUseMetallicTexture),
        0.0, 1.0);
    float occlusion = mix(
        1.0,
        mix(1.0, texture(uOcclusionTexture, vUv).r, uOcclusionStrength),
        uUseOcclusionTexture);
    vec3 emissive = uEmissiveColor * mix(
        vec3(1.0), texture(uEmissiveTexture, vUv).rgb, uUseEmissiveTexture);
    vec4 surface = vec4(sampled.rgb * uColor.rgb, sampled.a * uColor.a);
    if (uAlphaCutoff >= 0.0 && surface.a < uAlphaCutoff)
        discard;
    float diffuse = max(dot(normal, normalize(-uLightDirection)), 0.0);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 halfDirection = normalize(viewDirection - normalize(uLightDirection));
    float specularPower = mix(128.0, 8.0, roughness);
    float specular = pow(max(dot(normal, halfDirection), 0.0), specularPower);
    vec3 diffuseColor = surface.rgb * (1.0 - metallic);
    vec3 specularColor = mix(uDielectricSpecular, surface.rgb, metallic);
    vec3 litColor = diffuseColor * (
        uAmbientColor * uAmbientIntensity * occlusion
        + uDirectionalColor * uDirectionalIntensity * diffuse)
        + specularColor * specular * uDirectionalColor
            * uDirectionalIntensity + emissive;
    litColor = mix(litColor, surface.rgb + emissive, uUnlit);
    float fogSpan = max(uFogRange.y - uFogRange.x, 0.0001);
    float fogAmount = clamp(
        (distance(vWorldPosition, uCameraPosition) - uFogRange.x) / fogSpan,
        0.0,
        1.0) * uFogEnabled;
    outColor = vec4(mix(litColor, uFogColor.rgb, fogAmount), surface.a);
}
)glsl";

        constexpr char VertexShaderSourceWebGL1[] = R"glsl(
precision highp float;

attribute vec3 aPosition;
attribute vec3 aNormal;
attribute vec2 aUv;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

varying vec3 vWorldNormal;
varying vec2 vUv;
varying vec3 vWorldPosition;

void main()
{
    mat3 model3 = mat3(uModel);
    vec3 cofactor0 = cross(model3[1], model3[2]);
    vec3 cofactor1 = cross(model3[2], model3[0]);
    vec3 cofactor2 = cross(model3[0], model3[1]);
    float determinant = dot(model3[0], cofactor0);
    mat3 normalMatrix = abs(determinant) > 0.000001
        ? mat3(cofactor0, cofactor1, cofactor2) / determinant
        : mat3(1.0);
    vWorldNormal = normalMatrix * aNormal;
    vUv = aUv;
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    gl_Position = uProjection * uView * worldPosition;
}
)glsl";

        constexpr char FragmentShaderSourceWebGL1[] = R"glsl(
precision mediump float;

varying vec3 vWorldNormal;
varying vec2 vUv;
varying vec3 vWorldPosition;
uniform vec4 uColor;
uniform vec3 uLightDirection;
uniform vec3 uAmbientColor;
uniform float uAmbientIntensity;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform float uRoughness;
uniform float uMetallic;
uniform vec3 uDielectricSpecular;
uniform sampler2D uTexture;
uniform float uUseTexture;
uniform float uAlphaCutoff;
uniform sampler2D uMetallicRoughnessTexture;
uniform float uUseMetallicRoughnessTexture;
uniform sampler2D uRoughnessTexture;
uniform float uUseRoughnessTexture;
uniform sampler2D uMetallicTexture;
uniform float uUseMetallicTexture;
uniform sampler2D uOcclusionTexture;
uniform float uUseOcclusionTexture;
uniform float uOcclusionStrength;
uniform sampler2D uEmissiveTexture;
uniform float uUseEmissiveTexture;
uniform vec3 uEmissiveColor;
uniform float uUnlit;
uniform vec3 uCameraPosition;
uniform vec4 uFogColor;
uniform vec2 uFogRange;
uniform float uFogEnabled;

void main()
{
    vec3 normal = normalize(vWorldNormal);
    vec4 sampled = mix(vec4(1.0), texture2D(uTexture, vUv), uUseTexture);
    vec4 materialSample = mix(
        vec4(1.0),
        texture2D(uMetallicRoughnessTexture, vUv),
        uUseMetallicRoughnessTexture);
    float roughness = clamp(uRoughness * materialSample.g, 0.04, 1.0);
    float metallic = clamp(uMetallic * materialSample.b, 0.0, 1.0);
    roughness = clamp(roughness * mix(
        1.0, texture2D(uRoughnessTexture, vUv).g, uUseRoughnessTexture),
        0.04, 1.0);
    metallic = clamp(metallic * mix(
        1.0, texture2D(uMetallicTexture, vUv).b, uUseMetallicTexture),
        0.0, 1.0);
    float occlusion = mix(
        1.0,
        mix(1.0, texture2D(uOcclusionTexture, vUv).r, uOcclusionStrength),
        uUseOcclusionTexture);
    vec3 emissive = uEmissiveColor * mix(
        vec3(1.0), texture2D(uEmissiveTexture, vUv).rgb, uUseEmissiveTexture);
    vec4 surface = vec4(sampled.rgb * uColor.rgb, sampled.a * uColor.a);
    if (uAlphaCutoff >= 0.0 && surface.a < uAlphaCutoff)
        discard;
    float diffuse = max(dot(normal, normalize(-uLightDirection)), 0.0);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 halfDirection = normalize(viewDirection - normalize(uLightDirection));
    float specularPower = mix(128.0, 8.0, roughness);
    float specular = pow(max(dot(normal, halfDirection), 0.0), specularPower);
    vec3 diffuseColor = surface.rgb * (1.0 - metallic);
    vec3 specularColor = mix(uDielectricSpecular, surface.rgb, metallic);
    vec3 litColor = diffuseColor * (
        uAmbientColor * uAmbientIntensity * occlusion
        + uDirectionalColor * uDirectionalIntensity * diffuse)
        + specularColor * specular * uDirectionalColor
            * uDirectionalIntensity + emissive;
    litColor = mix(litColor, surface.rgb + emissive, uUnlit);
    float fogSpan = max(uFogRange.y - uFogRange.x, 0.0001);
    float fogAmount = clamp(
        (distance(vWorldPosition, uCameraPosition) - uFogRange.x) / fogSpan,
        0.0,
        1.0) * uFogEnabled;
    gl_FragColor = vec4(mix(litColor, uFogColor.rgb, fogAmount), surface.a);
}
)glsl";

        EM_JS(void, BrowserSetRendererBackend, (int version), {
            if (!document.body) return;
            document.body.dataset.lamaponRenderer = version >= 2
                ? "webgl2" : version == 1 ? "webgl1" : "canvas2d";
        });

        // Canvas2DはWebGL2経路の代替ではなくCompatibility Rendererです。
        // WebGL2が無効なBrowserや埋め込みPreviewでも、同じC++ Camera、Mesh、
        // Physics、Input、Audio Loopを維持してGameを実行可能にします。
        EM_JS(int, Canvas2DInitialize,
              (const char* selector, int width, int height), {
            const primaryCanvas = document.querySelector(UTF8ToString(selector));
            // 一部BrowserではWebGL Context作成に失敗したCanvasを2D Contextへ
            // 切り替えられません。専用Software Surfaceを用意し、空のSVGや
            // 半端に初期化されたWebGL CanvasにならないFallbackを保証します。
            const canvas = document.querySelector("#software-canvas") || primaryCanvas;
            if (!canvas) {
                console.error("LamaPon Canvas2D fallback: canvas not found");
                return 0;
            }
            const context = typeof canvas.getContext === "function"
                ? canvas.getContext("2d")
                : null;
            const svg = document.querySelector("#software3d");
            if (!context && !svg) {
                console.error("LamaPon software renderer: no Canvas2D or SVG target");
                return 0;
            }
            canvas.width = Math.max(1, width);
            canvas.height = Math.max(1, height);
            if (svg) {
                svg.setAttribute(
                    "viewBox",
                    "0 0 " + Math.max(1, width) + " " + Math.max(1, height));
            }
            globalThis.__lamaponCanvas2D = {
                canvas,
                context,
                svg,
                mode: context ? "canvas" : "svg",
                queue: [],
                fog: {
                    enabled: false,
                    color: [0.74, 0.60, 0.52],
                    start: 110.0,
                    end: 520.0,
                },
                sky: {
                    enabled: false,
                    top: [0.722, 0.620, 0.572],
                    horizon: [0.879, 0.780, 0.561],
                },
            };
            if (document.body) {
                document.body.dataset.lamaponRenderer = "canvas2d";
            }
            if (context) {
                canvas.style.display = "block";
                if (primaryCanvas) primaryCanvas.style.display = "none";
                if (svg) svg.style.display = "none";
            } else if (svg) {
                canvas.style.display = "none";
                svg.style.display = "block";
                console.info("LamaPon software renderer: SVG fallback active");
            }
            return 1;
        });

        EM_JS(void, Canvas2DResize, (int width, int height), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.queue = [];
            runtime.canvas.width = Math.max(1, width);
            runtime.canvas.height = Math.max(1, height);
            if (runtime.svg) {
                runtime.svg.setAttribute(
                    "viewBox",
                    "0 0 " + Math.max(1, width) + " " + Math.max(1, height));
            }
            if (document.body) {
                document.body.dataset.lamaponRenderResolution =
                    String(Math.max(1, width)) + "x"
                    + String(Math.max(1, height));
            }
        });

        EM_JS(void, Canvas2DBeginFrame,
              (float red, float green, float blue, float alpha), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.queue = [];
            const clamp = value => Math.max(0.0, Math.min(1.0, value));
            if (runtime.mode === "svg" && runtime.svg) {
                while (runtime.svg.firstChild) {
                    runtime.svg.removeChild(runtime.svg.firstChild);
                }
                if (runtime.sky?.enabled) {
                    const rgb = color => "rgb(" +
                        Math.round(clamp(color[0]) * 255) + "," +
                        Math.round(clamp(color[1]) * 255) + "," +
                        Math.round(clamp(color[2]) * 255) + ")";
                    const mix = amount => runtime.sky.top.map(
                        (value, index) => value +
                            (runtime.sky.horizon[index] - value) * amount);
                    runtime.svg.style.background = "linear-gradient(" +
                        rgb(runtime.sky.top) + " 0%," +
                        rgb(mix(0.05)) + " 3.2%," +
                        rgb(mix(0.30)) + " 9.7%," +
                        rgb(mix(0.60)) + " 19.5%," +
                        rgb(mix(0.825)) + " 29.2%," +
                        rgb(mix(0.925)) + " 35%," +
                        rgb(runtime.sky.horizon) + " 45.4%," +
                        rgb(runtime.sky.horizon) + " 100%)";
                } else {
                    runtime.svg.style.background = "rgb(" +
                        Math.round(clamp(red) * 255) + "," +
                        Math.round(clamp(green) * 255) + "," +
                        Math.round(clamp(blue) * 255) + ")";
                }
                return;
            }
            const context = runtime.context;
            context.setTransform(1, 0, 0, 1, 0, 0);
            if (runtime.sky?.enabled) {
                const gradient = context.createLinearGradient(
                    0, 0, 0, runtime.canvas.height);
                const rgb = color => "rgb(" +
                    Math.round(clamp(color[0]) * 255) + "," +
                    Math.round(clamp(color[1]) * 255) + "," +
                    Math.round(clamp(color[2]) * 255) + ")";
                const mix = amount => runtime.sky.top.map(
                    (value, index) => value +
                        (runtime.sky.horizon[index] - value) * amount);
                // Native版CarGameの1280x720 Sky出力から採取したSampleです。
                gradient.addColorStop(0.0, rgb(runtime.sky.top));
                gradient.addColorStop(0.032, rgb(mix(0.05)));
                gradient.addColorStop(0.097, rgb(mix(0.30)));
                gradient.addColorStop(0.195, rgb(mix(0.60)));
                gradient.addColorStop(0.292, rgb(mix(0.825)));
                gradient.addColorStop(0.35, rgb(mix(0.925)));
                gradient.addColorStop(0.454, rgb(runtime.sky.horizon));
                gradient.addColorStop(1.0, rgb(runtime.sky.horizon));
                context.fillStyle = gradient;
            } else {
                context.fillStyle = "rgba(" +
                    Math.round(clamp(red) * 255) + "," +
                    Math.round(clamp(green) * 255) + "," +
                    Math.round(clamp(blue) * 255) + "," + clamp(alpha) + ")";
            }
            context.fillRect(0, 0, runtime.canvas.width, runtime.canvas.height);
        });

        EM_JS(void, Canvas2DSetFog,
              (int enabled, float red, float green, float blue,
               float startDistance, float endDistance), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.fog = {
                enabled: !!enabled,
                color: [red, green, blue],
                start: Math.max(0.0, startDistance),
                end: Math.max(startDistance + 0.001, endDistance),
            };
        });

        EM_JS(void, Canvas2DSetSky,
              (int enabled,
               float topRed, float topGreen, float topBlue,
               float horizonRed, float horizonGreen, float horizonBlue), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.sky = {
                enabled: !!enabled,
                top: [topRed, topGreen, topBlue],
                horizon: [horizonRed, horizonGreen, horizonBlue],
            };
        });

        EM_JS(void, Canvas2DQueueTriangles,
              (const float* vertices, int floatCount,
               float red, float green, float blue, float alpha,
               int textureId, int alphaBlended, float alphaCutoff,
               int additiveBlend), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime || !vertices || floatCount < 32) return;
            const values = HEAPF32.subarray(
                vertices >> 2,
                (vertices >> 2) + floatCount);
            runtime.queue.push({
                values: new Float32Array(values),
                red, green, blue, alpha, textureId,
                alphaBlended: !!alphaBlended, alphaCutoff,
                additiveBlend: !!additiveBlend,
            });
        });

        EM_JS(void, Canvas2DEndFrame, (), {
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime || !runtime.queue) return;
            const rasterStarted = performance.now();
            const clamp = value => Math.max(0.0, Math.min(1.0, value));
            const triangleStride = 32;
            const triangles = [];
            for (const command of runtime.queue) {
                for (let index = 0;
                     index + triangleStride - 1 < command.values.length;
                     index += triangleStride) {
                    triangles.push({
                        depth: command.values[index + 15],
                        command,
                        index,
                    });
                }
            }
            // HairpinではTrack Chunk同士がWorld Spaceで重なる場合があります。
            // 不透明な背景Texture TriangleをView Distance順にまとめて並べ替え、
            // Skid、Smoke、CarのCommandより前へ戻します。Depth Bufferなしでも
            // Chunk間のOcclusionを安定させ、Roadが手前のCarを上書きしない順序です。
            const opaqueTexturedTriangles = triangles.filter(triangle =>
                triangle.command.textureId != 0
                && !triangle.command.alphaBlended);
            if (opaqueTexturedTriangles.length > 0) {
                opaqueTexturedTriangles.sort((left, right) =>
                    right.command.values[right.index + 16]
                    - left.command.values[left.index + 16]);
                const orderedTriangles = [];
                let insertedOpaqueTextures = false;
                for (const triangle of triangles) {
                    const opaqueTexture = triangle.command.textureId != 0
                        && !triangle.command.alphaBlended;
                    if (opaqueTexture) {
                        if (!insertedOpaqueTextures) {
                            orderedTriangles.push(...opaqueTexturedTriangles);
                            insertedOpaqueTextures = true;
                        }
                        continue;
                    }
                    orderedTriangles.push(triangle);
                }
                triangles.length = 0;
                triangles.push(...orderedTriangles);
            }
            if (runtime.mode === "svg" && runtime.svg) {
                const namespace = "http://www.w3.org/2000/svg";
                for (const triangle of triangles) {
                    const command = triangle.command;
                    const values = command.values;
                    const index = triangle.index;
                    const lightRed = Math.max(0.0, values[index + 6]);
                    const lightGreen = Math.max(0.0, values[index + 7]);
                    const lightBlue = Math.max(0.0, values[index + 8]);
                    const fog = runtime.fog;
                    const fogAmount = fog.enabled
                        ? clamp((values[index + 16] - fog.start)
                            / Math.max(0.001, fog.end - fog.start))
                        : 0.0;
                    const mixedRed = command.red * lightRed
                        + (fog.color[0] - command.red * lightRed) * fogAmount;
                    const mixedGreen = command.green * lightGreen
                        + (fog.color[1] - command.green * lightGreen) * fogAmount;
                    const mixedBlue = command.blue * lightBlue
                        + (fog.color[2] - command.blue * lightBlue) * fogAmount;
                    const polygon = document.createElementNS(namespace, "polygon");
                    polygon.setAttribute(
                        "points",
                        values[index] + "," + values[index + 1] + " " +
                        values[index + 2] + "," + values[index + 3] + " " +
                        values[index + 4] + "," + values[index + 5]);
                    const fillColor =
                        "rgba(" + Math.round(clamp(mixedRed) * 255) + "," +
                        Math.round(clamp(mixedGreen) * 255) + "," +
                        Math.round(clamp(mixedBlue) * 255) + "," +
                        clamp(command.alpha) + ")";
                    polygon.setAttribute("fill", fillColor);
                    polygon.setAttribute("stroke", fillColor);
                    polygon.setAttribute("stroke-width", "0.65");
                    polygon.setAttribute("stroke-linejoin", "round");
                    runtime.svg.appendChild(polygon);
                }
                runtime.queue = [];
                return;
            }
            const context = runtime.context;
            context.imageSmoothingEnabled = true;
            if ("imageSmoothingQuality" in context) {
                context.imageSmoothingQuality = "high";
            }
            let affinePaintCount = 0;
            const expandTriangle = (first, second, third, overlap = 1.00) => {
                const centerX = (first.x + second.x + third.x) / 3.0;
                const centerY = (first.y + second.y + third.y) / 3.0;
                return [first, second, third].map(vertex => {
                    const dx = vertex.x - centerX;
                    const dy = vertex.y - centerY;
                    const length = Math.hypot(dx, dy) || 1.0;
                    return [
                        vertex.x + dx / length * overlap,
                        vertex.y + dy / length * overlap,
                    ];
                });
            };
            const paintAffineTexture = (
                first, second, third, pattern, imageWidth, imageHeight) => {
                const sx0 = first.u * imageWidth;
                const sy0 = first.v * imageHeight;
                const sx1 = second.u * imageWidth;
                const sy1 = second.v * imageHeight;
                const sx2 = third.u * imageWidth;
                const sy2 = third.v * imageHeight;
                const dsx1 = sx1 - sx0;
                const dsy1 = sy1 - sy0;
                const dsx2 = sx2 - sx0;
                const dsy2 = sy2 - sy0;
                const det = dsx1 * dsy2 - dsx2 * dsy1;
                if (Math.abs(det) <= 0.001) return false;

                const dpx1 = second.x - first.x;
                const dpy1 = second.y - first.y;
                const dpx2 = third.x - first.x;
                const dpy2 = third.y - first.y;
                const a = (dpx1 * dsy2 - dpx2 * dsy1) / det;
                const c = (dsx1 * dpx2 - dsx2 * dpx1) / det;
                const b = (dpy1 * dsy2 - dpy2 * dsy1) / det;
                const d = (dsx1 * dpy2 - dsx2 * dpy1) / det;
                const expanded = expandTriangle(first, second, third);

                context.save();
                context.beginPath();
                context.moveTo(expanded[0][0], expanded[0][1]);
                context.lineTo(expanded[1][0], expanded[1][1]);
                context.lineTo(expanded[2][0], expanded[2][1]);
                context.closePath();
                context.clip();
                context.setTransform(
                    a, b, c, d,
                    first.x - a * sx0 - c * sy0,
                    first.y - b * sx0 - d * sy0);
                context.fillStyle = pattern;
                const minSourceX = Math.min(sx0, sx1, sx2) - imageWidth;
                const maxSourceX = Math.max(sx0, sx1, sx2) + imageWidth;
                const minSourceY = Math.min(sy0, sy1, sy2) - imageHeight;
                const maxSourceY = Math.max(sy0, sy1, sy2) + imageHeight;
                context.fillRect(
                    minSourceX,
                    minSourceY,
                    maxSourceX - minSourceX,
                    maxSourceY - minSourceY);
                context.restore();
                ++affinePaintCount;
                return true;
            };
            const perspectiveMidpoint = (first, second) => {
                const reciprocalW = (first.reciprocalW + second.reciprocalW) * 0.5;
                const reciprocalSum = first.reciprocalW + second.reciprocalW;
                return {
                    x: (first.x + second.x) * 0.5,
                    y: (first.y + second.y) * 0.5,
                    u: reciprocalSum > 0.0
                        ? (first.u * first.reciprocalW
                            + second.u * second.reciprocalW) / reciprocalSum
                        : (first.u + second.u) * 0.5,
                    v: reciprocalSum > 0.0
                        ? (first.v * first.reciprocalW
                            + second.v * second.reciprocalW) / reciprocalSum
                        : (first.v + second.v) * 0.5,
                    reciprocalW,
                };
            };
            const perspectiveEdgeError = (
                first, second, imageWidth, imageHeight) => {
                const midpoint = perspectiveMidpoint(first, second);
                const affineU = (first.u + second.u) * 0.5;
                const affineV = (first.v + second.v) * 0.5;
                return Math.hypot(
                    (midpoint.u - affineU) * imageWidth,
                    (midpoint.v - affineV) * imageHeight);
            };
            const paintPerspectiveTexture = (
                first, second, third, pattern, imageWidth, imageHeight) => {
                const pending = [{ vertices: [first, second, third], depth: 0 }];
                let painted = false;
                while (pending.length > 0) {
                    const item = pending.pop();
                    const vertices = item.vertices;
                    const area = Math.abs(
                        (vertices[1].x - vertices[0].x)
                            * (vertices[2].y - vertices[0].y)
                        - (vertices[1].y - vertices[0].y)
                            * (vertices[2].x - vertices[0].x));
                    const edges = [[0, 1], [1, 2], [2, 0]];
                    let splitEdge = 0;
                    let largestError = -1.0;
                    let largestLength = 0.0;
                    for (let edgeIndex = 0; edgeIndex < edges.length; ++edgeIndex) {
                        const edge = edges[edgeIndex];
                        const edgeError = perspectiveEdgeError(
                            vertices[edge[0]], vertices[edge[1]],
                            imageWidth, imageHeight);
                        if (edgeError > largestError) {
                            splitEdge = edgeIndex;
                            largestError = edgeError;
                            largestLength = Math.hypot(
                                vertices[edge[1]].x - vertices[edge[0]].x,
                                vertices[edge[1]].y - vertices[edge[0]].y);
                        }
                    }
                    const requiredError = 3.0 * Math.pow(2.0, item.depth);
                    if (item.depth < 3
                        && area > 4.0
                        && largestLength > 20.0
                        && largestError > requiredError) {
                        const edge = edges[splitEdge];
                        const remaining = 3 - edge[0] - edge[1];
                        const midpoint = perspectiveMidpoint(
                            vertices[edge[0]], vertices[edge[1]]);
                        pending.push({
                            vertices: [
                                vertices[edge[0]], midpoint, vertices[remaining]],
                            depth: item.depth + 1,
                        });
                        pending.push({
                            vertices: [
                                midpoint, vertices[edge[1]], vertices[remaining]],
                            depth: item.depth + 1,
                        });
                        continue;
                    }
                    painted = paintAffineTexture(
                        vertices[0], vertices[1], vertices[2],
                        pattern, imageWidth, imageHeight) || painted;
                }
                return painted;
            };
            const paintDepthBufferedFrame = sourceTriangles => {
                if (sourceTriangles.length === 0) return false;
                for (const triangle of sourceTriangles) {
                    if (triangle.command.textureId == 0) continue;
                    const slot = globalThis.__lamaponTextures?.[
                        triangle.command.textureId];
                    if (!slot || !slot.ready || !slot.softwareLevels
                        || slot.softwareLevels.length === 0) {
                        return false;
                    }
                }
                const width = runtime.canvas.width;
                const height = runtime.canvas.height;
                let frame;
                try {
                    frame = context.getImageData(0, 0, width, height);
                } catch (error) {
                    console.warn(
                        "LamaPon software depth buffer unavailable", error);
                    return false;
                }
                const output = frame.data;
                const pixelCount = width * height;
                if (!runtime.textureDepthBuffer
                    || runtime.textureDepthBuffer.length !== pixelCount) {
                    runtime.textureDepthBuffer = new Float32Array(pixelCount);
                }
                const depthBuffer = runtime.textureDepthBuffer;
                depthBuffer.fill(1.000001);
                let paintedPixels = 0;
                const orderedTriangles = sourceTriangles.slice().sort(
                    (left, right) => {
                        const leftTransparent =
                            left.command.alphaBlended;
                        const rightTransparent =
                            right.command.alphaBlended;
                        if (leftTransparent !== rightTransparent) {
                            return leftTransparent ? 1 : -1;
                        }
                        // 不透明Geometryは描画順に依存しないため、手前から処理します。
                        // 後続の隠れたFragmentを大きなImageData Bufferへ書き込む前に
                        // Depth Testで除外できます。
                        if (!leftTransparent) {
                            return left.command.values[left.index + 16]
                                - right.command.values[right.index + 16];
                        }
                        return right.command.values[right.index + 16]
                            - left.command.values[left.index + 16];
                    });
                for (const triangle of orderedTriangles) {
                    const command = triangle.command;
                    const values = command.values;
                    const index = triangle.index;
                    const x0 = values[index];
                    const y0 = values[index + 1];
                    const x1 = values[index + 2];
                    const y1 = values[index + 3];
                    const x2 = values[index + 4];
                    const y2 = values[index + 5];
                    const denominator = (y1 - y2) * (x0 - x2)
                        + (x2 - x1) * (y0 - y2);
                    if (Math.abs(denominator) < 0.00001) continue;
                    const inverseDenominator = 1.0 / denominator;
                    const minimumX = Math.max(
                        0, Math.floor(Math.min(x0, x1, x2)));
                    const maximumX = Math.min(
                        width - 1, Math.ceil(Math.max(x0, x1, x2)));
                    const minimumY = Math.max(
                        0, Math.floor(Math.min(y0, y1, y2)));
                    const maximumY = Math.min(
                        height - 1, Math.ceil(Math.max(y0, y1, y2)));
                    if (minimumX > maximumX || minimumY > maximumY) continue;

                    const textured = command.textureId != 0;
                    const slot = textured
                        ? globalThis.__lamaponTextures[command.textureId]
                        : null;
                    let level = null;
                    let source = null;
                    if (textured) {
                        const baseLevel = slot.softwareLevels[0];
                        const texelDensity = (
                            ax, ay, au, av, bx, by, bu, bv) =>
                            Math.hypot(
                                (bu - au) * baseLevel.width,
                                (bv - av) * baseLevel.height)
                            / Math.max(0.5, Math.hypot(
                                bx - ax, by - ay));
                        const maximumDensity = Math.max(
                            texelDensity(
                                x0, y0,
                                values[index + 9], values[index + 10],
                                x1, y1,
                                values[index + 11], values[index + 12]),
                            texelDensity(
                                x1, y1,
                                values[index + 11], values[index + 12],
                                x2, y2,
                                values[index + 13], values[index + 14]),
                            texelDensity(
                                x2, y2,
                                values[index + 13], values[index + 14],
                                x0, y0,
                                values[index + 9], values[index + 10]));
                        const mipIndex = Math.min(
                            slot.softwareLevels.length - 1,
                            Math.max(0, Math.floor(Math.log2(
                                Math.max(1.0, maximumDensity)))));
                        level = slot.softwareLevels[mipIndex];
                        source = level.pixels;
                    }
                    const q0 = values[index + 17];
                    const q1 = values[index + 18];
                    const q2 = values[index + 19];
                    for (let pixelY = minimumY;
                         pixelY <= maximumY; ++pixelY) {
                        const sampleY = pixelY + 0.5;
                        for (let pixelX = minimumX;
                             pixelX <= maximumX; ++pixelX) {
                            const sampleX = pixelX + 0.5;
                            const weight0 = ((y1 - y2) * (sampleX - x2)
                                + (x2 - x1) * (sampleY - y2))
                                * inverseDenominator;
                            const weight1 = ((y2 - y0) * (sampleX - x2)
                                + (x0 - x2) * (sampleY - y2))
                                * inverseDenominator;
                            const weight2 = 1.0 - weight0 - weight1;
                            if (weight0 < -0.00001
                                || weight1 < -0.00001
                                || weight2 < -0.00001) continue;
                            const pixel = pixelY * width + pixelX;
                            const depth = weight0 * values[index + 20]
                                + weight1 * values[index + 21]
                                + weight2 * values[index + 22];
                            if (depth >= depthBuffer[pixel]) continue;
                            const reciprocalW = weight0 * q0
                                + weight1 * q1 + weight2 * q2;
                            if (reciprocalW <= 0.0000001) continue;
                            const u = (weight0 * values[index + 9] * q0
                                + weight1 * values[index + 11] * q1
                                + weight2 * values[index + 13] * q2)
                                / reciprocalW;
                            const v = (weight0 * values[index + 10] * q0
                                + weight1 * values[index + 12] * q1
                                + weight2 * values[index + 14] * q2)
                                / reciprocalW;
                            let surfaceRed = command.red;
                            let surfaceGreen = command.green;
                            let surfaceBlue = command.blue;
                            let surfaceAlpha = clamp(command.alpha);
                            if (textured) {
                                const wrappedU = u - Math.floor(u);
                                const wrappedV = v - Math.floor(v);
                                const textureX = Math.min(
                                    level.width - 1,
                                    Math.floor(wrappedU * level.width));
                                const textureY = Math.min(
                                    level.height - 1,
                                    Math.floor(wrappedV * level.height));
                                const sourceOffset =
                                    (textureY * level.width + textureX) * 4;
                                surfaceRed *= source[sourceOffset] / 255.0;
                                surfaceGreen *= source[sourceOffset + 1] / 255.0;
                                surfaceBlue *= source[sourceOffset + 2] / 255.0;
                                surfaceAlpha *= source[sourceOffset + 3] / 255.0;
                            }
                            if (command.alphaCutoff >= 0.0
                                && surfaceAlpha < command.alphaCutoff) continue;
                            if (command.alphaCutoff >= 0.0) surfaceAlpha = 1.0;
                            const viewDistance = (
                                weight0 * values[index + 23] * q0
                                + weight1 * values[index + 24] * q1
                                + weight2 * values[index + 25] * q2)
                                / reciprocalW;
                            const fogAmount = runtime.fog.enabled
                                ? clamp((viewDistance - runtime.fog.start)
                                    / Math.max(
                                        0.001,
                                        runtime.fog.end - runtime.fog.start))
                                : 0.0;
                            const lightRed = (
                                weight0 * values[index + 6] * q0
                                + weight1 * values[index + 26] * q1
                                + weight2 * values[index + 29] * q2)
                                / reciprocalW;
                            const lightGreen = (
                                weight0 * values[index + 7] * q0
                                + weight1 * values[index + 27] * q1
                                + weight2 * values[index + 30] * q2)
                                / reciprocalW;
                            const lightBlue = (
                                weight0 * values[index + 8] * q0
                                + weight1 * values[index + 28] * q1
                                + weight2 * values[index + 31] * q2)
                                / reciprocalW;
                            const litRed = surfaceRed * lightRed;
                            const litGreen = surfaceGreen * lightGreen;
                            const litBlue = surfaceBlue * lightBlue;
                            const finalRed = clamp(
                                litRed + (runtime.fog.color[0] - litRed)
                                    * fogAmount);
                            const finalGreen = clamp(
                                litGreen + (runtime.fog.color[1] - litGreen)
                                    * fogAmount);
                            const finalBlue = clamp(
                                litBlue + (runtime.fog.color[2] - litBlue)
                                    * fogAmount);
                            const outputOffset = pixel * 4;
                            const inverseAlpha = 1.0 - surfaceAlpha;
                            output[outputOffset] = Math.round(Math.min(255,
                                finalRed * 255 * surfaceAlpha
                                + output[outputOffset]
                                    * (command.additiveBlend ? 1.0 : inverseAlpha)));
                            output[outputOffset + 1] = Math.round(Math.min(255,
                                finalGreen * 255 * surfaceAlpha
                                + output[outputOffset + 1]
                                    * (command.additiveBlend ? 1.0 : inverseAlpha)));
                            output[outputOffset + 2] = Math.round(Math.min(255,
                                finalBlue * 255 * surfaceAlpha
                                + output[outputOffset + 2]
                                    * (command.additiveBlend ? 1.0 : inverseAlpha)));
                            output[outputOffset + 3] = 255;
                            if (!command.alphaBlended) {
                                depthBuffer[pixel] = depth;
                            }
                            ++paintedPixels;
                        }
                    }
                }
                context.putImageData(frame, 0, 0);
                if (document.body) {
                    document.body.dataset.lamaponSoftwareDepth = "active";
                    document.body.dataset.lamaponDepthPixels =
                        String(paintedPixels);
                }
                return true;
            };
            // CanvasはWebGLのようにVertex Normalを補間できません。Triangle単位の
            // 単色描画では曲面Mesh、特に車体の対角線が目立ちます。Materialの
            // Draw CommandごとにScreen-space Lighting Gradientを1つ生成し、
            // 隣接Triangleが共有Edgeで同じ色をSampleするようにします。
            // 各Canvas Triangleで3点Gouraud Shaderを模倣するより滑らかで高速です。
            const smoothUntexturedFill = command => {
                if (Object.prototype.hasOwnProperty.call(
                        command, "smoothUntexturedFill")) {
                    return command.smoothUntexturedFill;
                }
                if (command.textureId != 0 || command.alphaBlended) {
                    command.smoothUntexturedFill = null;
                    return null;
                }
                const values = command.values;
                let sampleCount = 0;
                let averageRed = 0.0;
                let averageGreen = 0.0;
                let averageBlue = 0.0;
                let maximumDistance = 0.0;
                let darkest = null;
                let brightest = null;
                for (let index = 0;
                     index + triangleStride - 1 < values.length;
                     index += triangleStride) {
                    const red = Math.max(0.0, values[index + 6]);
                    const green = Math.max(0.0, values[index + 7]);
                    const blue = Math.max(0.0, values[index + 8]);
                    const sample = {
                        x: (values[index] + values[index + 2]
                            + values[index + 4]) / 3.0,
                        y: (values[index + 1] + values[index + 3]
                            + values[index + 5]) / 3.0,
                        red, green, blue,
                        brightness: red * 0.2126
                            + green * 0.7152 + blue * 0.0722,
                    };
                    averageRed += red;
                    averageGreen += green;
                    averageBlue += blue;
                    maximumDistance = Math.max(
                        maximumDistance, values[index + 16]);
                    if (!darkest
                        || sample.brightness < darkest.brightness) {
                        darkest = sample;
                    }
                    if (!brightest
                        || sample.brightness > brightest.brightness) {
                        brightest = sample;
                    }
                    ++sampleCount;
                }
                // 長い背景MeshはFog境界をまたぐため、従来どおりTriangle単位の
                // Distance Fogが必要です。Carなど近距離Objectは連続面として
                // Shadingしても問題ありません。
                if (sampleCount === 0
                    || (runtime.fog.enabled
                        && maximumDistance > runtime.fog.start)) {
                    command.smoothUntexturedFill = null;
                    return null;
                }
                averageRed /= sampleCount;
                averageGreen /= sampleCount;
                averageBlue /= sampleCount;
                const rgba = (red, green, blue) => "rgba(" +
                    Math.round(clamp(command.red * red) * 255) + "," +
                    Math.round(clamp(command.green * green) * 255) + "," +
                    Math.round(clamp(command.blue * blue) * 255) + "," +
                    clamp(command.alpha) + ")";
                const lightingRange = brightest.brightness
                    - darkest.brightness;
                const gradientLength = Math.hypot(
                    brightest.x - darkest.x,
                    brightest.y - darkest.y);
                if (lightingRange < 0.025 || gradientLength < 2.0) {
                    command.smoothUntexturedFill = rgba(
                        averageRed, averageGreen, averageBlue);
                    return command.smoothUntexturedFill;
                }
                const soften = (sample, average) =>
                    average + (sample - average) * 0.55;
                const gradient = context.createLinearGradient(
                    brightest.x, brightest.y, darkest.x, darkest.y);
                gradient.addColorStop(0.0, rgba(
                    soften(brightest.red, averageRed),
                    soften(brightest.green, averageGreen),
                    soften(brightest.blue, averageBlue)));
                gradient.addColorStop(0.5, rgba(
                    averageRed, averageGreen, averageBlue));
                gradient.addColorStop(1.0, rgba(
                    soften(darkest.red, averageRed),
                    soften(darkest.green, averageGreen),
                    soften(darkest.blue, averageBlue)));
                command.smoothUntexturedFill = gradient;
                return gradient;
            };
            const depthBufferedFramePainted =
                paintDepthBufferedFrame(triangles);
            for (const triangle of triangles) {
                if (depthBufferedFramePainted) break;
                const command = triangle.command;
                const values = command.values;
                const index = triangle.index;
                // Alpha StateはTriangleごとに設定します。透明なSmokeのglobalAlphaが
                // 後から描く不透明なCarへ漏れないようにします。
                context.globalAlpha = 1.0;
                context.globalCompositeOperation =
                    command.additiveBlend ? "lighter" : "source-over";
                context.filter = "none";
                const lightRed = Math.max(0.0, values[index + 6]);
                const lightGreen = Math.max(0.0, values[index + 7]);
                const lightBlue = Math.max(0.0, values[index + 8]);
                const brightness = clamp(
                    lightRed * 0.2126 + lightGreen * 0.7152 + lightBlue * 0.0722);
                const x0 = values[index];
                const y0 = values[index + 1];
                const x1 = values[index + 2];
                const y1 = values[index + 3];
                const x2 = values[index + 4];
                const y2 = values[index + 5];
                const centerX = (x0 + x1 + x2) / 3.0;
                const centerY = (y0 + y1 + y2) / 3.0;
                const expandPoint = (x, y) => {
                    const dx = x - centerX;
                    const dy = y - centerY;
                    const length = Math.hypot(dx, dy) || 1.0;
                    const overlap = 0.70;
                    return [
                        x + dx / length * overlap,
                        y + dy / length * overlap,
                    ];
                };
                const expanded0 = expandPoint(x0, y0);
                const expanded1 = expandPoint(x1, y1);
                const expanded2 = expandPoint(x2, y2);
                const slot = globalThis.__lamaponTextures?.[command.textureId];
                const image = slot && slot.ready ? slot.image : null;
                const fog = runtime.fog;
                const fogAmount = fog.enabled
                    ? clamp((values[index + 16] - fog.start)
                        / Math.max(0.001, fog.end - fog.start))
                    : 0.0;
                if (image) {
                    const iw = image.width || 1;
                    const ih = image.height || 1;
                    slot.pattern2d = slot.pattern2d
                        || context.createPattern(image, "repeat");
                    context.globalAlpha = clamp(command.alpha);
                    const texturePainted = slot.pattern2d
                        && paintPerspectiveTexture(
                            {
                                x: x0, y: y0,
                                u: values[index + 9],
                                v: values[index + 10],
                                reciprocalW: values[index + 17],
                            },
                            {
                                x: x1, y: y1,
                                u: values[index + 11],
                                v: values[index + 12],
                                reciprocalW: values[index + 18],
                            },
                            {
                                x: x2, y: y2,
                                u: values[index + 13],
                                v: values[index + 14],
                                reciprocalW: values[index + 19],
                            },
                            slot.pattern2d, iw, ih);
                    if (texturePainted) {
                        context.filter = "none";
                        // 不透明TextureにはSource Triangleごとに軽量なOverlayを1回描き、
                        // LightingとFogを適用します。各Perspective LeafへのCanvas Filterは
                        // 数倍遅く、Frame Pacingも不安定になるため使用しません。
                        if (!command.alphaBlended) {
                            const remainingTexture = brightness
                                * (1.0 - fogAmount);
                            const overlayAlpha = clamp(
                                1.0 - remainingTexture);
                            const fogContribution = overlayAlpha > 0.0001
                                ? fogAmount / overlayAlpha : 0.0;
                            if (overlayAlpha > 0.001) {
                                context.globalAlpha = 1.0;
                                context.fillStyle = "rgba(" +
                                    Math.round(clamp(
                                        fog.color[0] * fogContribution) * 255)
                                    + "," + Math.round(clamp(
                                        fog.color[1] * fogContribution) * 255)
                                    + "," + Math.round(clamp(
                                        fog.color[2] * fogContribution) * 255)
                                    + "," + overlayAlpha + ")";
                                context.beginPath();
                                context.moveTo(expanded0[0], expanded0[1]);
                                context.lineTo(expanded1[0], expanded1[1]);
                                context.lineTo(expanded2[0], expanded2[1]);
                                context.closePath();
                                context.fill();
                            }
                        }
                        continue;
                    }
                }
                const smoothFill = smoothUntexturedFill(command);
                if (smoothFill) {
                    context.fillStyle = smoothFill;
                } else {
                    const baseRed = command.red * lightRed;
                    const baseGreen = command.green * lightGreen;
                    const baseBlue = command.blue * lightBlue;
                    const mixedRed = baseRed
                        + (fog.color[0] - baseRed) * fogAmount;
                    const mixedGreen = baseGreen
                        + (fog.color[1] - baseGreen) * fogAmount;
                    const mixedBlue = baseBlue
                        + (fog.color[2] - baseBlue) * fogAmount;
                    context.fillStyle = "rgba(" +
                        Math.round(clamp(mixedRed) * 255) + "," +
                        Math.round(clamp(mixedGreen) * 255) + "," +
                        Math.round(clamp(mixedBlue) * 255) + "," +
                        clamp(command.alpha) + ")";
                }
                context.beginPath();
                context.moveTo(expanded0[0], expanded0[1]);
                context.lineTo(expanded1[0], expanded1[1]);
                context.lineTo(expanded2[0], expanded2[1]);
                context.closePath();
                context.fill();
                // Canvas PathはTriangleごとに独立してAnti-aliasされます。Software
                // Framebufferの表示倍率では、隣接面が同じFillでも点状のSub-pixel
                // Crackが見える場合があります。同じScreen-space Gradientで境界を
                // 再描画します。Draw Command全体でStyleを共有するためWireframe状の
                // Edgeにはなりません。
                if (smoothFill) {
                    context.strokeStyle = smoothFill;
                    context.lineWidth = 1.0;
                    context.lineJoin = "round";
                    context.stroke();
                }
            }
            context.globalAlpha = 1.0;
            context.globalCompositeOperation = "source-over";
            context.filter = "none";
            if (document.body) {
                const rasterMilliseconds = performance.now() - rasterStarted;
                document.body.dataset.lamaponTriangles = String(triangles.length);
                document.body.dataset.lamaponTexturePaints = String(affinePaintCount);
                document.body.dataset.lamaponRasterMilliseconds =
                    rasterMilliseconds.toFixed(2);
                runtime.measuredFrames = (runtime.measuredFrames || 0) + 1;
                if (runtime.measuredFrames > 30
                    && rasterMilliseconds > (runtime.maximumRasterMilliseconds || 0)) {
                    runtime.maximumRasterMilliseconds = rasterMilliseconds;
                    const hudTime = document.getElementById("hud-time");
                    document.body.dataset.lamaponRasterMaximum =
                        rasterMilliseconds.toFixed(2);
                    document.body.dataset.lamaponRasterMaximumAt =
                        hudTime ? hudTime.textContent : "unknown";
                    document.body.dataset.lamaponRasterMaximumTriangles =
                        String(triangles.length);
                    document.body.dataset.lamaponRasterMaximumTexturePaints =
                        String(affinePaintCount);
                }
            }
            runtime.queue = [];
        });

        EM_JS(int, BrowserTextureCreate,
              (const char* virtualPath, int rendererVersion), {
            const canvas = document.querySelector("#canvas");
            let gl = null;
            if (rendererVersion >= 2 && canvas
                && typeof canvas.getContext === "function") {
                gl = canvas.getContext("webgl2");
            } else if (rendererVersion == 1 && canvas
                && typeof canvas.getContext === "function") {
                gl = canvas.getContext("webgl")
                    || canvas.getContext("experimental-webgl");
            }
            if (!globalThis.FS) return 0;
            if (gl) globalThis.__lamaponWebGl = gl;
            globalThis.__lamaponTextures = globalThis.__lamaponTextures || {};
            globalThis.__lamaponNextTextureId = globalThis.__lamaponNextTextureId || 1;
            const id = globalThis.__lamaponNextTextureId++;
            const slot = {
                texture: null,
                image: null,
                pattern2d: null,
                softwareLevels: null,
                ready: false,
            };
            globalThis.__lamaponTextures[id] = slot;
            try {
                const bytes = FS.readFile(UTF8ToString(virtualPath));
                const imageMime = bytes.length >= 12
                    && bytes[0] === 0x52 && bytes[1] === 0x49
                    && bytes[2] === 0x46 && bytes[3] === 0x46
                    && bytes[8] === 0x57 && bytes[9] === 0x45
                    && bytes[10] === 0x42 && bytes[11] === 0x50
                        ? "image/webp"
                        : bytes.length >= 2
                            && bytes[0] === 0xff && bytes[1] === 0xd8
                                ? "image/jpeg"
                                : "image/png";
                const blob = new Blob([bytes], { type: imageMime });
                createImageBitmap(blob).then(bitmap => {
                    if (gl) {
                        const texture = gl.createTexture();
                        gl.bindTexture(gl.TEXTURE_2D, texture);
                        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
                        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
                        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
                        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
                        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
                        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
                        gl.generateMipmap(gl.TEXTURE_2D);
                        slot.texture = texture;
                        bitmap.close();
                    } else {
                        slot.image = bitmap;
                        // GPUなしのDepth Renderer用にCPUで読めるMip Chainを保持します。
                        // Perspective-correct UVでPixelをSampleし、RoadやWallがTriangleの
                        // 対角線間で曲がって見えるAffine歪みを防ぎます。
                        const levels = [];
                        let source = bitmap;
                        let width = Math.max(1, bitmap.width);
                        let height = Math.max(1, bitmap.height);
                        while (true) {
                            const surface = document.createElement("canvas");
                            surface.width = width;
                            surface.height = height;
                            const surfaceContext = surface.getContext(
                                "2d", { willReadFrequently: true });
                            if (!surfaceContext) break;
                            surfaceContext.imageSmoothingEnabled = true;
                            surfaceContext.imageSmoothingQuality = "high";
                            surfaceContext.drawImage(source, 0, 0, width, height);
                            levels.push({
                                width,
                                height,
                                pixels: surfaceContext.getImageData(
                                    0, 0, width, height).data,
                            });
                            if (width === 1 && height === 1) break;
                            source = surface;
                            width = Math.max(1, Math.floor(width * 0.5));
                            height = Math.max(1, Math.floor(height * 0.5));
                        }
                        slot.softwareLevels = levels;
                    }
                    slot.ready = true;
                }).catch(error => console.warn("LamaPon Web texture load failed", virtualPath, error));
                return id;
            } catch (error) {
                console.warn("LamaPon Web texture file unavailable", virtualPath, error);
                delete globalThis.__lamaponTextures[id];
                return 0;
            }
        });

        EM_JS(int, BrowserTextureBind, (int textureId), {
            const gl = globalThis.__lamaponWebGl;
            const slot = globalThis.__lamaponTextures?.[textureId];
            if (!gl || !slot || !slot.ready) return 0;
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, slot.texture);
            return 1;
        });

        struct ClipVertex final
        {
            float x{};
            float y{};
            float z{};
            float w{};
        };

        [[nodiscard]] ClipVertex TransformPoint(
            const Mat4& matrix,
            const Vec3& point) noexcept
        {
            return {
                matrix.values[0] * point.x
                    + matrix.values[4] * point.y
                    + matrix.values[8] * point.z
                    + matrix.values[12],
                matrix.values[1] * point.x
                    + matrix.values[5] * point.y
                    + matrix.values[9] * point.z
                    + matrix.values[13],
                matrix.values[2] * point.x
                    + matrix.values[6] * point.y
                    + matrix.values[10] * point.z
                    + matrix.values[14],
                matrix.values[3] * point.x
                    + matrix.values[7] * point.y
                    + matrix.values[11] * point.z
                    + matrix.values[15],
            };
        }

        [[nodiscard]] GLuint CompileShader(
            GLenum type,
            const char* source) noexcept
        {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE)
            {
                return shader;
            }
            glDeleteShader(shader);
            return 0;
        }

        [[nodiscard]] GLuint CreateProgram(bool webGL2) noexcept
        {
            const GLuint vertexShader = CompileShader(
                GL_VERTEX_SHADER,
                webGL2
                    ? VertexShaderSourceWebGL2
                    : VertexShaderSourceWebGL1);
            const GLuint fragmentShader = CompileShader(
                GL_FRAGMENT_SHADER,
                webGL2
                    ? FragmentShaderSourceWebGL2
                    : FragmentShaderSourceWebGL1);
            if (vertexShader == 0 || fragmentShader == 0)
            {
                if (vertexShader != 0) glDeleteShader(vertexShader);
                if (fragmentShader != 0) glDeleteShader(fragmentShader);
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            glBindAttribLocation(program, 0, "aPosition");
            glBindAttribLocation(program, 1, "aNormal");
            glBindAttribLocation(program, 2, "aUv");
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked == GL_TRUE)
            {
                return program;
            }
            glDeleteProgram(program);
            return 0;
        }
    }

    struct Renderer3D::Impl final
    {
        struct Mesh final
        {
            GLuint vertexBuffer{};
            GLuint indexBuffer{};
            GLsizei indexCount{};
            std::vector<Vertex3D> vertices;
            std::vector<std::uint32_t> indices;
        };

        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context{};
        GLuint program{};
        GLint modelLocation{ -1 };
        GLint viewLocation{ -1 };
        GLint projectionLocation{ -1 };
        GLint colorLocation{ -1 };
        GLint lightDirectionLocation{ -1 };
        GLint ambientColorLocation{ -1 };
        GLint ambientIntensityLocation{ -1 };
        GLint directionalColorLocation{ -1 };
        GLint directionalIntensityLocation{ -1 };
        GLint roughnessLocation{ -1 };
        GLint metallicLocation{ -1 };
        GLint dielectricSpecularLocation{ -1 };
        GLint textureLocation{ -1 };
        GLint useTextureLocation{ -1 };
        GLint alphaCutoffLocation{ -1 };
        GLint normalTextureLocation{ -1 };
        GLint useNormalTextureLocation{ -1 };
        GLint normalStrengthLocation{ -1 };
        GLint metallicRoughnessTextureLocation{ -1 };
        GLint useMetallicRoughnessTextureLocation{ -1 };
        GLint roughnessTextureLocation{ -1 };
        GLint useRoughnessTextureLocation{ -1 };
        GLint metallicTextureLocation{ -1 };
        GLint useMetallicTextureLocation{ -1 };
        GLint occlusionTextureLocation{ -1 };
        GLint useOcclusionTextureLocation{ -1 };
        GLint occlusionStrengthLocation{ -1 };
        GLint emissiveTextureLocation{ -1 };
        GLint useEmissiveTextureLocation{ -1 };
        GLint emissiveColorLocation{ -1 };
        GLint unlitLocation{ -1 };
        GLint cameraPositionLocation{ -1 };
        GLint fogColorLocation{ -1 };
        GLint fogRangeLocation{ -1 };
        GLint fogEnabledLocation{ -1 };
        int webGLVersion{};
        bool fallback2D{};
        std::unordered_map<MeshId, Mesh> meshes;
        MeshId nextMeshId{ 1 };
    };

    Renderer3D::Renderer3D()
        : m_impl(std::make_unique<Impl>())
    {
    }

    Renderer3D::~Renderer3D()
    {
        if (!m_impl)
        {
            return;
        }
        for (const auto& [id, mesh] : m_impl->meshes)
        {
            (void)id;
            if (!m_impl->fallback2D)
            {
                glDeleteBuffers(1, &mesh.vertexBuffer);
                glDeleteBuffers(1, &mesh.indexBuffer);
            }
        }
        if (m_impl->program != 0)
        {
            glDeleteProgram(m_impl->program);
        }
        if (m_impl->context != 0)
        {
            emscripten_webgl_destroy_context(m_impl->context);
        }
    }

    bool Renderer3D::Initialize(
        const char* canvasSelector,
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        if (m_initialized)
        {
            return true;
        }
        if (canvasSelector == nullptr || *canvasSelector == '\0')
        {
            return false;
        }
        emscripten_set_canvas_element_size(
            canvasSelector,
            static_cast<int>(std::max(width, 1u)),
            static_cast<int>(std::max(height, 1u)));
        constexpr std::array<int, 2> versions = { 2, 1 };
        for (const int version : versions)
        {
            EmscriptenWebGLContextAttributes attributes;
            emscripten_webgl_init_context_attributes(&attributes);
            attributes.alpha = EM_FALSE;
            attributes.depth = EM_TRUE;
            attributes.stencil = EM_FALSE;
            attributes.antialias = EM_TRUE;
            attributes.majorVersion = version;
            attributes.minorVersion = 0;
            m_impl->context = emscripten_webgl_create_context(
                canvasSelector,
                &attributes);
            if (m_impl->context <= 0
                || emscripten_webgl_make_context_current(m_impl->context)
                    != EMSCRIPTEN_RESULT_SUCCESS)
            {
                if (m_impl->context > 0)
                {
                    emscripten_webgl_destroy_context(m_impl->context);
                }
                m_impl->context = 0;
                continue;
            }
            if (version == 1
                && emscripten_webgl_enable_extension(
                    m_impl->context,
                    "OES_element_index_uint") != EMSCRIPTEN_RESULT_SUCCESS)
            {
                emscripten_webgl_destroy_context(m_impl->context);
                m_impl->context = 0;
                continue;
            }
            m_impl->program = CreateProgram(version >= 2);
            if (m_impl->program == 0)
            {
                emscripten_webgl_destroy_context(m_impl->context);
                m_impl->context = 0;
                continue;
            }
            m_impl->webGLVersion = version;
            break;
        }
        if (m_impl->context > 0 && m_impl->program != 0)
        {
            m_impl->modelLocation = glGetUniformLocation(
                m_impl->program,
                "uModel");
            m_impl->viewLocation = glGetUniformLocation(
                m_impl->program,
                "uView");
            m_impl->projectionLocation = glGetUniformLocation(
                m_impl->program,
                "uProjection");
            m_impl->colorLocation = glGetUniformLocation(
                m_impl->program,
                "uColor");
            m_impl->lightDirectionLocation = glGetUniformLocation(
                m_impl->program,
                "uLightDirection");
            m_impl->ambientColorLocation = glGetUniformLocation(
                m_impl->program,
                "uAmbientColor");
            m_impl->ambientIntensityLocation = glGetUniformLocation(
                m_impl->program,
                "uAmbientIntensity");
            m_impl->directionalColorLocation = glGetUniformLocation(
                m_impl->program,
                "uDirectionalColor");
            m_impl->directionalIntensityLocation = glGetUniformLocation(
                m_impl->program,
                "uDirectionalIntensity");
            m_impl->roughnessLocation = glGetUniformLocation(
                m_impl->program,
                "uRoughness");
            m_impl->metallicLocation = glGetUniformLocation(
                m_impl->program,
                "uMetallic");
            m_impl->dielectricSpecularLocation = glGetUniformLocation(
                m_impl->program,
                "uDielectricSpecular");
            m_impl->textureLocation = glGetUniformLocation(
                m_impl->program,
                "uTexture");
            m_impl->useTextureLocation = glGetUniformLocation(
                m_impl->program,
                "uUseTexture");
            m_impl->alphaCutoffLocation = glGetUniformLocation(
                m_impl->program,
                "uAlphaCutoff");
            m_impl->normalTextureLocation = glGetUniformLocation(
                m_impl->program,
                "uNormalTexture");
            m_impl->useNormalTextureLocation = glGetUniformLocation(
                m_impl->program,
                "uUseNormalTexture");
            m_impl->normalStrengthLocation = glGetUniformLocation(
                m_impl->program,
                "uNormalStrength");
            m_impl->metallicRoughnessTextureLocation = glGetUniformLocation(
                m_impl->program,
                "uMetallicRoughnessTexture");
            m_impl->useMetallicRoughnessTextureLocation = glGetUniformLocation(
                m_impl->program,
                "uUseMetallicRoughnessTexture");
            m_impl->roughnessTextureLocation = glGetUniformLocation(
                m_impl->program, "uRoughnessTexture");
            m_impl->useRoughnessTextureLocation = glGetUniformLocation(
                m_impl->program, "uUseRoughnessTexture");
            m_impl->metallicTextureLocation = glGetUniformLocation(
                m_impl->program, "uMetallicTexture");
            m_impl->useMetallicTextureLocation = glGetUniformLocation(
                m_impl->program, "uUseMetallicTexture");
            m_impl->occlusionTextureLocation = glGetUniformLocation(
                m_impl->program, "uOcclusionTexture");
            m_impl->useOcclusionTextureLocation = glGetUniformLocation(
                m_impl->program, "uUseOcclusionTexture");
            m_impl->occlusionStrengthLocation = glGetUniformLocation(
                m_impl->program, "uOcclusionStrength");
            m_impl->emissiveTextureLocation = glGetUniformLocation(
                m_impl->program, "uEmissiveTexture");
            m_impl->useEmissiveTextureLocation = glGetUniformLocation(
                m_impl->program, "uUseEmissiveTexture");
            m_impl->emissiveColorLocation = glGetUniformLocation(
                m_impl->program, "uEmissiveColor");
            m_impl->unlitLocation = glGetUniformLocation(
                m_impl->program, "uUnlit");
            m_impl->cameraPositionLocation = glGetUniformLocation(
                m_impl->program,
                "uCameraPosition");
            m_impl->fogColorLocation = glGetUniformLocation(
                m_impl->program,
                "uFogColor");
            m_impl->fogRangeLocation = glGetUniformLocation(
                m_impl->program,
                "uFogRange");
            m_impl->fogEnabledLocation = glGetUniformLocation(
                m_impl->program,
                "uFogEnabled");
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glCullFace(GL_BACK);
            // LamaPonのDirectX Procedural Meshは時計回りを表面とします。
            // WebGL既定の反時計回りへ任せずSource規約を明示し、Roadの誤った
            // Cullingを防ぎます。
            glFrontFace(GL_CW);
            BrowserSetRendererBackend(m_impl->webGLVersion);
        }
        if (m_impl->context <= 0 || m_impl->program == 0)
        {
            if (!Canvas2DInitialize(
                    canvasSelector,
                    static_cast<int>(std::max(width, 1u)),
                    static_cast<int>(std::max(height, 1u))))
            {
                return false;
            }
            m_impl->fallback2D = true;
        }
        m_initialized = true;
        Resize(width, height);
        return true;
    }

    void Renderer3D::Resize(
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        m_width = std::max(width, 1u);
        m_height = std::max(height, 1u);
        if (m_impl->fallback2D)
        {
            // Canvas Triangle RasterizationはFill Rateが律速になります。
            // 480p相当の内部Surfaceで16:9表示を保ち、GPUなしCompatibility Modeでも
            // 安定した30Hzを維持できる処理Budgetを確保します。
            constexpr std::uint32_t softwareMaximumWidth = 960u;
            if (m_width > softwareMaximumWidth)
            {
                const float scale = static_cast<float>(softwareMaximumWidth)
                    / static_cast<float>(m_width);
                m_width = softwareMaximumWidth;
                m_height = std::max(
                    1u,
                    static_cast<std::uint32_t>(
                        std::lround(static_cast<float>(m_height) * scale)));
            }
            Canvas2DResize(
                static_cast<int>(m_width),
                static_cast<int>(m_height));
            return;
        }
        if (!m_initialized && m_impl->context == 0)
        {
            return;
        }
        glViewport(
            0,
            0,
            static_cast<GLsizei>(m_width),
            static_cast<GLsizei>(m_height));
    }

    void Renderer3D::BeginFrame(Color clearColor) noexcept
    {
        if (!m_initialized)
        {
            return;
        }
        if (m_impl->fallback2D)
        {
            Canvas2DBeginFrame(
                clearColor.r,
                clearColor.g,
                clearColor.b,
                clearColor.a);
            return;
        }
        if (m_sky.enabled)
        {
            constexpr int stripCount = 96;
            glEnable(GL_SCISSOR_TEST);
            for (int strip = 0; strip < stripCount; ++strip)
            {
                const int bottom = static_cast<int>(m_height) * strip
                    / stripCount;
                const int top = static_cast<int>(m_height) * (strip + 1)
                    / stripCount;
                const float fromBottom =
                    (static_cast<float>(strip) + 0.5f)
                    / static_cast<float>(stripCount);
                const float fromTop = 1.0f - fromBottom;
                constexpr std::array<float, 7> skyPositions = {
                    0.0f, 0.032f, 0.097f, 0.195f,
                    0.292f, 0.35f, 0.454f };
                constexpr std::array<float, 7> skyAmounts = {
                    0.0f, 0.05f, 0.30f, 0.60f,
                    0.825f, 0.925f, 1.0f };
                float skyAmount = 1.0f;
                for (std::size_t point = 0;
                     point + 1 < skyPositions.size(); ++point)
                {
                    if (fromTop <= skyPositions[point + 1])
                    {
                        const float local = std::clamp(
                            (fromTop - skyPositions[point])
                                / (skyPositions[point + 1]
                                    - skyPositions[point]),
                            0.0f, 1.0f);
                        skyAmount = skyAmounts[point]
                            + (skyAmounts[point + 1] - skyAmounts[point])
                                * local;
                        break;
                    }
                }
                glScissor(0, bottom, static_cast<GLsizei>(m_width), top - bottom);
                glClearColor(
                    m_sky.topColor.r
                        + (m_sky.horizonColor.r - m_sky.topColor.r) * skyAmount,
                    m_sky.topColor.g
                        + (m_sky.horizonColor.g - m_sky.topColor.g) * skyAmount,
                    m_sky.topColor.b
                        + (m_sky.horizonColor.b - m_sky.topColor.b) * skyAmount,
                    1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            glClear(GL_DEPTH_BUFFER_BIT);
        }
        else
        {
            glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
    }

    void Renderer3D::EndFrame() noexcept
    {
        if (m_initialized && m_impl->fallback2D)
        {
            Canvas2DEndFrame();
        }
        // WebGLはBrowser Animation Frameの最後にPresentします。
    }

    bool Renderer3D::UsesCanvas2DFallback() const noexcept
    {
        return m_impl != nullptr && m_impl->fallback2D;
    }

    void Renderer3D::SetCamera(const Camera3D& camera) noexcept
    {
        const float aspectRatio = static_cast<float>(m_width)
            / static_cast<float>(std::max(m_height, 1u));
        m_view = LookAt(camera.position, camera.target, camera.up);
        m_projection = Perspective(
            camera.verticalFieldOfViewRadians,
            aspectRatio,
            camera.nearPlane,
            camera.farPlane);
        m_cameraPosition = camera.position;
    }

    void Renderer3D::SetFog(const Fog3D& fog) noexcept
    {
        m_fog = fog;
        m_fog.startDistance = std::max(0.0f, m_fog.startDistance);
        m_fog.endDistance = std::max(
            m_fog.startDistance + 0.001f,
            m_fog.endDistance);
        if (m_initialized && m_impl->fallback2D)
        {
            Canvas2DSetFog(
                m_fog.enabled ? 1 : 0,
                m_fog.color.r,
                m_fog.color.g,
                m_fog.color.b,
                m_fog.startDistance,
                m_fog.endDistance);
        }
    }

    void Renderer3D::SetLighting(const Lighting3D& lighting) noexcept
    {
        m_lighting = lighting;
        m_lighting.ambientIntensity = std::max(
            0.0f, m_lighting.ambientIntensity);
        m_lighting.directionalIntensity = std::max(
            0.0f, m_lighting.directionalIntensity);
        if (Length(m_lighting.directionalDirection) <= 0.0001f)
        {
            m_lighting.directionalDirection = { 0.0f, -1.0f, 0.0f };
        }
        else
        {
            m_lighting.directionalDirection = Normalize(
                m_lighting.directionalDirection);
        }
    }

    void Renderer3D::SetSky(const Sky3D& sky) noexcept
    {
        m_sky = sky;
        if (m_initialized && m_impl->fallback2D)
        {
            Canvas2DSetSky(
                m_sky.enabled ? 1 : 0,
                m_sky.topColor.r,
                m_sky.topColor.g,
                m_sky.topColor.b,
                m_sky.horizonColor.r,
                m_sky.horizonColor.g,
                m_sky.horizonColor.b);
        }
    }

    MeshId Renderer3D::CreateMesh(
        const std::vector<Vertex3D>& vertices,
        const std::vector<std::uint32_t>& indices) noexcept
    {
        if (!m_initialized || vertices.empty() || indices.empty())
        {
            return 0;
        }
        Impl::Mesh mesh{};
        mesh.vertices = vertices;
        mesh.indices = indices;
        mesh.indexCount = static_cast<GLsizei>(indices.size());
        const MeshId id = m_impl->nextMeshId++;
        if (m_impl->fallback2D)
        {
            m_impl->meshes.emplace(id, std::move(mesh));
            return id;
        }
        glGenBuffers(1, &mesh.vertexBuffer);
        glGenBuffers(1, &mesh.indexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vertexBuffer);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex3D)),
            vertices.data(),
            GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indexBuffer);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
            indices.data(),
            GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        m_impl->meshes.emplace(id, mesh);
        return id;
    }

    TextureId Renderer3D::CreateTexture(const char* virtualPath) noexcept
    {
        if (!m_initialized || virtualPath == nullptr || *virtualPath == '\0')
        {
            return 0;
        }
        return static_cast<TextureId>(BrowserTextureCreate(
            virtualPath, m_impl->fallback2D ? 0 : m_impl->webGLVersion));
    }

    void Renderer3D::UpdateMesh(
        MeshId meshId,
        const std::vector<Vertex3D>& vertices,
        const std::vector<std::uint32_t>& indices) noexcept
    {
        const auto found = m_impl->meshes.find(meshId);
        if (found == m_impl->meshes.end())
        {
            return;
        }
        found->second.vertices = vertices;
        found->second.indices = indices;
        found->second.indexCount = static_cast<GLsizei>(indices.size());
        if (m_impl->fallback2D)
        {
            return;
        }
        glBindBuffer(GL_ARRAY_BUFFER, found->second.vertexBuffer);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex3D)),
            vertices.empty() ? nullptr : vertices.data(),
            GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->second.indexBuffer);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
            indices.empty() ? nullptr : indices.data(),
            GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    void Renderer3D::DestroyMesh(MeshId meshId) noexcept
    {
        const auto found = m_impl->meshes.find(meshId);
        if (found == m_impl->meshes.end())
        {
            return;
        }
        if (!m_impl->fallback2D)
        {
            glDeleteBuffers(1, &found->second.vertexBuffer);
            glDeleteBuffers(1, &found->second.indexBuffer);
        }
        m_impl->meshes.erase(found);
    }

    void Renderer3D::DrawMesh(
        MeshId meshId,
        const Mat4& model,
        Color color,
        float roughness,
        TextureId texture,
        bool doubleSided,
        bool alphaBlended,
        float alphaCutoff,
        TextureId normalTexture,
        float normalStrength,
        float metallic,
        TextureId metallicRoughnessTexture,
        TextureId roughnessTexture,
        TextureId metallicTexture,
        TextureId occlusionTexture,
        float occlusionStrength,
        TextureId emissiveTexture,
        Color emissiveColor,
        bool unlit,
        Color dielectricSpecular,
        bool additiveBlend) noexcept
    {
        if (!m_initialized)
        {
            return;
        }
        const auto found = m_impl->meshes.find(meshId);
        if (found == m_impl->meshes.end())
        {
            return;
        }
        if (m_impl->fallback2D)
        {
            struct Triangle final
            {
                std::array<float, 32> values{};
                float depth{};
            };
            const Mat4 viewModel = Multiply(m_view, model);
            const Mat4 viewProjectionModel = Multiply(m_projection, viewModel);
            const Vec3 lightToSource = Normalize(
                m_lighting.directionalDirection * -1.0f);
            struct SoftwareVertex final
            {
                ClipVertex clip{};
                ClipVertex view{};
                Vec3 normal{};
                Vec2 uv{};
            };
            const auto transformNormal = [&model](const Vec3& source) noexcept
            {
                return Normalize({
                    model.values[0] * source.x
                        + model.values[4] * source.y
                        + model.values[8] * source.z,
                    model.values[1] * source.x
                        + model.values[5] * source.y
                        + model.values[9] * source.z,
                    model.values[2] * source.x
                        + model.values[6] * source.y
                        + model.values[10] * source.z,
                });
            };
            const float environmentSpecular =
                (1.0f - std::clamp(roughness, 0.0f, 1.0f)) * 0.08f;
            const auto lightForNormal = [this, &lightToSource,
                                         environmentSpecular, unlit](
                const Vec3& normal) noexcept
            {
                if (unlit)
                {
                    return std::array<float, 3>{ 1.0f, 1.0f, 1.0f };
                }
                const float diffuse = std::max(
                    Dot(normal, lightToSource), 0.0f);
                return std::array<float, 3>{
                    std::max(
                        0.0f,
                        m_lighting.ambientColor.r
                            * m_lighting.ambientIntensity
                            + environmentSpecular
                            + m_lighting.directionalColor.r
                                * m_lighting.directionalIntensity * diffuse),
                    std::max(
                        0.0f,
                        m_lighting.ambientColor.g
                            * m_lighting.ambientIntensity
                            + environmentSpecular
                            + m_lighting.directionalColor.g
                                * m_lighting.directionalIntensity * diffuse),
                    std::max(
                        0.0f,
                        m_lighting.ambientColor.b
                            * m_lighting.ambientIntensity
                            + environmentSpecular
                            + m_lighting.directionalColor.b
                                * m_lighting.directionalIntensity * diffuse),
                };
            };
            const auto interpolateVertex = [](const SoftwareVertex& first,
                                              const SoftwareVertex& second,
                                              float amount) noexcept
            {
                const auto interpolateClip = [amount](
                    const ClipVertex& left,
                    const ClipVertex& right) noexcept
                {
                    return ClipVertex{
                        left.x + (right.x - left.x) * amount,
                        left.y + (right.y - left.y) * amount,
                        left.z + (right.z - left.z) * amount,
                        left.w + (right.w - left.w) * amount,
                    };
                };
                return SoftwareVertex{
                    interpolateClip(first.clip, second.clip),
                    interpolateClip(first.view, second.view),
                    Normalize(first.normal
                        + (second.normal - first.normal) * amount),
                    {
                        first.uv.x + (second.uv.x - first.uv.x) * amount,
                        first.uv.y + (second.uv.y - first.uv.y) * amount,
                    },
                };
            };
            std::vector<Triangle> triangles;
            triangles.reserve(found->second.indices.size() / 2);
            const auto appendTriangle = [this, doubleSided,
                                         &triangles, &lightForNormal](
                const SoftwareVertex& first,
                const SoftwareVertex& second,
                const SoftwareVertex& third) noexcept
            {
                const auto projectX = [this](const ClipVertex& vertex) noexcept
                {
                    return (vertex.x / vertex.w * 0.5f + 0.5f)
                        * static_cast<float>(m_width);
                };
                const auto projectY = [this](const ClipVertex& vertex) noexcept
                {
                    return (1.0f - (vertex.y / vertex.w * 0.5f + 0.5f))
                        * static_cast<float>(m_height);
                };
                const float x1 = projectX(first.clip);
                const float y1 = projectY(first.clip);
                const float x2 = projectX(second.clip);
                const float y2 = projectY(second.clip);
                const float x3 = projectX(third.clip);
                const float y3 = projectY(third.clip);
                const float area = (x2 - x1) * (y3 - y1)
                    - (y2 - y1) * (x3 - x1);
                if (std::abs(area) < 0.01f)
                {
                    return;
                }
                // Screen YはOpenGL NDCに対して反転するため、WebGLで表面となる
                // 反時計回りTriangleの符号付き面積はここでは負になります。
                if (!doubleSided && area <= 0.0f)
                {
                    return;
                }
                constexpr float viewportMargin = 2.0f;
                if ((x1 < -viewportMargin
                        && x2 < -viewportMargin
                        && x3 < -viewportMargin)
                    || (x1 > static_cast<float>(m_width) + viewportMargin
                        && x2 > static_cast<float>(m_width) + viewportMargin
                        && x3 > static_cast<float>(m_width) + viewportMargin)
                    || (y1 < -viewportMargin
                        && y2 < -viewportMargin
                        && y3 < -viewportMargin)
                    || (y1 > static_cast<float>(m_height) + viewportMargin
                        && y2 > static_cast<float>(m_height) + viewportMargin
                        && y3 > static_cast<float>(m_height) + viewportMargin))
                {
                    return;
                }
                const float firstDepth = first.clip.z / first.clip.w;
                const float secondDepth = second.clip.z / second.clip.w;
                const float thirdDepth = third.clip.z / third.clip.w;
                if ((firstDepth < -1.0f
                        && secondDepth < -1.0f
                        && thirdDepth < -1.0f)
                    || (firstDepth > 1.0f
                        && secondDepth > 1.0f
                        && thirdDepth > 1.0f))
                {
                    return;
                }
                const std::array<float, 3> firstLight =
                    lightForNormal(first.normal);
                const std::array<float, 3> secondLight =
                    lightForNormal(second.normal);
                const std::array<float, 3> thirdLight =
                    lightForNormal(third.normal);
                triangles.push_back({
                    { x1, y1, x2, y2, x3, y3,
                      firstLight[0], firstLight[1], firstLight[2],
                      first.uv.x, first.uv.y,
                      second.uv.x, second.uv.y,
                      third.uv.x, third.uv.y,
                      (firstDepth + secondDepth + thirdDepth) / 3.0f,
                      std::max(0.0f,
                          -(first.view.z + second.view.z + third.view.z)
                              / 3.0f),
                      1.0f / first.clip.w,
                      1.0f / second.clip.w,
                      1.0f / third.clip.w,
                      firstDepth,
                      secondDepth,
                      thirdDepth,
                      std::max(0.0f, -first.view.z),
                      std::max(0.0f, -second.view.z),
                      std::max(0.0f, -third.view.z),
                      secondLight[0], secondLight[1], secondLight[2],
                      thirdLight[0], thirdLight[1], thirdLight[2] },
                    (firstDepth + secondDepth + thirdDepth) / 3.0f,
                });
            };
            for (std::size_t index = 0;
                 index + 2 < found->second.indices.size();
                 index += 3)
            {
                const std::uint32_t firstIndex = found->second.indices[index];
                const std::uint32_t secondIndex = found->second.indices[index + 1];
                const std::uint32_t thirdIndex = found->second.indices[index + 2];
                if (firstIndex >= found->second.vertices.size()
                    || secondIndex >= found->second.vertices.size()
                    || thirdIndex >= found->second.vertices.size())
                {
                    continue;
                }
                const Vertex3D& firstSource =
                    found->second.vertices[firstIndex];
                const Vertex3D& secondSource =
                    found->second.vertices[secondIndex];
                const Vertex3D& thirdSource =
                    found->second.vertices[thirdIndex];
                std::array<SoftwareVertex, 5> input = {{
                    {
                        TransformPoint(
                            viewProjectionModel, firstSource.position),
                        TransformPoint(viewModel, firstSource.position),
                        transformNormal(firstSource.normal),
                        firstSource.uv,
                    },
                    {
                        TransformPoint(
                            viewProjectionModel, secondSource.position),
                        TransformPoint(viewModel, secondSource.position),
                        transformNormal(secondSource.normal),
                        secondSource.uv,
                    },
                    {
                        TransformPoint(
                            viewProjectionModel, thirdSource.position),
                        TransformPoint(viewModel, thirdSource.position),
                        transformNormal(thirdSource.normal),
                        thirdSource.uv,
                    },
                }};
                std::array<SoftwareVertex, 5> clipped{};
                std::size_t inputCount = 3;
                std::size_t clippedCount = 0;
                constexpr float nearW = 0.1001f;
                SoftwareVertex previous = input[inputCount - 1];
                bool previousInside = previous.clip.w >= nearW;
                for (std::size_t vertexIndex = 0;
                     vertexIndex < inputCount;
                     ++vertexIndex)
                {
                    const SoftwareVertex current = input[vertexIndex];
                    const bool currentInside = current.clip.w >= nearW;
                    if (currentInside != previousInside)
                    {
                        const float denominator =
                            current.clip.w - previous.clip.w;
                        const float amount = std::abs(denominator) > 0.000001f
                            ? (nearW - previous.clip.w) / denominator
                            : 0.0f;
                        clipped[clippedCount++] = interpolateVertex(
                            previous, current, std::clamp(amount, 0.0f, 1.0f));
                    }
                    if (currentInside)
                    {
                        clipped[clippedCount++] = current;
                    }
                    previous = current;
                    previousInside = currentInside;
                }
                if (clippedCount < 3)
                {
                    continue;
                }
                for (std::size_t triangleIndex = 1;
                     triangleIndex + 1 < clippedCount;
                     ++triangleIndex)
                {
                    appendTriangle(
                        clipped[0],
                        clipped[triangleIndex],
                        clipped[triangleIndex + 1]);
                }
            }
            std::sort(
                triangles.begin(),
                triangles.end(),
                [](const Triangle& left, const Triangle& right) noexcept
                {
                    return left.depth > right.depth;
                });
            std::vector<float> payload;
            payload.reserve(triangles.size() * 32);
            for (const Triangle& triangle : triangles)
            {
                payload.insert(
                    payload.end(),
                    triangle.values.begin(),
                    triangle.values.end());
            }
            if (!payload.empty())
            {
                Canvas2DQueueTriangles(
                    payload.data(),
                    static_cast<int>(payload.size()),
                    color.r,
                    color.g,
                    color.b,
                    color.a,
                    static_cast<int>(texture),
                    (alphaBlended || color.a < 0.999f) ? 1 : 0,
                    alphaCutoff,
                    additiveBlend ? 1 : 0);
            }
            return;
        }
        glUseProgram(m_impl->program);
        glUniformMatrix4fv(m_impl->modelLocation, 1, GL_FALSE, model.values.data());
        glUniformMatrix4fv(m_impl->viewLocation, 1, GL_FALSE, m_view.values.data());
        glUniformMatrix4fv(
            m_impl->projectionLocation,
            1,
            GL_FALSE,
            m_projection.values.data());
        glUniform4f(m_impl->colorLocation, color.r, color.g, color.b, color.a);
        glUniform3f(
            m_impl->lightDirectionLocation,
            m_lighting.directionalDirection.x,
            m_lighting.directionalDirection.y,
            m_lighting.directionalDirection.z);
        glUniform3f(
            m_impl->ambientColorLocation,
            m_lighting.ambientColor.r,
            m_lighting.ambientColor.g,
            m_lighting.ambientColor.b);
        glUniform1f(
            m_impl->ambientIntensityLocation,
            m_lighting.ambientIntensity);
        glUniform3f(
            m_impl->directionalColorLocation,
            m_lighting.directionalColor.r,
            m_lighting.directionalColor.g,
            m_lighting.directionalColor.b);
        glUniform1f(
            m_impl->directionalIntensityLocation,
            m_lighting.directionalIntensity);
        glUniform1f(m_impl->roughnessLocation, std::clamp(roughness, 0.0f, 1.0f));
        glUniform1f(m_impl->metallicLocation, std::clamp(metallic, 0.0f, 1.0f));
        glUniform3f(
            m_impl->dielectricSpecularLocation,
            std::clamp(dielectricSpecular.r, 0.0f, 1.0f),
            std::clamp(dielectricSpecular.g, 0.0f, 1.0f),
            std::clamp(dielectricSpecular.b, 0.0f, 1.0f));
        glActiveTexture(GL_TEXTURE0);
        const bool textureReady = texture != 0
            && BrowserTextureBind(static_cast<int>(texture)) != 0;
        glUniform1i(m_impl->textureLocation, 0);
        glUniform1f(m_impl->useTextureLocation, textureReady ? 1.0f : 0.0f);
        glUniform1f(m_impl->alphaCutoffLocation, alphaCutoff);
        glActiveTexture(GL_TEXTURE1);
        const bool normalTextureReady = normalTexture != 0
            && BrowserTextureBind(static_cast<int>(normalTexture)) != 0;
        glUniform1i(m_impl->normalTextureLocation, 1);
        glUniform1f(
            m_impl->useNormalTextureLocation,
            normalTextureReady ? 1.0f : 0.0f);
        glUniform1f(
            m_impl->normalStrengthLocation,
            std::max(normalStrength, 0.0f));
        glActiveTexture(GL_TEXTURE2);
        const bool metallicRoughnessTextureReady =
            metallicRoughnessTexture != 0
            && BrowserTextureBind(
                static_cast<int>(metallicRoughnessTexture)) != 0;
        glUniform1i(m_impl->metallicRoughnessTextureLocation, 2);
        glUniform1f(
            m_impl->useMetallicRoughnessTextureLocation,
            metallicRoughnessTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE3);
        const bool roughnessTextureReady = roughnessTexture != 0
            && BrowserTextureBind(static_cast<int>(roughnessTexture)) != 0;
        glUniform1i(m_impl->roughnessTextureLocation, 3);
        glUniform1f(
            m_impl->useRoughnessTextureLocation,
            roughnessTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE4);
        const bool metallicTextureReady = metallicTexture != 0
            && BrowserTextureBind(static_cast<int>(metallicTexture)) != 0;
        glUniform1i(m_impl->metallicTextureLocation, 4);
        glUniform1f(
            m_impl->useMetallicTextureLocation,
            metallicTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE5);
        const bool occlusionTextureReady = occlusionTexture != 0
            && BrowserTextureBind(static_cast<int>(occlusionTexture)) != 0;
        glUniform1i(m_impl->occlusionTextureLocation, 5);
        glUniform1f(
            m_impl->useOcclusionTextureLocation,
            occlusionTextureReady ? 1.0f : 0.0f);
        glUniform1f(
            m_impl->occlusionStrengthLocation,
            std::clamp(occlusionStrength, 0.0f, 1.0f));
        glActiveTexture(GL_TEXTURE6);
        const bool emissiveTextureReady = emissiveTexture != 0
            && BrowserTextureBind(static_cast<int>(emissiveTexture)) != 0;
        glUniform1i(m_impl->emissiveTextureLocation, 6);
        glUniform1f(
            m_impl->useEmissiveTextureLocation,
            emissiveTextureReady ? 1.0f : 0.0f);
        glUniform3f(
            m_impl->emissiveColorLocation,
            std::max(emissiveColor.r, 0.0f),
            std::max(emissiveColor.g, 0.0f),
            std::max(emissiveColor.b, 0.0f));
        glUniform1f(m_impl->unlitLocation, unlit ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glUniform3f(
            m_impl->cameraPositionLocation,
            m_cameraPosition.x,
            m_cameraPosition.y,
            m_cameraPosition.z);
        glUniform4f(
            m_impl->fogColorLocation,
            m_fog.color.r,
            m_fog.color.g,
            m_fog.color.b,
            m_fog.color.a);
        glUniform2f(
            m_impl->fogRangeLocation,
            m_fog.startDistance,
            m_fog.endDistance);
        glUniform1f(
            m_impl->fogEnabledLocation,
            m_fog.enabled ? 1.0f : 0.0f);
        if (doubleSided)
        {
            glDisable(GL_CULL_FACE);
        }
        else
        {
            glEnable(GL_CULL_FACE);
        }
        const bool transparent = alphaBlended || color.a < 0.999f;
        if (transparent)
        {
            glEnable(GL_BLEND);
            glBlendFunc(
                GL_SRC_ALPHA,
                additiveBlend ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glDisable(GL_BLEND);
        }
        glDepthMask(transparent ? GL_FALSE : GL_TRUE);
        glBindBuffer(GL_ARRAY_BUFFER, found->second.vertexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->second.indexBuffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(
            0,
            3,
            GL_FLOAT,
            GL_FALSE,
            sizeof(Vertex3D),
            reinterpret_cast<const void*>(offsetof(Vertex3D, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1,
            3,
            GL_FLOAT,
            GL_FALSE,
            sizeof(Vertex3D),
            reinterpret_cast<const void*>(offsetof(Vertex3D, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(
            2,
            2,
            GL_FLOAT,
            GL_FALSE,
            sizeof(Vertex3D),
            reinterpret_cast<const void*>(offsetof(Vertex3D, uv)));
        glDrawElements(
            GL_TRIANGLES,
            found->second.indexCount,
            GL_UNSIGNED_INT,
            nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDepthMask(GL_TRUE);
    }
}
