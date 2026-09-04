#include "LamaPon/LamaPon.h"

#include <emscripten.h>

namespace
{
    // string_viewの終端に依存せずUTF-8の長さを渡します。
    // メッセージ中の%などを書式として解釈させません。
    EM_JS(void, WriteConsole, (int level, const char* data, size_t length), {
        const message = UTF8ToString(data, length);
        if (level === 2) console.error(message);
        else if (level === 1) console.warn(message);
        else console.info(message);
    });
}

namespace LamaPon
{
    void Logger::Info(const std::string_view message) const noexcept
    {
        WriteConsole(0, message.data(), message.size());
    }

    void Logger::Warning(const std::string_view message) const noexcept
    {
        WriteConsole(1, message.data(), message.size());
    }

    void Logger::Error(const std::string_view message) const noexcept
    {
        WriteConsole(2, message.data(), message.size());
    }
}
