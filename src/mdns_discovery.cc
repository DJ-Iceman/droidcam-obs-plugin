/*
Copyright (C) 2021 DEV47APPS, github.com/dev47apps

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>

#ifndef _WIN32
# include <arpa/inet.h>
# include <sys/errno.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <netdb.h>

// for mdns.h
# pragma GCC diagnostic ignored "-Wunused-function"
#endif
#ifdef __APPLE__
#include <ifaddrs.h>
#endif

#include "mdns.h"
#include "net.h"
#include "device_discovery.h"
#include "plugin.h"
#include "plugin_properties.h"
#include <util/platform.h>

// Callback handling parsing answers to queries sent
static int
query_callback(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry_type,
               uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data,
               size_t size, size_t name_offset, size_t name_length, size_t record_offset,
               size_t record_length, void* user_data)
{
    (void)sizeof(sock);
    (void)sizeof(query_id);
    (void)sizeof(name_length);
    (void)sizeof(ttl);

    MDNS *mdnsMgr = (MDNS *)user_data;
    char entrybuffer[256];

#ifdef DEBUG
    const char *entry_name, *record_name;
    switch (rtype) {
        case MDNS_RECORDTYPE_PTR:
            record_name = "PTR";
            break;
        case MDNS_RECORDTYPE_SRV:
            record_name = "SRV";
            break;
        case MDNS_RECORDTYPE_A:
            record_name = "A";
            break;
        case MDNS_RECORDTYPE_AAAA:
            record_name = "AAAA";
            break;
        default:
            record_name = "ANY";
    }

    switch (entry_type) {
        case MDNS_ENTRYTYPE_QUESTION:
            entry_name = "QUESTION";
            break;
        case MDNS_ENTRYTYPE_ANSWER:
            entry_name = "ANSWER";
            break;
        case MDNS_ENTRYTYPE_AUTHORITY:
            entry_name = "AUTHORITY";
            break;
        default:
            entry_name = "ADDITIONAL";
    }
#endif

    void *in_addr;
    char addrbuffer[INET6_ADDRSTRLEN] = {0};

    switch (from->sa_family) {
        case AF_INET: {
            struct sockaddr_in* sa = (struct sockaddr_in*) from;
            in_addr = &(sa->sin_addr);
            break;
        }
        case AF_INET6: {
            struct sockaddr_in6* sa = (struct sockaddr_in6*) from;
            in_addr = &(sa->sin6_addr);
            break;
        }
    }

    mdns_string_t fromaddrstr;
    fromaddrstr.str = inet_ntop(from->sa_family, in_addr, addrbuffer, (socklen_t)addrlen);
    if (fromaddrstr.str) {
        fromaddrstr.length = strnlen(fromaddrstr.str, sizeof(addrbuffer));
    } else {
        elog("mDNS: error parsing fromaddress: %s", strerror(errno));
        return 0;
    }

    dlog("mDNS: reply from %.*s entry=%s record=%s", MDNS_STRING_FORMAT(fromaddrstr), entry_name, record_name);

    if (entry_type == MDNS_ENTRYTYPE_ANSWER) {
        mdns_string_t record = mdns_record_parse_ptr(data, size, record_offset, record_length, entrybuffer, sizeof(Device::serial)-1);
        dlog("mDNS: ANSWER record=%.*s", MDNS_STRING_FORMAT(record));

        Device *dev = mdnsMgr->AddDevice(MDNS_STRING_ARGS(record));
        if (dev) {
            ilog("added new device with serial '%.*s'", MDNS_STRING_FORMAT(record));
            dev->handle = query_id;
            MDNS_STRING_LIMIT(fromaddrstr, sizeof(Device::address)-1);
            memcpy(dev->model, MDNS_STRING_ARGS(fromaddrstr));
            memcpy(dev->address, MDNS_STRING_ARGS(fromaddrstr));
        } else {
            elog("error adding device");
        }

        return 0;
    }


    if (entry_type != MDNS_ENTRYTYPE_ADDITIONAL) {
        return 0;
    }

    mdns_string_t entry = mdns_string_extract(data, size, &name_offset, entrybuffer, sizeof(Device::serial)-1);
    Device *dev = mdnsMgr->GetDevice(MDNS_STRING_ARGS(entry));
    if (dev == NULL) {
        elog("device '%.*s' not found", MDNS_STRING_FORMAT(entry));
        return 0;
    }
    // if (dev->handle != query_id) {
    //     dlog("device '%.*s' query_id mis-match", MDNS_STRING_FORMAT(entry));
    //     return 0;
    // }

    // ADDITIONAL section
    if (rtype == MDNS_RECORDTYPE_SRV) {
        char srvbuf[256];
        mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length, srvbuf, sizeof(srvbuf));
        (void) srv;
        // dlog("mDNS: SRV %.*s port=%d", MDNS_STRING_FORMAT(srv.name), srv.port);
        // Any way to also auto-discover port via USB (?) and remove the manual port input
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_TXT) {
        mdns_record_txt_t txtbuf[512];
        ssize_t parsed = mdns_record_parse_txt(data, size, record_offset, record_length, txtbuf, ARRAY_LEN(txtbuf));

        for (ssize_t t = 0; t < parsed; t++) {
            if (txtbuf[t].value.length == 0) {
                dlog("mDNS: TXT %.*s", MDNS_STRING_FORMAT(txtbuf[t].key));
                continue;
            }

            dlog("mDNS: TXT %.*s = %.*s", MDNS_STRING_FORMAT(txtbuf[t].key), MDNS_STRING_FORMAT(txtbuf[t].value));

            // DroidCam TXT records
            const char* name = "name";

            // name, aka device label. example result: 'Pixel 4a (WiFi)'
            if (strncmp(name, MDNS_STRING_ARGS(txtbuf[t].key)) == 0) {
                MDNS_STRING_LIMIT(txtbuf[t].value, sizeof(Device::model) - strlen(mdnsMgr->suffix) - 6 - 16);

                dlog("using model='%.*s' for '%.*s'", MDNS_STRING_FORMAT(txtbuf[t].value), MDNS_STRING_FORMAT(entry));
                snprintf(dev->model, sizeof(Device::model), "%.*s [%s] (%.*s)",
                    MDNS_STRING_FORMAT(txtbuf[t].value), mdnsMgr->suffix, MDNS_STRING_FORMAT(fromaddrstr));
            }
        }

        return 0;
    }

    // Use query_id to grab these instead of fromaddrstr for each device
    /*if (rtype == MDNS_RECORDTYPE_A) {
        struct sockaddr_in addr;
        mdns_record_parse_a(data, size, record_offset, record_length, &addr);
        return 0;
    }

    if (rtype == MDNS_RECORDTYPE_AAAA) {
        struct sockaddr_in6 addr;
        mdns_record_parse_aaaa(data, size, record_offset, record_length, &addr);
        return 0;
    }*/

    return 0;
}

static
int find_sockaddr(int network_mask) {
#ifdef __APPLE__
    struct ifaddrs* ifaddr = 0;
    struct ifaddrs* ifa = 0;

    if (getifaddrs(&ifaddr) < 0)
        return INVALID_SOCKET;

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* saddr = (struct sockaddr_in*)ifa->ifa_addr;
            if ((saddr->sin_addr.s_addr & 0xffff) == network_mask) {
                dlog("found ifaddr: %x (mask %x)", saddr->sin_addr.s_addr, network_mask);
                return mdns_socket_open_ipv4(saddr);
            }
        }
    }
#endif
    errno = ENXIO;
    return INVALID_SOCKET;
}

#define PARALLEL 3

void MDNS::DoReload(void) {
    const char* service_name = DROIDCAM_SERVICE_NAME;
    const mdns_record_type_t record = MDNS_RECORDTYPE_ANY;
    fd_set set;
    socket_t maxfd = 0;
    size_t capacity = 2048;
    void* buffer = malloc(capacity * PARALLEL);

    struct sockaddr* saddr = NULL;

    struct query {
        socket_t sock;
        void* buffer;
        int query_id;
    } socks[PARALLEL];

    FD_ZERO(&set);
    memset(&socks, 0, sizeof(struct query) * PARALLEL);

    if (bindIP && bindIP[0]) {
        dlog("mDNS: bindIP=%s", bindIP);
        saddr = net_sock_addr(bindIP);
    }

    for (int i = 0; i < PARALLEL; i++) {
        socket_t sock;

        if (networkPrefix)
        {
            sock = find_sockaddr(networkPrefix);
        }
        else if (saddr && saddr->sa_family == AF_INET)
        {
            sock = mdns_socket_open_ipv4((struct sockaddr_in*) saddr);
        }
        else if (saddr && saddr->sa_family == AF_INET6)
        {
            sock = mdns_socket_open_ipv6((struct sockaddr_in6*) saddr);
        }
        else
        {
            sock = mdns_socket_open_ipv4(NULL);
        }

        if (sock < 0) {
            elog("socket(): %s", strerror(errno));
            continue;
        }

        FD_SET(sock, &set); if (sock > maxfd) maxfd = sock;
        dlog("mDNS: query %s ANY via socket %d", service_name, sock);

        socks[i].sock = sock;
        socks[i].buffer = (char*)buffer + i * capacity;
        socks[i].query_id = mdns_query_send(sock, record, service_name, strlen(service_name),
            socks[i].buffer, capacity, sock);

        if (socks[i].query_id < 0) {
            elog("Failed to send mDNS query: %s\n", strerror(errno));
        }
    }

    if (maxfd)
    {
        const int NS_MS_FACTOR = 1000000;
        uint64_t time_end = 1750 + (os_gettime_ns() / NS_MS_FACTOR);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 150000;

        do {
            fd_set read_fds = set;
            int rc = select(maxfd+1, &read_fds, NULL, NULL, &timeout);
            if (rc == 0) continue;
            if (rc <  0) {
                WSAErrno();
                elog("mDNS: select failed (%d): %s", errno, strerror(errno));
                break;
            }

            for (int i = 0; i < PARALLEL; i++) {
                auto sock = socks[i].sock;
                auto buffer = socks[i].buffer;
                auto query_id = socks[i].query_id;
                if (query_id && FD_ISSET(sock, &read_fds)) {
                    void* user_data = this;
                    mdns_query_recv(sock, buffer, capacity, query_callback, user_data, query_id);
                }
            }
        } while ((os_gettime_ns() / NS_MS_FACTOR) < time_end);
    }

    free(buffer);
    for (int i = 0; i < PARALLEL; i++) {
        auto sock = socks[i].sock;
        if (sock > 0)
            mdns_socket_close(sock);
    }

    return;
}
