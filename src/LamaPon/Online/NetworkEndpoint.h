#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <charconv>
#include <cstdint>
#include <string>

namespace LamaPon::Detail
{
    struct NetworkEndpoint final
    {
        sockaddr_storage storage{};
        int size{};
        int Family() const noexcept { return storage.ss_family; }
        sockaddr* Address() noexcept { return reinterpret_cast<sockaddr*>(&storage); }
        const sockaddr* Address() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
        std::uint16_t Port() const noexcept
        {
            return ntohs(Family() == AF_INET
                ? reinterpret_cast<const sockaddr_in*>(&storage)->sin_port
                : reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        }
        std::string Host() const
        {
            char text[INET6_ADDRSTRLEN]{};
            if (Family() == AF_INET)
                InetNtopA(AF_INET, &reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr, text, sizeof(text));
            else
                InetNtopA(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr, text, sizeof(text));
            std::string result(text);
            if (Family() == AF_INET6 && reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_scope_id != 0)
                result += "%" + std::to_string(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_scope_id);
            return result;
        }
        std::string Text() const
        {
            return (Family() == AF_INET6 ? "[" + Host() + "]" : Host()) + ":" + std::to_string(Port());
        }
        bool Wildcard() const noexcept
        {
            if (Family() == AF_INET) return reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr.s_addr == 0;
            return IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr) != 0;
        }
        bool Unicast() const noexcept
        {
            if (Wildcard()) return false;
            if (Family() == AF_INET)
            {
                const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr.s_addr);
                return (ip >> 24) != 0 && (ip >> 24) < 224 && ip != 0xffffffff;
            }
            return !IN6_IS_ADDR_MULTICAST(&reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr);
        }
        static bool Parse(const std::string& text, const std::uint16_t defaultPort, NetworkEndpoint& result)
        {
            if (text.empty() || text.size() > 96) return false;
            std::string host = text, number;
            std::uint32_t port = defaultPort, scope{};
            if (host.front() == '[')
            {
                const auto end = host.find(']');
                if (end == std::string::npos) return false;
                if (end + 1 < host.size())
                {
                    if (host[end + 1] != ':') return false;
                    number = host.substr(end + 2);
                    if (number.empty()) return false;
                }
                host = host.substr(1, end - 1);
            }
            else if (const auto colon = host.find(':'); colon != std::string::npos && colon == host.rfind(':'))
            {
                number = host.substr(colon + 1);
                host.resize(colon);
                if (number.empty()) return false;
            }
            if (!number.empty())
            {
                const auto parsed = std::from_chars(number.data(), number.data() + number.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || port > 65535) return false;
            }
            if (host == "localhost") host = "127.0.0.1";
            if (const auto percent = host.find('%'); percent != std::string::npos)
            {
                const auto index = host.substr(percent + 1);
                const auto parsed = std::from_chars(index.data(), index.data() + index.size(), scope);
                if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size() || scope == 0) return false;
                host.resize(percent);
            }
            result = {};
            auto& v4 = *reinterpret_cast<sockaddr_in*>(&result.storage);
            if (scope == 0 && InetPtonA(AF_INET, host.c_str(), &v4.sin_addr) == 1)
            {
                v4.sin_family = AF_INET; v4.sin_port = htons(static_cast<u_short>(port)); result.size = sizeof(v4);
                return true;
            }
            auto& v6 = *reinterpret_cast<sockaddr_in6*>(&result.storage);
            if (InetPtonA(AF_INET6, host.c_str(), &v6.sin6_addr) != 1) return false;
            v6.sin6_family = AF_INET6; v6.sin6_port = htons(static_cast<u_short>(port)); v6.sin6_scope_id = scope;
            result.size = sizeof(v6);
            return true;
        }
    };
}
