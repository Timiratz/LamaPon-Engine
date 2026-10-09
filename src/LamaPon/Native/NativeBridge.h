#pragma once
#include <cstdint>
#include <cstddef>
namespace LamaPon::Native
{
    bool SavePortableText(const char* key, const char* value);
    char* LoadPortableText(const char* key);
    char* LoadPortableAssetText(const char* path);
    unsigned char* LoadPortableAssetBytes(const char* path, std::uint32_t* byteCount);
    void PublishPortableModelStatus(const char* path, const char* status, int parts);
    void PublishPortableModelAnimation(const char* name, int index, int count, float time, int playing);
    void PublishPortableInputActionCount(int count);
    void RenderPortableText(const char* objectName, double objectId, const char* text, const char* font, const char* fontAsset, float size, float r, float g, float b, float a, float x, float y, float width, float height, int wordWrap, int horizontal, int vertical, int sortOrder);
    void RenderPortableMask(double objectId, float x, float y, float width, float height, int shape);
    void BeginPortableUiFrame();
    void EndPortableUiFrame();
    void HidePortableObjectUi(double objectId);
    void RenderPortableSprite(const char* objectName, double objectId, const char* texturePath, float r, float g, float b, float a, float x, float y, float width, float height, float pivotX, float pivotY, float rotation, int sortOrder, float sourceX, float sourceY, float sourceWidth, float sourceHeight, int maskInteraction);
    void RenderPortableSpriteMesh(const char* objectName, double objectId, const char* texturePath,
        float r, float g, float b, float a, int sortOrder,
        const float* positions, const float* uvs, int vertexCount,
        const std::uint16_t* indices, int indexCount);
    void PublishPortableNumber(const char* key, double value);
    void PublishPortableString(const char* key, const char* value);
}
