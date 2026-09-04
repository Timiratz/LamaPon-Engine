#include "LamaPon/LamaPon.h"
#include "LamaPon/Web/WebInput.h"

#include <emscripten.h>

#include <stdexcept>
#include <string_view>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    int RegisteredEvents()
    {
        return EM_ASM_INT({ return JSEvents.eventHandlers.length; });
    }

    EM_BOOL ForeignKey(int, const EmscriptenKeyboardEvent*, void* data)
    {
        ++*static_cast<int*>(data);
        return EM_FALSE;
    }
}

int main()
{
    try
    {
        int foreignEvents{};
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
            &foreignEvents, EM_TRUE, &ForeignKey);
        const int initialEvents = RegisteredEvents();
        {
            LamaPon::Web::WebInput input;
            Require(!input.Initialize("#missing-canvas"), "Missing target must fail.");
            Require(RegisteredEvents() == initialEvents, "Partial registration must roll back.");
            Require(!input.Initialize("#missing-canvas"), "Failed initialization may be retried.");
            Require(RegisteredEvents() == initialEvents, "Retry must not leak callbacks.");
            Require(input.Initialize(), "Valid initialization must succeed after failure.");
            const int activeEvents = RegisteredEvents();
            {
                LamaPon::Web::WebInput other;
                Require(!other.Initialize(), "Another input must not steal window callbacks.");
            }
            Require(RegisteredEvents() == activeEvents, "Non-owner destruction must not remove callbacks.");
            EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyW'})); });
            Require(input.IsDown("KeyW") && input.WasPressed("KeyW"), "Key event must reach its owner.");
            input.EndFrame();
            Require(input.IsDown("KeyW") && !input.WasPressed("KeyW"), "Frame boundary must consume the edge only.");
        }
        Require(RegisteredEvents() == initialEvents, "Destruction must unregister all owned callbacks.");
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyA'})); });
        Require(foreignEvents == 2, "Other subsystems must retain their callbacks.");
        emscripten_html5_remove_event_listener(EMSCRIPTEN_EVENT_TARGET_WINDOW,
            &foreignEvents, EMSCRIPTEN_EVENT_KEYDOWN, reinterpret_cast<void*>(&ForeignKey));
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keyup', {code: 'KeyW'})); });
        {
            LamaPon::Web::WebInput replacement;
            Require(replacement.Initialize(), "Destruction must release ownership.");
        }
        EM_ASM({
            globalThis.testLogs = [];
            (['info', 'warn', 'error']).forEach(level => {
                const original = console[level];
                console[level] = message => { testLogs.push([level, message]); original.call(console, message); };
            });
        });
        LamaPon::Logger::Instance().Info(std::string_view("日本語100% suffix").substr(0, 13));
        LamaPon::Logger::Instance().Warning("warning");
        LamaPon::Logger::Instance().Error("error");
        Require(EM_ASM_INT({
            return JSON.stringify(testLogs) === JSON.stringify([
                ['info', '日本語100%'], ['warn', 'warning'], ['error', 'error']]);
        }), "Portable logging must preserve level, UTF-8, and string_view length.");
        EM_ASM({ document.body.dataset.testStatus = 'passed'; });
        return 0;
    }
    catch (const std::exception& exception)
    {
        EM_ASM({
            document.body.dataset.testStatus = 'failed';
            document.body.append(UTF8ToString($0));
        }, exception.what());
        return 1;
    }
}
