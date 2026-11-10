/* SPDX-License-Identifier: Apache-2.0 */
/* Based on https://github.com/osy/ThunderboltPatcher */

#include "AppleHPMLib.h"
#include "ssops.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFString.h>
#include <Availability.h>

// kIOMasterPortDefault was deprecated in macOS 12.0; keep the old name for
// builds against older SDKs.
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 120000
#define MAIN_PORT kIOMainPortDefault
#else
#define MAIN_PORT kIOMasterPortDefault
#endif

static bool g_verbose = false;
static int g_timeout_ms = 3000;

struct failure : public std::runtime_error {
    failure(const char *x) : std::runtime_error(x)
    {
    }

    failure(const std::string &x) : std::runtime_error(x)
    {
    }
};

static std::string hexerr(const char *what, IOReturn ret)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "%s (0x%x)", what, (unsigned)ret);
    return msg;
}

struct IOObjectDeleter {
    io_object_t arg;

    IOObjectDeleter(io_object_t arg) : arg(arg)
    {
    }

    ~IOObjectDeleter()
    {
        if (arg)
            IOObjectRelease(arg);
    }
};

// The original code constructed the deleter with a copy of `iter` while it was
// still 0, so the iterator created afterwards was never released. Holding a
// pointer to the variable fixes that.
struct IOIteratorDeleter {
    io_iterator_t *it;

    IOIteratorDeleter(io_iterator_t *it) : it(it)
    {
    }

    ~IOIteratorDeleter()
    {
        if (*it)
            IOObjectRelease(*it);
    }
};

static uint8_t byteAt(const std::string &s, size_t i)
{
    return i < s.size() ? (uint8_t)s[i] : 0;
}

struct HPMPluginInstance {
    IOCFPlugInInterface **plugin = nullptr;
    AppleHPMLib **device = nullptr;
    int rid = 0;

    // Ports that were only probed never entered DBMa mode; asking them to exit
    // it is pointless and noisy on newer hosts.
    bool exitDBMaOnClose = true;

    HPMPluginInstance(io_service_t service, int rid) : rid(rid)
    {
        SInt32 score;
        IOReturn ret = IOCreatePlugInInterfaceForService(service, kAppleHPMLibType,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
        if (ret != kIOReturnSuccess)
            throw failure(hexerr("IOCreatePlugInInterfaceForService failed (run as root?)", ret));

        HRESULT res = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kAppleHPMLibInterface),
                                               (LPVOID *)&device);
        if (res != S_OK)
            throw failure("QueryInterface failed");
    }

    ~HPMPluginInstance()
    {
        if (plugin) {
            if (exitDBMaOnClose) {
                printf("Exiting DBMa mode... ");
                if (this->command(0, 'DBMa', "\x00") == 0)
                    printf("OK\n");
                else
                    printf("Failed\n");
            }
            IODestroyPlugInInterface(plugin);
        }
    }

    std::string readRegister(uint64_t chipAddr, uint8_t dataAddr, int flags = 0)
    {
        std::string ret;
        ret.resize(64);
        uint64_t rlen = 0;
        IOReturn x = (*device)->Read(device, chipAddr, dataAddr, &ret[0], 64, flags, &rlen);
        if (x != 0)
            throw failure(hexerr("readRegister failed", x));
        if (rlen > 0 && rlen < ret.size())
            ret.resize((size_t)rlen);
        return ret;
    }

    void writeRegister(uint64_t chipAddr, uint8_t dataAddr, const std::string &value)
    {
        if (value.empty())
            return;
        IOReturn x = (*device)->Write(device, chipAddr, dataAddr, value.data(), value.length(), 0);
        if (x != 0)
            throw failure(hexerr("writeRegister failed", x));
    }

    int command(uint64_t chipAddr, uint32_t cmd, const std::string &args = "")
    {
        if (args.length()) {
            IOReturn w = (*device)->Write(device, chipAddr, 9, args.data(), args.length(), 0);
            if (w != 0)
                throw failure(hexerr("command argument write failed", w));
        }

        auto ret = (*device)->Command(device, chipAddr, cmd, 0);
        if (ret) {
            if (g_verbose)
                fprintf(stderr, "Command '%c' rejected by the driver: 0x%x\n",
                        (char)cmd, (unsigned)ret);
            return -1;
        }

        int status = byteAt(this->readRegister(chipAddr, 9), 0) & 0xfu;
        if (status && g_verbose)
            fprintf(stderr, "Command '%c' returned status %d\n", (char)cmd, status);
        return status;
    }
};

uint32_t GetUnlockKey()
{
    CFMutableDictionaryRef matching = IOServiceMatching("IOPlatformExpertDevice");
    if (!matching)
        throw failure("IOServiceMatching failed (IOPED)");

    io_iterator_t iter = 0;
    IOIteratorDeleter iterDel(&iter);
    io_service_t service = IOServiceGetMatchingService(MAIN_PORT, matching);
    if (!service)
        throw failure("IOServiceGetMatchingService failed (IOPED)");

    IOObjectDeleter deviceDel(service);

    // Zero-initialised: models whose registry name is shorter than 4 characters
    // used to read uninitialised stack bytes here, so the unlock key changed
    // from run to run.
    io_name_t deviceName = {};
    if (IORegistryEntryGetName(service, deviceName) != kIOReturnSuccess)
        throw failure("IORegistryEntryGetName failed (IOPED)");

    uint32_t key = 0;
    for (size_t i = 0; i < 4; i++)
        key = (key << 8) | (unsigned char)deviceName[i];

    printf("Host type: %s\n", deviceName);
    if (g_verbose)
        printf("Unlock key: 0x%08x\n", key);

    return key;
}

struct HPMDevice {
    io_service_t service;
    int rid;
    std::string path;
};

static std::vector<HPMDevice> ListHPMDevices()
{
    std::vector<HPMDevice> out;

    CFMutableDictionaryRef matching = IOServiceMatching("AppleHPM");
    if (!matching)
        throw failure("IOServiceMatching failed");

    io_iterator_t iter = 0;
    IOIteratorDeleter iterDel(&iter);
    if (IOServiceGetMatchingServices(MAIN_PORT, matching, &iter) != kIOReturnSuccess)
        throw failure("IOServiceGetMatchingServices failed");

    io_service_t device;
    while ((device = IOIteratorNext(iter))) {
        io_string_t pathName = {};
        if (IORegistryEntryGetPath(device, kIOServicePlane, pathName) != kIOReturnSuccess) {
            fprintf(stderr, "Failed to get device path\n");
            IOObjectRelease(device);
            continue;
        }

        int rid = -1;
        CFNumberRef data = (CFNumberRef)IORegistryEntryCreateCFProperty(device, CFSTR("RID"),
                                                        kCFAllocatorDefault, 0);
        if (data) {
            CFNumberGetValue(data, kCFNumberSInt32Type, &rid);
            CFRelease(data);
        } else if (g_verbose) {
            // A missing RID used to abort the whole scan; skip it instead.
            fprintf(stderr, "Skipping HPM object without RID: %s\n", pathName);
        }

        IOObjectRetain(device); // keep a reference for the selection pass
        out.push_back({device, rid, pathName});
        IOObjectRelease(device); // drop the iterator's reference
    }

    // RID 0 is the DFU port on most Macs, but not all of them: on the M4 Mac
    // mini the DFU-capable port is the middle rear one, RID 1. Prefer RID 0,
    // then the remaining ports in ascending RID order.
    std::sort(out.begin(), out.end(), [](const HPMDevice &a, const HPMDevice &b) {
        int ka = (a.rid == 0) ? 0 : (a.rid > 0 ? 1 : 2);
        int kb = (b.rid == 0) ? 0 : (b.rid > 0 ? 1 : 2);
        if (ka != kb)
            return ka < kb;
        return a.rid < b.rid;
    });

    return out;
}

static bool IsConnected(HPMPluginInstance &inst)
{
    try {
        return (byteAt(inst.readRegister(0, 0x3f), 0) & 1) != 0;
    } catch (failure &) {
        return false;
    }
}

std::unique_ptr<HPMPluginInstance> FindDevice(int requestedRid)
{
    auto devices = ListHPMDevices();
    if (devices.empty())
        throw failure("No AppleHPM devices found (is this an Apple Silicon Mac?)");

    printf("Looking for HPM devices...\n");

    std::unique_ptr<HPMPluginInstance> chosen, fallback;

    for (const auto &d : devices) {
        if (requestedRid >= 0 && d.rid != requestedRid)
            continue;

        std::unique_ptr<HPMPluginInstance> inst;
        try {
            inst = std::make_unique<HPMPluginInstance>(d.service, d.rid);
        } catch (failure &e) {
            if (requestedRid >= 0)
                throw; // an explicitly requested port must work
            if (g_verbose)
                printf("Skipping RID %d: %s\n", d.rid, e.what());
            continue;
        }

        bool connected = IsConnected(*inst);
        printf("Found: RID=%d %s%s\n", d.rid, d.path.c_str(), connected ? " (connected)" : "");

        if (connected) {
            chosen = std::move(inst);
            break;
        }

        inst->exitDBMaOnClose = false;
        if (!fallback)
            fallback = std::move(inst);
    }

    for (auto &d : devices)
        IOObjectRelease(d.service);

    if (!chosen) {
        if (requestedRid >= 0)
            throw failure("No AppleHPM device with RID " + std::to_string(requestedRid));
        if (!fallback)
            throw failure("No matching devices");
        printf("No port reported a connection, using RID %d\n", fallback->rid);
        chosen = std::move(fallback);
    }

    return chosen;
}

static std::string statusString(HPMPluginInstance &inst, int no)
{
    auto res = inst.readRegister(no, 0x03);
    size_t nul = res.find('\0');
    if (nul != std::string::npos)
        res.resize(nul);
    return res;
}

void UnlockAce(HPMPluginInstance &inst, int no, uint32_t key)
{
    printf("Unlocking... ");
    std::stringstream args;
    put(args, key);
    if (inst.command(no, 'LOCK', args.str())) {
        printf(" Failed.\n");
        printf("Trying to reset... ");
        if (inst.command(no, 'Gaid')) {
            printf("Failed.\n");
            throw failure("Failed to unlock device (wrong unlock key for this host?)");
        }
        printf("OK.\nUnlocking... ");
        if (inst.command(no, 'LOCK', args.str())) {
            printf(" Failed.\n");
            throw failure("Failed to unlock device");
        }
    }

    printf("OK\n");
}

void DoVDM(HPMPluginInstance &inst, int no, const std::vector<uint32_t> &vdm)
{
    auto before = inst.readRegister(no, 0x4d);
    uint8_t rxst = byteAt(before, 0);

    std::stringstream args;
    put(args, (uint8_t)(((3 << 4) | vdm.size())));
    for (uint32_t i : vdm)
        put(args, i);

    if (inst.command(no, 'VDMs', args.str()))
        throw failure("Failed to send VDM");

    /* Poll with a time budget instead of a fixed number of back-to-back reads:
     * controllers that post the reply after a few hundred microseconds lose the
     * race against 16 immediate reads. */
    std::string rs;
    int waited = 0;
    while (waited < g_timeout_ms) {
        rs = inst.readRegister(no, 0x4d);
        if (byteAt(rs, 0) != rxst)
            break;
        usleep(2000);
        waited += 2;
    }

    if (waited >= g_timeout_ms)
        throw failure("Timed out waiting for a VDM reply (" + std::to_string(g_timeout_ms) + "ms); "
                      "replug the cable if this setup used to work");

    uint32_t vdmhdr;
    std::stringstream reply;
    reply.str(rs);
    get(reply, rxst);
    get(reply, vdmhdr);

    if (vdmhdr != (vdm[0] | 0x40)) {
        printf("VDM failed (reply: 0x%08x)\n", vdmhdr);
        throw failure("VDM failed");
    }
}

int DoSerial(HPMPluginInstance &inst, int no)
{
    printf("Putting target into serial mode... ");

    std::vector<uint32_t> serial{0x5ac8012, 0x1840306};
    DoVDM(inst, no, serial);

    printf("OK\n");

    printf("Putting local end into serial mode... ");

    std::stringstream args;
    put(args, (uint32_t)0x1840306);

    int status = inst.command(no, 'DVEn', args.str());
    if (status) {
        printf("Failed (status %d).\n", status);
        printf("Recent Apple Silicon hosts (M4/M5, macOS 26+) reject the local serial\n"
               "routing command. Use 'reboot debugusb' and /dev/cu.kis-100000-ch-0 instead.\n");
        return 1;
    }
    printf("OK\n");

    return 0;
}

int DoDebugUSB(HPMPluginInstance &inst, int no)
{
    printf("Switching target into debug USB mode... ");

    std::vector<uint32_t> dfu{0x5ac8012, 0x1824606};
    DoVDM(inst, no, dfu);

    printf("OK\n");
    return 0;
}

int DoReboot(HPMPluginInstance &inst, int no)
{
    printf("Rebooting target into normal mode... ");

    std::vector<uint32_t> reboot{0x5ac8012, 0x105, 0x80000000};
    DoVDM(inst, no, reboot);

    printf("OK\n");
    return 0;
}

int WaitConnected(HPMPluginInstance &inst, int no)
{
    printf("Waiting for connection...");
    fflush(stdout);

    sleep(1);
    for (int i = 0; i < 100; i++) {
        printf(".");
        fflush(stdout);
        if (byteAt(inst.readRegister(no, 0x3f), 0) & 1) {
            printf(" Connected\n");
            sleep(1);
            return 0;
        }
        usleep(100000);
    }

    printf(" Timed out\n");
    return 1;
}

int DoDFU(HPMPluginInstance &inst, int no)
{
    printf("Rebooting target into DFU mode... ");

    std::vector<uint32_t> dfu{0x5ac8012, 0x106, 0x80010000};
    DoVDM(inst, no, dfu);

    printf("OK\n");
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [-p|--rid <rid>] [-t <seconds>] [-v] <command> [argument]\n", prog);
    printf("Commands:\n");
    printf("  serial            enter serial mode on both ends\n");
    printf("  debugusb          enter debugusb mode on the target\n");
    printf("  reboot            reboot the target\n");
    printf("  reboot serial     reboot the target and enter serial mode\n");
    printf("  reboot debugusb   reboot the target and enter debugusb mode\n");
    printf("  dfu               put the target into DFU mode\n");
    printf("  status            print the ports and the connection state\n");
    printf("  nop               do nothing\n");
    printf("Options:\n");
    printf("  -p, --rid <rid>   HPM port RID to use (default: auto-detect the connected port)\n");
    printf("  -t, --timeout <s> VDM reply timeout in seconds (default: 3)\n");
    printf("  -v, --verbose     print IOKit status codes and skipped ports\n");
    printf("Environment: MACVDMTOOL_RID=<rid> is equivalent to --rid.\n");
}

int main2(int argc, char **argv)
{
    int rid = -1; // -1 = auto-detect
    if (const char *env = getenv("MACVDMTOOL_RID"))
        rid = (int)strtol(env, nullptr, 0);

    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];

        if (a == "-v" || a == "--verbose") {
            g_verbose = true;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "-p" || a == "--rid") {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", a.c_str());
                return 1;
            }
            rid = (int)strtol(argv[++i], nullptr, 0);
            if (rid < 0) {
                fprintf(stderr, "invalid RID\n");
                return 1;
            }
        } else if (a == "-t" || a == "--timeout") {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", a.c_str());
                return 1;
            }
            long s = strtol(argv[++i], nullptr, 10);
            if (s <= 0 || s > 60) {
                fprintf(stderr, "invalid timeout\n");
                return 1;
            }
            g_timeout_ms = (int)s * 1000;
        } else {
            break; // first non-option argument is the command
        }
    }

    if (i >= argc) {
        usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[i];
    std::string arg = (i + 1 < argc) ? argv[i + 1] : "";

    uint32_t key = GetUnlockKey();

    auto inst = FindDevice(rid);
    int no = 0;

    auto t = inst->readRegister(no, 0x3f);
    bool connected = (byteAt(t, 0) & 1) != 0;
    std::string type = connected ? ((byteAt(t, 0) & 2) == 0 ? "Source" : "Sink") : "None";
    printf("Connection: %s\n", type.c_str());

    if (!connected)
        throw failure("No connection detected on RID " + std::to_string(inst->rid) +
                      ". Check the cable (SuperSpeed USB-C is required for serial mode) "
                      "and which port is the DFU port on this model.");

    std::string status = statusString(*inst, no);
    printf("Status: %s\n", status.c_str());

    if (status != "DBMa") {
        UnlockAce(*inst, no, key);
        printf("Entering DBMa mode... ");

        if (inst->command(no, 'DBMa', "\x01")) {
            printf("Failed.\n");
            throw failure("Failed to enter DBMa mode");
        }

        status = statusString(*inst, no);
        printf("Status: %s\n", status.c_str());
        if (status != "DBMa")
            throw failure("Failed to enter DBMa mode");
    }

    if (cmd == "serial")
        return DoSerial(*inst, no);
    else if (cmd == "debugusb")
        return DoDebugUSB(*inst, no);
    else if (cmd == "reboot") {
        bool do_serial = arg == "serial";
        bool do_debugusb = arg == "debugusb";

        if (!arg.empty() && !do_serial && !do_debugusb) {
            printf("Unknown reboot argument '%s'\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }

        int reboot = DoReboot(*inst, no);
        if (reboot)
            return 1;

        if (do_serial || do_debugusb) {
            if (WaitConnected(*inst, no))
                return 1;
            if (do_serial)
                return DoSerial(*inst, no);
            else if (do_debugusb)
                return DoDebugUSB(*inst, no);
        }
        return 0;
    } else if (cmd == "dfu")
        return DoDFU(*inst, no);
    else if (cmd == "status" || cmd == "nop")
        return 0;

    printf("Unknown command\n");
    usage(argv[0]);
    return 1;
}

int main(int argc, char **argv)
{
    // This makes sure we call the HPMPluginInstance destructor.
    try {
        return main2(argc, argv);
    } catch (failure e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
