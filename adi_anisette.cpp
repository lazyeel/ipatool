// Copyright 2026 lazyeel (https://github.com/lazyeel)
// SPDX-License-Identifier: Apache-2.0

// adi_anisette.cpp: native ADI-based anisette provider (Linux/Android/Termux).
//
// Loads the classic Apple stack from ./libs-classic (Apple Music 2.9.0):
//   libc++_shared -> ICU trio -> xml2 -> BlocksRuntime -> dispatch ->
//   CoreFoundation -> mediaplatform -> CoreADI, then libstoreservicescore.so
// and provisions/mints OTP locally via the stable obfuscated exports.
//
// Invariants (each of these caused a hard-to-trace failure):
//   * DS_ID is -2 ((uint64_t)-2), NOT -1: it is bound into the cpim
//     envelope and validated server-side (-29003 otherwise).
//   * SetAndroidID accepts 16-hex (Android ID); X-Mme-Device-Id header
//     stays the dashed UUID derived from the same random identity.
//   * All dlopen calls are RTLD_LOCAL: Apple's bundled zlib/allocators
//     must not interpose on libcurl.
//   * One keep-alive connection for lookup->start->finish (Provision does
//     exactly that).
//
// State: <state_dir>/adi-data/, <state_dir>/adi_identifier.

#include "adi_anisette.h"
#include "anisette.h"
#include "ipatool.h"
#include <curl/curl.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>

extern "C" {
/* ── ADI prototypes (ABI per Provision/adi.d) ─────────────────────────── */
typedef int  (*ADI_LoadLibraryWithPath_t)(const char *);
typedef int  (*ADI_SetAndroidID_t)(const char *, uint32_t);
typedef int  (*ADI_SetProvisioningPath_t)(const char *);
typedef int  (*ADI_GetLoginCode_t)(uint64_t);
typedef int  (*ADI_OTPRequest_t)(uint64_t, uint8_t **, uint32_t *, uint8_t **, uint32_t *);
typedef int  (*ADI_ProvisioningStart_t)(uint64_t, const uint8_t *, uint32_t,
                                        uint8_t **, uint32_t *, uint32_t *);
typedef int  (*ADI_ProvisioningEnd_t)(uint32_t, const uint8_t *, uint32_t,
                                      const uint8_t *, uint32_t);
typedef int  (*ADI_Dispose_t)(void *);

static const char *N_LOAD      = "kq56gsgHG6";
static const char *N_SETID     = "Sph98paBcz";
static const char *N_SETPATH   = "nf92ngaK92";
static const char *N_LOGINCODE = "aslgmuibau";
static const char *N_OTP       = "qi864985u0";
static const char *N_START     = "rsegvyrt87";
static const char *N_END       = "uv5t6nhkui";

#define DS_ID ((uint64_t)-2)
} /* extern "C" */

namespace {

struct Buf { std::string data; };

size_t wr_cb(void *p, size_t sz, size_t nm, void *ud) {
    static_cast<Buf *>(ud)->data.append(static_cast<const char *>(p), sz * nm);
    return sz * nm;
}

std::string xml_string_after_key(const std::string &xml, const std::string &key) {
    std::string pat = "<key>" + key + "</key>";
    auto k = xml.find(pat);
    if (k == std::string::npos) return {};
    auto s = xml.find("<string>", k);
    if (s == std::string::npos) return {};
    s += strlen("<string>");
    auto e = xml.find("</string>", s);
    if (e == std::string::npos) return {};
    return xml.substr(s, e - s);
}

std::string b64_encode(const uint8_t *in, size_t n) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i < n) {
        uint32_t v = in[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= in[i + 1] << 8;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += rem == 2 ? T[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::vector<uint8_t> b64_decode(const std::string &in) {
    static auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t acc = 0; int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((acc >> bits) & 0xFF); }
    }
    return out;
}

std::string read_all(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

void write_all(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f || fwrite(data.data(), 1, data.size(), f) != data.size()) {
        if (f) fclose(f);
        throw IpaError("adi-anisette: cannot write " + path);
    }
    fclose(f);
}

} // namespace

AnisetteData AnisetteData::generate_locally()
{
#ifdef _WIN32
    throw IpaError("adi-anisette: use the Windows iCloud provider here");
#else
    /* ── state directory: ~/.ipatool/adi ─────────────────────────────── */
    const char *home = getenv("HOME");
    if (!home || !*home) throw IpaError("adi-anisette: $HOME not set");
    std::string dir = std::string(home) + "/.ipatool/adi";
    mkdir(dir.c_str(), 0755);                       /* ok if exists */
    std::string ident_path  = dir + "/adi_identifier";
    std::string adidata_dir = dir + "/adi-data";
    mkdir(adidata_dir.c_str(), 0755);

    /* ── identity (Provision canon) ─────────────────────────────────── */
    std::string ident_raw = read_all(ident_path);
    uint8_t id[16];
    if (ident_raw.size() == 16) {
        memcpy(id, ident_raw.data(), 16);
    } else {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, id, 16) != 16) { if (fd >= 0) close(fd);
            throw IpaError("adi-anisette: cannot gather randomness"); }
        close(fd);
        write_all(ident_path, std::string(reinterpret_cast<char *>(id), 16));
    }
    char dev_uuid[64], lu[24], srl[24], adi_ident[8];
    snprintf(dev_uuid, sizeof dev_uuid,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             id[0],id[1],id[2],id[3],id[4],id[5],id[6],id[7],
             id[8],id[9],id[10],id[11],id[12],id[13],id[14],id[15]);
    for (int i = 0; i < 8; i++) sprintf(lu + i * 2, "%02X", id[i + 2]);
    lu[16] = 0;
    {
        static const char *hx = "0123456789abcdef";
        adi_ident[0] = hx[id[0] >> 4]; adi_ident[1] = hx[id[0] & 15];
        adi_ident[2] = hx[id[1] >> 4]; adi_ident[3] = hx[id[1] & 15];
        adi_ident[4] = 0;
    }
    static const char *alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 12; i++) srl[i] = alpha[(lu[i] + lu[i + 3]) % 34];
    srl[12] = 0;

    /* ── load the stack ─────────────────────────────────────────────── */
    auto load = [&](const std::string &sub, const std::string &name,
                    bool optional = false) -> void * {
        std::string p = sub + "/" + name;
        if (optional && access(p.c_str(), R_OK) != 0) return nullptr;
        void *h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) throw IpaError(std::string("adi-anisette: dlopen ") + name +
                               ": " + dlerror());
        return h;
    };
    /* locate kit dir */
    std::string libdir;
    const char *env_kit = getenv("ADI_LIBS");
    std::string home_kit = std::string(home) + "/.ipatool/adi/libs-classic";
    const char *kits[] = { "./libs-classic", "./libs", home_kit.c_str(),
                           env_kit ? env_kit : "./libs-classic" };
    for (const char *d : kits) {
        std::string probe = std::string(d) + "/libstoreservicescore.so";
        if (access(probe.c_str(), R_OK) == 0) { libdir = d; break; }
    }
    if (libdir.empty())
        throw IpaError("adi-anisette: no library kit found "
                       "(need libs-classic/ with Apple Music 2.9.0 .so files)");
    for (const char *dep : { "libc++_shared.so",
                             "libicudata_sv_apple.so", "libicuuc_sv_apple.so",
                             "libicui18n_sv_apple.so", "libxml2.so",
                             "libBlocksRuntime.so", "libdispatch.so",
                             "libCoreFoundation.so", "libmediaplatform.so",
                             "libCoreADI.so" })
        load(libdir, dep, /*optional=*/true);

    void *h = load(libdir, "libstoreservicescore.so");

    auto sym = [&](const char *n) -> void * {
        void *p = dlsym(h, n);
        if (!p) throw IpaError(std::string("adi-anisette: dlsym ") + n);
        return p;
    };
    auto pLoad   = reinterpret_cast<ADI_LoadLibraryWithPath_t>(sym(N_LOAD));
    auto pSetID  = reinterpret_cast<ADI_SetAndroidID_t>(sym(N_SETID));
    auto pSetPth = reinterpret_cast<ADI_SetProvisioningPath_t>(sym(N_SETPATH));
    auto pCode   = reinterpret_cast<ADI_GetLoginCode_t>(sym(N_LOGINCODE));
    auto pOTP    = reinterpret_cast<ADI_OTPRequest_t>(sym(N_OTP));
    auto pStart  = reinterpret_cast<ADI_ProvisioningStart_t>(sym(N_START));
    auto pEnd    = reinterpret_cast<ADI_ProvisioningEnd_t>(sym(N_END));

    char absdir[1024];
    if (!realpath(libdir.c_str(), absdir))
        throw IpaError("adi-anisette: realpath(libdir)");
    int rc = pLoad(absdir);
    if (rc) throw IpaError("adi-anisette: LoadLibraryWithPath=" + std::to_string(rc));
    if ((rc = pSetID(adi_ident, 4)) != 0 &&
        (rc = pSetID(dev_uuid, (uint32_t)strlen(dev_uuid))) != 0) {
        /* try 16-hex form as final fallback */
        char h16[17];
        for (int i = 0, j = 0; dev_uuid[j]; j++)
            if (dev_uuid[j] != '-') h16[i++] = dev_uuid[j];
        h16[16] = 0;
        if ((rc = pSetID(h16, 16)) != 0)
            throw IpaError("adi-anisette: SetAndroidID=" + std::to_string(rc));
    }
    char absdata[1024];
    if (!realpath(adidata_dir.c_str(), absdata)) {
        mkdir(adidata_dir.c_str(), 0755);
        if (!realpath(adidata_dir.c_str(), absdata))
            throw IpaError("adi-anisette: cannot create " + adidata_dir);
    }
    if ((rc = pSetPth(absdata)) != 0)
        throw IpaError("adi-anisette: SetProvisioningPath=" + std::to_string(rc));

    /* ── provision if needed (single keep-alive session) ────────────── */
    int code = pCode(DS_ID);
    if (code != 0 && code != -45061)
        throw IpaError("adi-anisette: unexpected login code " + std::to_string(code));

    if (code == -45061) {
        CURL *c = curl_easy_init();
        if (!c) throw IpaError("adi-anisette: curl init failed");
        struct curl_slist *hlist = NULL;
        hlist = curl_slist_append(hlist,
            "User-Agent: akd/1.0 CFNetwork/1404.0.5 Darwin/22.3.0");
        hlist = curl_slist_append(hlist,
            "Content-Type: application/x-www-form-urlencoded");
        char hdr[600];
        snprintf(hdr, sizeof hdr, "X-Mme-Device-Id: %s", dev_uuid);
        hlist = curl_slist_append(hlist, hdr);
        hlist = curl_slist_append(hlist,
            "X-MMe-Client-Info: <MacBookPro13,2> <macOS;13.1;22C65>"
            " <com.apple.AuthKit/1 (com.apple.dt.Xcode/3594.4.19)>");
        snprintf(hdr, sizeof hdr, "X-Apple-I-MD-LU: %s", lu);
        hlist = curl_slist_append(hlist, hdr);
        hlist = curl_slist_append(hlist, "X-Apple-Client-App-Name: Setup");

        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hlist);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr_cb);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
        /* CA pinning mirrors adi_test/apple_chain.pem resolution */
        for (const char *pin : { "apple_chain.pem", "libs-classic/../apple_chain.pem",
                                 ".ipatool/apple_chain.pem" }) {
            if (access(pin, R_OK) == 0) { curl_easy_setopt(c, CURLOPT_CAINFO, pin); break; }
        }
        Buf body;

        auto perform = [&](const char *url, const char *post) -> std::string {
            body.data.clear();
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
            if (post) {
                curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "POST");
                curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
            } else {
                curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, NULL);
                curl_easy_setopt(c, CURLOPT_POSTFIELDS, NULL);
                curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
            }
            curl_easy_setopt(c, CURLOPT_URL, url);
            CURLcode r = curl_easy_perform(c);
            long http_code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
            if (r != CURLE_OK)
                throw IpaError(std::string("adi-anisette: request failed: ") +
                               curl_easy_strerror(r));
            if (http_code != 200 || body.data.empty())
                throw IpaError("adi-anisette: provisioning request rejected (" +
                               std::to_string(http_code) + ")");
            return body.data;
        };

        std::string lookup = perform(
            "https://gsa.apple.com/grandslam/GsService2/lookup", nullptr);
        std::string u_start = xml_string_after_key(lookup, "midStartProvisioning");
        std::string u_fin   = xml_string_after_key(lookup, "midFinishProvisioning");
        if (u_start.empty() || u_fin.empty())
            throw IpaError("adi-anisette: url bag lacks provisioning endpoints");

        std::string sp_req =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
            "<plist version=\"1.0\"><dict>"
            "<key>Header</key><dict/><key>Request</key><dict/>"
            "</dict></plist>";
        std::string r1 = perform(u_start.c_str(), sp_req.c_str());
        std::string spim_b64 = xml_string_after_key(r1, "spim");
        if (spim_b64.empty()) throw IpaError("adi-anisette: no spim");
        std::vector<uint8_t> spim = b64_decode(spim_b64);

        uint8_t *cpim = nullptr; uint32_t cpim_n = 0; uint32_t session = 0;
        rc = pStart(DS_ID, spim.data(), (uint32_t)spim.size(),
                    &cpim, &cpim_n, &session);
        if (rc) { if (cpim) free(cpim);
            throw IpaError("adi-anisette: ProvisioningStart=" + std::to_string(rc)); }

        char fin_req[2048];
        std::string cpim_b64 = b64_encode(cpim, cpim_n);
        free(cpim);
        snprintf(fin_req, sizeof fin_req,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
                 "<plist version=\"1.0\"><dict><key>Header</key><dict/>"
                 "<key>Request</key><dict><key>cpim</key><string>%s</string></dict>"
                 "</dict></plist>", cpim_b64.c_str());
        std::string r2 = perform(u_fin.c_str(), fin_req);
        std::string ptm_b64 = xml_string_after_key(r2, "ptm");
        std::string tk_b64  = xml_string_after_key(r2, "tk");
        if (ptm_b64.empty() || tk_b64.empty())
            throw IpaError("adi-anisette: finish lacked ptm/tk");
        std::vector<uint8_t> ptm = b64_decode(ptm_b64);
        std::vector<uint8_t> tk  = b64_decode(tk_b64);
        rc = pEnd(session, ptm.data(), (uint32_t)ptm.size(),
                  tk.data(), (uint32_t)tk.size());
        if (rc) throw IpaError("adi-anisette: ProvisioningEnd=" + std::to_string(rc));
        curl_easy_cleanup(c);

        code = pCode(DS_ID);
        if (code != 0)
            throw IpaError("adi-anisette: still not provisioned after end()");
    }

    /* ── mint the OTP ───────────────────────────────────────────────── */
    uint8_t *mid = nullptr, *otp = nullptr;
    uint32_t mid_n = 0, otp_n = 0;
    rc = pOTP(DS_ID, &mid, &mid_n, &otp, &otp_n);
    if (rc) throw IpaError("adi-anisette: OTPRequest=" + std::to_string(rc));

    AnisetteData a;
    a.otp           = b64_encode(otp, otp_n);
    a.machineID     = b64_encode(mid, mid_n);
    a.localUserUUID = lu;
    a.deviceID      = dev_uuid;
    a.clientInfo    = "<MacBookPro13,2> <macOS;13.1;22C65>"
                      " <com.apple.AuthKit/1 (com.apple.dt.Xcode/3594.4.19)>";
    a.serialNo      = srl;
    a.routingInfo   = "50660608";   /* observed routing info of this engine */
    a.userAgent     = "akd/1.0 CFNetwork/1404.0.5 Darwin/22.3.0";
    /* clientTime intentionally empty: gsa.cpp stamps it when building */

    free(mid); free(otp);
    return a;
#endif
}
