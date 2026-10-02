#include "LamaPon/Assets/DataAsset.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace LamaPon
{
    // JSON実装を公開せず、型・値・表示名を構築する。
    struct DataAssetBuilder final
    {
        // 型・値・表示名を所有するアセットを作る(typeName: 型識別子, values: 所有する値の一覧, name: 任意の表示名)。
        static DataAsset Make(
            std::string typeName,
            std::vector<DataValue> values,
            std::string name = {})
        {
            // 型・値・表示名を持つ構築結果
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

        // スカラー要素を包むvalueキー
        constexpr const char* ScalarItemKey = "value";

        // 文字列を小文字に変換する(value: 変換する文字列)。
        [[nodiscard]] std::string Lowercase(std::string value)
        {
            // 各バイトを小文字にする(character: 符号なしのUTF8バイト)。
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

        // 対応するJSON値を写し、オブジェクトは単一要素リストにする(source: 元のJSON値, value: 変換先の値)。
        void ReadValue(
            const Json& source,
            DataValue& value);

        // JSONオブジェクトの対応フィールドを追記する(source: 元のオブジェクト, values: 値の追記先)。
        void ReadValues(
            const Json& source,
            std::vector<DataValue>& values)
        {
            if (!source.is_object())
            {
                return;
            }
            values.reserve(source.size());
            // key: フィールド名、item: 元のJSON値
            for (const auto& [key, item] : source.items())
            {
                // キーに対応するアセット値
                DataValue value;
                value.key = key;
                ReadValue(item, value);
                if (value.kind != DataValueKind::None)
                {
                    values.push_back(std::move(value));
                }
            }
        }

        // オブジェクトを展開し、スカラーはvalueキーで包む(source: 元のリスト要素)。
        [[nodiscard]] DataAsset MakeItem(const Json& source)
        {
            // 要素が保持する値の一覧
            std::vector<DataValue> values;
            if (source.is_object())
            {
                ReadValues(source, values);
            }
            else
            {
                // キーに対応するアセット値
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

        // 対応するJSON値を写し、オブジェクトは単一要素リストにする(source: 元のJSON値, value: 変換先の値)。
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
                // 処理するリスト要素
                for (const auto& item : source)
                {
                    value.items.push_back(MakeItem(item));
                }
                return;
            }
            if (source.is_object())
            {
                // オブジェクトは一要素のリストに包み、Item(key, 0)で読む。
                value.kind = DataValueKind::List;
                value.items.push_back(MakeItem(source));
                return;
            }

        }

        // 登録種別に従ってJSONへ戻し、リストは配列にする(value: 書き出す値)。
        [[nodiscard]] Json WriteValue(const DataValue& value);

        // キー付きの値一覧をJSONオブジェクトへ戻す(values: 書き出す値の一覧)。
        [[nodiscard]] Json WriteValues(
            const std::vector<DataValue>& values)
        {
            // 書き出すキー付きJSON
            Json result = Json::object();
            // キーに対応するアセット値
            for (const auto& value : values)
            {
                result[value.key] = WriteValue(value);
            }
            return result;
        }

        // value一項目はスカラーへ、他はオブジェクトへ戻す(item: 書き出すリスト要素)。
        [[nodiscard]] Json WriteItem(const DataAsset& item)
        {
            // 要素が保持する値の一覧
            const auto& values = item.Values();
            if (values.size() == 1
                && values.front().key == ScalarItemKey)
            {
                return WriteValue(values.front());
            }
            return WriteValues(values);
        }

        // 登録種別に従ってJSONへ戻し、リストは配列にする(value: 書き出す値)。
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
                // 書き出すリストのJSON配列
                Json array = Json::array();
                // 処理するリスト要素
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

        // 対応成分だけ上書きし、欠損・非対応型は既定値を保つ(value: 元の数値リスト, components: 補完値入りの出力配列, count: 出力成分数)。
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
            // 読み出す成分の番号
            for (std::size_t index = 0;
                index < count && index < value->items.size();
                ++index)
            {
                // 処理するリスト要素
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
        // 型・値・表示名を持つ構築結果
        DataAsset asset;
        asset.m_name = std::move(name);


        // 読み込み・書き出し用JSON
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
        // 読み込み・書き出し用JSON
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
        // キーに対応するアセット値
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
        // キーに対応するアセット値
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
        // キーに対応するアセット値
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
        // キーに対応するアセット値
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
        // キーに対応するアセット値
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
        // アセットパスのUTF8文字列
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
        // キーに対応するアセット値
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
        // キーに対応するアセット値
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
        // プロセス共有の空アセット
        static const DataAsset empty;
        return empty;
    }

    bool IsDataAssetPath(const std::filesystem::path& path)
    {
        return Lowercase(PathToUtf8(path.filename()))
            .ends_with(".asset.json");
    }
}
