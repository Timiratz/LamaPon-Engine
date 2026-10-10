#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Native/NativeRenderBridge.h"
#include "LamaPon/Native/NativeGL.h"
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <limits>

namespace LamaPon::Native
{
    namespace
    {
        std::unordered_map<std::string, std::uint32_t> imageCache;
        struct Texture
        {
            GLuint name{};
            int width{}, height{};
            std::vector<unsigned char> pixels;
        };
        std::unordered_map<std::uint32_t, Texture> textures;
        std::uint32_t nextTexture{1};

        bool Upload(Texture& texture)
        {
            glGenTextures(1, &texture.name);
            if (!texture.name) return false;
            glBindTexture(GL_TEXTURE_2D, texture.name);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.width, texture.height, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, texture.pixels.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            return glGetError() == GL_NO_ERROR;
        }

        std::uint32_t DecodeImage(const unsigned char* bytes, const std::size_t size)
        {
            if (!bytes || !size) return 0;
            SDL_Surface* source = SDL_LoadSurface_IO(SDL_IOFromConstMem(bytes, size), true);
            if (!source) throw std::runtime_error(SDL_GetError());
            SDL_Surface* rgba = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
            SDL_DestroySurface(source);
            if (!rgba) throw std::runtime_error(SDL_GetError());
            struct Release { SDL_Surface* value; ~Release() { SDL_DestroySurface(value); } } release{rgba};
            const auto rowSize = static_cast<std::size_t>(rgba->w) * 4;
            std::vector<unsigned char> pixels(rowSize * static_cast<std::size_t>(rgba->h));
            for (int row = 0; row < rgba->h; ++row)
                std::memcpy(pixels.data() + static_cast<std::size_t>(row) * rowSize,
                    static_cast<const unsigned char*>(rgba->pixels) + row * rgba->pitch, rowSize);
            const auto texture = UploadTexture(pixels.data(), rgba->w, rgba->h);
            if (!texture) throw std::runtime_error("Cannot upload image texture");
            return texture;
        }
    }

    std::uint32_t UploadTexture(const unsigned char* rgba, const int width, const int height)
    {
        if (!rgba || width <= 0 || height <= 0) return 0;
        if (!nextTexture || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(width) / 4) return 0;
        const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        Texture data;
        data.width = width; data.height = height;
        data.pixels.assign(rgba, rgba + size);
        const auto id = nextTexture++;
        auto& texture = textures.emplace(id, std::move(data)).first->second;
        if (Upload(texture)) return id;
        if (texture.name) glDeleteTextures(1, &texture.name);
        textures.erase(id);
        return 0;
    }

    std::uint32_t AssetTexture(const char* path)
    {
        if (!path || !*path) return 0;
        if (const auto found = imageCache.find(path); found != imageCache.end()) return found->second;
        try
        {
            const auto bytes = ReadAsset(path);
            const auto texture = DecodeImage(bytes.data(), bytes.size());
            if (!texture) throw std::runtime_error("Empty image asset");
            imageCache.emplace(path, texture);
            return texture;
        }
        catch (const std::exception& error)
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Texture %s: %s", path, error.what());
            return 0;
        }
    }

    void DestroyTexture(const std::uint32_t texture) noexcept
    {
        const auto found = textures.find(texture);
        if (found == textures.end()) return;
        if (found->second.name) glDeleteTextures(1, &found->second.name);
        textures.erase(found);
        std::erase_if(imageCache, [texture](const auto& item) { return item.second == texture; });
    }
    void DestroyTextures() noexcept
    {
        for (const auto& [id, texture] : textures)
        { (void)id; if (texture.name) glDeleteTextures(1, &texture.name); }
        textures.clear();
        imageCache.clear();
    }
    bool RestoreTextures() noexcept
    {
        for (auto& [id, texture] : textures) { (void)id; texture.name = 0; }
        for (auto& [id, texture] : textures) { (void)id; if (!Upload(texture)) return false; }
        return true;
    }
    bool BindTexture(const std::uint32_t texture) noexcept
    {
        const auto found = textures.find(texture);
        const auto name = found == textures.end() ? 0 : found->second.name;
        glBindTexture(GL_TEXTURE_2D, name);
        return name != 0;
    }
    int BrowserTextureCreate(const char* path, int, const unsigned char* encoded, const int byteCount)
    {
        if (!encoded) return static_cast<int>(AssetTexture(path));
        if (byteCount <= 0) return 0;
        try { return static_cast<int>(DecodeImage(encoded, static_cast<std::size_t>(byteCount))); }
        catch (const std::exception& error)
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Encoded texture: %s", error.what());
            return 0;
        }
    }
    int BrowserTextureBind(const int texture)
    {
        return BindTexture(static_cast<std::uint32_t>(texture)) ? 1 : 0;
    }
    void BrowserSetRendererBackend(int)
    { SDL_Log("Renderer: %s", reinterpret_cast<const char*>(glGetString(GL_VERSION))); }

    // ブラウザー用ソフトウェアfallbackはネイティブの出力に含めない。初期化失敗を呼出元へ返す。
    int Canvas2DInitialize(const char*, int, int) { return 0; }
    void Canvas2DResize(int, int) {}
    void Canvas2DBeginFrame(float, float, float, float) {}
    void Canvas2DSetFog(int, float, float, float, float, float) {}
    void Canvas2DSetSky(int, float, float, float, float, float, float) {}
    void Canvas2DQueueTriangles(const float*, int, float, float, float, float, int, int, float, int) {}
    void Canvas2DEndFrame() {}
}
