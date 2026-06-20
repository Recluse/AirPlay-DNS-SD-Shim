/*
 * dnssd_shim.c — drop-in replacement for Apple's dnssd.dll, just enough of the
 * dns_sd API for UxPlay's mDNS advertising of _airplay._tcp / _raop._tcp.
 *
 * Why: Apple's mDNSResponder.exe 3.1.0.1 crashes (0xc0000409 / BEX64) on hosts
 * with many virtual NICs (VMware/VirtualBox/WireGuard) — confirmed even with
 * the genuine Apple-signed build from winget. So UxPlay's `LoadLibraryA("dnssd.dll")`
 * gets nothing usable. This shim, placed in uxplay.exe's own directory, wins
 * Windows DLL search order over System32 and provides an embedded mDNS responder
 * (built on mjansson/mdns, public domain) instead — like 5KPlayer/AirServer do.
 *
 * Exports the 7 symbols UxPlay loads:
 *   DNSServiceRegister, DNSServiceRefDeallocate,
 *   TXTRecordCreate, TXTRecordSetValue, TXTRecordGetLength, TXTRecordGetBytesPtr,
 *   TXTRecordDeallocate.
 *
 * Build: from MSYS2 UCRT64 —
 *   gcc -O2 -Wall -shared -o dnssd.dll dnssd_shim.c -lws2_32 -liphlpapi
 * Then drop dnssd.dll next to uxplay.exe.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <winsvc.h>      /* Service Control Manager — Bonjour-detect */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdns.h"

#define DNSSD_EXPORT __declspec(dllexport)
#define DNSSD_API    __stdcall   /* no-op on x64; expected by UxPlay's typedef */

#define KDNSSERVICEERR_NOERROR        0
#define KDNSSERVICEERR_UNKNOWN     (-65537)
#define KDNSSERVICEERR_NOMEMORY    (-65539)
#define KDNSSERVICEERR_INVALID     (-65540)

typedef int32_t  DNSServiceErrorType;
typedef uint32_t DNSServiceFlags;

/* Forward declarations of opaque types — UxPlay only passes pointers. */
typedef struct _DNSServiceRef_t *DNSServiceRef;

/* Apple's TXTRecordRef is a 16-byte opaque blob the caller allocates on the
 * stack. We map it onto our own structure (must fit in 16 bytes). */
typedef union _TXTRecordRef_t {
    char PrivateData[16];
    struct {
        uint8_t *buf;       /* 8 bytes */
        uint16_t len;       /* 2 */
        uint16_t cap;       /* 2 */
        uint32_t flags;     /* 4 — bit 0 = "we malloc'd buf" */
    } impl;
} TXTRecordRef;

/* DNSServiceRegisterReply: UxPlay passes NULL for the callback in both registers
 * (lib/dnssd.c lines 348, 401), so we never need to invoke it — keep the
 * typedef for ABI compatibility only. */
typedef void (DNSSD_API *DNSServiceRegisterReply)(
    DNSServiceRef sdRef, DNSServiceFlags flags, DNSServiceErrorType errorCode,
    const char *name, const char *regtype, const char *domain, void *context);

/* ------------------------------------------------------------------------- */
/* Bonjour-coexistence proxy: if a working Apple Bonjour Service is installed on
 * the host, transparently forward DNSServiceRegister/Deallocate to System32's
 * dnssd.dll instead of running our own embedded mDNS responder. This lets us
 * coexist on machines where Bonjour actually works without taking it over —
 * but still fall back to embedded mDNS on machines where Bonjour is missing
 * or its service won't start (our original use case). */
static HMODULE g_apple_dll = NULL;
static DNSServiceErrorType (DNSSD_API *p_DNSServiceRegister)(
    DNSServiceRef *sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    const char *name, const char *regtype, const char *domain,
    const char *host, uint16_t port_net,
    uint16_t txtLen, const void *txtRecord,
    DNSServiceRegisterReply callBack, void *context) = NULL;
static void (DNSSD_API *p_DNSServiceRefDeallocate)(DNSServiceRef sdRef) = NULL;

/* (LOG macro is defined further down — declared again here as forward-decl-friendly.) */
#define DNSSD_SHIM_LOG(fmt, ...) do { fprintf(stderr, "[dnssd_shim] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

static BOOL is_bonjour_service_running(void) {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;
    SC_HANDLE svc = OpenServiceA(scm, "Bonjour Service", SERVICE_QUERY_STATUS);
    BOOL running = FALSE;
    if (svc) {
        SERVICE_STATUS_PROCESS status = {0};
        DWORD bytes_needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 (LPBYTE)&status, sizeof(status), &bytes_needed)) {
            running = (status.dwCurrentState == SERVICE_RUNNING);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

static void try_load_apple_bonjour(void) {
    char system_dir[MAX_PATH] = {0};
    if (!GetSystemDirectoryA(system_dir, sizeof(system_dir))) {
        DNSSD_SHIM_LOG("GetSystemDirectoryA failed (%lu); using embedded mDNS", GetLastError());
        return;
    }
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s\\dnssd.dll", system_dir);

    /* Avoid self-load loop: if System32\dnssd.dll happens to be OUR shim
     * (because somebody installed us there), skip. Compare by canonical path. */
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)&try_load_apple_bonjour, &self);
    char self_path[MAX_PATH * 2] = {0};
    if (self) GetModuleFileNameA(self, self_path, sizeof(self_path));
    if (self_path[0] && _stricmp(self_path, path) == 0) {
        DNSSD_SHIM_LOG("self is installed at %s; using embedded mDNS only", path);
        return;
    }

    if (!is_bonjour_service_running()) {
        DNSSD_SHIM_LOG("Bonjour Service not running; using embedded mDNS");
        return;
    }

    HMODULE m = LoadLibraryA(path);
    if (!m) {
        DNSSD_SHIM_LOG("LoadLibrary %s failed (%lu); using embedded mDNS", path, GetLastError());
        return;
    }

    p_DNSServiceRegister      = (void*)GetProcAddress(m, "DNSServiceRegister");
    p_DNSServiceRefDeallocate = (void*)GetProcAddress(m, "DNSServiceRefDeallocate");

    if (!p_DNSServiceRegister || !p_DNSServiceRefDeallocate) {
        DNSSD_SHIM_LOG("Apple dnssd.dll missing required syms; using embedded mDNS");
        FreeLibrary(m);
        p_DNSServiceRegister = NULL;
        p_DNSServiceRefDeallocate = NULL;
        return;
    }

    g_apple_dll = m;
    DNSSD_SHIM_LOG("Bonjour proxy mode ACTIVE: forwarding to %s", path);
}

/* ------------------------------------------------------------------------- */
/* TXT-record builder — Apple's API stores entries as a single byte buffer of
 *   <len><key>=<value><len><key>=<value>…
 * UxPlay builds the buffer here and then hands the bytes back to us via
 * DNSServiceRegister(txtRecord, txtLen, …). */

DNSSD_EXPORT void DNSSD_API
TXTRecordCreate(TXTRecordRef *txt, uint16_t bufferLen, void *buffer) {
    if (!txt) return;
    if (buffer && bufferLen) {
        txt->impl.buf = (uint8_t *)buffer;
        txt->impl.cap = bufferLen;
    } else {
        txt->impl.buf = NULL;
        txt->impl.cap = 0;
    }
    txt->impl.len = 0;
    txt->impl.flags = 0;
}

static int txt_grow(TXTRecordRef *txt, uint16_t need) {
    if (need <= txt->impl.cap) return 0;
    uint32_t newcap = txt->impl.cap ? txt->impl.cap : 256;
    while (newcap < need) newcap *= 2;
    if (newcap > 0xFFFF) newcap = 0xFFFF;
    if (newcap < need) return -1;
    uint8_t *nb;
    if (txt->impl.flags & 1u) {
        nb = (uint8_t *)realloc(txt->impl.buf, newcap);
    } else {
        nb = (uint8_t *)malloc(newcap);
        if (nb && txt->impl.len) memcpy(nb, txt->impl.buf, txt->impl.len);
    }
    if (!nb) return -1;
    txt->impl.buf = nb;
    txt->impl.cap = (uint16_t)newcap;
    txt->impl.flags |= 1u;
    return 0;
}

DNSSD_EXPORT DNSServiceErrorType DNSSD_API
TXTRecordSetValue(TXTRecordRef *txt, const char *key, uint8_t valueSize, const void *value) {
    if (!txt || !key) return KDNSSERVICEERR_INVALID;
    size_t klen = strlen(key);
    if (klen == 0 || klen > 254) return KDNSSERVICEERR_INVALID;
    /* entry layout: [1 byte len][key][=][value]  — len is key+1+valueSize, max 255 */
    size_t entry_payload = klen + (value && valueSize ? 1 + (size_t)valueSize : 0);
    if (entry_payload > 255) return KDNSSERVICEERR_INVALID;
    size_t need = (size_t)txt->impl.len + 1 + entry_payload;
    if (need > 0xFFFF) return KDNSSERVICEERR_NOMEMORY;
    if (txt_grow(txt, (uint16_t)need) < 0) return KDNSSERVICEERR_NOMEMORY;
    uint8_t *p = txt->impl.buf + txt->impl.len;
    *p++ = (uint8_t)entry_payload;
    memcpy(p, key, klen); p += klen;
    if (value && valueSize) {
        *p++ = '=';
        memcpy(p, value, valueSize);
    }
    txt->impl.len = (uint16_t)need;
    return KDNSSERVICEERR_NOERROR;
}

DNSSD_EXPORT uint16_t DNSSD_API
TXTRecordGetLength(const TXTRecordRef *txt) {
    return txt ? txt->impl.len : 0;
}

DNSSD_EXPORT const void * DNSSD_API
TXTRecordGetBytesPtr(const TXTRecordRef *txt) {
    return (txt && txt->impl.len) ? txt->impl.buf : NULL;
}

DNSSD_EXPORT void DNSSD_API
TXTRecordDeallocate(TXTRecordRef *txt) {
    if (!txt) return;
    if ((txt->impl.flags & 1u) && txt->impl.buf) free(txt->impl.buf);
    memset(txt, 0, sizeof(*txt));
}

/* ------------------------------------------------------------------------- */
/* mDNS responder — one per registered service (UxPlay calls DNSServiceRegister
 * twice, once for _raop._tcp, once for _airplay._tcp). */

#define LOG(fmt, ...) \
    do { fprintf(stderr, "[dnssd_shim] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

/* Parsed TXT entry pointing into the caller-owned blob (no copy). */
typedef struct {
    mdns_string_t key;
    mdns_string_t value;
    int has_value;
} txt_entry_t;

struct _DNSServiceRef_t {
    HANDLE thread;
    volatile LONG stop;

    char service_name[64];         /* e.g. "12AB34CD56EF@Windows-PC"  */
    char service_type[64];         /* e.g. "_raop._tcp.local."         */
    char service_instance[256];    /* "<name>.<type>"                  */
    char hostname[128];            /* "Windows-PC.local."              */
    uint16_t port;                 /* host byte order                  */

    struct sockaddr_in ipv4_addr;  /* A record value (first usable IPv4) */

    uint8_t *txt_blob;             /* malloc'd copy of UxPlay's TXT bytes */
    uint16_t txt_len;
    txt_entry_t *txt_entries;
    size_t txt_count;
};

static int g_wsa_initialized = 0;

static void ensure_wsa(void) {
    if (g_wsa_initialized) return;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    g_wsa_initialized = 1;
}

/* Pick the most useful IPv4 unicast address: prefer adapters that aren't
 * VMware/VirtualBox/WireGuard/Hyper-V/tunnels. Returns 1 on success. */
static int pick_local_ipv4(struct sockaddr_in *out) {
    IP_ADAPTER_ADDRESSES *aa = NULL;
    ULONG sz = 16 * 1024;
    int found = 0;
    struct sockaddr_in best = {0};
    int best_score = -1;

    for (int retry = 0; retry < 4; ++retry) {
        free(aa);
        aa = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!aa) return 0;
        DWORD r = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, aa, &sz);
        if (r == NO_ERROR) break;
        if (r != ERROR_BUFFER_OVERFLOW) { free(aa); return 0; }
        sz *= 2;
    }

    for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        int score = 0;
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD) score = 100;
        else if (a->IfType == IF_TYPE_IEEE80211) score = 90;
        else score = 10;
        /* downgrade obviously-virtual adapters by name */
        wchar_t *desc = a->Description ? a->Description : L"";
        if (wcsstr(desc, L"VMware") || wcsstr(desc, L"VirtualBox") ||
            wcsstr(desc, L"WireGuard") || wcsstr(desc, L"Hyper-V") ||
            wcsstr(desc, L"Tunnel") || wcsstr(desc, L"Loopback") ||
            wcsstr(desc, L"TAP") || wcsstr(desc, L"Pseudo"))
            score -= 80;

        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *si = (struct sockaddr_in *)u->Address.lpSockaddr;
            uint8_t *b = (uint8_t *)&si->sin_addr;
            if (b[0] == 127) continue;
            if (b[0] == 169 && b[1] == 254) continue;       /* link-local */
            if (score > best_score) {
                best = *si;
                best_score = score;
                found = 1;
            }
        }
    }
    free(aa);
    if (found) *out = best;
    return found;
}

/* Parse the Apple-format TXT blob (sequence of <len><key>=<value>) into an
 * array of txt_entry_t pointing into the blob (no copies). */
static int parse_txt_blob(DNSServiceRef sd) {
    sd->txt_entries = NULL;
    sd->txt_count = 0;
    if (!sd->txt_blob || sd->txt_len == 0) return 0;

    size_t cap = 16;
    sd->txt_entries = (txt_entry_t *)malloc(cap * sizeof(txt_entry_t));
    if (!sd->txt_entries) return -1;

    size_t i = 0;
    while (i < sd->txt_len) {
        uint8_t entry_len = sd->txt_blob[i++];
        if (entry_len == 0) continue;
        if (i + entry_len > sd->txt_len) break;          /* malformed — stop */

        const char *e = (const char *)(sd->txt_blob + i);
        size_t kv_total = entry_len;
        const char *eq = (const char *)memchr(e, '=', kv_total);
        size_t klen, vlen;
        const char *kp, *vp;
        if (eq) {
            klen = (size_t)(eq - e);
            kp = e;
            vp = eq + 1;
            vlen = kv_total - klen - 1;
        } else {
            klen = kv_total;
            kp = e;
            vp = NULL;
            vlen = 0;
        }

        if (sd->txt_count == cap) {
            cap *= 2;
            txt_entry_t *t = (txt_entry_t *)realloc(sd->txt_entries, cap * sizeof(txt_entry_t));
            if (!t) return -1;
            sd->txt_entries = t;
        }
        sd->txt_entries[sd->txt_count].key.str = kp;
        sd->txt_entries[sd->txt_count].key.length = klen;
        sd->txt_entries[sd->txt_count].value.str = vp ? vp : "";
        sd->txt_entries[sd->txt_count].value.length = vlen;
        sd->txt_entries[sd->txt_count].has_value = eq ? 1 : 0;
        sd->txt_count++;
        i += entry_len;
    }
    return 0;
}

/* Helper: build the mdns_record_t array for all our TXT entries. */
static size_t build_txt_records(const DNSServiceRef sd, mdns_string_t instance,
                                mdns_record_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < sd->txt_count && n < cap; ++i) {
        out[n].name = instance;
        out[n].type = MDNS_RECORDTYPE_TXT;
        out[n].data.txt.key = sd->txt_entries[i].key;
        out[n].data.txt.value = sd->txt_entries[i].value;
        out[n].rclass = 0;
        out[n].ttl = 0;
        n++;
    }
    return n;
}

/* Build the static-ish set of additionals (SRV + A + every TXT entry). */
static size_t build_additionals(const DNSServiceRef sd,
                                mdns_record_t srv, mdns_record_t a,
                                mdns_string_t instance,
                                mdns_record_t *out, size_t cap) {
    size_t n = 0;
    if (n < cap) out[n++] = srv;
    if (sd->ipv4_addr.sin_family == AF_INET && n < cap) out[n++] = a;
    n += build_txt_records(sd, instance, out + n, cap - n);
    return n;
}

/* mdns query callback: answer relevant queries. user_data = DNSServiceRef. */
static int answer_query_cb(int sock, const struct sockaddr *from, size_t addrlen,
                           mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                           uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                           size_t name_offset, size_t name_length, size_t record_offset,
                           size_t record_length, void *user_data) {
    (void)ttl; (void)name_length; (void)record_offset; (void)record_length;
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;

    DNSServiceRef sd = (DNSServiceRef)user_data;

    char namebuf[256];
    size_t off = name_offset;
    mdns_string_t name = mdns_string_extract(data, size, &off, namebuf, sizeof(namebuf));

    mdns_string_t service = {sd->service_type, strlen(sd->service_type)};
    mdns_string_t instance = {sd->service_instance, strlen(sd->service_instance)};
    mdns_string_t host = {sd->hostname, strlen(sd->hostname)};
    static const char dns_sd[] = "_services._dns-sd._udp.local.";
    int unicast = (rclass & MDNS_UNICAST_RESPONSE) ? 1 : 0;

    uint8_t sendbuf[2048];

    mdns_record_t rec_ptr = { .name = service, .type = MDNS_RECORDTYPE_PTR,
        .data.ptr.name = instance, .rclass = 0, .ttl = 0 };
    mdns_record_t rec_srv = { .name = instance, .type = MDNS_RECORDTYPE_SRV,
        .data.srv = { .name = host, .port = sd->port, .priority = 0, .weight = 0 },
        .rclass = 0, .ttl = 0 };
    mdns_record_t rec_a = { .name = host, .type = MDNS_RECORDTYPE_A,
        .data.a.addr = sd->ipv4_addr, .rclass = 0, .ttl = 0 };

    mdns_record_t additionals[64];
    size_t n_add;

    /* Case 1: meta-query "_services._dns-sd._udp.local." PTR — advertise our type. */
    if (name.length == sizeof(dns_sd) - 1 &&
        strncmp(name.str, dns_sd, sizeof(dns_sd) - 1) == 0 &&
        (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
        mdns_record_t answer = { .name = name, .type = MDNS_RECORDTYPE_PTR,
            .data.ptr.name = service, .rclass = 0, .ttl = 0 };
        if (unicast)
            mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                                      query_id, rtype, name.str, name.length, answer,
                                      NULL, 0, NULL, 0);
        else
            mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf), answer,
                                        NULL, 0, NULL, 0);
        return 0;
    }

    /* Case 2: PTR on our service type → instance, + SRV/A/TXT additionals. */
    if (name.length == service.length &&
        strncmp(name.str, service.str, name.length) == 0 &&
        (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY)) {
        n_add = build_additionals(sd, rec_srv, rec_a, instance, additionals,
                                  sizeof(additionals) / sizeof(additionals[0]));
        if (unicast)
            mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                                      query_id, rtype, name.str, name.length, rec_ptr,
                                      NULL, 0, additionals, n_add);
        else
            mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf), rec_ptr,
                                        NULL, 0, additionals, n_add);
        return 0;
    }

    /* Case 3: SRV/ANY on our service instance → SRV + A + TXT. */
    if (name.length == instance.length &&
        strncmp(name.str, instance.str, name.length) == 0 &&
        (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_TXT ||
         rtype == MDNS_RECORDTYPE_ANY)) {
        n_add = 0;
        if (sd->ipv4_addr.sin_family == AF_INET)
            additionals[n_add++] = rec_a;
        if (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY) {
            n_add += build_txt_records(sd, instance, additionals + n_add,
                                       sizeof(additionals) / sizeof(additionals[0]) - n_add);
            mdns_record_t ans = (rtype == MDNS_RECORDTYPE_SRV) ? rec_srv :
                                (sd->txt_count ? (mdns_record_t){
                                    .name = instance, .type = MDNS_RECORDTYPE_TXT,
                                    .data.txt.key = sd->txt_entries[0].key,
                                    .data.txt.value = sd->txt_entries[0].value,
                                    .rclass = 0, .ttl = 0 } : rec_srv);
            if (unicast)
                mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                                          query_id, rtype, name.str, name.length, ans,
                                          NULL, 0, additionals, n_add);
            else
                mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf), ans,
                                            NULL, 0, additionals, n_add);
        } else {
            /* TXT query specifically: answer first TXT, others as additionals. */
            if (sd->txt_count) {
                mdns_record_t ans = { .name = instance, .type = MDNS_RECORDTYPE_TXT,
                    .data.txt.key = sd->txt_entries[0].key,
                    .data.txt.value = sd->txt_entries[0].value,
                    .rclass = 0, .ttl = 0 };
                if (sd->ipv4_addr.sin_family == AF_INET) {
                    additionals[0] = rec_a; n_add = 1;
                } else { n_add = 0; }
                size_t more = (sd->txt_count > 1) ? build_txt_records(sd, instance,
                    additionals + n_add, sizeof(additionals)/sizeof(additionals[0]) - n_add) - 1 : 0;
                /* skip first since it's the answer */
                if (sd->txt_count > 1) {
                    for (size_t i = 1; i < sd->txt_count &&
                         n_add + i - 1 < sizeof(additionals)/sizeof(additionals[0]); ++i) {
                        additionals[n_add + i - 1].name = instance;
                        additionals[n_add + i - 1].type = MDNS_RECORDTYPE_TXT;
                        additionals[n_add + i - 1].data.txt.key = sd->txt_entries[i].key;
                        additionals[n_add + i - 1].data.txt.value = sd->txt_entries[i].value;
                        additionals[n_add + i - 1].rclass = 0;
                        additionals[n_add + i - 1].ttl = 0;
                    }
                    n_add += sd->txt_count - 1;
                }
                (void)more;
                if (unicast)
                    mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                                              query_id, rtype, name.str, name.length, ans,
                                              NULL, 0, additionals, n_add);
                else
                    mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf), ans,
                                                NULL, 0, additionals, n_add);
            }
        }
        return 0;
    }

    /* Case 4: A/ANY on our hostname → A. */
    if (name.length == host.length &&
        strncmp(name.str, host.str, name.length) == 0 &&
        (rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY) &&
        sd->ipv4_addr.sin_family == AF_INET) {
        if (unicast)
            mdns_query_answer_unicast(sock, from, addrlen, sendbuf, sizeof(sendbuf),
                                      query_id, rtype, name.str, name.length, rec_a,
                                      NULL, 0, NULL, 0);
        else
            mdns_query_answer_multicast(sock, sendbuf, sizeof(sendbuf), rec_a,
                                        NULL, 0, NULL, 0);
        return 0;
    }
    return 0;
}

/* Conflict-probe callback (RFC 6762 §8.1): flags a conflict if any *answer*
 * on the wire asserts our exact instance name from a different host. */
struct probe_ctx {
    const char *instance;
    uint32_t self_ip;          /* network byte order */
    volatile int conflict;
};

static int probe_cb(int sock, const struct sockaddr *from, size_t addrlen,
                    mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                    uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                    size_t name_offset, size_t name_length, size_t record_offset,
                    size_t record_length, void *user_data) {
    (void)sock; (void)addrlen; (void)query_id; (void)rtype; (void)rclass;
    (void)ttl; (void)name_length; (void)record_offset; (void)record_length;
    if (entry == MDNS_ENTRYTYPE_QUESTION) return 0;   /* ignore questions */
    struct probe_ctx *p = (struct probe_ctx *)user_data;
    char namebuf[256];
    size_t off = name_offset;
    mdns_string_t name = mdns_string_extract(data, size, &off, namebuf, sizeof(namebuf));
    size_t ilen = strlen(p->instance);
    if (name.length == ilen && strncmp(name.str, p->instance, ilen) == 0) {
        const struct sockaddr_in *f = (const struct sockaddr_in *)from;
        if (from && f->sin_family == AF_INET && f->sin_addr.s_addr != p->self_ip)
            p->conflict = 1;   /* a different host already owns this name */
    }
    return 0;
}

/* Service responder thread: one INADDR_ANY UDP socket on 5353. */
static DWORD WINAPI service_thread(LPVOID arg) {
    DNSServiceRef sd = (DNSServiceRef)arg;

    /* Bind to the chosen LAN IP (not INADDR_ANY): mjansson then sets
     * IP_MULTICAST_IF to that interface, so OUTGOING multicast goes out via the
     * LAN NIC instead of being grabbed by WireGuard / a virtual interface. */
    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(MDNS_PORT);
    if (sd->ipv4_addr.sin_family == AF_INET)
        bind_addr.sin_addr = sd->ipv4_addr.sin_addr;
    else
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    int sock = mdns_socket_open_ipv4(&bind_addr);
    if (sock < 0) {
        LOG("failed to open mDNS socket: WSA=%d", WSAGetLastError());
        return 1;
    }
    {
        uint8_t *b = (uint8_t *)&bind_addr.sin_addr;
        LOG("listening on %u.%u.%u.%u:5353 for service %s (instance %s, port %u)",
            b[0], b[1], b[2], b[3],
            sd->service_type, sd->service_instance, (unsigned)sd->port);
    }

    /* Probe for name conflicts before announcing. If another device on the LAN
     * already advertises our instance name, append " (2)", " (3)" … (like
     * Bonjour) so two receivers sharing a device name don't collide. _raop
     * carries a MAC prefix so it rarely clashes; _airplay (bare name) can. */
    {
        char base_name[64];
        snprintf(base_name, sizeof(base_name), "%s", sd->service_name);
        uint8_t pbuf[2048];
        /* Random start jitter (0-400 ms), seeded from our IP + uptime, so two
         * receivers that boot simultaneously don't probe in lockstep: the one
         * that finishes first announces, and the other's probe then sees it. */
        DWORD jitter = (GetTickCount() ^ ntohl(sd->ipv4_addr.sin_addr.s_addr)) % 400;
        Sleep(jitter);
        for (int attempt = 2; attempt <= 9 && !InterlockedCompareExchange(&sd->stop, 0, 0); ++attempt) {
            struct probe_ctx pc = { sd->service_instance, sd->ipv4_addr.sin_addr.s_addr, 0 };
            /* 3 probes ~250 ms apart (RFC 6762 §8.1). */
            for (int i = 0; i < 3 && !pc.conflict; ++i) {
                mdns_query_send(sock, MDNS_RECORDTYPE_ANY, sd->service_instance,
                                strlen(sd->service_instance), pbuf, sizeof(pbuf), 0);
                for (int t = 0; t < 250 && !pc.conflict; t += 50) {
                    fd_set rf; FD_ZERO(&rf); FD_SET((SOCKET)sock, &rf);
                    struct timeval tv = { 0, 50000 };  /* 50 ms */
                    if (select(0, &rf, NULL, NULL, &tv) > 0)
                        mdns_query_recv(sock, pbuf, sizeof(pbuf), probe_cb, &pc, 0);
                }
            }
            if (!pc.conflict) break;                  /* name is free */
            LOG("instance '%s' already on the LAN — renaming to avoid conflict",
                sd->service_instance);
            snprintf(sd->service_name, sizeof(sd->service_name), "%s (%d)", base_name, attempt);
            snprintf(sd->service_instance, sizeof(sd->service_instance), "%s.%s",
                     sd->service_name, sd->service_type);
        }
    }

    mdns_string_t service  = {sd->service_type, strlen(sd->service_type)};
    mdns_string_t instance = {sd->service_instance, strlen(sd->service_instance)};
    mdns_string_t host     = {sd->hostname, strlen(sd->hostname)};
    mdns_record_t rec_ptr = { .name = service, .type = MDNS_RECORDTYPE_PTR,
        .data.ptr.name = instance, .rclass = 0, .ttl = 0 };
    mdns_record_t rec_srv = { .name = instance, .type = MDNS_RECORDTYPE_SRV,
        .data.srv = { .name = host, .port = sd->port, .priority = 0, .weight = 0 },
        .rclass = 0, .ttl = 0 };
    mdns_record_t rec_a = { .name = host, .type = MDNS_RECORDTYPE_A,
        .data.a.addr = sd->ipv4_addr, .rclass = 0, .ttl = 0 };

    /* Initial announce: PTR + SRV/A/TXT as additionals. RFC 6762 §8.3 says send
     * 2-3 announces ~250ms apart so neighbours invalidate any cached older name. */
    uint8_t sendbuf[2048];
    mdns_record_t additionals[64];
    size_t n_add = build_additionals(sd, rec_srv, rec_a, instance, additionals,
                                     sizeof(additionals) / sizeof(additionals[0]));
    for (int i = 0; i < 3; ++i) {
        mdns_announce_multicast(sock, sendbuf, sizeof(sendbuf), rec_ptr,
                                NULL, 0, additionals, n_add);
        if (i < 2) Sleep(250);
    }

    uint8_t recvbuf[2048];
    while (!InterlockedCompareExchange(&sd->stop, 0, 0)) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET((SOCKET)sock, &rfds);
        struct timeval tv = { 0, 200000 };  /* 200 ms */
        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET((SOCKET)sock, &rfds)) {
            mdns_socket_listen(sock, recvbuf, sizeof(recvbuf), answer_query_cb, sd);
        } else if (r < 0) {
            LOG("select error: WSA=%d", WSAGetLastError());
            break;
        }
    }

    /* Goodbye. */
    n_add = build_additionals(sd, rec_srv, rec_a, instance, additionals,
                              sizeof(additionals) / sizeof(additionals[0]));
    mdns_goodbye_multicast(sock, sendbuf, sizeof(sendbuf), rec_ptr,
                           NULL, 0, additionals, n_add);
    mdns_socket_close(sock);
    return 0;
}

/* Determine our mDNS hostname: gethostname() + ".local." Falls back to "host". */
static void compute_hostname(char *out, size_t cap) {
    char name[64] = {0};
    if (gethostname(name, sizeof(name) - 1) != 0) strcpy(name, "host");
    /* sanitize: replace anything non-[A-Za-z0-9-] with '-' so it's a valid label */
    for (char *p = name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-')) *p = '-';
    snprintf(out, cap, "%s.local.", name);
}

/* ------------------------------------------------------------------------- */
/* The two exported lifecycle functions UxPlay calls dynamically. */

DNSSD_EXPORT DNSServiceErrorType DNSSD_API
DNSServiceRegister(DNSServiceRef *sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
                   const char *name, const char *regtype, const char *domain,
                   const char *host, uint16_t port_net,
                   uint16_t txtLen, const void *txtRecord,
                   DNSServiceRegisterReply callBack, void *context) {
    /* Bonjour proxy: if Apple Bonjour Service is alive on this host, forward
     * directly to its dnssd.dll. UxPlay never knows the difference. */
    if (p_DNSServiceRegister) {
        DNSSD_SHIM_LOG("DNSServiceRegister %s -> Apple Bonjour proxy", regtype ? regtype : "?");
        return p_DNSServiceRegister(sdRef, flags, interfaceIndex, name, regtype, domain,
                                    host, port_net, txtLen, txtRecord, callBack, context);
    }
    (void)flags; (void)interfaceIndex; (void)domain; (void)host;
    (void)callBack; (void)context;
    if (!sdRef || !name || !regtype) return KDNSSERVICEERR_INVALID;
    ensure_wsa();

    DNSServiceRef sd = (DNSServiceRef)calloc(1, sizeof(*sd));
    if (!sd) return KDNSSERVICEERR_NOMEMORY;

    /* Build "<regtype>.local." form (UxPlay passes "_raop._tcp" / "_airplay._tcp"). */
    snprintf(sd->service_name, sizeof(sd->service_name), "%s", name);
    if (strstr(regtype, ".local."))
        snprintf(sd->service_type, sizeof(sd->service_type), "%s", regtype);
    else
        snprintf(sd->service_type, sizeof(sd->service_type), "%s.local.", regtype);
    snprintf(sd->service_instance, sizeof(sd->service_instance), "%s.%s",
             sd->service_name, sd->service_type);

    compute_hostname(sd->hostname, sizeof(sd->hostname));
    sd->port = ntohs(port_net);

    pick_local_ipv4(&sd->ipv4_addr);

    if (txtLen && txtRecord) {
        sd->txt_blob = (uint8_t *)malloc(txtLen);
        if (!sd->txt_blob) { free(sd); return KDNSSERVICEERR_NOMEMORY; }
        memcpy(sd->txt_blob, txtRecord, txtLen);
        sd->txt_len = txtLen;
        parse_txt_blob(sd);
    }

    sd->thread = CreateThread(NULL, 0, service_thread, sd, 0, NULL);
    if (!sd->thread) {
        free(sd->txt_blob); free(sd->txt_entries); free(sd);
        return KDNSSERVICEERR_UNKNOWN;
    }

    LOG("registered %s on port %u (instance %s, host %s, txt_entries=%zu)",
        sd->service_type, (unsigned)sd->port, sd->service_instance,
        sd->hostname, sd->txt_count);

    *sdRef = sd;
    return KDNSSERVICEERR_NOERROR;
}

DNSSD_EXPORT void DNSSD_API
DNSServiceRefDeallocate(DNSServiceRef sdRef) {
    if (!sdRef) return;
    /* Bonjour proxy: the sdRef came from Apple's allocator if proxy mode
     * was active for the corresponding Register call. Forward to its deallocator
     * to avoid leaks / wrong-allocator crashes. */
    if (g_apple_dll && p_DNSServiceRefDeallocate) {
        p_DNSServiceRefDeallocate(sdRef);
        return;
    }
    InterlockedExchange(&sdRef->stop, 1);
    if (sdRef->thread) {
        WaitForSingleObject(sdRef->thread, 3000);
        CloseHandle(sdRef->thread);
    }
    free(sdRef->txt_entries);
    free(sdRef->txt_blob);
    free(sdRef);
}

/* DllMain — probe for working Apple Bonjour at load time; fall back to embedded. */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        try_load_apple_bonjour();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_apple_dll) {
            FreeLibrary(g_apple_dll);
            g_apple_dll = NULL;
        }
    }
    return TRUE;
}
