#include "LamaPon/LamaPon.h"
#include "LamaPon/Web/WebInput.h"

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
        }
        Require(RegisteredEvents() == initialEvents, "Destruction must unregister all owned callbacks.");
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyA'})); });
        Require(foreignEvents == 2, "Other subsystems must retain their callbacks.");
        emscripten_html5_remove_event_listener(EMSCRIPTEN_EVENT_TARGET_WINDOW,
            &foreignEvents, EMSCRIPTEN_EVENT_KEYDOWN, reinterpret_cast<void*>(&ForeignKey));
        EM_ASM({ window.dispatchEvent(new KeyboardEvent('keyup', {code: 'KeyW'})); });
        {
            // 破棄後に所有権を取得する入力
            LamaPon::Web::WebInput replacement;
            Require(replacement.Initialize(), "Destruction must release ownership.");
        }
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
