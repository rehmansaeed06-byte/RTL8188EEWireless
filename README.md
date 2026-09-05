# rtl8188ee-macos

A native macOS kext port of the Realtek **RTL8188EE** PCIe Wi-Fi chip
(PCI ID `10EC:8179`), for machines where no vendor driver exists for
current macOS versions.

This is not a from-scratch reimplementation of the chip's logic. It
compiles the real, unmodified upstream Linux kernel driver
(`drivers/net/wireless/realtek/rtlwifi`, vendored in-tree at
`src/rtlwifi/`) against a from-scratch compatibility layer
(`src/compat/`) that stands in for the Linux kernel APIs the driver
expects, and a small IOKit wrapper (`src/macos/`) that bridges the
result into `IOEthernetController`/`IO80211` on the macOS side.

## Status

Working:
- Kext loads and binds to the PCI device
- Scanning, WPA2 connect, and disconnect via the bundled CLI tool
  (`tools/ctl_rtl8188ee.c`)
- Sustained data transfer once connected

Known open issue:
- Under sustained load (e.g. opening a browser and generating a burst
  of connections), RX can stall out entirely for a period. A driver-side
  link-liveness watchdog gap that made this worse (a false "AP off"
  disconnect after only ~10s of beacon-frame starvation, independent
  of whether data was still flowing) has been closed — see
  `docs/findings.md` Section 109 and the `rtlwifi_mark_rx_activity()`
  fix. Whether real RX-frame delivery itself still stalls under load,
  separate from the watchdog false-positive, is still being tracked
  down — see `docs/findings.md` for the full history of what's been
  ruled in/out.

## Building

Requires Xcode Command Line Tools and the `MacKernelSDK` submodule
(fetched automatically via `git submodule update --init`).

```bash
git submodule update --init
make -f Makefile.rtl8188ee kext
```

The built kext lands at `build/out/rtl8188ee.kext`.

## Loading

```bash
sudo chown -R root:wheel build/out/rtl8188ee.kext
sudo kextutil build/out/rtl8188ee.kext
kextstat | grep -i rtl8188ee
```

On Hackintosh setups using OpenCore, the kext can instead be dropped
into `EFI/OC/Kexts/` and referenced from `config.plist`'s Kernel → Add
section for boot-time injection.

## Allowing unsigned/unverified kexts (OpenCore + Hackintosh)

This kext is unsigned — it's not notarized or signed with an Apple
Developer ID. macOS's default security posture (SIP + kext signing
enforcement) will refuse to load it, and on a Hackintosh, the normal
fix (Startup Security Utility → Reduced Security) usually isn't
available at all — it reports **"Startup Security Utility is not
supported on this Mac"** on OpenCore systems, since OpenCore doesn't
expose the real Apple Secure Boot hardware path that utility depends
on.

The actual fix on OpenCore is to relax SIP/kext-signing via
OpenCore's own boot-time NVRAM setting instead, in `config.plist`:

```xml
<key>NVRAM</key>
<dict>
    <key>Add</key>
    <dict>
        <key>7C436110-AB2A-4BBB-A880-FE41995C9F82</key>
        <dict>
            <key>csr-active-config</key>
            <data>AAAAAA==</data>
        </dict>
    </dict>
</dict>
```

`csr-active-config` is the same underlying SIP configuration value
`csrutil` manages on genuine Macs — OpenCore just lets you set it
directly since the native Startup Security Utility path isn't
available. The specific value above disables SIP checks broadly
enough to permit unsigned kext loading; consult
[Dortania's OpenCore guide](https://dortania.github.io/OpenCore-Install-Guide/)
for the exact bitmask that matches only what you need disabled if you
want something narrower than a full SIP disable.

After changing `config.plist`, reboot for the NVRAM change to take
effect. You may also still need to approve the kext once via
**System Settings → Privacy & Security** (look for a "System software
from developer... was blocked" prompt) — this can require a reboot
of its own (macOS reports this as a distinct step, sometimes
described as needing "Code 27" approval followed by a "Code 28"
reboot prompt in `kextutil`/`kmutil` output).

This is not something to leave in place permanently, or ship to
anyone else's machine without them understanding it — relaxing SIP
this way weakens macOS's normal integrity protections system-wide,
not just for this one kext.

## Testing

A small CLI tool is included for driving the kext directly via its
IOKit user client, without needing the full macOS Wi-Fi stack wired
up:

```bash
cc tools/ctl_rtl8188ee.c -o ctl_rtl8188ee -framework IOKit -framework CoreFoundation
./ctl_rtl8188ee scan
./ctl_rtl8188ee bsslist
./ctl_rtl8188ee connect "SSID" "password"
./ctl_rtl8188ee state
./ctl_rtl8188ee disconnect
```

## Repository layout

```
rtl8188ee-macos/
├── README.md
├── LICENSE
├── docs/
│   └── findings.md          -- detailed session-by-session debugging log
├── src/
│   ├── macos/                -- IOKit wrapper (IOEthernetController subclass,
│   │                            user client, kext entry point)
│   ├── compat/                -- Linux-kernel-API compatibility shims
│   └── rtlwifi/               -- vendored, unmodified upstream Linux driver
├── firmware/                  -- RTL8188EE firmware blob
├── tools/                     -- CLI test tool + firmware blob generator
└── build/                     -- build output (gitignored)
```

## Why vendor the Linux driver instead of reimplementing it?

The RTL8188EE has no public datasheet; the upstream Linux driver is
the closest thing to one. Reimplementing its RF/PHY initialization
sequences and packet formats from scratch would mean re-deriving
years of upstream tribal knowledge with no independent way to verify
correctness. Compiling the real thing against a compatibility shim
means every fix upstream has ever made is already present, and any
bug is either in the shim layer (auditable, all our own code) or a
genuine chip-specific issue (rare, since this exact driver runs on
millions of Linux machines).

## License

See `LICENSE`. The vendored `src/rtlwifi/` tree retains its original
upstream Linux kernel licensing (GPLv2); see the headers in those
files.
