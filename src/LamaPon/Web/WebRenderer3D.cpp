#include "LamaPon/Web/WebRenderer3D.h"

#if defined(LAMAPON_NATIVE_RUNTIME)
#include "LamaPon/Native/NativeGL.h"
#include "LamaPon/Native/NativeRenderBridge.h"
#else
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LamaPon::Web
{
    namespace
    {
        // WebGL2向け頂点GLSL
        constexpr char VertexShaderSourceWebGL2[] = R"glsl(#version 300 es
precision highp float;

// ローカル空間の頂点位置
layout(location = 0) in vec3 aPosition;
// ローカル空間の頂点法線
layout(location = 1) in vec3 aNormal;
// 頂点のテクスチャ座標
layout(location = 2) in vec2 aUv;

// 列優先のWorld変換行列
uniform mat4 uModel;
// 列優先のビュー変換行列
uniform mat4 uView;
// 列優先の透視投影行列
uniform mat4 uProjection;

// 補間するWorld法線
out vec3 vWorldNormal;
// 補間するテクスチャ座標
out vec2 vUv;
// 補間するWorld位置
out vec3 vWorldPosition;

// World法線と位置を計算して頂点を透視投影する。
void main()
{
    // World変換の線形成分
    mat3 model3 = mat3(uModel);
    // 第0列の余因子ベクトル
    vec3 cofactor0 = cross(model3[1], model3[2]);
    // 第1列の余因子ベクトル
    vec3 cofactor1 = cross(model3[2], model3[0]);
    // 第2列の余因子ベクトル
    vec3 cofactor2 = cross(model3[0], model3[1]);
    // World変換の行列式
    float determinant = dot(model3[0], cofactor0);
    // 法線に使う逆転置行列
    mat3 normalMatrix = abs(determinant) > 0.000001
        ? mat3(cofactor0, cofactor1, cofactor2) / determinant
        : mat3(1.0);
    vWorldNormal = normalMatrix * aNormal;
    vUv = aUv;
    // World変換後の同次位置
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    gl_Position = uProjection * uView * worldPosition;
}
)glsl";

        // WebGL2向け材質GLSL
        constexpr char FragmentShaderSourceWebGL2[] = R"glsl(#version 300 es
precision mediump float;

// 補間するWorld法線
in vec3 vWorldNormal;
// 補間するWorld位置
in vec3 vWorldPosition;
in vec2 vUv;
// 表面色のRGBA倍率
uniform vec4 uColor;
// 平行光が進むWorld方向
uniform vec3 uLightDirection;
uniform int uLocalLightCount;
uniform vec4 uLocalPositionRange[8];
uniform vec4 uLocalColorIntensity[8];
uniform vec4 uLocalDirectionOuter[8];
uniform vec2 uLocalInnerSpot[8];

// 環境光のRGB
uniform vec3 uAmbientColor;
// 環境光の強度
uniform float uAmbientIntensity;
// 平行光のRGB
uniform vec3 uDirectionalColor;
// 平行光の強度
uniform float uDirectionalIntensity;
// 粗さの倍率
uniform float uRoughness;
// 金属度の倍率
uniform float uMetallic;
// 非金属の反射RGB
uniform vec3 uDielectricSpecular;
// 表面色テクスチャ
uniform sampler2D uTexture;
// 表面色テクスチャの使用比
uniform float uUseTexture;
// 負値で無効のAlphaしきい値
uniform float uAlphaCutoff;
// 接空間の法線テクスチャ
uniform sampler2D uNormalTexture;
// 法線テクスチャを使うか
uniform float uUseNormalTexture;
// 接空間法線のXY倍率
uniform float uNormalStrength;
// G粗さ・B金属度テクスチャ
uniform sampler2D uMetallicRoughnessTexture;
// 複合材質テクスチャの使用比
uniform float uUseMetallicRoughnessTexture;
// G成分の粗さテクスチャ
uniform sampler2D uRoughnessTexture;
// 粗さテクスチャの使用比
uniform float uUseRoughnessTexture;
// B成分の金属度テクスチャ
uniform sampler2D uMetallicTexture;
// 金属度テクスチャの使用比
uniform float uUseMetallicTexture;
// R成分の遮蔽テクスチャ
uniform sampler2D uOcclusionTexture;
// 遮蔽テクスチャの使用比
uniform float uUseOcclusionTexture;
// 遮蔽成分の反映比
uniform float uOcclusionStrength;
// RGBの発光テクスチャ
uniform sampler2D uEmissiveTexture;
// 発光テクスチャの使用比
uniform float uUseEmissiveTexture;
// 発光RGBの倍率
uniform vec3 uEmissiveColor;
// 照明を除く色の混合比
uniform float uUnlit;
// 視点のWorld位置
uniform vec3 uCameraPosition;
// 距離フォグのRGBA
uniform vec4 uFogColor;
// フォグ開始・終了距離
uniform vec2 uFogRange;
// フォグを使うか
uniform float uFogEnabled;
// 最終的なRGBA出力
out vec4 outColor;

// 材質・照明・Alphaしきい値・距離フォグを適用する。
void main()
{
    // 正規化したWorld法線
    vec3 normal = normalize(vWorldNormal);
    if (uUseNormalTexture > 0.5)
    {
        // World位置の画面X微分
        vec3 positionDx = dFdx(vWorldPosition);
        // World位置の画面Y微分
        vec3 positionDy = dFdy(vWorldPosition);
        // UVの画面X微分
        vec2 uvDx = dFdx(vUv);
        // UVの画面Y微分
        vec2 uvDy = dFdy(vUv);
        // 接空間の接線基底
        vec3 tangent = positionDx * uvDy.y - positionDy * uvDx.y;
        // 接空間の従法線基底
        vec3 bitangent = -positionDx * uvDy.x + positionDy * uvDx.x;
        // 大きい基底の長さの二乗
        float basisLength = max(dot(tangent, tangent), dot(bitangent, bitangent));
        if (basisLength > 0.000001)
        {
            // 基底長の逆数
            float inverseBasisLength = inversesqrt(basisLength);
            tangent *= inverseBasisLength;
            bitangent *= inverseBasisLength;
            // 強度を適用した接空間法線
            vec3 mapped = texture(uNormalTexture, vUv).xyz * 2.0 - 1.0;
            mapped.xy *= uNormalStrength;
            normal = normalize(
                tangent * mapped.x + bitangent * mapped.y + normal * mapped.z);
        }
    }
    // 使用比を適用した表面色
    vec4 sampled = mix(vec4(1.0), texture(uTexture, vUv), uUseTexture);
    // 使用比を適用した複合材質
    vec4 materialSample = mix(
        vec4(1.0),
        texture(uMetallicRoughnessTexture, vUv),
        uUseMetallicRoughnessTexture);
    // テクスチャを反映した粗さ
    float roughness = clamp(uRoughness * materialSample.g, 0.04, 1.0);
    // テクスチャを反映した金属度
    float metallic = clamp(uMetallic * materialSample.b, 0.0, 1.0);
    roughness = clamp(roughness * mix(
        1.0, texture(uRoughnessTexture, vUv).g, uUseRoughnessTexture),
        0.04, 1.0);
    metallic = clamp(metallic * mix(
        1.0, texture(uMetallicTexture, vUv).b, uUseMetallicTexture),
        0.0, 1.0);
    // 反映比を適用した遮蔽係数
    float occlusion = mix(
        1.0,
        mix(1.0, texture(uOcclusionTexture, vUv).r, uOcclusionStrength),
        uUseOcclusionTexture);
    // テクスチャを反映した発光RGB
    vec3 emissive = uEmissiveColor * mix(
        vec3(1.0), texture(uEmissiveTexture, vUv).rgb, uUseEmissiveTexture);
    // 表面色とAlphaの乗算結果
    vec4 surface = vec4(sampled.rgb * uColor.rgb, sampled.a * uColor.a);
    if (uAlphaCutoff >= 0.0 && surface.a < uAlphaCutoff)
        discard;
    // 光源方向と法線の内積
    float diffuse = max(dot(normal, normalize(-uLightDirection)), 0.0);
    // 表面から視点への単位方向
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    // 視線と光源方向の中間方向
    vec3 halfDirection = normalize(viewDirection - normalize(uLightDirection));
    // 粗さから求めた鏡面の指数
    float specularPower = mix(128.0, 8.0, roughness);
    // 鏡面反射の強度
    float specular = pow(max(dot(normal, halfDirection), 0.0), specularPower);
    // 金属度を除いた拡散RGB
    vec3 diffuseColor = surface.rgb * (1.0 - metallic);
    // 材質に応じた反射RGB
    vec3 specularColor = mix(uDielectricSpecular, surface.rgb, metallic);
    // 照明と発光を反映したRGB
    vec3 litColor = diffuseColor * (
        uAmbientColor * uAmbientIntensity * occlusion
        + uDirectionalColor * uDirectionalIntensity * diffuse)
        + specularColor * specular * uDirectionalColor
            * uDirectionalIntensity + emissive;
    // 距離減衰と円錐の内外角を使った局所照明。
    for (int i = 0; i < 8; ++i) {
        if (i >= uLocalLightCount) break;
        vec3 offset = uLocalPositionRange[i].xyz - vWorldPosition;
        float lightDistance = length(offset);
        vec3 toLight = offset / max(lightDistance, 0.0001);
        float attenuation = max(1.0 - lightDistance / uLocalPositionRange[i].w, 0.0);
        attenuation *= attenuation;
        if (uLocalInnerSpot[i].y > 0.5) {
            float cone = dot(-toLight, uLocalDirectionOuter[i].xyz);
            attenuation *= clamp((cone - uLocalDirectionOuter[i].w)
                / max(uLocalInnerSpot[i].x - uLocalDirectionOuter[i].w, 0.0001), 0.0, 1.0);
        }
        float localDiffuse = max(dot(normal, toLight), 0.0);
        vec3 localHalf = normalize(viewDirection + toLight);
        float localSpecular = pow(max(dot(normal, localHalf), 0.0), specularPower);
        litColor += (diffuseColor * localDiffuse + specularColor * localSpecular)
            * uLocalColorIntensity[i].rgb * uLocalColorIntensity[i].a * attenuation;
    }
    litColor = mix(litColor, surface.rgb + emissive, uUnlit);
    // フォグ距離範囲の幅
    float fogSpan = max(uFogRange.y - uFogRange.x, 0.0001);
    // 表面に適用するフォグ比
    float fogAmount = clamp(
        (distance(vWorldPosition, uCameraPosition) - uFogRange.x) / fogSpan,
        0.0,
        1.0) * uFogEnabled;
    outColor = vec4(mix(litColor, uFogColor.rgb, fogAmount), surface.a);
}
)glsl";

        // WebGL1向け頂点GLSL
        constexpr char VertexShaderSourceWebGL1[] = R"glsl(
precision highp float;

// ローカル空間の頂点位置
attribute vec3 aPosition;
// ローカル空間の頂点法線
attribute vec3 aNormal;
// 頂点のテクスチャ座標
attribute vec2 aUv;

// 列優先のWorld変換行列
uniform mat4 uModel;
// 列優先のビュー変換行列
uniform mat4 uView;
// 列優先の透視投影行列
uniform mat4 uProjection;

// 補間するWorld法線
varying vec3 vWorldNormal;
// 補間するテクスチャ座標
varying vec2 vUv;
// 補間するWorld位置
varying vec3 vWorldPosition;

// World法線と位置を計算して頂点を透視投影する。
void main()
{
    // World変換の線形成分
    mat3 model3 = mat3(uModel);
    // 第0列の余因子ベクトル
    vec3 cofactor0 = cross(model3[1], model3[2]);
    // 第1列の余因子ベクトル
    vec3 cofactor1 = cross(model3[2], model3[0]);
    // 第2列の余因子ベクトル
    vec3 cofactor2 = cross(model3[0], model3[1]);
    // World変換の行列式
    float determinant = dot(model3[0], cofactor0);
    // 法線に使う逆転置行列
    mat3 normalMatrix = abs(determinant) > 0.000001
        ? mat3(cofactor0, cofactor1, cofactor2) / determinant
        : mat3(1.0);
    vWorldNormal = normalMatrix * aNormal;
    vUv = aUv;
    // World変換後の同次位置
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    gl_Position = uProjection * uView * worldPosition;
}
)glsl";

        // WebGL1向け材質GLSL
        constexpr char FragmentShaderSourceWebGL1[] = R"glsl(
precision mediump float;

// 補間するWorld法線
varying vec3 vWorldNormal;
// 補間するテクスチャ座標
varying vec2 vUv;
// 補間するWorld位置
varying vec3 vWorldPosition;
// 表面色のRGBA倍率
uniform vec4 uColor;
// 平行光が進むWorld方向
uniform vec3 uLightDirection;
uniform int uLocalLightCount;
uniform vec4 uLocalPositionRange[8];
uniform vec4 uLocalColorIntensity[8];
uniform vec4 uLocalDirectionOuter[8];
uniform vec2 uLocalInnerSpot[8];

// 環境光のRGB
uniform vec3 uAmbientColor;
// 環境光の強度
uniform float uAmbientIntensity;
// 平行光のRGB
uniform vec3 uDirectionalColor;
// 平行光の強度
uniform float uDirectionalIntensity;
// 粗さの倍率
uniform float uRoughness;
// 金属度の倍率
uniform float uMetallic;
// 非金属の反射RGB
uniform vec3 uDielectricSpecular;
// 表面色テクスチャ
uniform sampler2D uTexture;
// 表面色テクスチャの使用比
uniform float uUseTexture;
// 負値で無効のAlphaしきい値
uniform float uAlphaCutoff;
// G粗さ・B金属度テクスチャ
uniform sampler2D uMetallicRoughnessTexture;
// 複合材質テクスチャの使用比
uniform float uUseMetallicRoughnessTexture;
// G成分の粗さテクスチャ
uniform sampler2D uRoughnessTexture;
// 粗さテクスチャの使用比
uniform float uUseRoughnessTexture;
// B成分の金属度テクスチャ
uniform sampler2D uMetallicTexture;
// 金属度テクスチャの使用比
uniform float uUseMetallicTexture;
// R成分の遮蔽テクスチャ
uniform sampler2D uOcclusionTexture;
// 遮蔽テクスチャの使用比
uniform float uUseOcclusionTexture;
// 遮蔽成分の反映比
uniform float uOcclusionStrength;
// RGBの発光テクスチャ
uniform sampler2D uEmissiveTexture;
// 発光テクスチャの使用比
uniform float uUseEmissiveTexture;
// 発光RGBの倍率
uniform vec3 uEmissiveColor;
// 照明を除く色の混合比
uniform float uUnlit;
// 視点のWorld位置
uniform vec3 uCameraPosition;
// 距離フォグのRGBA
uniform vec4 uFogColor;
// フォグ開始・終了距離
uniform vec2 uFogRange;
// フォグを使うか
uniform float uFogEnabled;

// 材質・照明・Alphaしきい値・距離フォグを適用する。
void main()
{
    // 正規化したWorld法線
    vec3 normal = normalize(vWorldNormal);
    // 使用比を適用した表面色
    vec4 sampled = mix(vec4(1.0), texture2D(uTexture, vUv), uUseTexture);
    // 使用比を適用した複合材質
    vec4 materialSample = mix(
        vec4(1.0),
        texture2D(uMetallicRoughnessTexture, vUv),
        uUseMetallicRoughnessTexture);
    // テクスチャを反映した粗さ
    float roughness = clamp(uRoughness * materialSample.g, 0.04, 1.0);
    // テクスチャを反映した金属度
    float metallic = clamp(uMetallic * materialSample.b, 0.0, 1.0);
    roughness = clamp(roughness * mix(
        1.0, texture2D(uRoughnessTexture, vUv).g, uUseRoughnessTexture),
        0.04, 1.0);
    metallic = clamp(metallic * mix(
        1.0, texture2D(uMetallicTexture, vUv).b, uUseMetallicTexture),
        0.0, 1.0);
    // 反映比を適用した遮蔽係数
    float occlusion = mix(
        1.0,
        mix(1.0, texture2D(uOcclusionTexture, vUv).r, uOcclusionStrength),
        uUseOcclusionTexture);
    // テクスチャを反映した発光RGB
    vec3 emissive = uEmissiveColor * mix(
        vec3(1.0), texture2D(uEmissiveTexture, vUv).rgb, uUseEmissiveTexture);
    // 表面色とAlphaの乗算結果
    vec4 surface = vec4(sampled.rgb * uColor.rgb, sampled.a * uColor.a);
    if (uAlphaCutoff >= 0.0 && surface.a < uAlphaCutoff)
        discard;
    // 光源方向と法線の内積
    float diffuse = max(dot(normal, normalize(-uLightDirection)), 0.0);
    // 表面から視点への単位方向
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    // 視線と光源方向の中間方向
    vec3 halfDirection = normalize(viewDirection - normalize(uLightDirection));
    // 粗さから求めた鏡面の指数
    float specularPower = mix(128.0, 8.0, roughness);
    // 鏡面反射の強度
    float specular = pow(max(dot(normal, halfDirection), 0.0), specularPower);
    // 金属度を除いた拡散RGB
    vec3 diffuseColor = surface.rgb * (1.0 - metallic);
    // 材質に応じた反射RGB
    vec3 specularColor = mix(uDielectricSpecular, surface.rgb, metallic);
    // 照明と発光を反映したRGB
    vec3 litColor = diffuseColor * (
        uAmbientColor * uAmbientIntensity * occlusion
        + uDirectionalColor * uDirectionalIntensity * diffuse)
        + specularColor * specular * uDirectionalColor
            * uDirectionalIntensity + emissive;
    // 距離減衰と円錐の内外角を使った局所照明。
    for (int i = 0; i < 8; ++i) {
        if (i >= uLocalLightCount) break;
        vec3 offset = uLocalPositionRange[i].xyz - vWorldPosition;
        float lightDistance = length(offset);
        vec3 toLight = offset / max(lightDistance, 0.0001);
        float attenuation = max(1.0 - lightDistance / uLocalPositionRange[i].w, 0.0);
        attenuation *= attenuation;
        if (uLocalInnerSpot[i].y > 0.5) {
            float cone = dot(-toLight, uLocalDirectionOuter[i].xyz);
            attenuation *= clamp((cone - uLocalDirectionOuter[i].w)
                / max(uLocalInnerSpot[i].x - uLocalDirectionOuter[i].w, 0.0001), 0.0, 1.0);
        }
        float localDiffuse = max(dot(normal, toLight), 0.0);
        vec3 localHalf = normalize(viewDirection + toLight);
        float localSpecular = pow(max(dot(normal, localHalf), 0.0), specularPower);
        litColor += (diffuseColor * localDiffuse + specularColor * localSpecular)
            * uLocalColorIntensity[i].rgb * uLocalColorIntensity[i].a * attenuation;
    }
    litColor = mix(litColor, surface.rgb + emissive, uUnlit);
    // フォグ距離範囲の幅
    float fogSpan = max(uFogRange.y - uFogRange.x, 0.0001);
    // 表面に適用するフォグ比
    float fogAmount = clamp(
        (distance(vWorldPosition, uCameraPosition) - uFogRange.x) / fogSpan,
        0.0,
        1.0) * uFogEnabled;
    gl_FragColor = vec4(mix(litColor, uFogColor.rgb, fogAmount), surface.a);
}
)glsl";

        // 描画方式をDOMへ公開する(version: 2WebGL2／1WebGL1／0互換)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::BrowserSetRendererBackend;
#else
        EM_JS(void, BrowserSetRendererBackend, (int version), {
            if (!document.body) return;
            document.body.dataset.lamaponRenderer = version >= 2
                ? "webgl2" : version == 1 ? "webgl1" : "canvas2d";
        });
#endif


        // Canvas2DまたはSVGの描画先を用意する(selector: 元CanvasのCSS指定, width: 描画幅, height: 描画高さ)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DInitialize;
#else
        EM_JS(int, Canvas2DInitialize,
              (const char* selector, int width, int height), {
            // CSS指定から得た元Canvas
            const primaryCanvas = document.querySelector(UTF8ToString(selector));
            // WebGL初期化に使ったCanvasを再利用せず、専用のソフトウェア描画面を優先する。
            // 互換描画またはGLのCanvas
            const canvas = document.querySelector("#software-canvas") || primaryCanvas;
            if (!canvas) {
                console.error("LamaPon Canvas2D fallback: canvas not found");
                return 0;
            }
            // 互換描画のCanvas2D Context
            const context = typeof canvas.getContext === "function"
                ? canvas.getContext("2d")
                : null;
            // Canvasを使えない場合のSVG
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
#endif

        // 互換描画先のサイズを変更し、保留描画を破棄する(width: 描画幅, height: 描画高さ)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DResize;
#else
        EM_JS(void, Canvas2DResize, (int width, int height), {
            // ブラウザー共有の互換描画状態
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
#endif

        // 互換描画の背景を消去し、保留描画を破棄する(red: 背景R, green: 背景G, blue: 背景B, alpha: 背景Alpha)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DBeginFrame;
#else
        EM_JS(void, Canvas2DBeginFrame,
              (float red, float green, float blue, float alpha), {
            // ブラウザー共有の互換描画状態
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.queue = [];
            // 値を0～1に制限する(value: 制限する値)。
            const clamp = value => Math.max(0.0, Math.min(1.0, value));
            if (runtime.mode === "svg" && runtime.svg) {
                while (runtime.svg.firstChild) {
                    runtime.svg.removeChild(runtime.svg.firstChild);
                }
                if (runtime.sky?.enabled) {
                    // 空のRGBをCSS色へ変換する(color: RGB配列)。
                    const rgb = color => "rgb(" +
                        Math.round(clamp(color[0]) * 255) + "," +
                        Math.round(clamp(color[1]) * 255) + "," +
                        Math.round(clamp(color[2]) * 255) + ")";
                    // 空の上端色と水平線色を混合する(amount: 混合比, value: 上端の色成分, index: RGB成分番号)。
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
            // 互換描画のCanvas2D Context
            const context = runtime.context;
            context.setTransform(1, 0, 0, 1, 0, 0);
            if (runtime.sky?.enabled) {
                // 空または照明の色階調
                const gradient = context.createLinearGradient(
                    0, 0, 0, runtime.canvas.height);
                // 空のRGBをCSS色へ変換する(color: RGB配列)。
                const rgb = color => "rgb(" +
                    Math.round(clamp(color[0]) * 255) + "," +
                    Math.round(clamp(color[1]) * 255) + "," +
                    Math.round(clamp(color[2]) * 255) + ")";
                // 空の上端色と水平線色を混合する(amount: 混合比, value: 上端の色成分, index: RGB成分番号)。
                const mix = amount => runtime.sky.top.map(
                    (value, index) => value +
                        (runtime.sky.horizon[index] - value) * amount);
                // Native版と同じ空の階調になるよう補間点を配置します。
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
#endif

        // 互換描画の距離フォグを設定する(enabled: フォグを使うか, red: フォグR, green: フォグG, blue: フォグB, startDistance: 開始距離, endDistance: 終了距離)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DSetFog;
#else
        EM_JS(void, Canvas2DSetFog,
              (int enabled, float red, float green, float blue,
               float startDistance, float endDistance), {
            // ブラウザー共有の互換描画状態
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.fog = {
                enabled: !!enabled,
                color: [red, green, blue],
                start: Math.max(0.0, startDistance),
                end: Math.max(startDistance + 0.001, endDistance),
            };
        });
#endif

        // 互換描画の空の階調を設定する(enabled: 空を使うか, topRed: 上端R, topGreen: 上端G, topBlue: 上端B, horizonRed: 水平線R, horizonGreen: 水平線G, horizonBlue: 水平線B)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DSetSky;
#else
        EM_JS(void, Canvas2DSetSky,
              (int enabled,
               float topRed, float topGreen, float topBlue,
               float horizonRed, float horizonGreen, float horizonBlue), {
            // ブラウザー共有の互換描画状態
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime) return;
            runtime.sky = {
                enabled: !!enabled,
                top: [topRed, topGreen, topBlue],
                horizon: [horizonRed, horizonGreen, horizonBlue],
            };
        });
#endif

        // 32成分単位の三角形をJSへ複製して描画予約する(vertices: 転送データのポインター, floatCount: float成分数, red: 表面R, green: 表面G, blue: 表面B, alpha: 表面Alpha, textureId: 表面色テクスチャID, alphaBlended: 透過合成するか, alphaCutoff: Alphaしきい値, additiveBlend: 加算合成するか)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DQueueTriangles;
#else
        EM_JS(void, Canvas2DQueueTriangles,
              (const float* vertices, int floatCount,
               float red, float green, float blue, float alpha,
               int textureId, int alphaBlended, float alphaCutoff,
               int additiveBlend), {
            // ブラウザー共有の互換描画状態
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime || !vertices || floatCount < 32) return;
            // 三角形コマンドの成分列
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
#endif

        // 予約した三角形をSVG・深度バッファ・アフィン描画のいずれかで描く。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::Canvas2DEndFrame;
#else
        EM_JS(void, Canvas2DEndFrame, (), {
            // ブラウザー共有の互換描画状態
            const runtime = globalThis.__lamaponCanvas2D;
            if (!runtime || !runtime.queue) return;
            // 互換描画の開始時刻（ms）
            const rasterStarted = performance.now();
            // 値を0～1に制限する(value: 制限する値)。
            const clamp = value => Math.max(0.0, Math.min(1.0, value));
            // 転送三角形1つのfloat成分数
            const triangleStride = 32;
            // 描画する三角形の一覧
            const triangles = [];
            // 現在処理する描画コマンド
            for (const command of runtime.queue) {
                // 三角形の先頭成分位置
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
            // 不透明なテクスチャ付き三角形をまとめ、ビュー距離で奥から描く。
            // 不透明なテクスチャ付き三角形を抽出する(triangle: 描画候補)。
            const opaqueTexturedTriangles = triangles.filter(triangle =>
                triangle.command.textureId != 0
                && !triangle.command.alphaBlended);
            if (opaqueTexturedTriangles.length > 0) {
                // 不透明画像を奥から並べる(left: 左三角形, right: 右三角形)。
                opaqueTexturedTriangles.sort((left, right) =>
                    right.command.values[right.index + 16]
                    - left.command.values[left.index + 16]);
                // 合成順に並べた描画候補
                const orderedTriangles = [];
                // 不透明画像の描画群を挿入済みか
                let insertedOpaqueTextures = false;
                // 現在処理する描画三角形
                for (const triangle of triangles) {
                    // 不透明な画像付き三角形か
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
                // SVG要素の名前空間
                const namespace = "http://www.w3.org/2000/svg";
                // 現在処理する描画三角形
                for (const triangle of triangles) {
                    // 現在処理する描画コマンド
                    const command = triangle.command;
                    // 三角形コマンドの成分列
                    const values = command.values;
                    // 三角形の先頭成分位置
                    const index = triangle.index;
                    // 補間に使う照明R倍率
                    const lightRed = Math.max(0.0, values[index + 6]);
                    // 補間に使う照明G倍率
                    const lightGreen = Math.max(0.0, values[index + 7]);
                    // 補間に使う照明B倍率
                    const lightBlue = Math.max(0.0, values[index + 8]);
                    // 距離フォグの設定
                    const fog = runtime.fog;
                    // 適用する距離フォグ比
                    const fogAmount = fog.enabled
                        ? clamp((values[index + 16] - fog.start)
                            / Math.max(0.001, fog.end - fog.start))
                        : 0.0;
                    // フォグを掛けた表面R
                    const mixedRed = command.red * lightRed
                        + (fog.color[0] - command.red * lightRed) * fogAmount;
                    // フォグを掛けた表面G
                    const mixedGreen = command.green * lightGreen
                        + (fog.color[1] - command.green * lightGreen) * fogAmount;
                    // フォグを掛けた表面B
                    const mixedBlue = command.blue * lightBlue
                        + (fog.color[2] - command.blue * lightBlue) * fogAmount;
                    // SVGへ追加する三角形
                    const polygon = document.createElementNS(namespace, "polygon");
                    polygon.setAttribute(
                        "points",
                        values[index] + "," + values[index + 1] + " " +
                        values[index + 2] + "," + values[index + 3] + " " +
                        values[index + 4] + "," + values[index + 5]);
                    // SVGの塗りと境界に使う色
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
            // 互換描画のCanvas2D Context
            const context = runtime.context;
            context.imageSmoothingEnabled = true;
            if ("imageSmoothingQuality" in context) {
                context.imageSmoothingQuality = "high";
            }
            // アフィン描画した三角形数
            let affinePaintCount = 0;
            // 共有辺の隙間を埋めるよう三角形を拡張する(first: 第1頂点, second: 第2頂点, third: 第3頂点, overlap: 外へ広げる画素数)。
            const expandTriangle = (first, second, third, overlap = 1.00) => {
                // 三角形中心の画面X
                const centerX = (first.x + second.x + third.x) / 3.0;
                // 三角形中心の画面Y
                const centerY = (first.y + second.y + third.y) / 3.0;
                // 頂点を中心から外へ広げる(vertex: 三角形の頂点)。
                return [first, second, third].map(vertex => {
                    // 三角形中心から頂点へのX差
                    const dx = vertex.x - centerX;
                    // 三角形中心から頂点へのY差
                    const dy = vertex.y - centerY;
                    // 中心から頂点までの画素長
                    const length = Math.hypot(dx, dy) || 1.0;
                    return [
                        vertex.x + dx / length * overlap,
                        vertex.y + dy / length * overlap,
                    ];
                });
            };
            // UVと画面位置のアフィン変換でテクスチャを描く(first: 第1頂点, second: 第2頂点, third: 第3頂点, pattern: 繰返し画像, imageWidth: 画像幅, imageHeight: 画像高さ)。
            const paintAffineTexture = (
                first, second, third, pattern, imageWidth, imageHeight) => {
                // 第1頂点の画像X
                const sx0 = first.u * imageWidth;
                // 第1頂点の画像Y
                const sy0 = first.v * imageHeight;
                // 第2頂点の画像X
                const sx1 = second.u * imageWidth;
                // 第2頂点の画像Y
                const sy1 = second.v * imageHeight;
                // 第3頂点の画像X
                const sx2 = third.u * imageWidth;
                // 第3頂点の画像Y
                const sy2 = third.v * imageHeight;
                // 第1辺の画像X差
                const dsx1 = sx1 - sx0;
                // 第1辺の画像Y差
                const dsy1 = sy1 - sy0;
                // 第2辺の画像X差
                const dsx2 = sx2 - sx0;
                // 第2辺の画像Y差
                const dsy2 = sy2 - sy0;
                // UV辺の行列式
                const det = dsx1 * dsy2 - dsx2 * dsy1;
                if (Math.abs(det) <= 0.001) return false;

                // 第1辺の画面X差
                const dpx1 = second.x - first.x;
                // 第1辺の画面Y差
                const dpy1 = second.y - first.y;
                // 第2辺の画面X差
                const dpx2 = third.x - first.x;
                // 第2辺の画面Y差
                const dpy2 = third.y - first.y;
                // アフィン変換のX基底X成分
                const a = (dpx1 * dsy2 - dpx2 * dsy1) / det;
                // アフィン変換のY基底X成分
                const c = (dsx1 * dpx2 - dsx2 * dpx1) / det;
                // アフィン変換のX基底Y成分
                const b = (dpy1 * dsy2 - dpy2 * dsy1) / det;
                // アフィン変換のY基底Y成分
                const d = (dsx1 * dpy2 - dsx2 * dpy1) / det;
                // 隙間を埋める拡張済み三角形
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
                // アフィン塗りの画像X下限
                const minSourceX = Math.min(sx0, sx1, sx2) - imageWidth;
                // アフィン塗りの画像X上限
                const maxSourceX = Math.max(sx0, sx1, sx2) + imageWidth;
                // アフィン塗りの画像Y下限
                const minSourceY = Math.min(sy0, sy1, sy2) - imageHeight;
                // アフィン塗りの画像Y上限
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
            // 画面上の中点と透視補正したUVを求める(first: 第1頂点, second: 第2頂点)。
            const perspectiveMidpoint = (first, second) => {
                // 補間した同次Wの逆数
                const reciprocalW = (first.reciprocalW + second.reciprocalW) * 0.5;
                // 辺両端の同次W逆数の和
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
            // 辺の中点でアフィンUVとの差を画像画素単位で求める(first: 第1頂点, second: 第2頂点, imageWidth: 画像幅, imageHeight: 画像高さ)。
            const perspectiveEdgeError = (
                first, second, imageWidth, imageHeight) => {
                // 透視補正した辺中点
                const midpoint = perspectiveMidpoint(first, second);
                // 線形補間した辺中点のU
                const affineU = (first.u + second.u) * 0.5;
                // 線形補間した辺中点のV
                const affineV = (first.v + second.v) * 0.5;
                return Math.hypot(
                    (midpoint.u - affineU) * imageWidth,
                    (midpoint.v - affineV) * imageHeight);
            };
            // UVの誤差に応じて最大3段まで辺を分割して描く(first: 第1頂点, second: 第2頂点, third: 第3頂点, pattern: 繰返し画像, imageWidth: 画像幅, imageHeight: 画像高さ)。
            const paintPerspectiveTexture = (
                first, second, third, pattern, imageWidth, imageHeight) => {
                // 辺分割を待つ三角形一覧
                const pending = [{ vertices: [first, second, third], depth: 0 }];
                // 分割三角形を1つでも描いたか
                let painted = false;
                while (pending.length > 0) {
                    // 辺分割を待つ三角形
                    const item = pending.pop();
                    // 辺分割で処理する3頂点
                    const vertices = item.vertices;
                    // 画面上の二倍面積
                    const area = Math.abs(
                        (vertices[1].x - vertices[0].x)
                            * (vertices[2].y - vertices[0].y)
                        - (vertices[1].y - vertices[0].y)
                            * (vertices[2].x - vertices[0].x));
                    // 三角形の辺の頂点組
                    const edges = [[0, 1], [1, 2], [2, 0]];
                    // 二分割する辺の番号
                    let splitEdge = 0;
                    // 各辺の最大UV誤差
                    let largestError = -1.0;
                    // 最大誤差を持つ辺の画素長
                    let largestLength = 0.0;
                    // 評価する辺の番号
                    for (let edgeIndex = 0; edgeIndex < edges.length; ++edgeIndex) {
                        // 評価・分割する辺の頂点番号
                        const edge = edges[edgeIndex];
                        // 辺中点のUV誤差（画像画素）
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
                    // 現在の分割深さのUV誤差上限
                    const requiredError = 3.0 * Math.pow(2.0, item.depth);
                    if (item.depth < 3
                        && area > 4.0
                        && largestLength > 20.0
                        && largestError > requiredError) {
                        // 評価・分割する辺の頂点番号
                        const edge = edges[splitEdge];
                        // 分割辺に含まれない頂点番号
                        const remaining = 3 - edge[0] - edge[1];
                        // 透視補正した辺中点
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
            // 画像が揃えば透視補正UVと深度で描き、利用不可ならfalseを返す(sourceTriangles: 全描画候補)。
            const paintDepthBufferedFrame = sourceTriangles => {
                if (sourceTriangles.length === 0) return false;
                // 現在処理する描画三角形
                for (const triangle of sourceTriangles) {
                    if (triangle.command.textureId == 0) continue;
                    // ブラウザー共有の画像登録状態
                    const slot = globalThis.__lamaponTextures?.[
                        triangle.command.textureId];
                    if (!slot || !slot.ready || !slot.softwareLevels
                        || slot.softwareLevels.length === 0) {
                        return false;
                    }
                }
                // Canvasの描画幅
                const width = runtime.canvas.width;
                // Canvasの描画高さ
                const height = runtime.canvas.height;
                // 背景も含むCanvas画素データ
                let frame;
                try {
                    frame = context.getImageData(0, 0, width, height);
                }
                // 画素の取得失敗時は深度描画を使わない(error: 取得エラー)。
                catch (error) {
                    console.warn(
                        "LamaPon software depth buffer unavailable", error);
                    return false;
                }
                // Canvas出力のRGBA画素列
                const output = frame.data;
                // 描画先の総画素数
                const pixelCount = width * height;
                if (!runtime.textureDepthBuffer
                    || runtime.textureDepthBuffer.length !== pixelCount) {
                    runtime.textureDepthBuffer = new Float32Array(pixelCount);
                }
                // 画素ごとの最近NDC深度
                const depthBuffer = runtime.textureDepthBuffer;
                depthBuffer.fill(1.000001);
                // 合成した画素の累積数
                let paintedPixels = 0;
                // 不透明は手前・透過は奥から並べる(left: 左三角形, right: 右三角形)。
                // 合成順に並べた描画候補
                const orderedTriangles = sourceTriangles.slice().sort(
                    (left, right) => {
                        // 並べ替え左側は透過か
                        const leftTransparent =
                            left.command.alphaBlended;
                        // 並べ替え右側は透過か
                        const rightTransparent =
                            right.command.alphaBlended;
                        if (leftTransparent !== rightTransparent) {
                            return leftTransparent ? 1 : -1;
                        }
                        // 不透明な形状は手前から処理し、隠れた画素をImageDataへ書き込む前に深度テストで除外します。
                        if (!leftTransparent) {
                            return left.command.values[left.index + 16]
                                - right.command.values[right.index + 16];
                        }
                        return right.command.values[right.index + 16]
                            - left.command.values[left.index + 16];
                    });
                // 現在処理する描画三角形
                for (const triangle of orderedTriangles) {
                    // 現在処理する描画コマンド
                    const command = triangle.command;
                    // 三角形コマンドの成分列
                    const values = command.values;
                    // 三角形の先頭成分位置
                    const index = triangle.index;
                    // 第1頂点の画面X
                    const x0 = values[index];
                    // 第1頂点の画面Y
                    const y0 = values[index + 1];
                    // 第2頂点の画面X
                    const x1 = values[index + 2];
                    // 第2頂点の画面Y
                    const y1 = values[index + 3];
                    // 第3頂点の画面X
                    const x2 = values[index + 4];
                    // 第3頂点の画面Y
                    const y2 = values[index + 5];
                    // 重心座標に使う二倍面積
                    const denominator = (y1 - y2) * (x0 - x2)
                        + (x2 - x1) * (y0 - y2);
                    if (Math.abs(denominator) < 0.00001) continue;
                    // 二倍面積の逆数
                    const inverseDenominator = 1.0 / denominator;
                    // 描画候補画素の左端
                    const minimumX = Math.max(
                        0, Math.floor(Math.min(x0, x1, x2)));
                    // 描画候補画素の右端
                    const maximumX = Math.min(
                        width - 1, Math.ceil(Math.max(x0, x1, x2)));
                    // 描画候補画素の上端
                    const minimumY = Math.max(
                        0, Math.floor(Math.min(y0, y1, y2)));
                    // 描画候補画素の下端
                    const maximumY = Math.min(
                        height - 1, Math.ceil(Math.max(y0, y1, y2)));
                    if (minimumX > maximumX || minimumY > maximumY) continue;

                    // 画像付きの描画コマンドか
                    const textured = command.textureId != 0;
                    // ブラウザー共有の画像登録状態
                    const slot = textured
                        ? globalThis.__lamaponTextures[command.textureId]
                        : null;
                    // 画素取得に使うミップ
                    let level = null;
                    // 選択したミップのRGBA画素列
                    let source = null;
                    if (textured) {
                        // 画像の最大解像度ミップ
                        const baseLevel = slot.softwareLevels[0];
                        // 画面上の辺に対する画像画素数の比を求める(ax: 始点の画面X, ay: 始点の画面Y, au: 始点U, av: 始点V, bx: 終点の画面X, by: 終点の画面Y, bu: 終点U, bv: 終点V)。
                        const texelDensity = (
                            ax, ay, au, av, bx, by, bu, bv) =>
                            Math.hypot(
                                (bu - au) * baseLevel.width,
                                (bv - av) * baseLevel.height)
                            / Math.max(0.5, Math.hypot(
                                bx - ax, by - ay));
                        // 三角形各辺の最大画素密度
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
                        // 密度に応じて選ぶミップ番号
                        const mipIndex = Math.min(
                            slot.softwareLevels.length - 1,
                            Math.max(0, Math.floor(Math.log2(
                                Math.max(1.0, maximumDensity)))));
                        level = slot.softwareLevels[mipIndex];
                        source = level.pixels;
                    }
                    // 第1頂点の同次W逆数
                    const q0 = values[index + 17];
                    // 第2頂点の同次W逆数
                    const q1 = values[index + 18];
                    // 第3頂点の同次W逆数
                    const q2 = values[index + 19];
                    // 走査する画素のY
                    for (let pixelY = minimumY;
                         pixelY <= maximumY; ++pixelY) {
                        // 画素中心のY
                        const sampleY = pixelY + 0.5;
                        // 走査する画素のX
                        for (let pixelX = minimumX;
                             pixelX <= maximumX; ++pixelX) {
                            // 画素中心のX
                            const sampleX = pixelX + 0.5;
                            // 第1頂点の重心座標
                            const weight0 = ((y1 - y2) * (sampleX - x2)
                                + (x2 - x1) * (sampleY - y2))
                                * inverseDenominator;
                            // 第2頂点の重心座標
                            const weight1 = ((y2 - y0) * (sampleX - x2)
                                + (x0 - x2) * (sampleY - y2))
                                * inverseDenominator;
                            // 第3頂点の重心座標
                            const weight2 = 1.0 - weight0 - weight1;
                            if (weight0 < -0.00001
                                || weight1 < -0.00001
                                || weight2 < -0.00001) continue;
                            // Canvas内の画素番号
                            const pixel = pixelY * width + pixelX;
                            // 画素で補間したNDC深度
                            const depth = weight0 * values[index + 20]
                                + weight1 * values[index + 21]
                                + weight2 * values[index + 22];
                            if (depth >= depthBuffer[pixel]) continue;
                            // 補間した同次Wの逆数
                            const reciprocalW = weight0 * q0
                                + weight1 * q1 + weight2 * q2;
                            if (reciprocalW <= 0.0000001) continue;
                            // 透視補正したテクスチャU
                            const u = (weight0 * values[index + 9] * q0
                                + weight1 * values[index + 11] * q1
                                + weight2 * values[index + 13] * q2)
                                / reciprocalW;
                            // 透視補正したテクスチャV
                            const v = (weight0 * values[index + 10] * q0
                                + weight1 * values[index + 12] * q1
                                + weight2 * values[index + 14] * q2)
                                / reciprocalW;
                            // テクスチャ乗算後の表面R
                            let surfaceRed = command.red;
                            // テクスチャ乗算後の表面G
                            let surfaceGreen = command.green;
                            // テクスチャ乗算後の表面B
                            let surfaceBlue = command.blue;
                            // テクスチャ乗算後のAlpha
                            let surfaceAlpha = clamp(command.alpha);
                            if (textured) {
                                // 繰返しを適用したU
                                const wrappedU = u - Math.floor(u);
                                // 繰返しを適用したV
                                const wrappedV = v - Math.floor(v);
                                // ミップ内の参照X画素
                                const textureX = Math.min(
                                    level.width - 1,
                                    Math.floor(wrappedU * level.width));
                                // ミップ内の参照Y画素
                                const textureY = Math.min(
                                    level.height - 1,
                                    Math.floor(wrappedV * level.height));
                                // 画像参照画素の先頭RGBA位置
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
                            // 透視補正したビュー距離
                            const viewDistance = (
                                weight0 * values[index + 23] * q0
                                + weight1 * values[index + 24] * q1
                                + weight2 * values[index + 25] * q2)
                                / reciprocalW;
                            // 適用する距離フォグ比
                            const fogAmount = runtime.fog.enabled
                                ? clamp((viewDistance - runtime.fog.start)
                                    / Math.max(
                                        0.001,
                                        runtime.fog.end - runtime.fog.start))
                                : 0.0;
                            // 補間に使う照明R倍率
                            const lightRed = (
                                weight0 * values[index + 6] * q0
                                + weight1 * values[index + 26] * q1
                                + weight2 * values[index + 29] * q2)
                                / reciprocalW;
                            // 補間に使う照明G倍率
                            const lightGreen = (
                                weight0 * values[index + 7] * q0
                                + weight1 * values[index + 27] * q1
                                + weight2 * values[index + 30] * q2)
                                / reciprocalW;
                            // 補間に使う照明B倍率
                            const lightBlue = (
                                weight0 * values[index + 8] * q0
                                + weight1 * values[index + 28] * q1
                                + weight2 * values[index + 31] * q2)
                                / reciprocalW;
                            // 照明を掛けた画素R
                            const litRed = surfaceRed * lightRed;
                            // 照明を掛けた画素G
                            const litGreen = surfaceGreen * lightGreen;
                            // 照明を掛けた画素B
                            const litBlue = surfaceBlue * lightBlue;
                            // フォグを掛けた最終R
                            const finalRed = clamp(
                                litRed + (runtime.fog.color[0] - litRed)
                                    * fogAmount);
                            // フォグを掛けた最終G
                            const finalGreen = clamp(
                                litGreen + (runtime.fog.color[1] - litGreen)
                                    * fogAmount);
                            // フォグを掛けた最終B
                            const finalBlue = clamp(
                                litBlue + (runtime.fog.color[2] - litBlue)
                                    * fogAmount);
                            // 出力画素の先頭RGBA位置
                            const outputOffset = pixel * 4;
                            // 背景を残すAlphaの比
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

            // フォグ前の不透明な無地Mesh用の照明グラデーションを再利用する(command: 描画コマンド)。
            const smoothUntexturedFill = command => {
                if (Object.prototype.hasOwnProperty.call(
                        command, "smoothUntexturedFill")) {
                    return command.smoothUntexturedFill;
                }
                if (command.textureId != 0 || command.alphaBlended) {
                    command.smoothUntexturedFill = null;
                    return null;
                }
                // 三角形コマンドの成分列
                const values = command.values;
                // 照明平均に使うサンプル数
                let sampleCount = 0;
                // 無地Meshの平均照明R
                let averageRed = 0.0;
                // 無地Meshの平均照明G
                let averageGreen = 0.0;
                // 無地Meshの平均照明B
                let averageBlue = 0.0;
                // Mesh内の最大ビュー距離
                let maximumDistance = 0.0;
                // 最も暗い照明サンプル
                let darkest = null;
                // 最も明るい照明サンプル
                let brightest = null;
                // 三角形の先頭成分位置
                for (let index = 0;
                     index + triangleStride - 1 < values.length;
                     index += triangleStride) {
                    // 三角形の照明R倍率
                    const red = Math.max(0.0, values[index + 6]);
                    // 三角形の照明G倍率
                    const green = Math.max(0.0, values[index + 7]);
                    // 三角形の照明B倍率
                    const blue = Math.max(0.0, values[index + 8]);
                    // 三角形中心の照明サンプル
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
                // 三角形の平均距離がフォグ開始より遠いMeshでは、照明グラデーションを使わず個別に描く。
                if (sampleCount === 0
                    || (runtime.fog.enabled
                        && maximumDistance > runtime.fog.start)) {
                    command.smoothUntexturedFill = null;
                    return null;
                }
                averageRed /= sampleCount;
                averageGreen /= sampleCount;
                averageBlue /= sampleCount;
                // 照明と材質色を掛けてCSS色へ変換する(red: 照明R倍率, green: 照明G倍率, blue: 照明B倍率)。
                const rgba = (red, green, blue) => "rgba(" +
                    Math.round(clamp(command.red * red) * 255) + "," +
                    Math.round(clamp(command.green * green) * 255) + "," +
                    Math.round(clamp(command.blue * blue) * 255) + "," +
                    clamp(command.alpha) + ")";
                // 明暗サンプル間の輝度差
                const lightingRange = brightest.brightness
                    - darkest.brightness;
                // 照明階調の画面上の長さ
                const gradientLength = Math.hypot(
                    brightest.x - darkest.x,
                    brightest.y - darkest.y);
                if (lightingRange < 0.025 || gradientLength < 2.0) {
                    command.smoothUntexturedFill = rgba(
                        averageRed, averageGreen, averageBlue);
                    return command.smoothUntexturedFill;
                }
                // 照明差を平均の方向へ抑える(sample: 頂点の照明値, average: 平均照明値)。
                const soften = (sample, average) =>
                    average + (sample - average) * 0.55;
                // 空または照明の色階調
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
            // 深度バッファ描画が完了したか
            const depthBufferedFramePainted =
                paintDepthBufferedFrame(triangles);
            // 現在処理する描画三角形
            for (const triangle of triangles) {
                if (depthBufferedFramePainted) break;
                // 現在処理する描画コマンド
                const command = triangle.command;
                // 三角形コマンドの成分列
                const values = command.values;
                // 三角形の先頭成分位置
                const index = triangle.index;
                // 前の三角形のAlpha・合成・フィルター状態を次の描画へ持ち越さない。
                context.globalAlpha = 1.0;
                context.globalCompositeOperation =
                    command.additiveBlend ? "lighter" : "source-over";
                context.filter = "none";
                // 補間に使う照明R倍率
                const lightRed = Math.max(0.0, values[index + 6]);
                // 補間に使う照明G倍率
                const lightGreen = Math.max(0.0, values[index + 7]);
                // 補間に使う照明B倍率
                const lightBlue = Math.max(0.0, values[index + 8]);
                // 照明RGBから求めた輝度
                const brightness = clamp(
                    lightRed * 0.2126 + lightGreen * 0.7152 + lightBlue * 0.0722);
                // 第1頂点の画面X
                const x0 = values[index];
                // 第1頂点の画面Y
                const y0 = values[index + 1];
                // 第2頂点の画面X
                const x1 = values[index + 2];
                // 第2頂点の画面Y
                const y1 = values[index + 3];
                // 第3頂点の画面X
                const x2 = values[index + 4];
                // 第3頂点の画面Y
                const y2 = values[index + 5];
                // 三角形中心の画面X
                const centerX = (x0 + x1 + x2) / 3.0;
                // 三角形中心の画面Y
                const centerY = (y0 + y1 + y2) / 3.0;
                // 三角形の中心から頂点を0.7画素外へ広げる(x: 頂点X, y: 頂点Y)。
                const expandPoint = (x, y) => {
                    // 三角形中心から頂点へのX差
                    const dx = x - centerX;
                    // 三角形中心から頂点へのY差
                    const dy = y - centerY;
                    // 中心から頂点までの画素長
                    const length = Math.hypot(dx, dy) || 1.0;
                    // 隙間を埋める拡張画素数
                    const overlap = 0.70;
                    return [
                        x + dx / length * overlap,
                        y + dy / length * overlap,
                    ];
                };
                // 拡張した第1頂点の画面位置
                const expanded0 = expandPoint(x0, y0);
                // 拡張した第2頂点の画面位置
                const expanded1 = expandPoint(x1, y1);
                // 拡張した第3頂点の画面位置
                const expanded2 = expandPoint(x2, y2);
                // ブラウザー共有の画像登録状態
                const slot = globalThis.__lamaponTextures?.[command.textureId];
                // 読込済みのブラウザー画像
                const image = slot && slot.ready ? slot.image : null;
                // 距離フォグの設定
                const fog = runtime.fog;
                // 適用する距離フォグ比
                const fogAmount = fog.enabled
                    ? clamp((values[index + 16] - fog.start)
                        / Math.max(0.001, fog.end - fog.start))
                    : 0.0;
                if (image) {
                    // アフィン描画の画像幅
                    const iw = image.width || 1;
                    // アフィン描画の画像高さ
                    const ih = image.height || 1;
                    slot.pattern2d = slot.pattern2d
                        || context.createPattern(image, "repeat");
                    context.globalAlpha = clamp(command.alpha);
                    // 画像を分割して描画できたか
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
                        // 不透明テクスチャには元の三角形ごとにオーバーレイを1回描き、照明とフォグを適用します。
                        if (!command.alphaBlended) {
                            // 照明とフォグ後に残す画像比
                            const remainingTexture = brightness
                                * (1.0 - fogAmount);
                            // 照明・フォグ上塗りのAlpha
                            const overlayAlpha = clamp(
                                1.0 - remainingTexture);
                            // オーバーレイ内のフォグ比
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
                // 無地Meshに使う連続照明色
                const smoothFill = smoothUntexturedFill(command);
                if (smoothFill) {
                    context.fillStyle = smoothFill;
                } else {
                    // 照明を掛けた表面R
                    const baseRed = command.red * lightRed;
                    // 照明を掛けた表面G
                    const baseGreen = command.green * lightGreen;
                    // 照明を掛けた表面B
                    const baseBlue = command.blue * lightBlue;
                    // フォグを掛けた表面R
                    const mixedRed = baseRed
                        + (fog.color[0] - baseRed) * fogAmount;
                    // フォグを掛けた表面G
                    const mixedGreen = baseGreen
                        + (fog.color[1] - baseGreen) * fogAmount;
                    // フォグを掛けた表面B
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
                // 塗りと同じグラデーションで共有辺を描き、アンチエイリアスの隙間を埋める。
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
                // 今回の互換描画時間（ms）
                const rasterMilliseconds = performance.now() - rasterStarted;
                document.body.dataset.lamaponTriangles = String(triangles.length);
                document.body.dataset.lamaponTexturePaints = String(affinePaintCount);
                document.body.dataset.lamaponRasterMilliseconds =
                    rasterMilliseconds.toFixed(2);
                runtime.measuredFrames = (runtime.measuredFrames || 0) + 1;
                if (runtime.measuredFrames > 30
                    && rasterMilliseconds > (runtime.maximumRasterMilliseconds || 0)) {
                    runtime.maximumRasterMilliseconds = rasterMilliseconds;
                    // 最大描画時間を記録するHUD
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
#endif

        // 仮想FSの画像を非同期に読み込みIDを返す(virtualPath: 仮想FS内の画像パス, rendererVersion: 2WebGL2／1WebGL1／0互換)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::BrowserTextureCreate;
#else
        EM_JS(int, BrowserTextureCreate,
              (const char* virtualPath, int rendererVersion, const unsigned char* encoded, int byteCount), {
            // Rendererと同じ現在のContextへ接続し、任意のCanvasを扱う。
            const gl = rendererVersion > 0 && GL.currentContext ? GL.currentContext.GLctx : null;
            if ((!encoded || byteCount <= 0) && !globalThis.FS) return 0;
            if (gl) globalThis.__lamaponWebGl = gl;
            globalThis.__lamaponTextures = globalThis.__lamaponTextures || {};
            globalThis.__lamaponNextTextureId = globalThis.__lamaponNextTextureId || 1;
            // 新規画像に発行するID
            const id = globalThis.__lamaponNextTextureId++;
            // ブラウザー共有の画像登録状態
            const slot = {
                texture: null,
                image: null,
                pattern2d: null,
                softwareLevels: null,
                ready: false,
            };
            globalThis.__lamaponTextures[id] = slot;
            try {
                // 仮想FSから読んだ画像データ
                const bytes = encoded && byteCount > 0
                    ? HEAPU8.slice(encoded, encoded + byteCount)
                    : FS.readFile(UTF8ToString(virtualPath));
                // 画像先頭から判定したMIME
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
                // ブラウザーが読む画像バイト列
                const blob = new Blob([bytes], { type: imageMime });
                // 画像をGLまたはCPUミップへ登録する(bitmap: デコード済み画像, error: デコード・登録エラー)。
                slot.gl = gl;
                slot.generation = 0;
                slot.recreate = () => {
                    const generation = ++slot.generation;
                    slot.ready = false;
                    slot.texture = null;
                    return createImageBitmap(blob).then(bitmap => {
                    if (generation !== slot.generation || (gl && gl.isContextLost())) {
                        bitmap.close(); return;
                    }
                    if (gl) {
                        // 画像を保持するGL Texture
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
                        // 透視補正したUVで深度描画するため、CPU参照用のミップ列を保持する。
                        // CPU描画用ミップの一覧
                        const levels = [];
                        // 次のミップを生成する画像
                        let source = bitmap;
                        // 生成中のミップ幅
                        let width = Math.max(1, bitmap.width);
                        // 生成中のミップ高さ
                        let height = Math.max(1, bitmap.height);
                        while (true) {
                            // ミップ生成に使うCanvas
                            const surface = document.createElement("canvas");
                            surface.width = width;
                            surface.height = height;
                            // ミップ生成のCanvas2D Context
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
                };
                slot.recreate();
                return id;
            }
            // 同期読込の失敗時は画像IDを破棄する(error: 読込エラー)。
            catch (error) {
                console.warn("LamaPon Web texture file unavailable", virtualPath, error);
                delete globalThis.__lamaponTextures[id];
                return 0;
            }
        });
#endif

        // 読込済み画像をGL_TEXTURE0へ接続する(textureId: ブラウザー共有の画像ID)。
        #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::BrowserTextureBind;
#else
        EM_JS(int, BrowserTextureBind, (int textureId), {
            // 画像を接続する共有WebGL
            const gl = globalThis.__lamaponWebGl;
            // ブラウザー共有の画像登録状態
            const slot = globalThis.__lamaponTextures?.[textureId];
            if (!gl || !slot || !slot.ready) return 0;
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, slot.texture);
            return 1;
        });
#endif

        struct ClipVertex final
        {
            // 同次座標のX
            float x{};
            // 同次座標のY
            float y{};
            // 同次座標のZ
            float z{};
            // 透視除算のW成分
            float w{};
        };

        // 点を同次座標へ変換する(matrix: 変換行列, point: 3D位置)。
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

        // GLSLをコンパイルし、失敗時はShaderを破棄して0を返す(type: Shader種別, source: 終端付きGLSL文字列)。
        [[nodiscard]] GLuint CompileShader(
            GLenum type,
            const char* source) noexcept
        {
#if defined(LAMAPON_NATIVE_RUNTIME) && !defined(__ANDROID__)
            // GLES 3の共通ShaderをデスクトップGLSL 3.30へ接続する。
            std::string desktopSource(source);
            const auto version = desktopSource.find("#version 300 es");
            if (version != std::string::npos) desktopSource.replace(version, 15, "#version 330 core");
            for (const std::string_view precision : {"precision highp float;", "precision mediump float;"})
            {
                const auto offset = desktopSource.find(precision);
                if (offset != std::string::npos) desktopSource.erase(offset, precision.size());
            }
            source = desktopSource.c_str();
#endif
            // コンパイルするGL Shader
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            // Shaderコンパイルの成否
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE)
            {
                return shader;
            }
            std::array<char, 4096> diagnostics{};
            glGetShaderInfoLog(shader, diagnostics.size(), nullptr, diagnostics.data());
            std::fprintf(stderr, "LamaPon Web shader compile failed: %s\n", diagnostics.data());
            glDeleteShader(shader);
            return 0;
        }

        // 描画用Programをリンクし、失敗時はリソースを破棄して0を返す(webGL2: GLSL ES 3.0を使うか)。
        [[nodiscard]] GLuint CreateProgram(bool webGL2) noexcept
        {
            // リンクする頂点Shader
            const GLuint vertexShader = CompileShader(
                GL_VERTEX_SHADER,
                webGL2
                    ? VertexShaderSourceWebGL2
                    : VertexShaderSourceWebGL1);
            // リンクする材質Shader
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
            // リンクするGL Program
            const GLuint program = glCreateProgram();
            glAttachShader(program, vertexShader);
            glAttachShader(program, fragmentShader);
            glBindAttribLocation(program, 0, "aPosition");
            glBindAttribLocation(program, 1, "aNormal");
            glBindAttribLocation(program, 2, "aUv");
            glLinkProgram(program);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            // Programリンクの成否
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked == GL_TRUE)
            {
                return program;
            }
            std::array<char, 4096> diagnostics{};
            glGetProgramInfoLog(program, diagnostics.size(), nullptr, diagnostics.data());
            std::fprintf(stderr, "LamaPon Web shader link failed: %s\n", diagnostics.data());
            glDeleteProgram(program);
            return 0;
        }
    }

    struct Renderer3D::Impl final
    {
        struct Mesh final
        {
            // WebGLの頂点バッファ
            GLuint vertexBuffer{};
            // WebGLのIndexバッファ
            GLuint indexBuffer{};
            // 登録したIndexの個数
            GLsizei indexCount{};
            // CPU側に保持する頂点配列
            std::vector<Vertex3D> vertices;
            // CPU側に保持するIndex列
            std::vector<std::uint32_t> indices;
        };

        // 所有するWebGL Context
#if defined(LAMAPON_NATIVE_RUNTIME)
        // ネイティブGL contextはSDLアプリケーションが所有する。
        int context{};
#else
        EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context{};
#endif
        // 材質描画用のGL Program
        GLuint program{};
        // World行列のUniform位置
        GLint modelLocation{ -1 };
        // ビュー行列のUniform位置
        GLint viewLocation{ -1 };
        // 透視行列のUniform位置
        GLint projectionLocation{ -1 };
        // 表面RGBAのUniform位置
        GLint colorLocation{ -1 };
        // 光方向のUniform位置
        GLint lightDirectionLocation{ -1 };
        // 環境光色のUniform位置
        GLint ambientColorLocation{ -1 };
        // 環境光強度のUniform位置
        GLint ambientIntensityLocation{ -1 };
        // 平行光色のUniform位置
        GLint directionalColorLocation{ -1 };
        // 平行光強度のUniform位置
        GLint directionalIntensityLocation{ -1 };
        GLint localLightCountLocation{ -1 };
        GLint localPositionRangeLocation{ -1 };
        GLint localColorIntensityLocation{ -1 };
        GLint localDirectionOuterLocation{ -1 };
        GLint localInnerSpotLocation{ -1 };
        // 粗さのUniform位置
        GLint roughnessLocation{ -1 };
        // 金属度のUniform位置
        GLint metallicLocation{ -1 };
        // 非金属反射色のUniform位置
        GLint dielectricSpecularLocation{ -1 };
        // 表面色SamplerのUniform位置
        GLint textureLocation{ -1 };
        // 表面色使用比のUniform位置
        GLint useTextureLocation{ -1 };
        // Alphaしきい値のUniform位置
        GLint alphaCutoffLocation{ -1 };
        // 法線SamplerのUniform位置
        GLint normalTextureLocation{ -1 };
        // 法線使用状態のUniform位置
        GLint useNormalTextureLocation{ -1 };
        // 法線強度のUniform位置
        GLint normalStrengthLocation{ -1 };
        // 複合材質SamplerのUniform位置
        GLint metallicRoughnessTextureLocation{ -1 };
        // 複合材質使用比のUniform位置
        GLint useMetallicRoughnessTextureLocation{ -1 };
        // 粗さSamplerのUniform位置
        GLint roughnessTextureLocation{ -1 };
        // 粗さ使用比のUniform位置
        GLint useRoughnessTextureLocation{ -1 };
        // 金属度SamplerのUniform位置
        GLint metallicTextureLocation{ -1 };
        // 金属度使用比のUniform位置
        GLint useMetallicTextureLocation{ -1 };
        // 遮蔽SamplerのUniform位置
        GLint occlusionTextureLocation{ -1 };
        // 遮蔽使用比のUniform位置
        GLint useOcclusionTextureLocation{ -1 };
        // 遮蔽強度のUniform位置
        GLint occlusionStrengthLocation{ -1 };
        // 発光SamplerのUniform位置
        GLint emissiveTextureLocation{ -1 };
        // 発光使用比のUniform位置
        GLint useEmissiveTextureLocation{ -1 };
        // 発光RGBのUniform位置
        GLint emissiveColorLocation{ -1 };
        // 照明無効比のUniform位置
        GLint unlitLocation{ -1 };
        // 視点位置のUniform位置
        GLint cameraPositionLocation{ -1 };
        // フォグ色のUniform位置
        GLint fogColorLocation{ -1 };
        // フォグ距離のUniform位置
        GLint fogRangeLocation{ -1 };
        // フォグ有効比のUniform位置
        GLint fogEnabledLocation{ -1 };
        // 選んだWebGL版番号
        int webGLVersion{};
        std::string canvasSelector;
        // Canvas2D/SVGを使うか
        bool fallback2D{};
        // IDごとに所有するMesh資源
        std::unordered_map<MeshId, Mesh> meshes;
        // 次に発行するMesh ID
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
        // id: Mesh ID、mesh: 破棄する描画資源
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
#if !defined(LAMAPON_NATIVE_RUNTIME)
        if (!m_impl->canvasSelector.empty())
        {
            emscripten_set_webglcontextlost_callback(m_impl->canvasSelector.c_str(), nullptr, false, nullptr);
            emscripten_set_webglcontextrestored_callback(m_impl->canvasSelector.c_str(), nullptr, false, nullptr);
        }
        if (m_impl->context != 0)
        {
            EM_ASM({
                const context = GL.getContext($0);
                const gl = context ? context.GLctx : null;
                for (const [id, slot] of Object.entries(globalThis.__lamaponTextures || {})) {
                    if (!gl || slot.gl !== gl) continue;
                    ++slot.generation;
                    slot.recreate = null;
                    if (slot.image) slot.image.close();
                    if (slot.texture && !gl.isContextLost()) gl.deleteTexture(slot.texture);
                    delete globalThis.__lamaponTextures[id];
                }
            }, m_impl->context);
            emscripten_webgl_destroy_context(m_impl->context);
        }
#endif
    }

    void Renderer3D::ConfigureGraphicsState() noexcept
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
        m_impl->localLightCountLocation = glGetUniformLocation(m_impl->program, "uLocalLightCount");
        m_impl->localPositionRangeLocation = glGetUniformLocation(m_impl->program, "uLocalPositionRange[0]");
        m_impl->localColorIntensityLocation = glGetUniformLocation(m_impl->program, "uLocalColorIntensity[0]");
        m_impl->localDirectionOuterLocation = glGetUniformLocation(m_impl->program, "uLocalDirectionOuter[0]");
        m_impl->localInnerSpotLocation = glGetUniformLocation(m_impl->program, "uLocalInnerSpot[0]");
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
        // DirectXの手続き型メッシュと合わせ、時計回りを表面とする。
        glFrontFace(GL_CW);
    BrowserSetRendererBackend(m_impl->webGLVersion);
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
#if defined(LAMAPON_NATIVE_RUNTIME)
        m_impl->program = CreateProgram(true);
        if (m_impl->program == 0) return false;
        m_impl->context = 1;
        m_impl->webGLVersion = 2;
        {
#else
        emscripten_set_canvas_element_size(
            canvasSelector,
            static_cast<int>(std::max(width, 1u)),
            static_cast<int>(std::max(height, 1u)));
        // 試すWebGL版の優先順
        constexpr std::array<int, 2> versions = { 2, 1 };
        // 初期化を試すWebGL版
        for (const int version : versions)
        {
            // 作成するWebGL Context設定
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
                && !emscripten_webgl_enable_extension(
                    m_impl->context,
                    "OES_element_index_uint"))
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
#endif
            ConfigureGraphicsState();
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
#if !defined(LAMAPON_NATIVE_RUNTIME)
        if (!m_impl->fallback2D)
        {
            m_impl->canvasSelector = canvasSelector;
            if (emscripten_set_webglcontextlost_callback(canvasSelector, this, false,
                [](int, const void*, void* data) -> bool {
                    auto& renderer = *static_cast<Renderer3D*>(data);
                    renderer.m_initialized = false;
                    EM_ASM({
                        document.__lamaponContextLost = true;
                        document.body.dataset.lamaponGraphicsRecovery = 'pending';
                    });
                    return true; // preventDefault permits the browser to restore this context.
                }) != EMSCRIPTEN_RESULT_SUCCESS
                || emscripten_set_webglcontextrestored_callback(canvasSelector, this, false,
                [](int, const void*, void* data) -> bool {
                    auto& renderer = *static_cast<Renderer3D*>(data);
                    const bool restored = renderer.RestoreWebGraphics();
                    EM_ASM({
                        if (!$0) {
                            document.body.dataset.lamaponGraphicsRecovery = 'failed';
                            document.body.dataset.lamaponStatus = 'failed';
                            document.body.dataset.lamaponError = 'Graphics could not be restored.';
                            const help = document.getElementById('help');
                            if (help) {
                                help.textContent = document.body.dataset.lamaponError;
                                help.style.color = '#ffd0d0';
                            }
                            const reload = document.getElementById('reload');
                            if (reload) reload.hidden = false;
                        }
                    }, restored);
                    return true;
                }) != EMSCRIPTEN_RESULT_SUCCESS) return false;
        }
#endif
        m_initialized = true;
        Resize(width, height);
        return true;
    }

#if defined(LAMAPON_NATIVE_RUNTIME)
    bool Renderer3D::RestoreNativeGraphics() noexcept
    {
        // Never delete names from the previous context in the new context.
        m_initialized = false;
        m_impl->program = 0;
        m_impl->context = 0;
        for (auto& [id, mesh] : m_impl->meshes)
        {
            (void)id;
            mesh.vertexBuffer = mesh.indexBuffer = 0;
        }
        if (!Initialize("native", m_width, m_height)) return false;
        for (auto& [id, mesh] : m_impl->meshes)
        {
            glGenBuffers(1, &mesh.vertexBuffer);
            glGenBuffers(1, &mesh.indexBuffer);
            if (!mesh.vertexBuffer || !mesh.indexBuffer) return false;
            UpdateMesh(id, mesh.vertices, mesh.indices);
        }
        return glGetError() == GL_NO_ERROR;
    }
#endif

#if !defined(LAMAPON_NATIVE_RUNTIME)
    bool Renderer3D::RestoreWebGraphics() noexcept
    {
        if (!m_impl->context || emscripten_webgl_make_context_current(m_impl->context)
            != EMSCRIPTEN_RESULT_SUCCESS) return false;
        if (m_impl->webGLVersion == 1 && !emscripten_webgl_enable_extension(
                m_impl->context, "OES_element_index_uint")) return false;
        m_impl->program = CreateProgram(m_impl->webGLVersion >= 2);
        if (!m_impl->program) return false;
        ConfigureGraphicsState();
        m_initialized = true;
        for (auto& [id, mesh] : m_impl->meshes)
        {
            glGenBuffers(1, &mesh.vertexBuffer);
            glGenBuffers(1, &mesh.indexBuffer);
            if (!mesh.vertexBuffer || !mesh.indexBuffer) { m_initialized = false; return false; }
            UpdateMesh(id, mesh.vertices, mesh.indices);
        }
        EM_ASM({
            const gl = GL.currentContext.GLctx;
            globalThis.__lamaponWebGl = gl;
            const recovery = document.__lamaponRecoveryGeneration || 0;
            const slots = Object.values(globalThis.__lamaponTextures || {})
                .filter(slot => slot.gl === gl);
            const restorations = slots.map(slot => slot.recreate
                ? Promise.resolve(slot.recreate())
                : Promise.reject(new Error('Web texture has no restoration callback')));
            document.body.dataset.lamaponGraphicsRecovery = 'pending';
            const publishRecovery = restored => {
                if (recovery !== (document.__lamaponRecoveryGeneration || 0)
                    || gl.isContextLost()) return;
                document.body.dataset.lamaponGraphicsRecovery = restored ? 'restored' : 'failed';
                if (restored) {
                    document.__lamaponContextLost = false;
                    if (document.__lamaponVisibility) document.__lamaponVisibility.changed = true;
                    const help = document.getElementById('help');
                    if (help) {
                        help.textContent = 'Graphics restored.';
                        help.style.removeProperty('color');
                    }
                } else {
                    document.body.dataset.lamaponStatus = 'failed';
                    document.body.dataset.lamaponError = 'Graphics resources could not be restored.';
                    const help = document.getElementById('help');
                    if (help) {
                        help.textContent = document.body.dataset.lamaponError;
                        help.style.color = '#ffd0d0';
                    }
                    const reload = document.getElementById('reload');
                    if (reload) reload.hidden = false;
                }
            };
            Promise.all(restorations).then(() => publishRecovery(
                !gl.isContextLost() && slots.every(slot => slot.ready
                    && Boolean(slot.texture) && gl.isTexture(slot.texture))))
                .catch(error => {
                    console.warn('LamaPon Web graphics resource restoration failed', error);
                    publishRecovery(false);
                });
        });
        Resize(m_width, m_height);
        if (glGetError() != GL_NO_ERROR) { m_initialized = false; return false; }
        return true;
    }
#endif

    void Renderer3D::Resize(
        std::uint32_t width,
        std::uint32_t height) noexcept
    {
        m_width = std::max(width, 1u);
        m_height = std::max(height, 1u);
        if (m_impl->fallback2D)
        {
            // ソフトウェア描画の画素数を抑えるため、内部解像度の幅を制限する。
            // ソフトウェア描画幅の上限
            constexpr std::uint32_t softwareMaximumWidth = 960u;
            if (m_width > softwareMaximumWidth)
            {
                // 描画幅に合わせる縮小比
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
            // 空の階調を描く帯の個数
            constexpr int stripCount = 96;
            glEnable(GL_SCISSOR_TEST);
            // 現在描画する帯の番号
            for (int strip = 0; strip < stripCount; ++strip)
            {
                // 帯の下端画素
                const int bottom = static_cast<int>(m_height) * strip
                    / stripCount;
                // 帯の上端画素
                const int top = static_cast<int>(m_height) * (strip + 1)
                    / stripCount;
                // 下端からの帯中心位置比
                const float fromBottom =
                    (static_cast<float>(strip) + 0.5f)
                    / static_cast<float>(stripCount);
                // 上端からの帯中心位置比
                const float fromTop = 1.0f - fromBottom;
                // 空の階調の画面位置比
                constexpr std::array<float, 7> skyPositions = {
                    0.0f, 0.032f, 0.097f, 0.195f,
                    0.292f, 0.35f, 0.454f };
                // 各階調点の水平線色への混合比
                constexpr std::array<float, 7> skyAmounts = {
                    0.0f, 0.05f, 0.30f, 0.60f,
                    0.825f, 0.925f, 1.0f };
                // 現在の帯の空色混合比
                float skyAmount = 1.0f;
                // 階調を補間する区間番号
                for (std::size_t point = 0;
                     point + 1 < skyPositions.size(); ++point)
                {
                    if (fromTop <= skyPositions[point + 1])
                    {
                        // 階調区間内の補間比
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
        // WebGLはブラウザーの描画フレーム終了時に画面へ反映します。
    }

    bool Renderer3D::UsesCanvas2DFallback() const noexcept
    {
        return m_impl != nullptr && m_impl->fallback2D;
    }

    void Renderer3D::SetCamera(const Camera3D& camera) noexcept
    {
        // 現在の描画幅と高さの比
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

    void Renderer3D::SetLocalLights(const std::vector<LocalLight3D>& lights) noexcept
    {
        m_lighting.localLightCount = static_cast<std::uint32_t>(
            std::min(lights.size(), m_lighting.localLights.size()));
        for (std::uint32_t i = 0; i < m_lighting.localLightCount; ++i)
        {
            auto light = lights[i];
            light.range = std::max(0.1f, light.range);
            light.intensity = std::clamp(light.intensity, 0.0f, 64.0f);
            light.direction = Length(light.direction) > 0.0001f
                ? Normalize(light.direction) : Vec3{ 0.0f, 0.0f, -1.0f };
            light.outerConeAngle = std::clamp(light.outerConeAngle, 0.0174533f, 1.553343f);
            light.innerConeAngle = std::clamp(light.innerConeAngle, 0.0174533f, light.outerConeAngle);
            m_lighting.localLights[i] = light;
        }
    }

    void Renderer3D::SetLighting(const Lighting3D& lighting) noexcept
    {
        m_lighting = lighting;
        m_lighting.localLightCount = std::min<std::uint32_t>(
            m_lighting.localLightCount, m_lighting.localLights.size());
        for (std::uint32_t i = 0; i < m_lighting.localLightCount; ++i)
        {
            auto& light = m_lighting.localLights[i];
            light.range = std::max(0.1f, light.range);
            light.intensity = std::clamp(light.intensity, 0.0f, 64.0f);
            light.direction = Length(light.direction) > 0.0001f
                ? Normalize(light.direction) : Vec3{0,0,-1};
            light.outerConeAngle = std::clamp(light.outerConeAngle, 0.0174533f, 1.553343f);
            light.innerConeAngle = std::clamp(light.innerConeAngle, 0.0174533f, light.outerConeAngle);
        }
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
        // 新規Meshの資源とCPUデータ
        Impl::Mesh mesh{};
        mesh.vertices = vertices;
        mesh.indices = indices;
        mesh.indexCount = static_cast<GLsizei>(indices.size());
        // 新規Meshに発行するID
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
            virtualPath, m_impl->fallback2D ? 0 : m_impl->webGLVersion, nullptr, 0));
    }

    TextureId Renderer3D::CreateTextureEncoded(const std::vector<unsigned char>& bytes) noexcept
    {
        if (!m_initialized || bytes.empty()
            || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return 0;
        return static_cast<TextureId>(BrowserTextureCreate(nullptr,
            m_impl->fallback2D ? 0 : m_impl->webGLVersion, bytes.data(), static_cast<int>(bytes.size())));
    }

    // 既存Meshの全データを置き換える(meshId: 登録済みID, vertices: 頂点配列, indices: 三角形の頂点番号列)。
    void Renderer3D::UpdateMesh(
        MeshId meshId,
        const std::vector<Vertex3D>& vertices,
        const std::vector<std::uint32_t>& indices) noexcept
    {
        // 登録済みMeshの検索位置
        const auto found = m_impl->meshes.find(meshId);
        if (found == m_impl->meshes.end())
        {
            return;
        }
        found->second.vertices = vertices;
        found->second.indices = indices;
        found->second.indexCount = static_cast<GLsizei>(indices.size());
        if (m_impl->fallback2D || !m_initialized)
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

    // 登録済みMeshとGPUバッファを破棄する(meshId: 対象ID)。
    void Renderer3D::DestroyMesh(MeshId meshId) noexcept
    {
        // 登録済みMeshの検索位置
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

    // 登録済みMeshを描画する(meshId: 対象ID, model: 列優先のWorld変換, color: 表面のRGBA, roughness: 粗さ0～1, texture: 表面色テクスチャID, doubleSided: 両面描画するか, alphaBlended: 透過合成するか, alphaCutoff: 負値で無効のAlphaしきい値, normalTexture: 法線テクスチャID, normalStrength: 法線の強さ, metallic: 金属度0～1, metallicRoughnessTexture: G粗さ・B金属度のID, roughnessTexture: G粗さテクスチャID, metallicTexture: B金属度テクスチャID, occlusionTexture: R遮蔽テクスチャID, occlusionStrength: 遮蔽の強さ0～1, emissiveTexture: 発光テクスチャID, emissiveColor: 発光RGB, unlit: 照明を無効にするか, dielectricSpecular: 非金属の反射RGB, additiveBlend: 透過時に加算合成するか)。
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
        // 登録済みMeshの検索位置
        const auto found = m_impl->meshes.find(meshId);
        if (found == m_impl->meshes.end())
        {
            return;
        }
        if (m_impl->fallback2D)
        {
            struct Triangle final
            {
                // 0～5画面XY、6～8頂点1照明、9～14 UV、15平均深度、16平均距離、17～19 W逆数、20～22各深度、23～25各距離、26～31頂点2・3照明の順でJSへ渡す。
                // JSへ渡す三角形の32成分
                std::array<float, 32> values{};
                // 3頂点の平均NDC深度
                float depth{};
            };
            // ローカルからビューへの行列
            const Mat4 viewModel = Multiply(m_view, model);
            // ローカルからクリップへの行列
            const Mat4 viewProjectionModel = Multiply(m_projection, viewModel);
            // 表面から光源への単位方向
            const Vec3 lightToSource = Normalize(
                m_lighting.directionalDirection * -1.0f);
            struct SoftwareVertex final
            {
                // クリップ空間の同次位置
                ClipVertex clip{};
                // ビュー空間の同次位置
                ClipVertex view{};
                // World空間の法線
                Vec3 normal{};
                // テクスチャ座標
                Vec2 uv{};
                Vec3 world{};
            };
            // Canvas版は逆転置を使わないため、非一様な倍率ではWebGL版と法線が異なる。
            // modelの線形成分で法線を変換し正規化する(source: ローカル法線)。
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
            // 粗さによる簡易反射の補正
            const float environmentSpecular =
                (1.0f - std::clamp(roughness, 0.0f, 1.0f)) * 0.08f;
            // 環境光と平行光から頂点の照明倍率を求める(normal: World法線)。
            const auto lightForNormal = [this, &lightToSource,
                                         environmentSpecular, unlit](
                const Vec3& normal, const Vec3& position) noexcept
            {
                if (unlit)
                {
                    return std::array<float, 3>{ 1.0f, 1.0f, 1.0f };
                }
                // 法線と光源方向の内積
                const float diffuse = std::max(
                    Dot(normal, lightToSource), 0.0f);
                auto result = std::array<float, 3>{
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
                for (std::uint32_t i = 0; i < m_lighting.localLightCount; ++i)
                {
                    const auto& light = m_lighting.localLights[i];
                    const Vec3 offset = light.position - position;
                    const float distance = Length(offset);
                    const Vec3 toLight = offset * (1.0f / std::max(distance, 0.0001f));
                    float attenuation = std::max(1.0f - distance / light.range, 0.0f);
                    attenuation *= attenuation;
                    if (light.spot)
                        attenuation *= std::clamp((Dot(toLight * -1.0f, light.direction)
                            - std::cos(light.outerConeAngle)) / std::max(
                                std::cos(light.innerConeAngle) - std::cos(light.outerConeAngle), 0.0001f), 0.0f, 1.0f);
                    const float amount = attenuation * light.intensity * std::max(Dot(normal, toLight), 0.0f);
                    result[0] += light.color.r * amount;
                    result[1] += light.color.g * amount;
                    result[2] += light.color.b * amount;
                }
                return result;
            };
            // クリップ頂点を線形補間する(first: 始点, second: 終点, amount: 補間比0～1)。
            const auto interpolateVertex = [](const SoftwareVertex& first,
                                              const SoftwareVertex& second,
                                              float amount) noexcept
            {
                // 同次座標を線形補間する(left: 始点, right: 終点)。
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
                    first.world + (second.world - first.world) * amount,
                };
            };
            // 描画予約する投影済み三角形
            std::vector<Triangle> triangles;
            triangles.reserve(found->second.indices.size() / 2);
            // 投影・裏面判定を通った三角形を追加する(first: 第1頂点, second: 第2頂点, third: 第3頂点)。
            const auto appendTriangle = [this, doubleSided,
                                         &triangles, &lightForNormal](
                const SoftwareVertex& first,
                const SoftwareVertex& second,
                const SoftwareVertex& third) noexcept
            {
                // 同次位置を画面Xへ投影する(vertex: クリップ位置)。
                const auto projectX = [this](const ClipVertex& vertex) noexcept
                {
                    return (vertex.x / vertex.w * 0.5f + 0.5f)
                        * static_cast<float>(m_width);
                };
                // 同次位置を画面Yへ反転投影する(vertex: クリップ位置)。
                const auto projectY = [this](const ClipVertex& vertex) noexcept
                {
                    return (1.0f - (vertex.y / vertex.w * 0.5f + 0.5f))
                        * static_cast<float>(m_height);
                };
                // 第1頂点の画面X
                const float x1 = projectX(first.clip);
                // 第1頂点の画面Y
                const float y1 = projectY(first.clip);
                // 第2頂点の画面X
                const float x2 = projectX(second.clip);
                // 第2頂点の画面Y
                const float y2 = projectY(second.clip);
                // 第3頂点の画面X
                const float x3 = projectX(third.clip);
                // 第3頂点の画面Y
                const float y3 = projectY(third.clip);
                // 画面上の符号付き二倍面積
                const float area = (x2 - x1) * (y3 - y1)
                    - (y2 - y1) * (x3 - x1);
                if (std::abs(area) < 0.01f)
                {
                    return;
                }
                // 画面Yの反転により、時計回りの表面は画面上の符号付き面積が正となる。
                if (!doubleSided && area <= 0.0f)
                {
                    return;
                }
                // 画面外判定の余裕画素
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
                // 第1頂点のNDC深度
                const float firstDepth = first.clip.z / first.clip.w;
                // 第2頂点のNDC深度
                const float secondDepth = second.clip.z / second.clip.w;
                // 第3頂点のNDC深度
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
                // 第1頂点の照明RGB倍率
                const std::array<float, 3> firstLight =
                    lightForNormal(first.normal, first.world);
                // 第2頂点の照明RGB倍率
                const std::array<float, 3> secondLight =
                    lightForNormal(second.normal, second.world);
                // 第3頂点の照明RGB倍率
                const std::array<float, 3> thirdLight =
                    lightForNormal(third.normal, third.world);
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
            // 三角形の先頭Index位置
            for (std::size_t index = 0;
                 index + 2 < found->second.indices.size();
                 index += 3)
            {
                // 第1頂点の配列番号
                const std::uint32_t firstIndex = found->second.indices[index];
                // 第2頂点の配列番号
                const std::uint32_t secondIndex = found->second.indices[index + 1];
                // 第3頂点の配列番号
                const std::uint32_t thirdIndex = found->second.indices[index + 2];
                if (firstIndex >= found->second.vertices.size()
                    || secondIndex >= found->second.vertices.size()
                    || thirdIndex >= found->second.vertices.size())
                {
                    continue;
                }
                // 第1頂点の元データ
                const Vertex3D& firstSource =
                    found->second.vertices[firstIndex];
                // 第2頂点の元データ
                const Vertex3D& secondSource =
                    found->second.vertices[secondIndex];
                // 第3頂点の元データ
                const Vertex3D& thirdSource =
                    found->second.vertices[thirdIndex];
                // クリップ前の変換済み頂点
                std::array<SoftwareVertex, 5> input = {{
                    {
                        TransformPoint(
                            viewProjectionModel, firstSource.position),
                        TransformPoint(viewModel, firstSource.position),
                        transformNormal(firstSource.normal),
                        firstSource.uv,
                        { TransformPoint(model, firstSource.position).x,
                          TransformPoint(model, firstSource.position).y,
                          TransformPoint(model, firstSource.position).z },
                    },
                    {
                        TransformPoint(
                            viewProjectionModel, secondSource.position),
                        TransformPoint(viewModel, secondSource.position),
                        transformNormal(secondSource.normal),
                        secondSource.uv,
                        { TransformPoint(model, secondSource.position).x,
                          TransformPoint(model, secondSource.position).y,
                          TransformPoint(model, secondSource.position).z },
                    },
                    {
                        TransformPoint(
                            viewProjectionModel, thirdSource.position),
                        TransformPoint(viewModel, thirdSource.position),
                        transformNormal(thirdSource.normal),
                        thirdSource.uv,
                        { TransformPoint(model, thirdSource.position).x,
                          TransformPoint(model, thirdSource.position).y,
                          TransformPoint(model, thirdSource.position).z },
                    },
                }};
                // 近端でクリップした頂点
                std::array<SoftwareVertex, 5> clipped{};
                // 元の三角形の頂点数
                std::size_t inputCount = 3;
                // クリップ後の頂点数
                std::size_t clippedCount = 0;
                // クリップする同次Wの下限
                constexpr float nearW = 0.1001f;
                // 前に判定したクリップ頂点
                SoftwareVertex previous = input[inputCount - 1];
                // 前頂点が近端より先にあるか
                bool previousInside = previous.clip.w >= nearW;
                // 判定中の頂点番号
                for (std::size_t vertexIndex = 0;
                     // 元の三角形の頂点数
                     vertexIndex < inputCount;
                     ++vertexIndex)
                {
                    // 現在判定するクリップ頂点
                    const SoftwareVertex current = input[vertexIndex];
                    // 現在の頂点が近端より先か
                    const bool currentInside = current.clip.w >= nearW;
                    if (currentInside != previousInside)
                    {
                        // 辺の同次W成分の差
                        const float denominator =
                            current.clip.w - previous.clip.w;
                        // 近端と交差する補間比
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
                // 扇状分割する頂点番号
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
            // 平均NDC深度で奥から順に並べる(left: 左三角形, right: 右三角形)。
            std::sort(
                triangles.begin(),
                triangles.end(),
                [](const Triangle& left, const Triangle& right) noexcept
                {
                    return left.depth > right.depth;
                });
            // JSへ渡す連続した三角形データ
            std::vector<float> payload;
            payload.reserve(triangles.size() * 32);
            // JS転送データへ追加する三角形
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
        glUniform1i(m_impl->localLightCountLocation, m_lighting.localLightCount);
        std::array<float, 32> positions{}, colors{}, directions{};
        std::array<float, 16> cones{};
        for (std::uint32_t i = 0; i < m_lighting.localLightCount; ++i)
        {
            const auto& light = m_lighting.localLights[i];
            positions[i*4] = light.position.x;
            positions[i*4+1] = light.position.y;
            positions[i*4+2] = light.position.z;
            positions[i*4+3] = light.range;
            colors[i*4] = light.color.r;
            colors[i*4+1] = light.color.g;
            colors[i*4+2] = light.color.b;
            colors[i*4+3] = light.intensity;
            directions[i*4] = light.direction.x;
            directions[i*4+1] = light.direction.y;
            directions[i*4+2] = light.direction.z;
            directions[i*4+3] = std::cos(light.outerConeAngle);
            cones[i*2] = std::cos(light.innerConeAngle);
            cones[i*2+1] = light.spot ? 1.0f : 0.0f;
        }
        if (m_lighting.localLightCount > 0)
        {
            glUniform4fv(m_impl->localPositionRangeLocation, m_lighting.localLightCount, positions.data());
            glUniform4fv(m_impl->localColorIntensityLocation, m_lighting.localLightCount, colors.data());
            glUniform4fv(m_impl->localDirectionOuterLocation, m_lighting.localLightCount, directions.data());
            glUniform2fv(m_impl->localInnerSpotLocation, m_lighting.localLightCount, cones.data());
        }
        glUniform1f(m_impl->roughnessLocation, std::clamp(roughness, 0.0f, 1.0f));
        glUniform1f(m_impl->metallicLocation, std::clamp(metallic, 0.0f, 1.0f));
        glUniform3f(
            m_impl->dielectricSpecularLocation,
            std::clamp(dielectricSpecular.r, 0.0f, 1.0f),
            std::clamp(dielectricSpecular.g, 0.0f, 1.0f),
            std::clamp(dielectricSpecular.b, 0.0f, 1.0f));
        glActiveTexture(GL_TEXTURE0);
        // 表面色テクスチャの準備結果
        const bool textureReady = texture != 0
            && BrowserTextureBind(static_cast<int>(texture)) != 0;
        glUniform1i(m_impl->textureLocation, 0);
        glUniform1f(m_impl->useTextureLocation, textureReady ? 1.0f : 0.0f);
        glUniform1f(m_impl->alphaCutoffLocation, alphaCutoff);
        glActiveTexture(GL_TEXTURE1);
        // 法線テクスチャの準備結果
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
        // 複合材質テクスチャの準備結果
        const bool metallicRoughnessTextureReady =
            metallicRoughnessTexture != 0
            && BrowserTextureBind(
                static_cast<int>(metallicRoughnessTexture)) != 0;
        glUniform1i(m_impl->metallicRoughnessTextureLocation, 2);
        glUniform1f(
            m_impl->useMetallicRoughnessTextureLocation,
            metallicRoughnessTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE3);
        // 粗さテクスチャの準備結果
        const bool roughnessTextureReady = roughnessTexture != 0
            && BrowserTextureBind(static_cast<int>(roughnessTexture)) != 0;
        glUniform1i(m_impl->roughnessTextureLocation, 3);
        glUniform1f(
            m_impl->useRoughnessTextureLocation,
            roughnessTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE4);
        // 金属度テクスチャの準備結果
        const bool metallicTextureReady = metallicTexture != 0
            && BrowserTextureBind(static_cast<int>(metallicTexture)) != 0;
        glUniform1i(m_impl->metallicTextureLocation, 4);
        glUniform1f(
            m_impl->useMetallicTextureLocation,
            metallicTextureReady ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE5);
        // 遮蔽テクスチャの準備結果
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
        // 発光テクスチャの準備結果
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
        // 透過として合成するか
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
