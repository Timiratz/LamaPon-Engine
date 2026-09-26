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
        NetworkEndpoint endpoint;
        if (!NetworkEndpoint::Parse(address, 1, endpoint) || endpoint.Family() != AF_INET || !endpoint.Unicast()) return false;
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
        const auto actual = m_gateway.Inspect(m_entry.port);
        if (!actual || !Owns(*actual) || !actual->enabled || actual->leaseSeconds == 0 || actual->leaseSeconds > 120)
        { Release(); return false; }
        return true;
    }
    bool NetworkPortLease::Renew()
    {
        if (!m_owned) return false;
        const auto actual = m_gateway.Inspect(m_entry.port);
        if (!actual || !Owns(*actual)) { m_owned = false; return false; }
        if (!m_gateway.Add(m_entry.port, m_entry)) return false;
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
            const auto actual = m_gateway.Inspect(m_entry.port);
            if (actual && Owns(*actual)) m_gateway.Remove(m_entry.port);
        }
        catch (...) { /* 応答がない場合も、要求した短期leaseの期限で失効します。 */ }
    }
    namespace
    {
        using Microsoft::WRL::ComPtr;
        struct Variant final
        {
            VARIANT value{};
            Variant() { VariantInit(&value); }
            ~Variant() { VariantClear(&value); }
            Variant(const Variant&) = delete;
            Variant& operator=(const Variant&) = delete;
            void Array(const ULONG size)
            {
                value.vt = VT_ARRAY | VT_VARIANT; value.parray = SafeArrayCreateVector(VT_VARIANT, 0, size);
                if (!value.parray) throw std::bad_alloc();
            }
            void Text(const wchar_t* text) { value.vt = VT_BSTR; value.bstrVal = SysAllocString(text); if (!value.bstrVal) throw std::bad_alloc(); }
        };
        void Put(VARIANT& array, const LONG index, VARIANT& value)
        {
            auto position = index;
            if (FAILED(SafeArrayPutElement(array.parray, &position, &value))) throw std::runtime_error("UPnP引数が無効です。");
        }
        void PutText(VARIANT& array, const LONG index, const std::string& text)
        {
            Variant value; const std::wstring wide(text.begin(), text.end()); value.Text(wide.c_str()); Put(array, index, value.value);
        }
        void PutNumber(VARIANT& array, const LONG index, const std::uint32_t number)
        {
            Variant value; value.value.vt = VT_UI4; value.value.ulVal = number; Put(array, index, value.value);
        }
        HRESULT Action(IUPnPService* service, const wchar_t* name, Variant& input, Variant& output)
        {
            Variant action, returned; action.Text(name); output.Array(0); returned.Array(0);
            return service->InvokeAction(action.value.bstrVal, input.value, &output.value, &returned.value);
        }
        std::string TextAt(Variant& values, const LONG index)
        {
            if (values.value.vt != (VT_ARRAY | VT_VARIANT)) throw std::runtime_error("UPnP応答が無効です。");
            auto position = index; Variant value, text;
            if (FAILED(SafeArrayGetElement(values.value.parray, &position, &value.value))
                || FAILED(VariantChangeType(&text.value, &value.value, 0, VT_BSTR))) throw std::runtime_error("UPnP応答が無効です。");
            std::string result;
            if (SysStringLen(text.value.bstrVal) > 128) throw std::runtime_error("UPnP応答の上限です。");
            for (UINT i = 0; i < SysStringLen(text.value.bstrVal); ++i)
            {
                const auto c = text.value.bstrVal[i];
                if (c < 32 || c > 126) throw std::runtime_error("UPnP応答の文字が無効です。");
                result += static_cast<char>(c);
            }
            return result;
        }
        std::uint32_t NumberAt(Variant& values, const LONG index)
        {
            const auto text = TextAt(values, index); std::uint32_t result{};
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) throw std::runtime_error("UPnP数値が無効です。");
            return result;
        }
        class WindowsGateway final : public INetworkGateway
        {
        public:
            explicit WindowsGateway(ComPtr<IUPnPService> service) : m_service(std::move(service)) {}
            std::optional<NetworkPortEntry> Inspect(const std::uint16_t port) override
            {
                Variant input, output; input.Array(3); PutText(input.value, 0, ""); PutNumber(input.value, 1, port); PutText(input.value, 2, "TCP");
                const auto hr = Action(m_service.Get(), L"GetSpecificPortMappingEntry", input, output);
                // 714 NoSuchEntryInArrayだけを、既存設定がないことの証拠として扱います。
                if (hr == UPNP_E_ACTION_SPECIFIC_BASE + (714 - FAULT_ACTION_SPECIFIC_BASE)) return std::nullopt;
                if (FAILED(hr)) throw std::runtime_error("ルーターの既存ポート設定を確認できません。");
                const auto internal = NumberAt(output, 0);
                if (internal == 0 || internal > 65535) throw std::runtime_error("UPnP内部ポートが無効です。");
                Variant enabled, converted; LONG enabledIndex = 2;
                if (FAILED(SafeArrayGetElement(output.value.parray, &enabledIndex, &enabled.value))
                    || FAILED(VariantChangeType(&converted.value, &enabled.value, 0, VT_BOOL)))
                    throw std::runtime_error("UPnP有効状態が無効です。");
                return NetworkPortEntry{ TextAt(output, 1), static_cast<std::uint16_t>(internal), TextAt(output, 3),
                    NumberAt(output, 4), converted.value.boolVal != VARIANT_FALSE };
            }
            bool Add(const std::uint16_t externalPort, const NetworkPortEntry& entry) override
            {
                Variant input, output; input.Array(8); PutText(input.value, 0, ""); PutNumber(input.value, 1, externalPort);
                PutText(input.value, 2, "TCP"); PutNumber(input.value, 3, entry.port); PutText(input.value, 4, entry.client);
                Variant enabled; enabled.value.vt = VT_BOOL; enabled.value.boolVal = VARIANT_TRUE; Put(input.value, 5, enabled.value);
                PutText(input.value, 6, entry.description); PutNumber(input.value, 7, entry.leaseSeconds);
                return SUCCEEDED(Action(m_service.Get(), L"AddPortMapping", input, output));
            }
            void Remove(const std::uint16_t externalPort) override
            {
                Variant input, output; input.Array(3); PutText(input.value, 0, ""); PutNumber(input.value, 1, externalPort); PutText(input.value, 2, "TCP");
                static_cast<void>(Action(m_service.Get(), L"DeletePortMapping", input, output));
            }
            std::string ExternalAddress() override
            {
                Variant input, output; input.Array(0);
                if (FAILED(Action(m_service.Get(), L"GetExternalIPAddress", input, output))) return {};
                return TextAt(output, 0);
            }
        private:
            ComPtr<IUPnPService> m_service;
        };
        template<class Item, class Collection> std::vector<ComPtr<Item>> Enumerate(Collection* collection)
        {
            std::vector<ComPtr<Item>> result;
            if (!collection) return result;
            ComPtr<IUnknown> unknown; ComPtr<IEnumVARIANT> iterator;
            if (FAILED(collection->get__NewEnum(&unknown)) || FAILED(unknown.As(&iterator))) return result;
            for (int count = 0; count < 64; ++count)
            {
                Variant value; ULONG fetched{};
                if (iterator->Next(1, &value.value, &fetched) != S_OK || fetched != 1) break;
                if (value.value.vt != VT_DISPATCH || !value.value.pdispVal) continue;
                ComPtr<Item> item;
                if (SUCCEEDED(value.value.pdispVal->QueryInterface(IID_PPV_ARGS(&item)))) result.push_back(std::move(item));
            }
            return result;
        }
        ComPtr<IUPnPService> GatewayService(IUPnPDevice* device, const int depth = 0)
        {
            if (!device || depth > 4) return {};
            ComPtr<IUPnPServices> services; device->get_Services(&services);
            for (const auto& service : Enumerate<IUPnPService>(services.Get()))
            {
                BSTR type{};
                if (FAILED(service->get_ServiceTypeIdentifier(&type))) continue;
                const std::wstring name(type ? type : L""); SysFreeString(type);
                if (name == L"urn:schemas-upnp-org:service:WANIPConnection:1"
                    || name == L"urn:schemas-upnp-org:service:WANIPConnection:2"
                    || name == L"urn:schemas-upnp-org:service:WANPPPConnection:1") return service;
            }
            ComPtr<IUPnPDevices> children; device->get_Children(&children);
            for (const auto& child : Enumerate<IUPnPDevice>(children.Get()))
                if (auto service = GatewayService(child.Get(), depth + 1)) return service;
            return {};
        }
    }
    struct NetworkPortMapping::Implementation final
    {
        mutable std::mutex mutex;
        std::condition_variable_any wake;
        std::stop_source cancel;
        std::string endpoint;
        std::string status;
        struct Task final
        {
            std::shared_ptr<Implementation> state;
            std::string listenAddress;
            HMODULE module{};
        };
        static void CALLBACK Callback(PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK)
        {
            std::unique_ptr<Task> task(static_cast<Task*>(context));
            // 待機中にSessionが破棄されても、状態と実装DLLはcallbackの完了まで生存します。
            FreeLibraryWhenCallbackReturns(instance, task->module);
            static_cast<void>(CallbackMayRunLong(instance));
            task->state->Run(task->state->cancel.get_token(), task->listenAddress);
        }
        void Set(std::string text, std::string address = {})
        {
            std::lock_guard lock(mutex); status = std::move(text); endpoint = std::move(address);
        }
        void Run(const std::stop_token stop, const std::string& listenAddress)
        {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) { Set("WindowsのUPnPを開始できません。手動設定を利用してください。"); return; }
            struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
            try
            {
                NetworkEndpoint listen;
                if (!NetworkEndpoint::Parse(listenAddress, 0, listen) || listen.Family() != AF_INET || listen.Port() == 0)
                { Set("自動ポート設定はIPv4の待受で利用できます。IPv6は回線とファイアウォールを確認してください。"); return; }
                SOCKADDR_INET destination{}, source{}; destination.si_family = AF_INET;
                destination.Ipv4.sin_addr.s_addr = htonl(0xc6120001);
                MIB_IPFORWARD_ROW2 route{};
                if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR)
                { Set("外部回線のIPv4インターフェースが見つかりません。"); return; }
                NetworkEndpoint local; local.storage.ss_family = AF_INET;
                *reinterpret_cast<sockaddr_in*>(&local.storage) = source.Ipv4;
                if (!listen.Wildcard() && listen.Host() != local.Host())
                { Set("待受先が外部回線のIPv4と異なります。0.0.0.0または回線側のアドレスを選んでください。"); return; }
                ComPtr<IUPnPDeviceFinder> finder;
                if (FAILED(CoCreateInstance(CLSID_UPnPDeviceFinder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&finder))))
                { Set("WindowsのUPnP検索が利用できません。"); return; }
                Variant type; type.Text(L"urn:schemas-upnp-org:device:InternetGatewayDevice:1");
                ComPtr<IUPnPDevices> devices;
                if (FAILED(finder->FindByType(type.value.bstrVal, 0, &devices)))
                { Set("対応ルーターを検索できません。UPnP設定または手動ポート転送を確認してください。"); return; }
                if (stop.stop_requested()) return;
                for (const auto& device : Enumerate<IUPnPDevice>(devices.Get()))
                {
                    auto service = GatewayService(device.Get()); if (!service) continue;
                    WindowsGateway gateway(std::move(service));
                    const auto external = gateway.ExternalAddress();
                    if (!IsPublicNetworkIpv4(external))
                    { Set("公開IPv4を確認できません。共有IPv4・二重ルーター・回線設定を確認してください。"); continue; }
                    if (stop.stop_requested()) return;
                    auto marker = RandomNetworkKey();
                    NetworkPortLease lease(gateway, local.Host(), listen.Port(), "LamaPon-" + Hex(std::span(marker).first(8)));
                    SecureZeroMemory(marker.data(), marker.size());
                    if (!lease.Acquire()) { Set("短期のポート転送を確保できません。既存設定・UPnP対応状況を確認してください。"); continue; }
                    Set("ルーターへ短期のポート転送を設定しました。別回線からの到達は未確認です。", external + ":" + std::to_string(listen.Port()));
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
        auto task = std::make_unique<Implementation::Task>(); task->state = m_impl; task->listenAddress = listenAddress;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Implementation::Callback), &task->module))
        { m_impl->Set("非同期のUPnP処理を開始できません。"); return; }
        const auto work = CreateThreadpoolWork(&Implementation::Callback, task.get(), nullptr);
        if (!work) { FreeLibrary(task->module); m_impl->Set("非同期のUPnP処理を開始できません。"); return; }
        task.release(); SubmitThreadpoolWork(work); CloseThreadpoolWork(work);
    }
    void NetworkPortMapping::Stop() noexcept
    {
        // WindowsのUPnP応答待ちはUIを止めません。処理側がキャンセルを確認して自分のleaseを解放します。
        m_impl->cancel.request_stop(); m_impl->wake.notify_all();
        m_impl->Set({});
    }
    std::string NetworkPortMapping::Endpoint() const { std::lock_guard lock(m_impl->mutex); return m_impl->endpoint; }
    std::string NetworkPortMapping::Status() const { std::lock_guard lock(m_impl->mutex); return m_impl->status; }
}
