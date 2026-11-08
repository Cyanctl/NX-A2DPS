#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <sys/stat.h>
#include "sbc_receiver.h"
#include "native_bridge.h"
#include "native_sink.h"

static FILE *report;
static u64 bluetooth_pid;
static LoaderModuleInfo bluetooth_module;
static NativeSink sink;

static void inspect_bluetooth_module(void);
static void snapshot_bluetooth_code(void);

static void logline(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (report) {
        va_start(ap, fmt);
        vfprintf(report, fmt, ap);
        va_end(ap);
        fflush(report);
    }
}

static void result(const char *name, Result rc) {
    logline("%-32s 0x%08lX\n", name, (unsigned long)rc);
}

static void probe(void) {
    logline("Switch Audio Sink 0.11.0 (experimental native backend)\n\n");
    u32 hos = hosversionGet();
    logline("HOS: %lu.%lu.%lu   Atmosphere: %s\n", (unsigned long)((hos >> 16) & 255),
        (unsigned long)((hos >> 8) & 255), (unsigned long)(hos & 255),
        hosversionIsAtmosphere() ? "yes" : "no");
    Result rc = setsysInitialize();
    result("setsysInitialize", rc);
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw = {0};
        rc = setsysGetFirmwareVersion(&fw);
        result("setsysGetFirmwareVersion", rc);
        if (R_SUCCEEDED(rc)) logline("Firmware: %.24s\n", fw.display_version);
        setsysExit();
    }
    rc = btmInitialize();
    result("btmInitialize", rc);
    if (R_SUCCEEDED(rc)) {
        BtmState state = 0;
        rc = btmGetState(&state);
        result("btmGetState", rc);
        if (R_SUCCEEDED(rc)) logline("BTM state: %u\n", (unsigned)state);
        BtmHostDeviceProperty host = {0};
        rc = btmGetHostDeviceProperty(&host);
        result("btmGetHostDeviceProperty", rc);
        if (R_SUCCEEDED(rc)) {
            const char *name = hosversionAtLeast(13,0,0) ? host.v13.name : host.v1.name.name;
            logline("Host name: %.31s\n", name);
        }
        if (hosversionAtLeast(13,0,0)) {
            BtmDeviceCondition devices[8] = {0};
            s32 count = 0;
            rc = btmGetDeviceCondition(BtmProfile_Audio, devices, 8, &count);
            result("btmGetDeviceCondition(Audio)", rc);
            if (R_SUCCEEDED(rc)) logline("BT audio device count: %ld\n", (long)count);
        }
        btmExit();
    }
    rc = btdrvInitialize();
    result("btdrvInitialize", rc);
    if (R_SUCCEEDED(rc)) {
        if (hosversionAtLeast(12,0,0)) {
            BtdrvAdapterProperty property = {0};
            rc = btdrvGetAdapterProperty(BtdrvAdapterPropertyType_ClassOfDevice, &property);
            result("btdrvGetAdapterProperty(CoD)", rc);
            if (R_SUCCEEDED(rc) && property.size >= 3)
                logline("Class of device: %02X%02X%02X\n", property.data[2], property.data[1], property.data[0]);
        }
        btdrvExit();
    }
    u8 mc_present = 0;
    SmServiceName mc_name = smEncodeName("mc");
    rc = hosversionIsAtmosphere()
        ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, mc_name, mc_present)
        : MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    result("Atmosphere HasService(mc)", rc);
    if (R_SUCCEEDED(rc) && mc_present) {
        Service mc = {0};
        rc = smGetService(&mc, "mc");
        result("smGetService(mc)", rc);
        if (R_SUCCEEDED(rc)) {
            u32 version = 0;
            rc = serviceDispatchOut(&mc, 0, version);
            result("MissionControl GetVersion", rc);
            if (R_SUCCEEDED(rc)) logline("MissionControl version raw: 0x%08lX\n", (unsigned long)version);
            serviceClose(&mc);
        }
    }
    inspect_bluetooth_module();
    int failures = sbc_receiver_selftest();
    logline("RTP/SBC selftest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    failures = audio_pipeline_selftest();
    logline("Audio pipeline selftest: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    logline("\nNo Bluetooth reset, pairing or profile changes performed.\n");
    logline("Known libnx audio IPC exposes output; no receive API.\n");
    logline("A: audio  B: native bridge test  X: code snapshot\nY: USB transfer  PLUS: exit\n");
    logline("ZL: start native sink  ZR: stop native sink\n");
    logline("Report: sdmc:/switch/switch-audio-probe/report.txt\n");
}

static void inspect_bluetooth_module(void) {
    Result rc = pmdmntInitialize();
    result("pmdmntInitialize", rc);
    if (R_FAILED(rc)) return;
    // Atmosphere SystemProgramId::Bluetooth, verified against ncm IDs.
    rc = pmdmntGetProcessId(&bluetooth_pid, 0x010000000000000BULL);
    result("Bluetooth process ID", rc);
    pmdmntExit();
    if (R_FAILED(rc)) return;
    rc = ldrDmntInitialize();
    result("ldrDmntInitialize", rc);
    if (R_FAILED(rc)) return;
    LoaderModuleInfo modules[8] = {0};
    s32 count = 0;
    rc = ldrDmntGetProcessModuleInfo(bluetooth_pid, modules, 8, &count);
    result("Bluetooth module metadata", rc);
    ldrDmntExit();
    if (R_FAILED(rc) || count <= 0 || count > 8) return;
    for (s32 i = 0; i < count; i++) {
        logline("Module %ld base=%016lX size=%lX build=", (long)i,
            (unsigned long)modules[i].base_address, (unsigned long)modules[i].size);
        for (size_t j = 0; j < sizeof(modules[i].build_id); j++) logline("%02X", modules[i].build_id[j]);
        logline("\n");
        if (modules[i].size > bluetooth_module.size) bluetooth_module = modules[i];
    }
}

static void snapshot_bluetooth_code(void) {
    // Copy only executable and read-only mappings. Heap, writable data, BSS,
    // pairing keys and other running-process state are deliberately excluded.
    // No process-memory writes, breakpoints or executable patches are used.
    if (!bluetooth_pid || !bluetooth_module.size || bluetooth_module.size > 32 * 1024 * 1024) {
        logline("No bounded Bluetooth module available to snapshot.\n");
        return;
    }
    if (!envIsSyscallHinted(0x60) || !envIsSyscallHinted(0x69) || !envIsSyscallHinted(0x6A)) {
        logline("Debug/read syscalls are not available in this loader.\n");
        return;
    }
    size_t size = bluetooth_module.size;
    u8 *copy = calloc(1, size);
    if (!copy) { logline("Snapshot allocation failed.\n"); return; }
    logline("Snapshotting BT code; debug attachment is temporary.\n");
    consoleUpdate(NULL);
    Handle debug = INVALID_HANDLE;
    Result rc = svcDebugActiveProcess(&debug, bluetooth_pid);
    size_t copied = 0;
    if (R_SUCCEEDED(rc)) {
        u64 base = bluetooth_module.base_address;
        u64 cursor = base;
        u64 end = base + size;
        // While attached, use only memory and SVC operations. In particular do
        // not call services or write to SD while Bluetooth is suspended.
        while (cursor < end) {
            MemoryInfo info = {0};
            u32 page = 0;
            rc = svcQueryDebugProcessMemory(&info, &page, debug, cursor);
            if (R_FAILED(rc)) break;
            u64 next = info.addr + info.size;
            if (next <= cursor) { rc = MAKERESULT(Module_Libnx, LibnxError_BadInput); break; }
            if (next > end) next = end;
            if ((info.perm & Perm_R) && !(info.perm & Perm_W)) {
                rc = svcReadDebugProcessMemory(copy + (cursor - base), debug, cursor, next - cursor);
                if (R_FAILED(rc)) break;
                copied += next - cursor;
            }
            cursor = next;
        }
        // Closing the last debug handle detaches and resumes the process.
        svcCloseHandle(debug);
    }
    result("Bluetooth code snapshot", rc);
    if (R_SUCCEEDED(rc) && copied) {
        const char *path = "sdmc:/switch/switch-audio-probe/bluetooth-code.bin";
        FILE *file = fopen(path, "wb");
        if (!file) logline("Could not create code snapshot.\n");
        else {
            size_t written = fwrite(copy, 1, size, file);
            int closed = fclose(file);
            logline("Snapshot bytes read=%lu written=%lu/%lu closed=%d\n",
                (unsigned long)copied, (unsigned long)written, (unsigned long)size, closed);
            logline("Snapshot module base=%016lX; writable regions are zero.\n",
                (unsigned long)bluetooth_module.base_address);
        }
    }
    free(copy);
}

static void test_audio(void) {
    Result rc = audoutInitialize();
    result("audoutInitialize", rc);
    if (R_FAILED(rc)) return;
    u32 rate = audoutGetSampleRate();
    u32 channels = audoutGetChannelCount();
    logline("Output: %lu Hz / %lu channels / PCM format %u\n", (unsigned long)rate,
        (unsigned long)channels, (unsigned)audoutGetPcmFormat());
    if (!rate || !channels || channels > 8 || audoutGetPcmFormat() != PcmFormat_Int16) {
        logline("Unsupported test output format.\n");
        audoutExit();
        return;
    }
    size_t frames = rate / 2;
    size_t bytes = frames * channels * sizeof(s16);
    size_t allocation = (bytes + 0xfff) & ~(size_t)0xfff;
    s16 *pcm = memalign(0x1000, allocation);
    if (!pcm) { logline("Audio allocation failed.\n"); audoutExit(); return; }
    memset(pcm, 0, allocation);
    for (size_t i = 0; i < frames; i++) {
        // Quiet half-second tone with 10 ms ramps to avoid clicks.
        double ramp = fmin(1.0, fmin((double)i / (rate / 100),
                                    (double)(frames - 1 - i) / (rate / 100)));
        s16 value = (s16)(1800.0 * ramp * sin(2.0 * 3.141592653589793 * 440.0 * i / rate));
        for (u32 ch = 0; ch < channels; ch++) pcm[i * channels + ch] = value;
    }
    AudioOutBuffer buffer = { .buffer = pcm, .buffer_size = allocation, .data_size = bytes };
    rc = audoutAppendAudioOutBuffer(&buffer);
    result("audoutAppendAudioOutBuffer", rc);
    if (R_SUCCEEDED(rc)) {
        rc = audoutStartAudioOut();
        result("audoutStartAudioOut", rc);
        if (R_SUCCEEDED(rc)) {
            AudioOutBuffer *released = NULL;
            u32 count = 0;
            rc = audoutWaitPlayFinish(&released, &count, 2000000000ULL);
            result("audoutWaitPlayFinish", rc);
            if (R_SUCCEEDED(rc)) logline("Released buffers: %lu\n", (unsigned long)count);
        }
        audoutStopAudioOut();
    }
    // Closing the service releases any queued buffer before freeing storage.
    audoutExit();
    free(pcm);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    mkdir("sdmc:/switch/switch-audio-probe", 0777);
    report = fopen("sdmc:/switch/switch-audio-probe/report.txt", "w");
    if (!report) printf("Could not open SD report file.\n");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    probe();
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_ZL) {
            Result rc = native_sink_start(&sink, bluetooth_pid, &bluetooth_module, report);
            result("Start native sink", rc);
        }
        if (down & HidNpadButton_ZR) result("Stop native sink", native_sink_stop(&sink));
        if (down & HidNpadButton_Plus) {
            Result rc = native_sink_stop(&sink);
            if (R_SUCCEEDED(rc)) break;
            result("Cleanup pending", rc);
        }
        if ((down & HidNpadButton_A) && !sink.active) test_audio();
        if ((down & HidNpadButton_B) && !sink.active) {
            int failures = native_bridge_test(bluetooth_pid, &bluetooth_module, report);
            logline("Native bridge test: %s\n", failures ? "FAIL" : "PASS");
        }
        if (down & HidNpadButton_X) snapshot_bluetooth_code();
        if (down & HidNpadButton_Y) {
            Result rc = native_sink_stop(&sink);
            if (R_SUCCEEDED(rc)) {
                envSetNextLoad("sdmc:/switch/haze.nro", "sdmc:/switch/haze.nro");
                break;
            }
            result("Cleanup pending", rc);
        }
        Result rc = native_sink_pump(&sink);
        if (R_FAILED(rc)) {
            result("Sink pump", rc);
            result("Sink cleanup", native_sink_stop(&sink));
        }
        consoleUpdate(NULL);
    }
    native_sink_stop(&sink);
    if (report) fclose(report);
    consoleExit(NULL);
    return 0;
}
