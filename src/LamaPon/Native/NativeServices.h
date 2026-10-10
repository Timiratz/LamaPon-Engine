#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon::Native
{
    // ゲーム起動時に保存先とアセットの基点を固定する。読み取り用assetと保存先を分離する。
    void ConfigureServices(SDL_Window* window, std::filesystem::path assetRoot,
        std::filesystem::path dataRoot);
    SDL_Window* GameWindow() noexcept;
    std::vector<unsigned char> ReadAsset(const char* virtualPath);
    void ShutdownUi() noexcept;
    void ForgetUiGraphics() noexcept;
    void ShutdownAudio() noexcept;
    void UpdateAudio(float deltaTime) noexcept;
    void SuspendAudio(bool suspend) noexcept;
    // Stable runtime handles; never expose the current context's GLuint names.
    std::uint32_t UploadTexture(const unsigned char* rgba, int width, int height);
    std::uint32_t AssetTexture(const char* virtualPath);
    void DestroyTexture(std::uint32_t texture) noexcept;
    void DestroyTextures() noexcept;
    bool RestoreTextures() noexcept;
    bool BindTexture(std::uint32_t texture) noexcept;
}
