#include "LamaPon/Assets/DataAsset.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace LamaPon
{
    // privateへ書き込むための構築ヘルパーです。ヘッダから
    // nlohmann/jsonを隠したまま、JSONからDataAssetを組み立てます。
    struct DataAssetBuilder final
    {
        static DataAsset Make(
            std::string typeName,
            std::vector<DataValue> values,
            std::string name = {})
        {
            DataAsset asset;
            asset.m_typeName = std::move(typeName);
            asset.m_values = std::move(values);
            asset.m_name = std::move(name);
            return asset;
        }
    };

    namespace
    {
        using Json = nlohmann::json;

        // 数値や文字列だけのList要素は、この名前のキー1つを持つ
        // DataAssetとして保持します。要素がオブジェクトのときは、
        // そのフィールドがそのまま入ります。
        constexpr const char* ScalarItemKey = "value";

        [[nodiscard]] std::string Lowercase(std::string value)
        {
            std::ranges::transform(
                value,
                value.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(
                        std::tolower(character));
                });
            return value;
        }

        // JSONの1値をDataValueへ写します。objectとarrayは
        // どちらもListとして持ち、要素をDataAssetで包みます。
        void ReadValue(
            const Json& source,
            DataValue& value);

        // objectのフィールドをDataAssetの値として読み込みます。
        void ReadValues(
            const Json& source,
            std::vector<DataValue>& values)
        {
            if (!source.is_object())
            {
                return;
            }
            values.reserve(source.size());
            for (const auto& [key, item] : source.items())
            {
                DataValue value;
                value.key = key;
                ReadValue(item, value);
                if (value.kind != DataValueKind::None)
                {
                    values.push_back(std::move(value));
                }
            }
        }

        [[nodiscard]] DataAsset MakeItem(const Json& source)
        {
            std::vector<DataValue> values;
            if (source.is_object())
            {
                ReadValues(source, values);
            }
            else
            {
                DataValue value;
                value.key = ScalarItemKey;
                ReadValue(source, value);
                if (value.kind != DataValueKind::None)
                {
                    values.push_back(std::move(value));
                }
            }
            return DataAssetBuilder::Make(
                {},
                std::move(values));
        }

        void ReadValue(
            const Json& source,
            DataValue& value)
        {
            if (source.is_boolean())
            {
                value.kind = DataValueKind::Boolean;
                value.boolean = source.get<bool>();
                return;
            }
            if (source.is_number())
            {
                value.kind = DataValueKind::Number;
                value.number = source.get<double>();
                return;
            }
            if (source.is_string())
            {
                value.kind = DataValueKind::Text;
                value.text =
                    source.get_ref<const std::string&>();
                return;
            }
            if (source.is_array())
            {
                value.kind = DataValueKind::List;
                value.items.reserve(source.size());
                for (const auto& item : source)
                {
                    value.items.push_back(MakeItem(item));
                }
                return;
            }
            if (source.is_object())
            {
                // 単体のobjectも「要素1つのList」として持ちます。
                // Item(key, 0)で中身を引けます。
                value.kind = DataValueKind::List;
                value.items.push_back(MakeItem(source));
                return;
            }
            // null等は「無い」と同じ扱いにします。
        }

        [[nodiscard]] Json WriteValue(const DataValue& value);

        [[nodiscard]] Json WriteValues(
            const std::vector<DataValue>& values)
        {
            Json result = Json::object();
            for (const auto& value : values)
            {
                result[value.key] = WriteValue(value);
            }
            return result;
        }

        [[nodiscard]] Json WriteItem(const DataAsset& item)
        {
            const auto& values = item.Values();
            if (values.size() == 1
                && values.front().key == ScalarItemKey)
            {
                return WriteValue(values.front());
            }
            return WriteValues(values);
        }

        [[nodiscard]] Json WriteValue(const DataValue& value)
        {
            switch (value.kind)
            {
            case DataValueKind::Boolean:
                return value.boolean;
            case DataValueKind::Number:
                return value.number;
            case DataValueKind::Text:
                return value.text;
            case DataValueKind::List:
            {
                Json array = Json::array();
                for (const auto& item : value.items)
                {
                    array.push_back(WriteItem(item));
                }
                return array;
            }
            case DataValueKind::None:
            default:
                return Json{};
            }
        }

        // 数値のListから成分を1つ読みます。足りない成分は
        // 呼び出し側の既定値のままにします。
        void ReadComponents(
            const DataValue* value,
            float* components,
            const std::size_t count) noexcept
        {
            if (value == nullptr
                || value->kind != DataValueKind::List)
            {
                return;
            }
            for (std::size_t index = 0;
                index < count && index < value->items.size();
                ++index)
            {
                const auto& item = value->items[index];
                if (item.Has(ScalarItemKey))
                {
                    components[index] = item.GetFloat(
                        ScalarItemKey,
                        components[index]);
                }
            }
        }
    }

    DataAsset DataAsset::FromJson(
        const std::string_view json,
        std::string name)
    {
        DataAsset asset;
        asset.m_name = std::move(name);

        // 壊れたJSONでも例外を投げない形（allow_exceptions=false）で
        // 読みます。空のDataAssetになるだけで、ゲームは続きます。
        const auto document = Json::parse(
            json.begin(),
            json.end(),
            nullptr,
            false);
        if (document.is_discarded()
            || !document.is_object())
        {
            return asset;
        }

        if (document.contains("type")
            && document.at("type").is_string())
        {
            asset.m_typeName =
                document.at("type")
                    .get_ref<const std::string&>();
        }
        if (document.contains("values"))
        {
            ReadValues(
                document.at("values"),
                asset.m_values);
        }
        return asset;
    }

    std::string DataAsset::SerializeToJson() const
    {
        Json document;
        document["format"] = "LamaPonDataAsset";
        document["version"] = 1;
        document["type"] = m_typeName;
        document["values"] = WriteValues(m_values);
        return document.dump(2);
    }

    const DataValue* DataAsset::Find(
        const std::string_view key) const noexcept
    {
        for (const auto& value : m_values)
        {
            if (value.key == key)
            {
                return &value;
            }
        }
        return nullptr;
    }

    bool DataAsset::Has(
        const std::string_view key) const noexcept
    {
        return Find(key) != nullptr;
    }

    bool DataAsset::GetBool(
        const std::string_view key,
        const bool defaultValue) const noexcept
    {
        const auto* value = Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }
        switch (value->kind)
        {
        case DataValueKind::Boolean:
            return value->boolean;
        case DataValueKind::Number:
            return value->number != 0.0;
        default:
            return defaultValue;
        }
    }

    int DataAsset::GetInt(
        const std::string_view key,
        const int defaultValue) const noexcept
    {
        const auto* value = Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }
        switch (value->kind)
        {
        case DataValueKind::Number:
            return static_cast<int>(value->number);
        case DataValueKind::Boolean:
            return value->boolean ? 1 : 0;
        default:
            return defaultValue;
        }
    }

    float DataAsset::GetFloat(
        const std::string_view key,
        const float defaultValue) const noexcept
    {
        const auto* value = Find(key);
        if (value == nullptr)
        {
            return defaultValue;
        }
        switch (value->kind)
        {
        case DataValueKind::Number:
            return static_cast<float>(value->number);
        case DataValueKind::Boolean:
            return value->boolean ? 1.0f : 0.0f;
        default:
            return defaultValue;
        }
    }

    std::string DataAsset::GetText(
        const std::string_view key,
        std::string defaultValue) const
    {
        const auto* value = Find(key);
        if (value == nullptr
            || value->kind != DataValueKind::Text)
        {
            return defaultValue;
        }
        return value->text;
    }

    DirectX::XMFLOAT2 DataAsset::GetVector2(
        const std::string_view key,
        DirectX::XMFLOAT2 defaultValue) const noexcept
    {
        ReadComponents(Find(key), &defaultValue.x, 2);
        return defaultValue;
    }

    DirectX::XMFLOAT3 DataAsset::GetVector3(
        const std::string_view key,
        DirectX::XMFLOAT3 defaultValue) const noexcept
    {
        ReadComponents(Find(key), &defaultValue.x, 3);
        return defaultValue;
    }

    DirectX::XMFLOAT4 DataAsset::GetVector4(
        const std::string_view key,
        DirectX::XMFLOAT4 defaultValue) const noexcept
    {
        ReadComponents(Find(key), &defaultValue.x, 4);
        return defaultValue;
    }

    DirectX::XMFLOAT4 DataAsset::GetColor(
        const std::string_view key,
        DirectX::XMFLOAT4 defaultValue) const noexcept
    {
        ReadComponents(Find(key), &defaultValue.x, 4);
        return defaultValue;
    }

    std::filesystem::path DataAsset::GetAssetPath(
        const std::string_view key) const
    {
        const auto text = GetText(key);
        if (text.empty())
        {
            return {};
        }
        return PathFromUtf8(text);
    }

    std::size_t DataAsset::Count(
        const std::string_view key) const noexcept
    {
        const auto* value = Find(key);
        return value != nullptr
                && value->kind == DataValueKind::List
            ? value->items.size()
            : 0;
    }

    const DataAsset& DataAsset::Item(
        const std::string_view key,
        const std::size_t index) const noexcept
    {
        const auto* value = Find(key);
        if (value == nullptr
            || value->kind != DataValueKind::List
            || index >= value->items.size())
        {
            return Empty();
        }
        return value->items[index];
    }

    bool DataAsset::GetBoolAt(
        const std::string_view key,
        const std::size_t index,
        const bool defaultValue) const noexcept
    {
        return Item(key, index).GetBool(
            ScalarItemKey,
            defaultValue);
    }

    int DataAsset::GetIntAt(
        const std::string_view key,
        const std::size_t index,
        const int defaultValue) const noexcept
    {
        return Item(key, index).GetInt(
            ScalarItemKey,
            defaultValue);
    }

    float DataAsset::GetFloatAt(
        const std::string_view key,
        const std::size_t index,
        const float defaultValue) const noexcept
    {
        return Item(key, index).GetFloat(
            ScalarItemKey,
            defaultValue);
    }

    std::string DataAsset::GetTextAt(
        const std::string_view key,
        const std::size_t index,
        std::string defaultValue) const
    {
        return Item(key, index).GetText(
            ScalarItemKey,
            std::move(defaultValue));
    }

    const DataAsset& DataAsset::Empty() noexcept
    {
        static const DataAsset empty;
        return empty;
    }

    bool IsDataAssetPath(const std::filesystem::path& path)
    {
        return Lowercase(PathToUtf8(path.filename()))
            .ends_with(".asset.json");
    }
}
