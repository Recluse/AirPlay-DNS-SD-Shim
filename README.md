# AirPlay dnssd shim (`dnssd.dll`)

A drop-in replacement for Apple's `dnssd.dll` on **Windows** — just enough of the
`dns_sd` API for [UxPlay](https://github.com/FDH2/UxPlay)-style AirPlay servers to
advertise `_airplay._tcp` / `_raop._tcp` over mDNS, backed by an **embedded mDNS
responder** ([mjansson/mdns](https://github.com/mjansson/mdns), public domain) — so
you don't need Apple's Bonjour service at all.

## Why

Apple's `mDNSResponder.exe` (Bonjour) **crashes** (`0xc0000409` / BEX64) on hosts
with many virtual NICs — VMware / VirtualBox / WireGuard — even with the genuine
Apple-signed build. When that happens, an app's `LoadLibraryA("dnssd.dll")` still
loads a `dnssd.dll` that can't talk to a working service, so the subsequent
`DNSServiceRegister` call returns `-65563 kDNSServiceErr_ServiceNotRunning`. This
shim sidesteps Bonjour entirely.

## What it provides

The 7 symbols UxPlay loads:
`DNSServiceRegister`, `DNSServiceRefDeallocate`, `TXTRecordCreate`,
`TXTRecordSetValue`, `TXTRecordGetLength`, `TXTRecordGetBytesPtr`,
`TXTRecordDeallocate`.

Plus one extra that is **not** part of `dns_sd`: `PopyachsaShimVersion()` returns
a static string naming this build and which of the two paths is live. See
"Which build is this?" below.

## Choosing the adapter (`interfaceIndex`)

`DNSServiceRegister`'s `interfaceIndex` is honoured: pass the Windows `IfIndex`
of an adapter and the embedded responder binds its mDNS socket to *that*
adapter's IPv4 and advertises that address — including a `169.254.x.x`
link-local one, which is the direct-cable case link-local mDNS exists for. Pass
`0` (or `kDNSServiceInterfaceIndexAny`) and it picks an address itself, as
before, preferring real Ethernet/Wi-Fi over VMware/VirtualBox/WireGuard/Hyper-V
adapters and skipping link-local. A **non-zero** index with no usable IPv4 —
adapter down, gone, IPv6-only, or its address still `Tentative`/`Duplicate` — is
refused with `-65540`
(`kDNSServiceErr_BadParam`; the shim's own macro for it is misnamed
`KDNSSERVICEERR_INVALID`) and a log line, rather than silently falling back to
the guess: a caller that named an adapter has pinned its sockets to it, and
advertising a different one announces the service where it isn't listening. In
Bonjour-proxy mode the index is passed straight to Apple's `dnssd.dll`.

Registration is refused the same way — `-65537` (`kDNSServiceErr_Unknown`) — when
the mDNS socket cannot be opened on that address or cannot be scoped to it
(`IP_MULTICAST_IF`, which on Windows and not the `bind` is what picks the
outgoing interface). The socket is therefore opened *before* `DNSServiceRegister`
returns, not on the responder thread: callers pass `callBack = NULL`, so a
failure found later has no way back to them, and "accepted an address, failed to
bind, reported success" is exactly the outcome the refusal exists to prevent.

Note the index is a lossy identifier: an adapter may carry several IPv4
addresses and the first usable one is used. dns_sd gives the caller no way to
name an address, so on a multi-address adapter the advertised A record can be a
sibling of the address the caller's sockets bound to.

## Build

From an MSYS2 **UCRT64** (or any MinGW-w64) shell:
```bash
gcc -O2 -Wall -shared -o dnssd.dll dnssd_shim.c -lws2_32 -liphlpapi
```
Single translation unit + the vendored `mdns.h` header — no external deps.

## Which build is this?

The version is compiled in, and there are three ways to read it back. Each also
says which of the two paths is live.

**1. From the host, at runtime — the one that works in an application log.**
Call the extra export:

```c
typedef const char * (__stdcall *shim_version_t)(void);
shim_version_t v = (shim_version_t) GetProcAddress(dll, "PopyachsaShimVersion");
/* "dnssd shim 1.1.0 (embedded mDNS)" | "… (Apple Bonjour proxy)" | NULL */
```

Apple's real `dnssd.dll` does not export this, so a `NULL` is itself the answer
to "whose `dnssd.dll` got loaded?". The returned string is static — do not free
it. Valid from load onward: the proxy-vs-embedded decision is made in `DllMain`.

**2. From the shim's own stderr — only if the host captures it.** The shim
prints `[dnssd_shim] dnssd shim 1.1.0 (embedded mDNS)` on its **first
registration** (not at DLL load: a host normally redirects its stdio after
startup, so a line printed from `DllMain` goes nowhere). That reaches you from a
console build such as `uxplay.exe` run in a shell. It does **not** reach a GUI
host: a windows-subsystem binary has no console and captures nothing on stderr,
so the line is lost no matter when it is printed. Measured, not assumed — a
registration demonstrably happened, the engine logged it, and no `[dnssd_shim]`
line appeared anywhere. That measurement is why option 1 exists.

**3. From the file on disk**, with no process running:

```bash
strings dnssd.dll | grep "dnssd shim"
```

Bump `DNSSD_SHIM_VERSION` in `dnssd_shim.c` for every published build and tag the
commit to match. Builds before 1.0.0 carried no version at all, and the cost was
real: a debug variant of this file — one that appended every mDNS registration to
a hardcoded log path — was shipped to users, and identifying it after the fact
took a source diff and `strings` on the binary.

## Install — the load-order gotcha (important)

`C:\Windows\System32\dnssd.dll` (Apple Bonjour) is found by the Win32 DLL search
order **before** `PATH`. So this shim **must sit in the same directory as the host
`.exe`** (the application directory is searched before System32). Drop `dnssd.dll`
next to `uxplay.exe` / your AirPlay server's executable — not in System32, not on
PATH.

## Licence

**MIT** (the shim) — see [LICENSE](LICENSE). The vendored `mdns.h` is **public
domain** (Mattias Jansson). Used by
[Popyachsa AirPlay](https://github.com/Recluse/Popyachsa-AirPlay).
