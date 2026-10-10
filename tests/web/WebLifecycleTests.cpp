#include "LamaPon/LamaPon.h"
#include "LamaPon/Web/WebInput.h"
#include "LamaPon/Web/WebRenderer3D.h"
#include "../EncodedImageFixture.h"
#include <GLES3/gl3.h>

#include <emscripten.h>

#include <stdexcept>
#include <string_view>

namespace
{
    // 条件違反を例外で検査結果へ伝える(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const char* message)
    {
        // 成立しない条件をテスト失敗にします。
        if (!condition) throw std::runtime_error(message);
    }

    // Emscriptenに登録済みのDOMイベント件数を返す。
    int RegisteredEvents()
    {
        return EM_ASM_INT({ return JSEvents.eventHandlers.length; });
    }

    // 外部のキー通知件数を増やして非所有Listenerの維持を検査する(data: 外部通知件数への借用ポインター)。
    EM_BOOL ForeignKey(int, const EmscriptenKeyboardEvent*, void* data)
    {
        ++*static_cast<int*>(data);
        return EM_FALSE;
    }

    // 実際のGPU出力で光の距離減衰と円錐方向を検査する。
    void TestLighting(const char* selector)
    {
        using namespace LamaPon::Web;
        Renderer3D renderer;
        Require(renderer.Initialize(selector, 64, 64), "Lighting renderer must initialize.");
        if (renderer.UsesCanvas2DFallback())
        {
            Require(!EM_ASM_INT({ return location.search.includes('requireWebGL=1'); }),
                "Requested WebGL shader tests must not silently fall back.");
            return;
        }
        Camera3D camera;
        camera.position = {0, 0, 3};
        renderer.SetCamera(camera);
        Lighting3D lighting;
        lighting.ambientIntensity = lighting.directionalIntensity = 0;
        renderer.SetLighting(lighting);
        const auto mesh = renderer.CreateMesh({
            {{-1,-1,0},{0,0,1},{}}, {{1,-1,0},{0,0,1},{}}, {{0,1,0},{0,0,1},{}}
        }, {0,1,2});
        Require(mesh != 0, "Light test mesh must exist.");
        const auto sample = [&](const std::vector<LocalLight3D>& lights) {
            renderer.SetLocalLights(lights);
            renderer.BeginFrame({0,0,0,1});
            renderer.DrawMesh(mesh, Mat4::Identity(), {1,1,1,1}, 1.0f, 0, true);
            renderer.EndFrame();
            std::array<unsigned char, 4> pixel{};
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            Require(glGetError() == GL_NO_ERROR, "Reading light test output must succeed.");
            return pixel;
        };
        const auto dark = sample({});
        LocalLight3D light;
        light.position = {0,0,2};
        light.color = {1,0,0,1};
        light.intensity = 1;
        light.range = 8;
        const auto point = sample({light});
        Require(point[0] > dark[0] + 30 && point[0] > point[1] + 30,
            "Point light must illuminate with its color.");
        light.range = 1;
        const auto outside = sample({light});
        Require(outside[0] <= dark[0] + 2, "Light must not illuminate beyond its range.");
        light.range = 8;
        light.spot = true;
        light.direction = {0,0,-1};
        const auto spot = sample({light});
        light.direction = {0,0,1};
        const auto away = sample({light});
        Require(spot[0] > away[0] + 30, "Spot light must respect its cone direction.");
        const auto texture = renderer.CreateTextureEncoded(LamaPonTest::EncodedImage());
        Require(texture != 0 && EM_ASM_INT({
            return Boolean(globalThis.__lamaponTextures[$0]);
        }, texture), "Pending encoded texture must belong to the renderer context.");
    }
}

// WebInputの所有権・失敗時の解除とPortableログを検査し、DOMへ結果を公開する。
int main()
{
    // WebInput検査の例外を終了コードへ変換します。
    try
    {
        // 外部Listenerへ届いた通知件数
        int foreignEvents{};
        emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
            &foreignEvents, EM_TRUE, &ForeignKey);
        // 外部Listener込みの初期件数
        const int initialEvents = RegisteredEvents();
        {
            // イベント所有と再初期化の検査対象
            LamaPon::Web::WebInput input;
            Require(!input.Initialize("#missing-canvas"), "Missing target must fail.");
            Require(RegisteredEvents() == initialEvents, "Partial registration must roll back.");
            Require(!input.Initialize("#missing-canvas"), "Failed initialization may be retried.");
            Require(RegisteredEvents() == initialEvents, "Retry must not leak callbacks.");
            Require(input.Initialize(), "Valid initialization must succeed after failure.");
            // 所有者の登録後のイベント件数
            const int activeEvents = RegisteredEvents();
            {
                // 所有権を取得できない別の入力
                LamaPon::Web::WebInput other;
                Require(!other.Initialize(), "Another input must not steal window callbacks.");
            }
            Require(RegisteredEvents() == activeEvents, "Non-owner destruction must not remove callbacks.");
            EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyW'})); });
            Require(input.IsDown("KeyW") && input.WasPressed("KeyW"), "Key event must reach its owner.");
            input.EndFrame();
            Require(input.IsDown("KeyW") && !input.WasPressed("KeyW"), "Frame boundary must consume the edge only.");
            EM_ASM({ window.dispatchEvent(new FocusEvent('blur')); });
            Require(!input.IsDown("KeyW") && !input.WasPressed("KeyW"), "Blur must clear held and pending input.");
            EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'F1'})); });
            Require(input.IsDown("F1") && input.ControlValue("KeyboardF1") == 1.0f
                && input.ControlWasPressed("KeyboardF1"), "Function key input must resolve through portable controls.");
            input.EndFrame();
            EM_ASM({ window.dispatchEvent(new KeyboardEvent('keyup', {code: 'F1'})); });
            Require(!input.IsDown("F1") && input.ControlValue("KeyboardF1") == 0.0f
                && input.ControlWasReleased("KeyboardF1"), "Function key release must resolve through portable controls.");
            input.EndFrame();
            EM_ASM({
                const canvas = document.getElementById('canvas');
                canvas.style.width = '400px';
                canvas.style.height = '200px';
                canvas.style.transformOrigin = 'top left';
                canvas.style.transform = 'scale(0.5)';
                const rect = canvas.getBoundingClientRect();
                canvas.dispatchEvent(new MouseEvent('mousedown', { button: 0,
                    clientX: rect.left + 100, clientY: rect.top + 50, bubbles: true }));
            });
            Require(input.PointerButtonDown(0), "Mouse press must be held.");
            Require(input.PointerX() == 200.0f && input.PointerY() == 100.0f,
                "Scaled canvas input must use logical coordinates.");
            EM_ASM({ window.dispatchEvent(new MouseEvent('mouseup', { button: 0, clientX: 0, clientY: 0 })); });
            Require(!input.PointerButtonDown(0) && input.PointerButtonReleased(0),
                "Releasing outside the canvas must not leave a held button.");
            input.EndFrame();
            EM_ASM({
                const canvas = document.getElementById('canvas');
                const rect = canvas.getBoundingClientRect();
                const touch = new Touch({identifier: 1, target: canvas,
                    clientX: rect.left + 100, clientY: rect.top + 50});
                canvas.dispatchEvent(new TouchEvent('touchstart', {touches: [touch], changedTouches: [touch], targetTouches: [touch]}));
            });
            Require(input.PointerButtonDown(0) && input.PointerButtonPressed(0)
                && input.PointerX() == 200.0f && input.PointerY() == 100.0f,
                "Scaled touch input must press at logical canvas coordinates.");
            input.EndFrame();
            EM_ASM({
                const canvas = document.getElementById('canvas');
                const rect = canvas.getBoundingClientRect();
                const touch = new Touch({identifier: 1, target: canvas,
                    clientX: rect.left + 100, clientY: rect.top + 50});
                canvas.dispatchEvent(new TouchEvent('touchend', {touches: [], changedTouches: [touch], targetTouches: []}));
            });
            Require(!input.PointerButtonDown(0) && input.PointerButtonReleased(0),
                "Touch end must release the pointer button.");
            input.EndFrame();
            EM_ASM({
                const canvas = document.getElementById('canvas');
                const rect = canvas.getBoundingClientRect();
                const touch = new Touch({identifier: 1, target: canvas,
                    clientX: rect.left + 100, clientY: rect.top + 50});
                canvas.dispatchEvent(new TouchEvent('touchstart', {touches: [touch], changedTouches: [touch], targetTouches: [touch]}));
                canvas.dispatchEvent(new TouchEvent('touchcancel', {touches: [], changedTouches: [touch], targetTouches: []}));
            });
            Require(!input.PointerValid() && !input.PointerButtonDown(0)
                && !input.PointerButtonReleased(0), "Cancelled touch must not generate a UI click.");
        }
        Require(RegisteredEvents() == initialEvents, "Destruction must unregister all owned callbacks.");
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyA'})); });
        Require(foreignEvents == 3, "Other subsystems must retain their callbacks.");
        emscripten_html5_remove_event_listener(EMSCRIPTEN_EVENT_TARGET_WINDOW,
            &foreignEvents, EMSCRIPTEN_EVENT_KEYDOWN, reinterpret_cast<void*>(&ForeignKey));
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keyup', {code: 'KeyW'})); });
        {
            // 破棄後に所有権を取得する入力
            LamaPon::Web::WebInput replacement;
            Require(replacement.Initialize(), "Destruction must release ownership.");
        }
        const int rendererEvents = RegisteredEvents();
        TestLighting("#canvas");
        Require(!EM_ASM_INT({ return Object.keys(globalThis.__lamaponTextures || {}).length; }),
            "Destroyed renderer must release pending encoded images.");
        EM_ASM({
            const canvas = document.createElement('canvas');
            canvas.id = 'webgl1-test';
            const getContext = canvas.getContext.bind(canvas);
            canvas.getContext = (type, attributes) => type === 'webgl2' ? null : getContext(type, attributes);
            document.body.appendChild(canvas);
        });
        TestLighting("#webgl1-test");
        Require(!EM_ASM_INT({ return Object.keys(globalThis.__lamaponTextures || {}).length; }),
            "Custom canvas renderer must release its encoded images.");
        Require(RegisteredEvents() == rendererEvents, "Renderer must release owned context event callbacks.");
        EM_ASM({
            // Consoleへ渡されたレベルと文面の記録
            globalThis.testLogs = [];
            // Consoleの各メソッドへ記録処理を差し込む(level: Consoleメソッドの名前)。
            (['info', 'warn', 'error']).forEach(level => {
                // 記録後に呼ぶ元のConsoleメソッド
                const original = console[level];
                // 文面を記録し元のConsoleへ転送する(message: 出力する文面)。
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
    // 検査失敗の文面をDOMへ公開する(exception: 捕捉した検査エラー)。
    catch (const std::exception& exception)
    {
        EM_ASM({
            document.body.dataset.testStatus = 'failed';
            document.body.append(UTF8ToString($0));
        }, exception.what());
        return 1;
    }
}
