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

## Choosing the adapter (`interfaceIndex`)

`DNSServiceRegister`'s `interfaceIndex` is honoured: pass the Windows `IfIndex`
of an adapter and the embedded responder binds its mDNS socket to *that*
adapter's IPv4 and advertises that address — including a `169.254.x.x`
link-local one, which is the direct-cable case link-local mDNS exists for. Pass
`0` (or `kDNSServiceInterfaceIndexAny`) and it picks an address itself, as
before, preferring real Ethernet/Wi-Fi over VMware/VirtualBox/WireGuard/Hyper-V
adapters and skipping link-local. A **non-zero** index with no usable IPv4 —
adapter unplugged, gone, or IPv6-only — is refused with `-65540`
(`kDNSServiceErr_BadParam`; the shim's own macro for it is misnamed
`KDNSSERVICEERR_INVALID`) and a log line, rather than silently falling back to
the guess: a caller that named an adapter has pinned its sockets to it, and
advertising a different one announces the service where it isn't listening. In
Bonjour-proxy mode the index is passed straight to Apple's `dnssd.dll`.

Note the index is a lossy identifier: an adapter may carry several IPv4
addresses and the first is used. dns_sd gives the caller no way to name an
address, so on a multi-address adapter the advertised A record can be a sibling
of the address the caller's sockets bound to.

## Build

From an MSYS2 **UCRT64** (or any MinGW-w64) shell:
```bash
gcc -O2 -Wall -shared -o dnssd.dll dnssd_shim.c -lws2_32 -liphlpapi
```
Single translation unit + the vendored `mdns.h` header — no external deps.

## Which build is this?

The version is compiled in. On its **first registration** the shim announces
itself on stderr — `[dnssd_shim] dnssd shim 1.0.0 (embedded mDNS)`, or
`(Apple Bonjour proxy)` when it is forwarding — which also tells you which of the
two paths is live. Deliberately not at DLL load: a host normally redirects its
stdio after startup, so a line printed from `DllMain` goes nowhere (measured on
Windows). The same string is in the file regardless:

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
