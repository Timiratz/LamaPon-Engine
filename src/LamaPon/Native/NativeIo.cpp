#include "LamaPon/Native/NativeBridge.h"
#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Core/PathUtils.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace LamaPon::Native
{
    namespace
    {
        SDL_Window* window{};
        std::filesystem::path assets;
        std::filesystem::path saves;
        nlohmann::json savedValues = nlohmann::json::object();
        bool savesReady{};

        std::string AssetFile(const char* value)
        {
            if (!value) throw std::invalid_argument("Null asset path");
            std::string_view text(value);
            if (text.starts_with("/assets/")) text.remove_prefix(8);
            else if (text.starts_with("assets/")) text.remove_prefix(7);
            const auto relative = PathFromUtf8(text).lexically_normal();
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                throw std::invalid_argument("Asset path must be relative to the game assets");
            for (const auto& part : relative)
                if (part == "..") throw std::invalid_argument("Asset path escapes game assets");
            return PathToUtf8(assets / relative);
        }

        char* CopyText(const std::string_view text)
        {
            auto* result = static_cast<char*>(std::malloc(text.size() + 1));
            if (!result) return nullptr;
            if (!text.empty()) std::memcpy(result, text.data(), text.size());
            result[text.size()] = '\0';
            return result;
        }
    }

    void ConfigureServices(SDL_Window* gameWindow, std::filesystem::path assetRoot,
        std::filesystem::path dataRoot)
    {
        savesReady = false;
        if (dataRoot.empty() || !dataRoot.is_absolute())
            throw std::invalid_argument("Game save directory must be absolute");
        auto next = nlohmann::json::object();
        std::ifstream file(dataRoot / "values.json", std::ios::binary);
        if (!file && std::filesystem::exists(dataRoot / "values.json"))
            throw std::runtime_error("Existing game save document cannot be read");
        if (file)
        {
            auto document = nlohmann::json::parse(file, nullptr, false);
            if (!document.is_object()) throw std::runtime_error("Game save document is invalid");
            for (const auto& value : document)
                if (!value.is_string()) throw std::runtime_error("Game save value is invalid");
            next = std::move(document);
        }
        window = gameWindow;
        assets = std::move(assetRoot);
        saves = std::move(dataRoot);
        savedValues = std::move(next);
        savesReady = true;
    }

    SDL_Window* GameWindow() noexcept { return window; }

    std::vector<unsigned char> ReadAsset(const char* path)
    {
        const auto file = AssetFile(path);
        std::size_t size{};
        void* memory = SDL_LoadFile(file.c_str(), &size);
        if (!memory) throw std::runtime_error(std::string("Cannot read game asset: ") + file + ": " + SDL_GetError());
        struct Release { void* value; ~Release() { SDL_free(value); } } release{memory};
        const auto* first = static_cast<const unsigned char*>(memory);
        return {first, first + size};
    }

    char* LoadPortableAssetText(const char* path)
    {
        try
        {
            const auto bytes = ReadAsset(path);
            return CopyText({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        }
        catch (const std::exception& error) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.what()); return nullptr; }
    }

    unsigned char* LoadPortableAssetBytes(const char* path, std::uint32_t* byteCount)
    {
        if (!byteCount) return nullptr;
        *byteCount = 0;
        try
        {
            const auto bytes = ReadAsset(path);
            if (bytes.empty() || bytes.size() > std::numeric_limits<std::uint32_t>::max()) return nullptr;
            auto* result = static_cast<unsigned char*>(std::malloc(bytes.size()));
            if (!result) return nullptr;
            std::memcpy(result, bytes.data(), bytes.size());
            *byteCount = static_cast<std::uint32_t>(bytes.size());
            return result;
        }
        catch (const std::exception& error) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", error.what()); return nullptr; }
    }

    bool SavePortableText(const char* key, const char* value)
    {
        try
        {
            if (!key || !value || !savesReady) throw std::invalid_argument("Invalid or uninitialized save request");
            auto next = savedValues;
            next[key] = value;
            std::filesystem::create_directories(saves);
            const auto pending = saves / "values.pending";
            const auto target = saves / "values.json";
            {
                std::ofstream output(pending, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("Cannot open pending save document");
                const auto encoded = next.dump();
                output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                output.flush();
                if (!output) throw std::runtime_error("Cannot write save document");
                output.close();
                if (!output) throw std::runtime_error("Cannot close save document");
            }
#if defined(_WIN32)
            if (!MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot publish save document");
#else
            std::filesystem::rename(pending, target);
#endif
            savedValues = std::move(next);
            return true;
        }
        catch (const std::exception& error) { SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Save failed: %s", error.what()); return false; }
    }

    char* LoadPortableText(const char* key)
    {
        if (!key || !savesReady) return nullptr;
        const auto found = savedValues.find(key);
        return found == savedValues.end() ? nullptr : CopyText(found->get_ref<const std::string&>());
    }

    void PublishPortableModelStatus(const char* path, const char* status, int parts)
    { SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Model %s: %s (%d parts)", path, status, parts); }
    void PublishPortableModelAnimation(const char*, int, int, float, int) {}
    void PublishPortableInputActionCount(int count)
    { SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Input actions: %d", count); }
    void PublishPortableNumber(const char*, double) {}
    void PublishPortableString(const char*, const char*) {}
}
