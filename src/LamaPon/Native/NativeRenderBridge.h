#pragma once
#include <cstdint>
#include <cstddef>
namespace LamaPon::Native
{
    void BrowserSetRendererBackend(int version);
    int Canvas2DInitialize(const char* selector, int width, int height);
    void Canvas2DResize(int width, int height);
    void Canvas2DBeginFrame(float red, float green, float blue, float alpha);
    void Canvas2DSetFog(int enabled, float red, float green, float blue, float startDistance, float endDistance);
    void Canvas2DSetSky(int enabled, float topRed, float topGreen, float topBlue, float horizonRed, float horizonGreen, float horizonBlue);
    void Canvas2DQueueTriangles(const float* vertices, int floatCount, float red, float green, float blue, float alpha, int textureId, int alphaBlended, float alphaCutoff, int additiveBlend);
    void Canvas2DEndFrame();
    int BrowserTextureCreate(const char* virtualPath, int rendererVersion,
        const unsigned char* encoded = nullptr, int byteCount = 0);
    int BrowserTextureBind(int textureId);
}
