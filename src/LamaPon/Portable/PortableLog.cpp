#include "LamaPon/LamaPon.h"

#include <emscripten.h>

namespace
{
    // 文字列の長さを指定してコンソールへ送る(level: 0情報／1警告／2エラー, data: UTF-8文字列, length: バイト数)。
    EM_JS(void, WriteConsole, (int level, const char* data, size_t length), {
        // 指定長から変換したログ本文
        const message = UTF8ToString(data, length);
        if (level === 2) console.error(message);
        else if (level === 1) console.warn(message);
        else console.info(message);
    });
}

namespace LamaPon
{
    // 情報メッセージをWebコンソールへ送る(message: UTF-8文字列)。
    void Logger::Info(const std::string_view message) const noexcept
    {
        WriteConsole(0, message.data(), message.size());
    }

    // 警告メッセージをWebコンソールへ送る(message: UTF-8文字列)。
    void Logger::Warning(const std::string_view message) const noexcept
    {
        WriteConsole(1, message.data(), message.size());
    }

    // エラーメッセージをWebコンソールへ送る(message: UTF-8文字列)。
    void Logger::Error(const std::string_view message) const noexcept
    {
        WriteConsole(2, message.data(), message.size());
    }
}
