#include "utility.h"

using namespace std;

#ifndef __linux__
vector<asio::ip::address> utility::get_interface_addresses()
{
    vector<asio::ip::address> retval;
    asio::io_service io_service;
    asio::ip::udp::resolver resolver(io_service);
    auto h = asio::ip::host_name();
    for_each(resolver.resolve({h, ""}), {}, [&](const auto& re) {
        auto addr = re.endpoint().address();
        if ((addr.is_v4() && addr != asio::ip::address_v4::any()) ||
            (addr.is_v6() && addr != asio::ip::address_v6::any()))
            retval.push_back(addr);
    });
    return retval;
}
#else

#include <functional>

typedef function<void(const char* host_addr)> fn_host_addr;

static void show_interface(int fd, const char* name, fn_host_addr fn)
{
    int family;
    struct ifreq ifreq;
    char host[128];
    memset(&ifreq, 0, sizeof ifreq);
    strncpy(ifreq.ifr_name, name, IFNAMSIZ);
    if (ioctl(fd, SIOCGIFADDR, &ifreq) != 0) {
        return;
    }
    switch (family = ifreq.ifr_addr.sa_family) {
    case AF_UNSPEC:
        return; /* ignore */
    case AF_INET:
    case AF_INET6:
        getnameinfo(&ifreq.ifr_addr, sizeof ifreq.ifr_addr, host, sizeof host, 0, 0, NI_NUMERICHOST);
        fn(host);
        break;
    default:
        break;
    }
}

static void list_interfaces(int fd, fn_host_addr fn, void (*show)(int fd, const char* name, fn_host_addr fn))
{
    struct ifreq* ifreq;
    struct ifconf ifconf;
    char buf[16384];
    int i;
    size_t len;

    ifconf.ifc_len = sizeof buf;
    ifconf.ifc_buf = buf;
    if (ioctl(fd, SIOCGIFCONF, &ifconf) != 0) {
        return;
    }

    ifreq = ifconf.ifc_req;
    for (i = 0; i < ifconf.ifc_len;) {
/* some systems have ifr_addr.sa_len and adjust the length that
* way, but not mine. weird */
#ifndef linux
        len = IFNAMSIZ + ifreq->ifr_addr.sa_len;
#else
        len = sizeof *ifreq;
#endif
        if (show) {
            show(fd, ifreq->ifr_name, fn);
        }
        ifreq = (struct ifreq*)((char*)ifreq + len);
        i += len;
    }
}

static void show_all_interfaces(int family, fn_host_addr fn)
{
    int fd;

    fd = socket(family, SOCK_DGRAM, 0);
    if (fd >= 0) {
        list_interfaces(fd, fn, show_interface);
        close(fd);
    }
}

vector<asio::ip::address> utility::get_interface_addresses()
{
    vector<asio::ip::address> retval;
    show_all_interfaces(PF_INET, [&](const char* host) { retval.push_back(asio::ip::address_v4::from_string(host)); });
    show_all_interfaces(PF_INET6, [&](const char* host) { retval.push_back(asio::ip::address_v6::from_string(host)); });
    return retval;
}
#endif