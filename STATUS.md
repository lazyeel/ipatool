# STATUS: native ADI anisette engine (`adi-engine` branch)

State as of 2026-08-24. Read this before touching the code. Each invariant in the next section was found the hard way.

This document tracks all branches. The same file ships on `main`, `adi-engine`, and `sap-engine`.

## What works

All points are verified on a device running Termux (Android arm64), most recently from a fresh clone.

1. The ADI engine runs outside any JVM. The classic Apple stack from Apple Music for Android 2.9.0 loads, provisions, and generates anisette tokens locally:

```text
./get_libs.sh                      # downloads the APK, extracts libs, verifies symbols
./adi_test ./libs-classic
...
[prov] PROVISIONING COMPLETE
[adi] re-check ADIGetLoginCode=0
=== SUCCESS ===
X-Apple-I-MD:   AAAABQAAABC...
X-Apple-I-MD-M: EYg4gDqhnNLecv8N...
```

After provisioning, state lives in `./adi-data/` plus `./adi_identifier`. Later runs mint OTPs instantly, with no round-trips to MidService.

2. ipatool itself uses the engine. `AnisetteData::generate_locally()` exists for Linux/Termux builds (`adi_anisette.cpp`). On non-Windows, `fetch_anisette()` prefers it and falls back to public SideStore-style servers. The chosen source is printed to stderr as `[anisette] source: ...`. Provisioning state lives in `~/.ipatool/adi/`.

3. Clean-room verified: a stranger's path works end to end: fresh clone, `get_libs.sh`, build, provision, SUCCESS. No Apple binaries are stored in the repo; the script fetches them and checks each library for its expected symbol markers before use.

4. The protocol details, verified against the shipped binary:

| Item | Value |
|---|---|
| Engine library (classic) | `libstoreservicescore.so` (Apple Music ≤ 5.x) |
| Obfuscated exports | `kq56gsgHG6`=LoadLibraryWithPath, `Sph98paBcz`=SetAndroidID, `nf92ngaK92`=SetProvisioningPath, `aslgmuibau`=GetLoginCode, `qi864985u0`=OTPRequest, `rsegvyrt87`=ProvisioningStart, `uv5t6nhkui`=ProvisioningEnd, `jk24uiwqrg`=Dispose |
| New engine (6.5.x) | `libstoreapi.so` carries rotated names. Real addresses are recoverable from its unobfuscated JNI wrappers (+0x17d2b0 through +0x17d3d0) |
| Anonymous machine DS_ID | −2, bound into the cpim envelope and validated server-side |
| Identity canon | `SetAndroidID` takes a 16-hex Android ID; header `X-Mme-Device-Id` carries the dashed uppercase UUID derived from the same random bytes; `X-Apple-I-MD-LU` is the first 8 bytes as 16 uppercase hex |
| Provisioning flow | `GsService2/lookup` → `MidService/startMachineProvisioning` (spim) → `ADIProvisioningStart` (cpim, session) → `MidService/finishMachineProvisioning` (ptm+tk) → `ADIProvisioningEnd`. All three HTTP hops ride one keep-alive connection |

## Invariants

Each of these produced a confusing failure mode before it was pinned down.

- DS_ID is −2, not −1. `0xFFFFFFFFFFFFFFFF` is −1 and poisons every ADI call: `start` completes, then `finish` answers `-29003 environment mismatch`.
- Open every library with `RTLD_LOCAL`. With `RTLD_GLOBAL`, zlib and allocator symbols bundled inside Apple's libraries interpose on libcurl bindings, and response bodies vanish inside your own process: HTTP 200, correct Content-Length, zero bytes delivered.
- Preload dependencies explicitly, in dependency order. Bionic resolves DT_NEEDED against system paths, not `$ORIGIN`: `libc++_shared`, the ICU trio, `xml2`, `BlocksRuntime`, `dispatch`, `CoreFoundation`, `mediaplatform`, `CoreADI`, then the entry `.so`.
- Pin TLS to Apple's chain. `gsa.apple.com` is served from Apple's own PKI. Its root (Apple Root CA, SHA-256 `B0:B1:73:0E:CB:C7:FF:45:05:14:2C:49:F1:29:5E:6E:DA:6B:CA:ED:7E:2C:68:C5:BE:91:B5:A1:10:01:F0:24`) is published by Apple but absent from Mozilla bundles shipped with Termux. `apple_chain.pem` in this branch contains it.
- Keep header identity consistent: `X-Mme-Device-Id` always carries the dashed UUID, whatever identifier format the engine accepted internally.
- Send a fresh `X-Apple-I-Client-Time` on every request. Provision stamps local time with a numeric offset.

## The wall

App Store commerce authentication rejects third-party clients as of August 2026. Controlled experiment with everything above:

```text
GSA SRP + SMS 2FA .................. PASSED (credentials accepted)
Anisette source .................... native ADI engine (provisioned locally)
MZFinance/authenticate ............. HTTP 403 (pod 6), empty body, all pods
```

This reproduces what the ecosystem hit in waves: [ipatool #513](https://github.com/majd/ipatool/issues/513), [ipatool #520](https://github.com/majd/ipatool/issues/520), [ipatool #522](https://github.com/majd/ipatool/issues/522), [ipatool #523](https://github.com/majd/ipatool/issues/523), [iDescriptor #121](https://github.com/iDescriptor/iDescriptor/issues/121), [IPA_Downloader #11](https://github.com/kda2495/IPA_Downloader/issues/11).

Independent reverse engineering by thegaiko ([comment in ipatool #522](https://github.com/majd/ipatool/issues/522#issuecomment-5362720682)) traced the new gate:

- the store credential (`X-Token`) is minted by `AMSMediaTokenService` as a Private Access Token (Privacy Pass) signed by a Secure Enclave P-384 key;
- key use requires `appstoreagent` entitlements (`com.apple.security.attestation.access`, `com.apple.keystore.absinthe`, `com.apple.keystore.sik.access`) that AMFI validates against Apple's code signature, and the SEP enforces key ACLs independently of SIP;
- GrandSlam alone is confirmed insufficient: feeding PET as `X-Token` yields `failureType 2002` or `2042`.

Anisette was not the blocker. The missing piece is the SAP/FairPlay action signature (`X-Apple-ActionSignature`) over an attested key.

Confirmed by an independent test ([comment in ipatool #522](https://github.com/majd/ipatool/issues/522)): iTunes 12.6.5.3 running on genuine Apple hardware (iMac14,1 Boot Camp) with FairPlay 2.12.8 also receives an error (-50) on sign-in. The client provisions its ADI identity successfully but `SC Info.sidb` is never created. This confirms the gate is Secure Enclave attestation, not third-party client detection.

## FairPlay SAP route (`sap-engine` branch)

Full reverse engineering of the FairPlay SAP and FPDI subsystems in
Apple Music for Android 6.5.x, verified through direct function calls
on Termux.

### Two separate subsystems

FairPlay contains two independent engines that we initially conflated:

| Subsystem | Purpose | Status |
|---|---|---|
| **FairPlay SAP** | DRM/streaming authentication (AirPlay-style) | Fully operational on Termux |
| **FPDI** | FairPlay Device Identity for commerce signing | Blocked by circular dependency |

The commerce gate uses FPDI, NOT SAP. Our successful SAPInit and InitContext
calls are for the streaming subsystem; they do not unblock FPDICreate.

### Architecture

Both subsystems live in `libstoreapi.so` 6.5.x as internal functions at
addresses 0x17d4xx–0x17d7xx. They are NOT PLT imports from `libCoreFP.so`
(as initially assumed). The obfuscated export names (`bsawCXd`, `Fc3vhtJDvr`,
etc.) are defined AND exported by libstoreapi.so itself.

`tools/fpmap.py` maps JNI wrappers to internal worker addresses:

```text
FairPlaySetAndroidID                   → bsawCXd
FairPlayGetGUID                        → FKgu8fbnvGFG
FairPlaySAPInit                        → cp2g1b9ro
FairPlaySAPExchange                    → Mib5yocT
FairPlaySAPSign                        → Fc3vhtJDvr
FairPlaySAPPrimeForAction              → jfkdDAjba3jd
FairPlaySAPProcessResponse             → gLg1CWr7p
FairPlayGenerateSubscriptionBagRequest → V3lNO
FairPlayImportSubscriptionBag          → jr3lMuU8uaAR
FPDIPlatformInit                       → RhsJgiCAMX
FPDIAttrInit                           → jsf09djfs0df
FPDICreate                             → d2234hmbdf
FPDIInit                               → g9000sds9
FPDISetup                              → fsmklk123
FPDISign                               → sldksmfm1n
```

### Verified working calls (Termux arm64)

All made via dlsym on obfuscated export names, bypassing JNI entirely.
No Java runtime, no emulator, no anisette servers needed for this layer.

| Function | Obfuscated name | Result | Notes |
|---|---|---|---|
| LoadLibraryWithPath | `N8jdR29h` | PASS (0) | Needs data directory path, not library path |
| SetAndroidID | `bsawCXd` | PASS (0) | Takes 16-hex Android ID + length |
| GetGUID | `FKgu8fbnvGFG` | PASS (0) | Populates FairPlayHWInfo struct. GUID: `12000000ba69c18b...` |
| SAPInit | `cp2g1b9ro` | PASS (0) | Takes (&ctx_handle, hw_info_ptr). Returns uint64 context handle |
| InitContext | `XtCqEf5X` | PASS (0) | Takes (NULL, hw_info_ptr, dataDir, &context_id). Returns int32 |
| FPDIPlatformInit | `RhsJgiCAMX` | PASS (0) | Takes struct{PlatformIDCallback*, DRMLicenseChallengeCallback*}. NULL rejected with -44650 |
| FPDIAttrInit | `jsf09djfs0df` | PASS (0) | Takes &attr. Allocates 40-byte (0x28) struct on heap |
| FPDIAttrSetAttestationMode | `RXm4IJLE3xR` | PASS (0) | Takes (attr, mode). Accepts modes 0-2 |

### Blocked calls

| Function | Obfuscated name | Error | Cause |
|---|---|---|---|
| FPDICreate | `d2234hmbdf` | -44660/-44684 | Needs server-provided subscription bag before creating session |
| FPDIInit | `g9000sds9` | -44650 | Session not created yet (depends on Create) |
| FPDISetup | `fsmklk123` | -44650 | Session not created yet |
| FPDISign | `sldksmfm1n` | -44650/-42085 | No valid session or no server exchange completed |
| InitContext(DS_ID=-2) without FPDI | `XtCqEf5X` | SIGSEGV at NULL+0x1b | Dereferences null because FPDICreate never allocated context |
| GenerateSubscriptionBagRequest | `V3lNO` | -42032 | Needs initialized FairPlay context to know what to request |
| SAPExchange | `Mib5yocT` | -42091 | Context exists but no server round-trip has established keys |
| SAPPrimeForAction | `jfkdDAjba3jd` | -42023/-42085 | State machine not advanced past exchange step |
| FPDIGetVersion | `RXm4IJLE3xR` | -44650 | Subsystem reports version only after successful init chain |

### Error code map (undocumented anywhere else)

These codes are from the FPDI/FairPlay Commerce subsystem, distinct from
the standard FairPlay Streaming error range (-42xxx documented by Apple).

| Code | Meaning | Context |
|---|---|---|
| -44650 | kFPDINotInitialized | FPDI subsystem bootstrapped via PlatformInit(callbacks) but no config loaded |
| -44654 | Config format error | FPDIInit accepted 2-arg form but rejected the config content |
| -44660 | Session/context creation failed | PlatformInit passed, attrs set, but Create can't proceed without server data |
| -44684 | Invalid argument | Correct ABI but wrong values (e.g. DS_ID=-2 passed as arg instead of attr pointer) |
| -42032 | No FairPlay session | Core init done but no SAP context established |
| -42023 | Not primed | Exchange attempted before PrimeForAction succeeded |
| -42085 | Sign/exchange state invalid | Context exists but server exchange hasn't established keys |
| -42091 | Exchange not ready | SAP context exists but server round-trip hasn't happened |

### Root cause of FPDICreate failure

Circular dependency confirmed across multiple test configurations:

1. `FPDICreate` generates an `initRequest` blob to send to `fpinit.itunes.apple.com/v1/fpdi/init`
2. The server responds with subscription bag data containing device-specific cryptographic keys
3. `FPDIInit` processes the server response and generates a `setupRequest`
4. Setup completes after sending setupRequest to `/v1/fpdi/setup`

Step 1 fails because FPDICreate checks whether the FPDI subsystem has been
initialized with server-provided configuration data. Without a prior successful
server exchange, it refuses to generate a new request. This is a deliberate
security measure: the first exchange must happen through trusted channels
(the real Apple Music app) which validates the device via Secure Enclave.

Independent confirmation from [#522 comment](https://github.com/majd/ipatool/issues/522):
iTunes 12.6.5.3 on genuine Apple hardware also cannot complete this exchange.
Apple withholds FairPlay keys from ALL clients that lack current SE attestation.

### Server endpoints discovered

```text
https://fpinit.itunes.apple.com/v1/fpdi/init    ← POST initRequest here
https://fpinit.itunes.apple.com/v1/fpdi/setup   ← POST setupRequest here
https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/fpsRequest ← FairPlay requests
https://init.itunes.apple.com/WebObjects/MZInit.woa/wa/fpsCertificate ← FPS cert (expired 2014)
```

The fpdi/init endpoint requires:
- Valid JSON body generated by FPDICreate (not arbitrary JSON)
- GSA session tokens for authentication
- Returns subscription bag data for FPDIInit

Without valid authentication, the endpoint returns 405 (no GET), 415 (wrong
content type), or 500 (empty/invalid body).

### Callback signatures (from JavaCPP annotations)

```kotlin
PlatformIDCallback:
    int call(BytePointer uniqueId, IntPointer uniqueIdLength)

DRMLicenseChallengeCallback:
    int call(BytePointer pushBox, int pushBoxSize,
             BytePointer drmLicenseChallenge,
             IntPointer drmLicenseChallengeSize)
```

These callbacks ARE invoked by the FPDI subsystem during normal operation.
Our implementations were provided to `FPDIPlatformInit` via a simple struct
of two function pointers, which was accepted (returned 0). However, they were
never called during `FPDICreate`, suggesting Create fails before reaching
the callback invocation point.

### Full FPDI API from JavaCPP annotations (authoritative)

```text
FPDIGetVersion(type, &major, &minor, &revision) → int
FPDIPlatformInit(functionPointerTable) → int
FPDIAttrInit(&attr) → int
FPDIAttrDestroy(&attr) → int
FPDIAttrSetAttestationMode(attr, attestationMode) → int
FPDIAttrSetPrimeMode(attr, primeMode) → int
FPDIAttrSetPrivacyLevel(attr, privacyLevel) → int
FPDICreate(&session, attr, &initRequest, &initRequestLength) → int
FPDIInit(session, initResponse[], initResponseLength, &setupRequest, &setupRequestLength) → int
FPDISetup(session, setupResponse[], setupResponseLength) → int
FPDISign(session, message[], messageLength, &signature, &signatureLength) → int
FPDIDestroy(&session) → int
FPDIDataDestroy(&data) → int
```

The protocol is a multi-round server exchange:
1. FPDICreate generates initRequest
2. POST initRequest → server returns initResponse
3. FPDIInit processes initResponse, generates setupRequest
4. POST setupRequest → server returns setupResponse
5. FPDISetup processes setupResponse. Initialization complete.
6. FPDISign can now sign commerce requests

## Ways forward

1. Monitor the ecosystem. Watch [ipatool #522](https://github.com/majd/ipatool/issues/522) for a community break. All known approaches have been tried.

2. MITM on live Apple Music app. Repackage APK with custom CA trust, capture the actual fpdi/init request that the real app sends. This would reveal the exact request format and required headers. Risk: Play Integrity API rejection.

3. Frida hook on repackaged APK. Hook `FairPlaySAPSign` and `FPDICreate` at runtime to capture ground-truth inputs and outputs. Same risk as above.

4. Sideload use cases. Signing an IPA with a free developer certificate needs GSA and the developer portal, not `buyProduct`. The engine already covers that authentication path today.

5. The route that works today: a real Mac, or an already-signed-in Apple device.

## Repo map

| Path | Purpose |
|---|---|
| `adi_test.c` | standalone tester: load stack, provision, mint one OTP pair. Build: `clang adi_test.c -O2 -Wall -o adi_test -ldl -lcurl -lcrypto -Wl,-rpath,'$ORIGIN/libs-classic'` |
| `get_libs.sh` | downloads Apple Music 2.9.0 + latest 6.5.x, extracts both stacks, verifies symbols |
| `libs-classic/` | created by `get_libs.sh` at runtime; never committed |
| `libs-new/` | created by `get_libs.sh` for 6.5.x stack; never committed |
| `apple_chain.pem` | captured gsa.apple.com chain; root fingerprint matched against Apple's PKI publication |
| `adi_anisette.cpp`, `adi_anisette.h` | production integration: `AnisetteData::generate_locally()` for non-Windows builds |
| `build_termux.sh` | one-shot Termux build of ipatool with the engine |
| `sap_test.c` | FairPlay SAP/FPDI harness (sap-engine branch only) |
| `tools/fpmap.py` | JNI wrapper to internal worker address mapper (sap-engine only) |
| `tools/sap_trace.py` | disassembles individual JNI wrappers, recovers BL targets and ABI (sap-engine only) |
| `tools/check_exports.py` | preflight gate: verifies the full FairPlay SAP/FPDI export topology (20 SAP workers in libstoreapi.so, 7 FPDI workers in libFPDIFor3P.so, dependency edge, libCoreFP.so fingerprint) before any dlsym (sap-engine only) |

## Platform notes

The ADI engine runs where the Apple libraries run: Android arm64 (Termux) and any aarch64 Linux.

On x86_64 Linux, the arm64 `.so` files cannot load natively because Apple does not ship x86_64 native libraries for Android Music in any current version (5.x or 6.x). The build compiles cleanly and `fetch_anisette()` falls back to public servers with `[anisette] native ADI unavailable` on stderr.

Three approaches were evaluated for x86_64 support:

1. **Dadoum/Provision** ([github.com/Dadoum/Provision](https://github.com/Dadoum/Provision)): already solves this with a D-implemented custom ELF loader that maps arm64 sections and provides ~30 Bionic function shims on glibc. Works natively at full speed. Pre-built Docker images available (`dadoum/anisette-v3-server`). This is the recommended path for x86_64 users who need local anisette generation.

2. **QEMU user-mode emulation**: tested with a Debian arm64 chroot + `qemu-aarch64-static`. The chroot boots successfully but adds ~400 MB overhead, requires root for mount operations, and introduces 3-5x latency from instruction translation. Rejected: too heavyweight for production use.

3. **Custom PE loader for Windows CoreADI64.dll**: built a working PE32+ loader that maps sections at the preferred base address (0x7c800000), applies base relocations, and resolves all 103 Win32 API imports via shim functions. However, calling exports crashes because `_DllMainCRTStartup` requires full Windows CRT initialization (TLS layout, SEH registration, heap setup) that cannot be provided outside a real Windows environment. Abandoned: implementing Wine-level compatibility is not practical for this project.

For most users, the public SideStore-style anisette servers (already supported by `fetch_anisette()`) provide sufficient service on x86_64 without needing a native ADI engine.

## Reproduce

```bash
# Termux (ADI engine):
pkg install clang libcurl openssl ca-certificates python
git clone -b adi-engine https://github.com/lazyeel/ipatool && cd ipatool
./get_libs.sh
clang adi_test.c -O2 -Wall -o adi_test -ldl -lcurl -lcrypto -Wl,-rpath,'$ORIGIN/libs-classic'
./adi_test ./libs-classic          # first run provisions (~10 s), later runs are instant

# Termux (FairPlay SAP research, sap-engine branch):
git checkout sap-engine
gcc sap_test.c -O2 -Wall -o sap_test -ldl -lcurl -lcrypto -Wl,-rpath,'$ORIGIN/libs-classic'
./sap_test ./libs-classic

# NixOS/Nix:
nix build                          # flake.nix handles everything
nix run . -- auth login -e you@example.com

# or build the full tool with the engine inside:
bash build_termux.sh
```

Use a burner account, and avoid provisioning many machines from one IP: Apple tracks that.

---

## Legal & Research License

The reverse-engineering documentation, protocol invariant analysis, error code maps, and architectural research in this document are licensed under [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/).
The code implementation is licensed under the [Apache License, Version 2.0](LICENSE) with [`NOTICE`](NOTICE).
