#include "LamaPon/Online/NetworkEndpoint.h"
#include "LamaPon/Online/NetworkPortMapping.h"
#include "LamaPon/Online/NetworkCrypto.h"

#include <Windows.h>
#include <iphlpapi.h>
#include <upnp.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <stop_token>
#include <vector>

namespace LamaPon::Detail
{
    bool IsPublicNetworkIpv4(const std::string& address)
    {
        // 検査する外部IPv4アドレス
        NetworkEndpoint endpoint;
        if (!NetworkEndpoint::Parse(address, 1, endpoint) || endpoint.Family() != AF_INET || !endpoint.Unicast()) return false;
        // IPv4のホスト順表現
        const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(&endpoint.storage)->sin_addr.s_addr);
        return (ip >> 24) != 10 && (ip >> 24) != 127 && (ip >> 16) != 0xa9fe
            && (ip & 0xfff00000) != 0xac100000 && (ip >> 16) != 0xc0a8
            && (ip & 0xffc00000) != 0x64400000
            && (ip & 0xffffff00) != 0xc0000000 && (ip & 0xffffff00) != 0xc0000200
            && (ip & 0xfffe0000) != 0xc6120000 && (ip & 0xffffff00) != 0xc6336400
            && (ip & 0xffffff00) != 0xcb007100;
    }
    NetworkPortLease::NetworkPortLease(INetworkGateway& gateway, std::string client,
        const std::uint16_t port, std::string description)
        : m_gateway(gateway), m_entry{ std::move(client), port, std::move(description), 120 } {}
    NetworkPortLease::~NetworkPortLease() { Release(); }
    bool NetworkPortLease::Owns(const NetworkPortEntry& entry) const
    {
        return entry.client == m_entry.client && entry.port == m_entry.port && entry.description == m_entry.description;
    }
    bool NetworkPortLease::Acquire()
    {
        if (m_owned || m_gateway.Inspect(m_entry.port).has_value()) return false;
        if (!m_gateway.Add(m_entry.port, m_entry)) return false;
        m_owned = true;
        // ルーター上の現在の転送設定
        const auto actual = m_gateway.Inspect(m_entry.port);
        if (!actual || !Owns(*actual) || !actual->enabled || actual->leaseSeconds == 0 || actual->leaseSeconds > 120)
        { Release(); return false; }
        return true;
    }
    bool NetworkPortLease::Renew()
    {
        if (!m_owned) return false;
        // ルーター上の現在の転送設定
        const auto actual = m_gateway.Inspect(m_entry.port);
        if (!actual || !Owns(*actual)) { m_owned = false; return false; }
        if (!m_gateway.Add(m_entry.port, m_entry)) return false;
        // 更新後の転送設定
        const auto renewed = m_gateway.Inspect(m_entry.port);
        if (!renewed || !Owns(*renewed) || !renewed->enabled || renewed->leaseSeconds == 0 || renewed->leaseSeconds > 120)
        { Release(); return false; }
        return true;
    }
    void NetworkPortLease::Release() noexcept
    {
        if (!m_owned) return;
        m_owned = false;
        try
        {
            // ルーター上の現在の転送設定
            const auto actual = m_gateway.Inspect(m_entry.port);
            if (actual && Owns(*actual)) m_gateway.Remove(m_entry.port);
        }
        // 応答がない場合も、要求した短期leaseの期限で失効します。
        catch (...) { }
    }
    namespace
    {
        using Microsoft::WRL::ComPtr;
        struct Variant final
        {
            // 所有するCOM応答または引数
            VARIANT value{};
            // 空のVARIANTを初期化する。
            Variant() { VariantInit(&value); }
            // 所有するVARIANTの内容を解放する。
            ~Variant() { VariantClear(&value); }
            // VARIANT所有権の複製を禁止する。
            Variant(const Variant&) = delete;
            // VARIANT所有権のコピー代入を禁止する。
            Variant& operator=(const Variant&) = delete;
            // VARIANT配列を確保する(size: 配列の要素数)。
            void Array(const ULONG size)
            {
                value.vt = VT_ARRAY | VT_VARIANT; value.parray = SafeArrayCreateVector(VT_VARIANT, 0, size);
                if (!value.parray) throw std::bad_alloc();
            }
            // BSTR文字列を確保する(text: ワイド文字列)。
            void Text(const wchar_t* text) { value.vt = VT_BSTR; value.bstrVal = SysAllocString(text); if (!value.bstrVal) throw std::bad_alloc(); }
        };
        // 配列へVARIANTを複写する(array: 格納先の配列, index: 格納位置, value: 複写する値)。
        void Put(VARIANT& array, const LONG index, VARIANT& value)
        {
            // SAFEARRAY内の要素位置
            auto position = index;
            if (FAILED(SafeArrayPutElement(array.parray, &position, &value))) throw std::runtime_error("UPnP引数が無効です。");
        }
        // ASCII文字列をUPnP引数にする(array: 引数配列, index: 格納位置, text: ASCII文字列)。
        void PutText(VARIANT& array, const LONG index, const std::string& text)
        {
            // 所有するCOM応答または引数
            Variant value;
            // ASCII引数のワイド文字列
            const std::wstring wide(text.begin(), text.end()); value.Text(wide.c_str()); Put(array, index, value.value);
        }
        // 符号なし整数をUPnP引数にする(array: 引数配列, index: 格納位置, number: 格納する数値)。
        void PutNumber(VARIANT& array, const LONG index, const std::uint32_t number)
        {
            // 所有するCOM応答または引数
            Variant value; value.value.vt = VT_UI4; value.value.ulVal = number; Put(array, index, value.value);
        }
        // UPnP操作を呼び出す(service: ルーターの操作口, name: 操作名, input: 引数配列, output: 応答配列の出力先)。
        HRESULT Action(IUPnPService* service, const wchar_t* name, Variant& input, Variant& output)
        {
            // 呼び出す操作名と戻り値
            Variant action, returned; action.Text(name); output.Array(0); returned.Array(0);
            return service->InvokeAction(action.value.bstrVal, input.value, &output.value, &returned.value);
        }
        // 最大128文字の印字可能ASCII応答を読む(values: 応答配列, index: 要素位置)。
        std::string TextAt(Variant& values, const LONG index)
        {
            if (values.value.vt != (VT_ARRAY | VT_VARIANT)) throw std::runtime_error("UPnP応答が無効です。");
            // SAFEARRAY内の要素位置
            auto position = index;
            // 応答値と文字列変換の結果
            // 応答の数字文字列
            Variant value, text;
            if (FAILED(SafeArrayGetElement(values.value.parray, &position, &value.value))
                || FAILED(VariantChangeType(&text.value, &value.value, 0, VT_BSTR))) throw std::runtime_error("UPnP応答が無効です。");
            // 列挙または変換した応答の出力
            std::string result;
            if (SysStringLen(text.value.bstrVal) > 128) throw std::runtime_error("UPnP応答の上限です。");
            // 応答文字列の文字位置
            for (UINT i = 0; i < SysStringLen(text.value.bstrVal); ++i)
            {
                // 応答のASCII候補文字
                const auto c = text.value.bstrVal[i];
                if (c < 32 || c > 126) throw std::runtime_error("UPnP応答の文字が無効です。");
                result += static_cast<char>(c);
            }
            return result;
        }
        // 応答要素を符号なし整数として読む(values: 応答配列, index: 要素位置)。
        std::uint32_t NumberAt(Variant& values, const LONG index)
        {
            // 応答の数字文字列
            const auto text = TextAt(values, index);
            // 列挙または変換した応答の出力
            std::uint32_t result{};
            // 整数解析の終了位置と状態
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) throw std::runtime_error("UPnP数値が無効です。");
            return result;
        }
        class WindowsGateway final : public INetworkGateway
        {
        public:
            // UPnPサービスの所有権を受け取る(service: ルーターの操作口)。
            explicit WindowsGateway(ComPtr<IUPnPService> service) : m_service(std::move(service)) {}
            // 既存TCP転送を読み、未登録だけを空にする(port: 外部ポート番号)。
            std::optional<NetworkPortEntry> Inspect(const std::uint16_t port) override
            {
                // 要求引数と応答値
                Variant input, output; input.Array(3); PutText(input.value, 0, ""); PutNumber(input.value, 1, port); PutText(input.value, 2, "TCP");
                // UPnP操作の終了コード
                const auto hr = Action(m_service.Get(), L"GetSpecificPortMappingEntry", input, output);
                // 714 NoSuchEntryInArrayだけを、既存設定がないことの証拠として扱います。
                if (hr == UPNP_E_ACTION_SPECIFIC_BASE + (714 - FAULT_ACTION_SPECIFIC_BASE)) return std::nullopt;
                if (FAILED(hr)) throw std::runtime_error("ルーターの既存ポート設定を確認できません。");
                // ルーター上の内部ポート番号
                const auto internal = NumberAt(output, 0);
                if (internal == 0 || internal > 65535) throw std::runtime_error("UPnP内部ポートが無効です。");
                // 有効状態とBOOLへの変換結果
                // 整数解析の終了位置と状態
                Variant enabled, converted;
                // 有効状態を格納する応答位置
                LONG enabledIndex = 2;
                if (FAILED(SafeArrayGetElement(output.value.parray, &enabledIndex, &enabled.value))
                    || FAILED(VariantChangeType(&converted.value, &enabled.value, 0, VT_BOOL)))
                    throw std::runtime_error("UPnP有効状態が無効です。");
                return NetworkPortEntry{ TextAt(output, 1), static_cast<std::uint16_t>(internal), TextAt(output, 3),
                    NumberAt(output, 4), converted.value.boolVal != VARIANT_FALSE };
            }
            // 短期TCP転送を登録する(externalPort: 外部ポート番号, entry: 転送先と期限)。
            bool Add(const std::uint16_t externalPort, const NetworkPortEntry& entry) override
            {
                // 要求引数と応答値
                Variant input, output; input.Array(8); PutText(input.value, 0, ""); PutNumber(input.value, 1, externalPort);
                PutText(input.value, 2, "TCP"); PutNumber(input.value, 3, entry.port); PutText(input.value, 4, entry.client);
                // 有効指定のVARIANT
                Variant enabled; enabled.value.vt = VT_BOOL; enabled.value.boolVal = VARIANT_TRUE; Put(input.value, 5, enabled.value);
                PutText(input.value, 6, entry.description); PutNumber(input.value, 7, entry.leaseSeconds);
                return SUCCEEDED(Action(m_service.Get(), L"AddPortMapping", input, output));
            }
            // TCP転送の削除を要求する(externalPort: 外部ポート番号)。
            void Remove(const std::uint16_t externalPort) override
            {
                // 要求引数と応答値
                Variant input, output; input.Array(3); PutText(input.value, 0, ""); PutNumber(input.value, 1, externalPort); PutText(input.value, 2, "TCP");
                static_cast<void>(Action(m_service.Get(), L"DeletePortMapping", input, output));
            }
            // 外部IPv4アドレスを取得し、操作失敗時は空を返す。
            std::string ExternalAddress() override
            {
                // 要求引数と応答値
                Variant input, output; input.Array(0);
                if (FAILED(Action(m_service.Get(), L"GetExternalIPAddress", input, output))) return {};
                return TextAt(output, 0);
            }
        private:
            // ルーターのUPnPサービス
            ComPtr<IUPnPService> m_service;
        };
        // COMコレクションから最大64件を取り出す(collection: 借用するコレクション)。
        template<class Item, class Collection> std::vector<ComPtr<Item>> Enumerate(Collection* collection)
        {
            // 列挙または変換した応答の出力
            std::vector<ComPtr<Item>> result;
            if (!collection) return result;
            // COM列挙インターフェース
            ComPtr<IUnknown> unknown;
            // VARIANT列挙の所有先
            ComPtr<IEnumVARIANT> iterator;
            if (FAILED(collection->get__NewEnum(&unknown)) || FAILED(unknown.As(&iterator))) return result;
            // 最大64件の列挙位置
            for (int count = 0; count < 64; ++count)
            {
                // 所有するCOM応答または引数
                Variant value;
                // 今回列挙できた要素数
                ULONG fetched{};
                if (iterator->Next(1, &value.value, &fetched) != S_OK || fetched != 1) break;
                if (value.value.vt != VT_DISPATCH || !value.value.pdispVal) continue;
                // 列挙したCOM要素の所有先
                ComPtr<Item> item;
                if (SUCCEEDED(value.value.pdispVal->QueryInterface(IID_PPV_ARGS(&item)))) result.push_back(std::move(item));
            }
            return result;
        }
        // 深さ4まで子を探してWAN接続サービスを返す(device: 探索するデバイス, depth: 現在の階層深度)。
        ComPtr<IUPnPService> GatewayService(IUPnPDevice* device, const int depth = 0)
        {
            if (!device || depth > 4) return {};
            // デバイスが持つサービス一覧
            ComPtr<IUPnPServices> services; device->get_Services(&services);
            // 見つかったUPnPサービス
            for (const auto& service : Enumerate<IUPnPService>(services.Get()))
            {
                // サービス種別または検索種別
                BSTR type{};
                if (FAILED(service->get_ServiceTypeIdentifier(&type))) continue;
                // UPnPサービスの種別名
                const std::wstring name(type ? type : L""); SysFreeString(type);
                if (name == L"urn:schemas-upnp-org:service:WANIPConnection:1"
                    || name == L"urn:schemas-upnp-org:service:WANIPConnection:2"
                    || name == L"urn:schemas-upnp-org:service:WANPPPConnection:1") return service;
            }
            // 子デバイスの一覧
            ComPtr<IUPnPDevices> children; device->get_Children(&children);
            // サービスを探す子デバイス
            for (const auto& child : Enumerate<IUPnPDevice>(children.Get()))
                // 見つかったUPnPサービス
                if (auto service = GatewayService(child.Get(), depth + 1)) return service;
            return {};
        }
    }
    struct NetworkPortMapping::Implementation final
    {
        // 共有状態を守るミューテックス
        mutable std::mutex mutex;
        // 更新待機を中断する通知
        std::condition_variable_any wake;
        // ワーカーへの停止要求
        std::stop_source cancel;
        // 確認できた公開接続先
        std::string endpoint;
        // 利用者へ表示する転送状態
        std::string status;
        struct Task final
        {
            // ワーカーが保持する共有状態
            std::shared_ptr<Implementation> state;
            // ワーカーが扱うTCP待受先
            std::string listenAddress;
            // 完了まで保持する実装DLL
            HMODULE module{};
        };
        // 処理とDLLを完了まで保持してワーカーを実行する(instance: スレッドプールの呼出状態, context: 所有権を受け取るTask)。
        static void CALLBACK Callback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK)
        {
            // ワーカーへ移譲した処理の所有先
            std::unique_ptr<Task> task(static_cast<Task*>(context));
            // 待機中にSessionが破棄されても、状態と実装DLLはcallbackの完了まで生存します。
            FreeLibraryWhenCallbackReturns(instance, task->module);
            static_cast<void>(CallbackMayRunLong(instance));
            task->state->Run(task->state->cancel.get_token(), task->listenAddress);
        }
        // 状態文と公開接続先を排他的に更新する(text: 表示する状態, address: 公開接続先・空なら未確定)。
        void Set(std::string text, std::string address = {})
        {
            // 共有状態を更新する排他ロック
            std::lock_guard lock(mutex); status = std::move(text); endpoint = std::move(address);
        }
        // COMワーカーで転送を確保し45秒ごとに更新する(stop: 停止要求, listenAddress: TCP待受先)。
        void Run(const std::stop_token stop, const std::string& listenAddress)
        {
            // COM初期化の終了コード
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) { Set("WindowsのUPnPを開始できません。手動設定を利用してください。"); return; }
            // このワーカーのCOM終了処理
            struct ComScope
            {
                // 初期化したCOMを終了する。
                ~ComScope() { CoUninitialize(); }
            } comScope;
            try
            {
                // 解析したTCP待受先
                NetworkEndpoint listen;
                if (!NetworkEndpoint::Parse(listenAddress, 0, listen) || listen.Family() != AF_INET || listen.Port() == 0)
                { Set("自動ポート設定はIPv4の待受で利用できます。IPv6は回線とファイアウォールを確認してください。"); return; }
                // 経路検索先と選ばれた送信元
                SOCKADDR_INET destination{}, source{}; destination.si_family = AF_INET;
                destination.Ipv4.sin_addr.s_addr = htonl(0xc6120001);
                // 外部回線への経路情報
                MIB_IPFORWARD_ROW2 route{};
                if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR)
                { Set("外部回線のIPv4インターフェースが見つかりません。"); return; }
                // 転送先に使う内部IPv4
                NetworkEndpoint local; local.storage.ss_family = AF_INET;
                *reinterpret_cast<sockaddr_in*>(&local.storage) = source.Ipv4;
                if (!listen.Wildcard() && listen.Host() != local.Host())
                { Set("待受先が外部回線のIPv4と異なります。0.0.0.0または回線側のアドレスを選んでください。"); return; }
                // UPnPデバイス検索の所有先
                ComPtr<IUPnPDeviceFinder> finder;
                if (FAILED(CoCreateInstance(CLSID_UPnPDeviceFinder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&finder))))
                { Set("WindowsのUPnP検索が利用できません。"); return; }
                // サービス種別または検索種別
                Variant type; type.Text(L"urn:schemas-upnp-org:device:InternetGatewayDevice:1");
                // 見つかったゲートウェイ一覧
                ComPtr<IUPnPDevices> devices;
                if (FAILED(finder->FindByType(type.value.bstrVal, 0, &devices)))
                { Set("対応ルーターを検索できません。UPnP設定または手動ポート転送を確認してください。"); return; }
                if (stop.stop_requested()) return;
                // 操作する候補ゲートウェイ
                for (const auto& device : Enumerate<IUPnPDevice>(devices.Get()))
                {
                    // 見つかったUPnPサービス
                    auto service = GatewayService(device.Get()); if (!service) continue;
                    // 見つかったルーターの操作口
                    WindowsGateway gateway(std::move(service));
                    // ルーターが返した外部IPv4
                    const auto external = gateway.ExternalAddress();
                    if (!IsPublicNetworkIpv4(external))
                    { Set("公開IPv4を確認できません。共有IPv4・二重ルーター・回線設定を確認してください。"); continue; }
                    if (stop.stop_requested()) return;
                    // 転送の所有者識別用の乱数
                    auto marker = RandomNetworkKey();
                    // 識別用乱数を持つ120秒の転送
                    NetworkPortLease lease(gateway, local.Host(), listen.Port(), "LamaPon-" + Hex(std::span(marker).first(8)));
                    SecureZeroMemory(marker.data(), marker.size());
                    if (!lease.Acquire()) { Set("短期のポート転送を確保できません。既存設定・UPnP対応状況を確認してください。"); continue; }
                    Set("ルーターへ短期のポート転送を設定しました。別回線からの到達は未確認です。", external + ":" + std::to_string(listen.Port()));
                    // 更新待機中に解放できるロック
                    std::unique_lock lock(mutex);
                    while (!stop.stop_requested())
                    {
                        wake.wait_for(lock, stop, std::chrono::seconds(45), [] { return false; });
                        if (stop.stop_requested()) break;
                        lock.unlock();
                        if (!lease.Renew()) { Set("ポート転送を更新できません。新しい参加には手動設定が必要です。"); return; }
                        lock.lock();
                    }
                    return;
                }
                // 検索結果を更新する排他ロック
                std::lock_guard lock(mutex);
                if (status == "対応ルーターを検索中") status = "対応ルーターが見つかりません。UPnP設定または手動ポート転送を確認してください。";
            }
            catch (...) { Set("ルーターの応答を確認できません。手動ポート転送を利用してください。"); }
        }
    };
    NetworkPortMapping::NetworkPortMapping() : m_impl(std::make_shared<Implementation>()) {}
    NetworkPortMapping::~NetworkPortMapping() { Stop(); }
    void NetworkPortMapping::Start(const std::string& listenAddress)
    {
        Stop(); m_impl = std::make_shared<Implementation>(); m_impl->Set("対応ルーターを検索中");
        // 共有状態と待受先を持つ処理
        auto task = std::make_unique<Implementation::Task>(); task->state = m_impl; task->listenAddress = listenAddress;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Implementation::Callback), &task->module))
        { m_impl->Set("非同期のUPnP処理を開始できません。"); return; }
        // スレッドプールの処理ハンドル
        const auto work = CreateThreadpoolWork(&Implementation::Callback, task.get(), nullptr);
        if (!work) { FreeLibrary(task->module); m_impl->Set("非同期のUPnP処理を開始できません。"); return; }
        task.release(); SubmitThreadpoolWork(work); CloseThreadpoolWork(work);
    }
    void NetworkPortMapping::Stop() noexcept
    {
        // 停止は応答待ちをブロックせず、ワーカーがキャンセルを確認して自身の転送を解放します。
        m_impl->cancel.request_stop(); m_impl->wake.notify_all();
        m_impl->Set({});
    }
    std::string NetworkPortMapping::Endpoint() const
    {
        // 共有状態を読む排他ロック
        std::lock_guard lock(m_impl->mutex);
        return m_impl->endpoint;
    }
    std::string NetworkPortMapping::Status() const
    {
        // 共有状態を読む排他ロック
        std::lock_guard lock(m_impl->mutex);
        return m_impl->status;
    }
}
