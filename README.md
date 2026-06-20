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

## Build

From an MSYS2 **UCRT64** (or any MinGW-w64) shell:
```bash
gcc -O2 -Wall -shared -o dnssd.dll dnssd_shim.c -lws2_32 -liphlpapi
```
Single translation unit + the vendored `mdns.h` header — no external deps.

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
