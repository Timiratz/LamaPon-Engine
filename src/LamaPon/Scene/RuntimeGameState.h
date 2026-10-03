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
    // 値は実行中のメモリーに保持し、取得時に型変換は行いません。
    // 値を変更するキーは空文字列を禁止します。
    class RuntimeGameState final
    {
    public:
        // 実行時に保持する四種類の値
        using Value = std::variant<
            std::int64_t,
            double,
            bool,
            std::string>;

        // 整数値を登録または置換します(key: キー名, value: 保存する整数)。
        void SetInteger(std::string key, std::int64_t value);
        // 浮動小数点値を登録または置換します(key: キー名, value: 保存する数値)。
        void SetNumber(std::string key, double value);
        // 真偽値を登録または置換します(key: キー名, value: 保存する真偽値)。
        void SetBoolean(std::string key, bool value);
        // 文字列を登録または置換します(key: キー名, value: 所有権を渡す文字列)。
        void SetString(std::string key, std::string value);

        // 整数値を取得し、未登録か型違いなら既定値を返します(key: キー名, fallback: 既定値)。
        [[nodiscard]] std::int64_t Integer(
            std::string_view key,
            std::int64_t fallback = 0) const noexcept;
        // 浮動小数点値を取得し、未登録か型違いなら既定値を返します(key: キー名, fallback: 既定値)。
        [[nodiscard]] double Number(
            std::string_view key,
            double fallback = 0.0) const noexcept;
        // 真偽値を取得し、未登録か型違いなら既定値を返します(key: キー名, fallback: 既定値)。
        [[nodiscard]] bool Boolean(
            std::string_view key,
            bool fallback = false) const noexcept;
        // 文字列をコピーし、未登録か型違いなら既定値を返します(key: キー名, fallback: 既定値)。
        [[nodiscard]] std::string String(
            std::string_view key,
            std::string fallback = {}) const;

        // キーの値を非所有参照で返し、未登録ならnullptrです(key: キー名)。
        // 対象の置換・削除・全消去後は参照を使用しません。
        [[nodiscard]] const Value* Find(
            std::string_view key) const noexcept;
        // キーの値が存在するか返します(key: キー名)。
        [[nodiscard]] bool Contains(
            std::string_view key) const noexcept;
        // 現在のキーと値を順序未指定のコピーで取得します。
        [[nodiscard]] std::vector<
            std::pair<std::string, Value>> Snapshot() const;
        // キーの値を削除し、削除できたか返します(key: キー名)。
        bool Remove(std::string_view key);
        // 全てのキーと値を消去します。
        void Clear() noexcept;
        // 保持するキーの件数を返します。
        [[nodiscard]] std::size_t Size() const noexcept
        {
            return m_values.size();
        }

    private:
        // 空でないキーか検査します(key: キー名)。
        // 空の場合はinvalid_argumentです。
        static void ValidateKey(std::string_view key);

        // キーごとに保持する実行時の値
        std::unordered_map<std::string, Value> m_values;
    };
}
