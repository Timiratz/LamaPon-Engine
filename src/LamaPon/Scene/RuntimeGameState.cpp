#include "LamaPon/Scene/RuntimeGameState.h"

#include <stdexcept>
#include <utility>

namespace LamaPon
{
    void RuntimeGameState::SetInteger(
        std::string key,
        const std::int64_t value)
    {
        ValidateKey(key);
        m_values.insert_or_assign(
            std::move(key),
            value);
    }

    void RuntimeGameState::SetNumber(
        std::string key,
        const double value)
    {
        ValidateKey(key);
        m_values.insert_or_assign(
            std::move(key),
            value);
    }

    void RuntimeGameState::SetBoolean(
        std::string key,
        const bool value)
    {
        ValidateKey(key);
        m_values.insert_or_assign(
            std::move(key),
            value);
    }

    void RuntimeGameState::SetString(
        std::string key,
        std::string value)
    {
        ValidateKey(key);
        m_values.insert_or_assign(
            std::move(key),
            std::move(value));
    }

    std::int64_t RuntimeGameState::Integer(
        const std::string_view key,
        const std::int64_t fallback) const noexcept
    {
        const auto* value = Find(key);
        const auto* result = value != nullptr
            ? std::get_if<std::int64_t>(value)
            : nullptr;
        return result != nullptr ? *result : fallback;
    }

    double RuntimeGameState::Number(
        const std::string_view key,
        const double fallback) const noexcept
    {
        const auto* value = Find(key);
        const auto* result = value != nullptr
            ? std::get_if<double>(value)
            : nullptr;
        return result != nullptr ? *result : fallback;
    }

    bool RuntimeGameState::Boolean(
        const std::string_view key,
        const bool fallback) const noexcept
    {
        const auto* value = Find(key);
        const auto* result = value != nullptr
            ? std::get_if<bool>(value)
            : nullptr;
        return result != nullptr ? *result : fallback;
    }

    std::string RuntimeGameState::String(
        const std::string_view key,
        std::string fallback) const
    {
        const auto* value = Find(key);
        const auto* result = value != nullptr
            ? std::get_if<std::string>(value)
            : nullptr;
        return result != nullptr ? *result : std::move(fallback);
    }

    const RuntimeGameState::Value*
        RuntimeGameState::Find(
            const std::string_view key) const noexcept
    {
        for (const auto& [storedKey, value] : m_values)
        {
            if (storedKey == key)
            {
                return &value;
            }
        }
        return nullptr;
    }

    bool RuntimeGameState::Contains(
        const std::string_view key) const noexcept
    {
        return Find(key) != nullptr;
    }

    std::vector<std::pair<std::string, RuntimeGameState::Value>>
        RuntimeGameState::Snapshot() const
    {
        std::vector<std::pair<std::string, Value>> values;
        values.reserve(m_values.size());
        for (const auto& [key, value] : m_values)
        {
            values.emplace_back(key, value);
        }
        return values;
    }

    bool RuntimeGameState::Remove(
        const std::string_view key)
    {
        return m_values.erase(std::string(key)) != 0;
    }

    void RuntimeGameState::Clear() noexcept
    {
        m_values.clear();
    }

    void RuntimeGameState::ValidateKey(
        const std::string_view key)
    {
        if (key.empty())
        {
            throw std::invalid_argument(
                "Runtime game state key cannot be empty.");
        }
    }
}
