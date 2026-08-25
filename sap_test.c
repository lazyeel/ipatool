/*
 * Copyright 2026 lazyeel (https://github.com/lazyeel)
 * SPDX-License-Identifier: Apache-2.0
 */

/* sap_test.c v27 — FPDICreate: minimal args, NULL session, only AttestationMode
 *
 * KEY INSIGHT: -44660 (not -44650) means PlatformInit DID bootstrap FPDI.
 * The problem is in what we pass to Create.
 *
 * From JavaCPP:
 *   FPDICreate(@ByPtrPtr FPDIContextRef session,  → void** (in/out)
 *              FPDIAttrRef attr,                    → void* (input)
 *              @ByPtrPtr BytePointer initRequest,   → void** (output)
 *              IntPointer initRequestLength)        → uint32_t* (output)
 *
 * Let's try the SIMPLEST possible call:
 *   session = {NULL} — let Create allocate
 *   attr = from AttrInit + SetAttestationMode ONLY
 *   initRequest = {NULL} — output
 *   len = {0} — output
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

static void segfault_handler(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    fprintf(stderr, "\n[SIGSEGV] fault: %p pc: %p\n", info->si_addr,
#ifdef __aarch64__
        (void*)uc->uc_mcontext.pc);
#else
        (void*)uc->uc_mcontext.gregs[16]);
#endif
    _exit(139);
}


/* ── Callback implementations ── */
static int platform_id_impl(uint8_t *uid, uint32_t *len) {
    printf("  [cb] PlatformIDCallback called (uid=%p, len=%p)\n", uid, len);
    const char *id = "0123456789ABCDEF";
    size_t id_len = strlen(id);
    if (*len >= id_len && uid) {
        memcpy(uid, id, id_len);
        *len = id_len;
        printf("  [cb] Returned device ID: %s (%zu bytes)\n", id, id_len);
        return 0;
    }
    if (len) *len = id_len;
    printf("  [cb] Buffer too small, need %zu bytes\n", id_len);
    return -1;
}

static int drm_challenge_impl(uint8_t *push_box, int push_box_size,
                               uint8_t *drm_license_challenge,
                               uint32_t *drm_license_challenge_size) {
    printf("  [cb] DRMLicenseChallengeCallback called (pushBox=%p, size=%d)\n", 
           push_box, push_box_size);
    if (drm_license_challenge_size) {
        *drm_license_challenge_size = 0;
        printf("  [cb] Returning empty challenge\n");
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *libdir_new = "./libs-new";
    const char *libdir_classic = "./libs-classic";
    char path_buf[512];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segfault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    printf("[sap] v27: minimal FPDICreate\n");

    snprintf(path_buf, sizeof(path_buf), "%s/libc++_shared.so", libdir_new);
    if (!dlopen(path_buf, RTLD_NOW | RTLD_GLOBAL)) { fprintf(stderr, "[sap] libc++\n"); return 1; }

    const char *classic_deps[] = {
        "libBlocksRuntime.so", "libdispatch.so",
        "libCoreFoundation.so",
        "libicudata_sv_apple.so", "libicuuc_sv_apple.so", "libicui18n_sv_apple.so",
        "libxml2.so", NULL };
    for (int i = 0; classic_deps[i]; i++) {
        snprintf(path_buf, sizeof(path_buf), "%s/%s", libdir_classic, classic_deps[i]);
        if (!dlopen(path_buf, RTLD_NOW | RTLD_LOCAL))
            fprintf(stderr, "[sap] preload %s\n", classic_deps[i]);
    }

    const char *new_deps[] = {
        "libCoreFoundation.so",
        "libmediaplatform.so",
        "libCoreADI.so", "libCoreLSKD.so", "libCoreFP.so",
        "libFPDIFor3P.so", "libdaapkit.so",
        "libcrashlytics-common.so", "libcrashlytics-handler.so",
        "libcrashlytics-trampoline.so", "libcrashlytics.so",
        "librenderscript-toolkit.so", "libandroidx.graphics.path.so",
        "libmedialibrarycore.so",
        "libandroidappmusic.so", "libstoreapi.so", NULL };

    for (int i = 0; new_deps[i]; i++) {
        snprintf(path_buf, sizeof(path_buf), "%s/%s", libdir_new, new_deps[i]);
        if (!dlopen(path_buf, RTLD_NOW | RTLD_LOCAL)) {
            fprintf(stderr, "[sap] %s: %s\n", new_deps[i], dlerror());
            if (!strstr(new_deps[i], "mediaplatform") &&
                !strstr(new_deps[i], "medialibrary") &&
                !strstr(new_deps[i], "androidappmusic")) return 1;
        } else printf("[sap] loaded %s\n", new_deps[i]);
    }

    void *h = dlopen("./libs-new/libstoreapi.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) { fprintf(stderr, "[sap] storeapi not loaded\n"); return 1; }

    #define DLSYM(var, name) \
        void *var = dlsym(h, name); \
        printf("[sap] %-30s = %p\n", name, var); \
        if (!var) { fprintf(stderr, "[sap] MISSING: %s\n", name); return 1; }

    DLSYM(fp_loadlib,       "N8jdR29h")
    DLSYM(fp_setid,         "bsawCXd")
    DLSYM(fpdi_platforminit,"RhsJgiCAMX")
    DLSYM(fpdi_attrinit,    "jsf09djfs0df")
    DLSYM(fpdi_create,      "d2234hmbdf")
    DLSYM(set_attest,       "RXm4IJLE3xR")

    /* ── Phase A: LoadLib + SetID ── */
    printf("\n[sap] === PHASE A ===\n");
    mkdir("./fairplay-data", 0755);
    static char fp_data[512];
    realpath("./fairplay-data", fp_data);

    int rc = ((int (*)(const char *))fp_loadlib)(fp_data);
    printf("[A1] LoadLib => %d\n", rc);
    rc = ((int (*)(const char *, uint32_t))fp_setid)("0123456789ABCDEF", 16);
    printf("[A2] SetID => %d\n", rc);

    /* ── Phase B: PlatformInit with callbacks ── */
    printf("\n[sap] === PHASE B: PlatformInit ===\n");
    
    struct { 
        int (*platform_id)(uint8_t *, uint32_t *);
        int (*drm_challenge)(uint8_t *, int, uint8_t *, uint32_t *);
    } callbacks = {
        .platform_id = platform_id_impl,
        .drm_challenge = drm_challenge_impl
    };

    rc = ((int (*)(void *))fpdi_platforminit)(&callbacks);
    printf("[B1] PlatformInit(callbacks) => %d\n", rc);

    /* ── Phase C: Minimal attributes ── */
    printf("\n[sap] === PHASE C: Attributes ===\n");
    
    void *attr = NULL;
    rc = ((int (*)(void **))fpdi_attrinit)(&attr);
    printf("[C1] AttrInit => %d, attr=%p\n", rc, attr);
    
    rc = ((int (*)(void *, uint32_t))set_attest)(attr, 2);
    printf("[C2] AttestationMode(2) => %d\n", rc);

    /* Dump attr contents for debugging */
    uint32_t *f = (uint32_t *)attr;
    printf("[dbg] attr: ");
    for (int i = 0; i < 10; i++) printf("%08x ", f[i]);
    printf("\n");

    /* ── Phase D: Multiple FPDICreate attempts ── */
    printf("\n[sap] === PHASE D: FPDICreate variants ===\n");
    
    void *init_req = NULL;
    uint32_t req_len = 0;

    /* Variant 1: session=NULL (let Create allocate), minimal attrs */
    printf("\n[D1] create({NULL}, attr, &req, &len)\n");
    void *session = NULL;
    typedef int (*create_v1_t)(void **, void *, void **, uint32_t *);
    rc = ((create_v1_t)fpdi_create)(&session, attr, &init_req, &req_len);
    printf("  => %d (session=%p, req=%p, len=%u)\n", rc, session, init_req, req_len);
    
    /* Dump attr state after Create */
    f = (uint32_t *)attr;
    printf("[dbg] attr after create: ");
    for (int i = 0; i < 10; i++) printf("%08x ", f[i]);
    printf("\n");

    /* Variant 2: session=NULL pointer directly (not address of NULL) */
    printf("\n[D2] create(NULL, attr, &req, &len)\n");
    rc = ((create_v1_t)fpdi_create)(NULL, attr, &init_req, &req_len);
    printf("  => %d (req=%p, len=%u)\n", rc, init_req, req_len);

    /* Variant 3: Only 2 args (session, attr) — maybe request is optional? */
    printf("\n[D3] create(&session, attr) [only 2 args]\n");
    typedef int (*create_v3_t)(void **, void *);
    session = NULL;
    rc = ((create_v3_t)fpdi_create)(&session, attr);
    printf("  => %d (session=%p)\n", rc, session);

    /* Variant 4: Try with SAPInit context handle as session */
    printf("\n[D4] create(sap_ctx_as_session, attr, &req, &len)\n");
    uint64_t sap_ctx = 0;
    /* First do SAPInit */
    void *fp_sapinit = dlsym(h, "cp2g1b9ro");
    if (fp_sapinit) {
        static uint8_t hw_buf[512]; memset(hw_buf, 0, sizeof(hw_buf));
        void *getguid_fn = dlsym(h, "FKgu8fbnvGFG");
        rc = ((int (*)(char*,int))getguid_fn)((char*)hw_buf, sizeof(hw_buf));
        
        typedef int (*sapinit_t)(uint64_t*, void*);
        rc = ((sapinit_t)fp_sapinit)(&sap_ctx, hw_buf);
        printf("  SAPInit => %d, ctx=%lu\n", rc, (unsigned long)sap_ctx);
        
        /* Use SAP context as session for FPDICreate */
        session = (void*)(uintptr_t)sap_ctx;
        rc = ((create_v1_t)fpdi_create)(&session, attr, &init_req, &req_len);
        printf("  => %d (req=%p, len=%u)\n", rc, init_req, req_len);
    }

    /* Variant 5: Try calling with the JNI wrapper that includes full marshaling */
    printf("\n[D5] create via JNI wrapper\n");
    void *create_wrapper = dlsym(h,
        "Java_com_apple_android_music_foothill_javanative_FPDIInterface_00024Companion_FPDICreate");
    if (create_wrapper) {
        printf("  wrapper at %p\n", create_wrapper);
        /* Build fake env with NewByteArray and other needed functions */
        /* ... this gets complex. Skip for now. */
        printf("  Skipping complex JNI wrapper call.\n");
    }

    printf("\n[sap] done.\n");
    return 0;
}
