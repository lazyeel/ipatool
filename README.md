# ipatool-adi

Extended C++ App Store client with an embedded standalone Anisette engine for Linux/Android (Termux) and SMS two-factor login. Zero external server dependencies.

A hard fork of [Sorvigolova/ipatool](https://github.com/Sorvigolova/ipatool) (itself a C++ port of [majd/ipatool](https://github.com/majd/ipatool)).

**Current status:** the ADI anisette engine works on Termux (Android arm64). See [STATUS.md](STATUS.md) for the full state, protocol invariants, and what currently blocks App Store downloads for every third-party client.

---

## Branches

| Branch | Contents | Use it when |
|---|---|---|
| [`main`](https://github.com/lazyeel/ipatool/tree/main) | Upstream Sorvigolova code + SMS 2FA + `--store-front` override + Termux/Linux/NixOS build support. Anisette comes from public SideStore-style servers or the native MS Store iCloud provider on Windows. | You want the stable tool without the experimental engine |
| [`adi-engine`](https://github.com/lazyeel/ipatool/tree/adi-engine) | Everything in `main`, plus the embedded ADI anisette engine: Apple libraries load in-process, provisioning runs locally, zero third-party servers. Also carries `STATUS.md` with the full research log. | You want self-contained anisette generation on your own device |
| [`sap-engine`](https://github.com/lazyeel/ipatool/tree/sap-engine) | Everything in `adi-engine`, plus FairPlay SAP and FPDI reverse engineering: JNI wrapper mapping, error code documentation, server endpoint discovery, test harness. Research branch. | You want to explore the FairPlay SAP protocol |

All branches share the same platform support (Termux, Linux x86_64/aarch64, NixOS), the same SMS 2FA flow, and the same upstream feature set. Each branch is a strict superset of the one above it.

## Differences from Sorvigolova/ipatool

| Area | Upstream | This fork |
|---|---|---|
| Anisette on Linux/Termux | Public SideStore-style servers only (external dependency, availability not guaranteed) | Native in-process ADI engine (`adi-engine` branch): loads Apple's classic libraries from Apple Music 2.9.0 and provisions against GrandSlam locally |
| SMS two-factor login | Absent | `--sms`: interactive flow for accounts without trusted Apple devices (phone list, code request, code entry) |
| `--store-front` | Absent | Override the storefront for purchase/download/list-versions |
| Store error reporting | Bare "failed" messages | Surfaces `failureType`, `customerMessage` and the full response plist |
| Login failure handling | Half-authenticated state could be saved | iTunes auth failure is fatal |
| Packaging | Windows-centric | Nix flake + NixOS module + `default.nix`, CI workflow, Termux build script |

Everything upstream has is preserved: machine-bound credential encryption, static builds on all three desktop platforms, the full command set (`auth`, `search`, `purchase`, `download`, `list-versions`).

## The ADI anisette engine (`adi-engine` branch)

The engine loads Apple's classic libraries from Apple Music for Android 2.9.0 (`libstoreservicescore.so` plus dependencies), provisions a virtual device against GrandSlam, and mints `X-Apple-I-MD` / `X-Apple-I-MD-M` pairs in-process. No JVM, no emulator, no Docker, no third-party servers.

The Apple binaries are proprietary and are **not stored in this repo**. `get_libs.sh` downloads them, extracts the required set and verifies each library against expected symbol markers before use.

**The engine currently runs on Termux (Android arm64) only.** On x86_64 Linux the arm64 libraries cannot load; builds succeed there and the tool falls back to public servers automatically. A native x86_64 port would require a custom ELF loader with Bionic symbol shims (see STATUS.md, "Platform notes").

## Usage

### Termux quick start (`adi-engine` branch)

```bash
pkg install -y clang cmake make openssl libcurl zlib nlohmann-json libminizip python
git clone -b adi-engine https://github.com/lazyeel/ipatool.git && cd ipatool
./get_libs.sh          # fetch Apple libraries (~68 MB APK, verified)
bash build_termux.sh   # builds ./ipatool
```

### Verify the ADI engine standalone

```bash
clang adi_test.c -O2 -Wall -o adi_test -ldl -lcurl -lcrypto -Wl,-rpath,'$ORIGIN/libs-classic'
./adi_test ./libs-classic
```

First run provisions a virtual device (~10 s) and prints one `X-Apple-I-MD` / `X-Apple-I-MD-M` pair. Later runs are instant: provisioning state persists in `./adi-data/`.

### Log in with SMS 2FA (both branches)

```bash
./ipatool auth login -e you@example.com --sms
# -> prints trusted phone numbers, asks which to use
# -> sends the SMS, prompts for the received code
```

On the `adi-engine` branch, anisette headers come from the embedded engine:

```text
[anisette] source: native ADI engine
```

If `libs-classic/` is missing or cannot load, the tool prints
`[anisette] source: public servers (fallback)` and continues.

### Purchase and download

```bash
./ipatool purchase -b com.example.app --store-front 143441
./ipatool download -b com.example.app --store-front 143441 -o app.ipa
```

See `--help` for the full flag set, including `--keychain-passphrase`.

### NixOS / Nix

```bash
nix run github:lazyeel/ipatool/adi-engine -- auth info     # adi-engine branch
nix run github:lazyeel/ipatool/main      -- auth info      # main branch
nix build github:lazyeel/ipatool/adi-engine
```

A NixOS module (`programs.ipatool-cpp-sms.enable`) ships in `nixos-module.nix`; see the module header for overlay and home-manager usage.

## Current status and known limitation

App Store **downloads are currently blocked by Apple server-side** for all third-party clients (August 2026): commerce authentication now demands a SAP/Private Access Token signature that no open stack can produce without genuine Apple hardware. This affects Sorvigolova's upstream, majd's original, iMazing, iDescriptor and this fork equally.

Authentication (GSA SRP + SMS) works, anisette generation works natively, and the FairPlay SAP subsystem has been successfully initialized on Termux: core functions (`LoadLibraryWithPath`, `SetAndroidID`, `GetGUID`, `SAPInit`, `InitContext`) all pass. The remaining blocker is `FPDICreate`, which requires a subscription bag from Apple's servers that cannot be obtained without Secure Enclave attestation. Full analysis including error code map, function signatures, and server endpoints: [STATUS.md](STATUS.md).

## License

This project distribution and its native engine components are licensed under the **Apache License, Version 2.0 with a NOTICE file**.

* All native ARM64 Bionic ADI/Anisette loader implementations, StoreServices/FPDI protocol research, SMS 2FA subsystems, and new components authored by **lazyeel** are licensed under the **Apache License, Version 2.0**
* Upstream C++ codebase components inherited from **Sorvigolova/ipatool** and original protocol implementations from **majd/ipatool** remain licensed under their original **MIT License** (see [`LICENSE-MIT`](LICENSE-MIT)).

See [`LICENSE`](LICENSE) for the full Apache-2.0 terms, [`LICENSE-MIT`](LICENSE-MIT) for upstream terms, and [`NOTICE`](NOTICE) for mandatory downstream attribution requirements.

### Attribution & Downstream Use

Both open-source and commercial/proprietary projects are welcome to use, adapt, and embed this codebase. Pursuant to **Section 4(d) of the Apache License 2.0**, any distribution of derivative works (in source or binary form) must reproduce the attribution notices defined in [`NOTICE`](NOTICE) within your product's About dialog, third-party legal notices, or documentation.

Suggested attribution:
> *"Native ADI engine and FairPlay Bionic loader implementation based on research and code by lazyeel (https://github.com/lazyeel/ipatool)."*

---

## Acknowledgements

* **[Sorvigolova/ipatool](https://github.com/Sorvigolova/ipatool):** the C++ codebase this fork builds on; inherited features are documented in the upstream README.
* **[majd/ipatool](https://github.com/majd/ipatool):** the original tool and protocol implementation.
* **[Dadoum/Provision](https://github.com/Dadoum/Provision):** mapped out the GrandSlam ADI provisioning protocol and the roles of the obfuscated exports in `libstoreservicescore.so`. Our C++ engine follows the approach documented there.
* **[SideStore](https://github.com/SideStore):** the `DS_ID = -2` anonymous-machine semantics and anisette server design ([omnisette](https://github.com/SideStore/omnisette)).
* **Apple:** the ADI libraries themselves come from the Apple Music for Android package and remain proprietary; they are downloaded at runtime by `get_libs.sh`, never redistributed here.
* **[thegaiko](https://github.com/thegaiko) and [kda2495](https://github.com/kda2495):** reverse-engineering notes and field reports on the 2026 commerce gate (see `STATUS.md`, *"The wall"*).

---

# Upstream documentation (Sorvigolova/ipatool)

Features inherited unchanged from upstream (machine-bound credential encryption, static builds on all three desktop platforms, the full command set: `auth`, `search`, `purchase`, `download`, `list-versions`, native MS Store iCloud anisette on Windows) are documented in the [Sorvigolova/ipatool README](https://github.com/Sorvigolova/ipatool#readme).
