#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "LamaPon/LamaPon.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Native/NativeGL.h"
#include "LamaPon/Native/NativeInput.h"
#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Web/WebRenderer3D.h"
#include "LamaPon/Web/WebAudioRuntime.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifndef LAMAPON_PORTABLE_GAME_NAME
#define LAMAPON_PORTABLE_GAME_NAME "LamaPon Game"
#endif
#ifndef LAMAPON_PORTABLE_SCENE_PATH
#define LAMAPON_PORTABLE_SCENE_PATH "/assets/scenes/Main.scene.json"
#endif

#if defined(LAMAPON_NATIVE_FRAME_PROBE)
// Integration-test hook runs before presentation, while the back buffer is defined.
void LamaPonNativeFrameProbe(int width, int height);
#endif

namespace
{
    struct Game final
    {
        SDL_Window* window{};
        SDL_GLContext context{};
        GLuint vao{};
        bool glLoaded{}, backgrounded{}, minimized{}, probe{};
        bool Suspended() const noexcept { return backgrounded || minimized; }
        int frames{}, frameLimit{};
        Uint64 lastTime{};
        float accumulator{};
        LamaPon::Native::NativeInput input;
        LamaPon::Web::WebAudioRuntime audio;
        std::unique_ptr<LamaPon::Web::Renderer3D> renderer;
        std::unique_ptr<LamaPon::Scene> scene;
        std::mutex eventMutex;
        std::vector<SDL_Event> pendingEvents;

        ~Game()
        {
            // SDL、GL context、Scriptが借用するサービスの順序を維持する。
            scene.reset(); renderer.reset();
            LamaPon::Native::ShutdownAudio();
            if (glLoaded)
            {
                LamaPon::Native::ShutdownUi(); LamaPon::Native::DestroyTextures();
                if (vao) glDeleteVertexArrays(1, &vao);
            }
            if (context) SDL_GL_DestroyContext(context);
            if (window) SDL_DestroyWindow(window);
        }
    };

    void Fail(const std::string& message) { throw std::runtime_error(message + ": " + SDL_GetError()); }
}

SDL_AppResult SDL_AppInit(void** appstate, const int argc, char** argv)
{
    auto game = std::make_unique<Game>();
    *appstate = game.get();
    try
    {
        std::filesystem::path assetRoot, dataRoot, cacheRoot;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == "--probe") { game->probe = true; game->frameLimit = 8; }
            else if (argument == "--frames" && index + 1 < argc) game->frameLimit = std::stoi(argv[++index]);
            else if (argument == "--asset-root" && index + 1 < argc) assetRoot = LamaPon::PathFromUtf8(argv[++index]);
            else if (argument == "--data-dir" && index + 1 < argc) dataRoot = LamaPon::PathFromUtf8(argv[++index]);
            else if (argument == "--cache-dir" && index + 1 < argc) cacheRoot = LamaPon::PathFromUtf8(argv[++index]);
            else throw std::invalid_argument("Unknown or incomplete game argument: " + std::string(argument));
        }
        if (game->probe && dataRoot.empty())
            throw std::invalid_argument("--probe requires --data-dir to keep verification saves in the chosen directory");
#if defined(__ANDROID__)
        LamaPon::SetAndroidApplicationCacheDirectory(std::move(cacheRoot));
#else
        if (!cacheRoot.empty()) throw std::invalid_argument("--cache-dir is only available on Android");
#endif
        if (game->frameLimit < 0) throw std::invalid_argument("--frames must be nonnegative");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) Fail("SDL initialization failed");
#if defined(__ANDROID__)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        if (assetRoot.empty()) assetRoot = "assets";
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        if (assetRoot.empty())
        {
            const char* base = SDL_GetBasePath();
            if (!base) Fail("Cannot locate game files");
            assetRoot = LamaPon::PathFromUtf8(base) / "assets";
        }
#endif
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        game->window = SDL_CreateWindow(LAMAPON_PORTABLE_GAME_NAME, 1280, 720,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (game->probe ? SDL_WINDOW_HIDDEN : 0));
        if (!game->window) Fail("Cannot create game window");
        game->context = SDL_GL_CreateContext(game->window);
        if (!game->context || !SDL_GL_MakeCurrent(game->window, game->context)) Fail("Cannot create OpenGL context");
        if (!LamaPon::Native::GL::Load()) Fail("Cannot load OpenGL procedures");
        game->glLoaded = true;
        glGenVertexArrays(1, &game->vao); glBindVertexArray(game->vao);
        SDL_GL_SetSwapInterval(game->probe ? 0 : 1);
        if (dataRoot.empty())
        {
            char* path = SDL_GetPrefPath("LamaPon", LAMAPON_PORTABLE_GAME_NAME);
            if (!path) Fail("Cannot determine game save directory");
            dataRoot = LamaPon::PathFromUtf8(path); SDL_free(path);
        }
        LamaPon::Native::ConfigureServices(game->window, assetRoot, dataRoot);
        if (!game->input.Initialize(game->window)) Fail("Cannot initialize input");
        game->audio.Initialize();
        int width{}, height{}; SDL_GetWindowSizeInPixels(game->window, &width, &height);
        game->renderer = std::make_unique<LamaPon::Web::Renderer3D>();
        if (!game->renderer->Initialize("native", static_cast<std::uint32_t>(std::max(width, 1)),
            static_cast<std::uint32_t>(std::max(height, 1)))) Fail("Cannot initialize renderer");
        game->scene = std::make_unique<LamaPon::Scene>(*game->renderer, game->audio, game->input);
        if (!game->scene->Load(LamaPon::PathFromUtf8(LAMAPON_PORTABLE_SCENE_PATH)))
            throw std::runtime_error("Cannot load game scene");
        game->scene->Graphics().SetUiSize(static_cast<std::uint32_t>(std::max(width, 1)),
            static_cast<std::uint32_t>(std::max(height, 1)));
        game->scene->StartScripts();
        game->lastTime = SDL_GetTicksNS();
        SDL_Log("LamaPon native runtime started");
        game.release();
        return SDL_APP_CONTINUE;
    }
    catch (const std::exception& error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game initialization failed: %s", error.what());
        game.release(); // SDL_AppQuitが初期化途中の状態も破棄する。
        return SDL_APP_FAILURE;
    }
}

static SDL_AppResult ProcessGameEvent(void* appstate, SDL_Event* event)
{
    auto* game = static_cast<Game*>(appstate);
    if (!game || !event) return SDL_APP_FAILURE;
    if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return SDL_APP_SUCCESS;
    try
    {
        game->input.ProcessEvent(*event);
        const bool wasSuspended = game->Suspended();
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND)
            game->backgrounded = true;
        if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) game->backgrounded = false;
        if (event->type == SDL_EVENT_WINDOW_MINIMIZED) game->minimized = true;
        if (event->type == SDL_EVENT_WINDOW_RESTORED) game->minimized = false;
        if (!wasSuspended && game->Suspended())
        {
            game->input.Reset(); LamaPon::Native::SuspendAudio(true);
        }
        if (wasSuspended && !game->Suspended())
        {
            if (!SDL_GL_MakeCurrent(game->window, game->context)) Fail("Cannot restore graphics context");
            game->accumulator = 0; game->lastTime = SDL_GetTicksNS();
            LamaPon::Native::SuspendAudio(false);
        }
        return SDL_APP_CONTINUE;
    }
    catch (const std::exception& error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game event failed: %s", error.what());
        return SDL_APP_FAILURE;
    }
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto* game = static_cast<Game*>(appstate);
    if (!game || !event) return SDL_APP_FAILURE;
    // SDL may dispatch lifecycle events from the thread that pushed them.
    // Only the main iteration owns input, Scene and GL context operations.
    try
    {
        std::scoped_lock lock(game->eventMutex);
        game->pendingEvents.push_back(*event);
        return SDL_APP_CONTINUE;
    }
    catch (const std::exception& error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot queue game event: %s", error.what());
        return SDL_APP_FAILURE;
    }
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* game = static_cast<Game*>(appstate);
    if (!game || !game->scene) return SDL_APP_FAILURE;
    try
    {
        std::vector<SDL_Event> events;
        {
            std::scoped_lock lock(game->eventMutex);
            events.swap(game->pendingEvents);
        }
        if (std::any_of(events.begin(), events.end(), [](const SDL_Event& event) {
            return event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        })) return SDL_APP_SUCCESS;
        // SDL Android restores/replaces the EGL context before posting RESET.
        // Recover before foreground events try to make the old handle current.
        if (std::any_of(events.begin(), events.end(), [game](const SDL_Event& event) {
            return event.type == SDL_EVENT_RENDER_DEVICE_RESET
                && (!event.render.windowID || event.render.windowID == SDL_GetWindowID(game->window));
        }))
        {
            const auto restored = SDL_GL_GetCurrentContext();
            if (!restored) Fail("SDL did not restore a graphics context");
            game->context = restored;
            if (!LamaPon::Native::GL::Load()) Fail("Cannot reload graphics procedures");
            game->vao = 0;
            LamaPon::Native::ForgetUiGraphics();
            glGenVertexArrays(1, &game->vao); glBindVertexArray(game->vao);
            if (!game->vao || !game->renderer->RestoreNativeGraphics()
                || !LamaPon::Native::RestoreTextures())
                throw std::runtime_error("Cannot restore native graphics resources");
            game->accumulator = 0; game->lastTime = SDL_GetTicksNS();
            SDL_Log("LamaPon native graphics resources restored");
        }
        for (auto& event : events)
        {
            const auto result = ProcessGameEvent(game, &event);
            if (result != SDL_APP_CONTINUE) return result;
        }
        if (game->Suspended()) { SDL_Delay(10); return SDL_APP_CONTINUE; }
        const auto now = SDL_GetTicksNS();
        const float delta = std::clamp(static_cast<float>(static_cast<double>(now - game->lastTime) * 1e-9), 0.0f, 0.25f);
        game->lastTime = now;
        int width{}, height{}; SDL_GetWindowSizeInPixels(game->window, &width, &height);
        if (width > 0 && height > 0)
            game->scene->Graphics().SetUiSize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        game->input.BeginFrame();
        constexpr float step = 1.0f / 60.0f;
        game->accumulator += delta;
        while (game->accumulator >= step)
        { game->scene->FixedUpdate(step); game->accumulator -= step; }
        game->scene->SetPhysicsInterpolationAlpha(game->accumulator / step);
        float remaining = delta; bool first = true;
        do
        {
            const float update = std::min(remaining, step);
            game->scene->Graphics().Input().SetEdgeEventsEnabled(first);
            game->scene->Update(update); remaining -= update; first = false;
        } while (remaining > 0);
        game->scene->Graphics().Input().SetEdgeEventsEnabled(true);
        if (width > 0 && height > 0)
        {
            game->renderer->Resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
            game->scene->Render();
#if defined(LAMAPON_NATIVE_FRAME_PROBE)
            LamaPonNativeFrameProbe(width, height);
#endif
            if (game->probe && glGetError() != GL_NO_ERROR) throw std::runtime_error("Native rendering produced an OpenGL error");
            if (!SDL_GL_SwapWindow(game->window)) Fail("Cannot present frame");
            ++game->frames;
        }
        game->input.EndFrame();
        if (game->frameLimit > 0 && game->frames >= game->frameLimit)
        { SDL_Log("LamaPon native frame limit reached"); return SDL_APP_SUCCESS; }
        return SDL_APP_CONTINUE;
    }
    catch (const std::exception& error)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Game update failed: %s", error.what());
        return SDL_APP_FAILURE;
    }
}

void SDL_AppQuit(void* appstate, SDL_AppResult)
{
    delete static_cast<Game*>(appstate);
    SDL_Quit();
}
