#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <utility>
#include <vector>

namespace LamaPon
{
    class RuntimeGameState final
    {
    public:
        using Value = std::variant<
            std::int64_t,
            double,
            bool,
            std::string>;

        void SetInteger(std::string key, std::int64_t value);
        void SetNumber(std::string key, double value);
        void SetBoolean(std::string key, bool value);
        void SetString(std::string key, std::string value);

        [[nodiscard]] std::int64_t Integer(
            std::string_view key,
            std::int64_t fallback = 0) const noexcept;
        [[nodiscard]] double Number(
            std::string_view key,
            double fallback = 0.0) const noexcept;
        [[nodiscard]] bool Boolean(
            std::string_view key,
            bool fallback = false) const noexcept;
        [[nodiscard]] std::string String(
            std::string_view key,
            std::string fallback = {}) const;

        [[nodiscard]] const Value* Find(
            std::string_view key) const noexcept;
        [[nodiscard]] bool Contains(
            std::string_view key) const noexcept;
        // AI/デバッグ用に現在値を列挙します。返り値はコピーなので、
        // ゲーム更新中に呼び出しても保持中の値を変更しません。
        [[nodiscard]] std::vector<
            std::pair<std::string, Value>> Snapshot() const;
        bool Remove(std::string_view key);
        void Clear() noexcept;
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_values.size();
        }

    private:
        static void ValidateKey(std::string_view key);

        std::unordered_map<std::string, Value> m_values;
    };
}
