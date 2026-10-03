#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    class DataAsset;
    // JSON実装をヘッダーから隠す構築用の前方宣言。
    struct DataAssetBuilder;

    // 読み込み時に確定するアセット値の種別。
    enum class DataValueKind
    {
        // 未対応・未設定の種別
        None,
        // 真偽値の種別
        Boolean,
        // 整数・実数の共通種別
        Number,
        // 文字列の種別
        Text,
        // 配列・オブジェクトの要素列
        List
    };

    // キーに対応する値をDataAssetのアクセサーから参照する。
    struct DataValue final
    {
        // 大小を区別する値のキー
        std::string key;
        // 使用する値領域の種別
        DataValueKind kind{ DataValueKind::None };
        // Boolean種別の値
        bool boolean{};
        // 整数・実数を保持するdouble
        double number{};
        // Text種別のUTF8文字列
        std::string text;
        // valueキーでスカラーを包む要素
        std::vector<DataAsset> items;
    };

    // 型（フィールドの並び）はGame ModuleがLAMAPON_DATA_ASSETで宣言し、値はエディターのインスペクターで編集して*.asset.jsonへ保存します。
    // 配布アーカイブからも読めるよう、取得にはAssetManager::LoadDataAssetを使う。
    class DataAsset final
    {
    public:
        // JSONオブジェクトを読み、構文不正・非オブジェクトは空の値にする(json: JSONの文字列, name: 任意の表示名)。
        [[nodiscard]] static DataAsset FromJson(
            std::string_view json,
            std::string name = {});
        // リストを配列へ正規化して、型名と値をJSONへ書き出す。
        [[nodiscard]] std::string SerializeToJson() const;

        // 宣言と結び付く型識別子を借用して返す。
        [[nodiscard]] const std::string& TypeName() const noexcept
        {
            return m_typeName;
        }
        // 任意の表示名を借用して返す。
        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }
        // 表示名を置き換える(name: 新しい表示名)。
        void SetName(std::string name)
        {
            m_name = std::move(name);
        }
        // 登録された値がないか調べる。
        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return m_values.empty();
        }
        // 所有元の寿命内で使う値一覧を借用して返す。
        [[nodiscard]] const std::vector<DataValue>&
            Values() const noexcept
        {
            return m_values;
        }

        // キーに対応する値があるか調べる(key: 大小を区別するキー)。
        [[nodiscard]] bool Has(
            std::string_view key) const noexcept;

        // 真偽値または数値の非ゼロを真偽として得る(key: 大小を区別するキー, defaultValue: 不在・非対応型の既定値)。
        [[nodiscard]] bool GetBool(
            std::string_view key,
            bool defaultValue = false) const noexcept;
        // 数値を整数へ、真偽値を0・1へ変換する(key: 大小を区別するキー, defaultValue: 不在・非対応型の既定値)。
        [[nodiscard]] int GetInt(
            std::string_view key,
            int defaultValue = 0) const noexcept;
        // 数値を実数へ、真偽値を0・1へ変換する(key: 大小を区別するキー, defaultValue: 不在・非対応型の既定値)。
        [[nodiscard]] float GetFloat(
            std::string_view key,
            float defaultValue = 0.0f) const noexcept;
        // 文字列を取得し、不在・型不一致は既定値にする(key: 大小を区別するキー, defaultValue: 不在・型不一致の既定値)。
        [[nodiscard]] std::string GetText(
            std::string_view key,
            std::string defaultValue = {}) const;

        // 数値リストからXYを読み、不足・非対応成分は補完する(key: 大小を区別するキー, defaultValue: 各成分の補完値)。
        [[nodiscard]] DirectX::XMFLOAT2 GetVector2(
            std::string_view key,
            DirectX::XMFLOAT2 defaultValue = {}) const noexcept;
        // 数値リストからXYZを読み、不足・非対応成分は補完する(key: 大小を区別するキー, defaultValue: 各成分の補完値)。
        [[nodiscard]] DirectX::XMFLOAT3 GetVector3(
            std::string_view key,
            DirectX::XMFLOAT3 defaultValue = {}) const noexcept;
        // 数値リストからXYZWを読み、不足・非対応成分は補完する(key: 大小を区別するキー, defaultValue: 各成分の補完値)。
        [[nodiscard]] DirectX::XMFLOAT4 GetVector4(
            std::string_view key,
            DirectX::XMFLOAT4 defaultValue = {}) const noexcept;
        // 数値リストからRGBAを読み、不足・非対応成分は指定値で補完する(key: 大小を区別するキー, defaultValue: 各成分の補完値)。
        [[nodiscard]] DirectX::XMFLOAT4 GetColor(
            std::string_view key,
            DirectX::XMFLOAT4 defaultValue = {
                1.0f,
                1.0f,
                1.0f,
                1.0f
            }) const noexcept;

        // 文字列をアセット参照パスへ変換し、不在は空パスにする(key: 大小を区別するキー)。
        [[nodiscard]] std::filesystem::path GetAssetPath(
            std::string_view key) const;

        // リストの要素数を得て、不在・非リストは0にする(key: 大小を区別するキー)。
        [[nodiscard]] std::size_t Count(
            std::string_view key) const noexcept;
        // 所有元の寿命内で要素を借用し、範囲外・非リストは共有の空値を返す(key: 大小を区別するキー, index: ゼロ始まりの要素番号)。
        [[nodiscard]] const DataAsset& Item(
            std::string_view key,
            std::size_t index) const noexcept;

        // リスト内のスカラーを真偽値として得る(key: リストのキー, index: ゼロ始まりの要素番号, defaultValue: 取得できない場合の既定値)。
        [[nodiscard]] bool GetBoolAt(
            std::string_view key,
            std::size_t index,
            bool defaultValue = false) const noexcept;
        // リスト内のスカラーを整数として得る(key: リストのキー, index: ゼロ始まりの要素番号, defaultValue: 取得できない場合の既定値)。
        [[nodiscard]] int GetIntAt(
            std::string_view key,
            std::size_t index,
            int defaultValue = 0) const noexcept;
        // リスト内のスカラーを実数として得る(key: リストのキー, index: ゼロ始まりの要素番号, defaultValue: 取得できない場合の既定値)。
        [[nodiscard]] float GetFloatAt(
            std::string_view key,
            std::size_t index,
            float defaultValue = 0.0f) const noexcept;
        // リスト内の文字列を取得する(key: リストのキー, index: ゼロ始まりの要素番号, defaultValue: 取得できない場合の既定値)。
        [[nodiscard]] std::string GetTextAt(
            std::string_view key,
            std::size_t index,
            std::string defaultValue = {}) const;

        // プロセス内で共有する空アセットを借用して返す。
        [[nodiscard]] static const DataAsset& Empty() noexcept;

    private:
        friend struct DataAssetBuilder;

        // 所有元の寿命内で値を借用し、不在はnullptrにする(key: 大小を区別するキー)。
        [[nodiscard]] const DataValue* Find(
            std::string_view key) const noexcept;

        // 宣言と結び付く型識別子
        std::string m_typeName;
        // 任意の表示名
        std::string m_name;
        // 所有するキー付き値の一覧
        std::vector<DataValue> m_values;
    };

    // ファイル名が.asset.jsonで終わるか大小を区別せず調べる(path: 判定するパス)。
    [[nodiscard]] bool IsDataAssetPath(
        const std::filesystem::path& path);
}
