/*
 * ctl_rtw88.c -- standalone diagnostic/control tool for the rtl8188ee kext.
 *
 * Extends ctl_getstate.c (which only called kRTW88GetState) to cover the
 * full RTW88UserClient selector table confirmed in src/kext/RTW88UserClient.hpp
 * and src/kext/RTW88UserClient.cpp:
 *
 *   kRTW88Scan        = 0   -- cmdScan(), no args
 *   kRTW88Connect     = 1   -- cmdConnect(ssid, password), input = RTW88ConnectArgs
 *   kRTW88Disconnect  = 2   -- cmdDisconnect(), no args
 *   kRTW88GetState    = 3   -- cmdGetState(), output = RTW88StateResult
 *   kRTW88GetBSSList  = 4   -- not yet wired up here (raw bytes, max 16KB;
 *                              layout not confirmed against source yet)
 *   kRTW88GetRSSI     = 5   -- not yet wired up here (output scalar; simplest
 *                              to add once needed, see NOTE below)
 *   kRTW88SetDebug    = 6   -- not yet wired up here (input scalar)
 *   kRTW88GetLog      = 7   -- not yet wired up here (output = log bytes)
 *   kRTW88PowerOn     = 8   -- cmdPowerOn(), no args
 *   kRTW88PowerOff    = 9   -- cmdPowerOff(), no args
 *
 * This does NOT exist anywhere in the upstream repo, same as
 * ctl_getstate.c before it -- purely a diagnostic/bring-up tool, not
 * part of the committed project.
 *
 * Build:
 *   clang -o ctl_rtw88 ctl_rtw88.c -framework IOKit -framework CoreFoundation
 *
 * Run:
 *   ./ctl_rtw88 state
 *   ./ctl_rtw88 poweron
 *   ./ctl_rtw88 scan
 *   ./ctl_rtw88 connect "MySSID" "MyPassword"
 *   ./ctl_rtw88 disconnect
 *   ./ctl_rtw88 poweroff
 *
 * Typical first real association test, in order:
 *   ./ctl_rtw88 poweron
 *   ./ctl_rtw88 state        # confirm powered=1 before going further
 *   ./ctl_rtw88 scan
 *   ./ctl_rtw88 connect "YourSSID" "YourPassword"
 *   ./ctl_rtw88 state        # watch `state` field transition, check bssid/rssi/channel
 *
 * NOTE on selectors 4-7 (GetBSSList / GetRSSI / SetDebug / GetLog):
 * their exact I/O shapes (scalar vs struct vs raw-bytes, and any
 * output struct layout for GetBSSList) weren't confirmed against
 * RTW88UserClient.cpp/.hpp before this tool was written. Adding them
 * is mechanical once confirmed -- see the stubs left below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

enum {
    kRTW88Scan        = 0,
    kRTW88Connect     = 1,
    kRTW88Disconnect  = 2,
    kRTW88GetState    = 3,
    kRTW88GetBSSList  = 4,
    kRTW88GetRSSI     = 5,
    kRTW88SetDebug    = 6,
    kRTW88GetLog      = 7,
    kRTW88PowerOn     = 8,
    kRTW88PowerOff    = 9,
};

/* Must match RTW88UserClient.hpp exactly */
struct RTW88ConnectArgs {
    char ssid[33];
    char password[64];
};

struct RTW88StateResult {
    uint32_t state;
    uint8_t  bssid[6];
    char     ssid[33];
    int32_t  rssi;
    uint32_t channel;
    uint8_t  mac_addr[6];
    uint16_t fw_version;
    uint8_t  fw_sub_version;
    char     chip_name[32];
    uint32_t rx_byte_count;
    uint32_t tx_byte_count;
    uint8_t  scan_offload_supported;
    uint8_t  powered;
};

/* ------------------------------------------------------------------ */
/*  Connection setup, shared by every subcommand                       */
/* ------------------------------------------------------------------ */

static int open_connection(io_connect_t *out_conn)
{
    CFMutableDictionaryRef matching = IOServiceMatching("RTW88PCIDevice");
    if (!matching) {
        fprintf(stderr, "IOServiceMatching failed to build dictionary\n");
        return 1;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (!service) {
        fprintf(stderr, "No RTW88PCIDevice service found in the IORegistry.\n");
        fprintf(stderr, "(This means the kext never matched/attached to a real PCI device.)\n");
        return 1;
    }

    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, out_conn);
    IOObjectRelease(service);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Pretty-printer, shared by every subcommand that wants state after  */
/* ------------------------------------------------------------------ */

static int print_state(io_connect_t conn)
{
    struct RTW88StateResult result;
    memset(&result, 0, sizeof(result));
    size_t outSize = sizeof(result);

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88GetState,
                                                  NULL, 0,
                                                  &result, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88GetState failed: 0x%x\n", kr);
        return 1;
    }

    printf("state:          %u\n", result.state);
    printf("chip_name:      %s\n", result.chip_name);
    printf("powered:        %u\n", result.powered);
    printf("mac_addr:       %02x:%02x:%02x:%02x:%02x:%02x\n",
           result.mac_addr[0], result.mac_addr[1], result.mac_addr[2],
           result.mac_addr[3], result.mac_addr[4], result.mac_addr[5]);
    printf("fw_version:     %u.%u\n", result.fw_version, result.fw_sub_version);
    printf("ssid:           %s\n", result.ssid);
    printf("bssid:          %02x:%02x:%02x:%02x:%02x:%02x\n",
           result.bssid[0], result.bssid[1], result.bssid[2],
           result.bssid[3], result.bssid[4], result.bssid[5]);
    printf("rssi:           %d\n", result.rssi);
    printf("channel:        %u\n", result.channel);
    printf("rx_byte_count:  %u\n", result.rx_byte_count);
    printf("tx_byte_count:  %u\n", result.tx_byte_count);
    printf("scan_offload:   %u\n", result.scan_offload_supported);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Subcommands                                                        */
/* ------------------------------------------------------------------ */

static int cmd_state(io_connect_t conn)
{
    return print_state(conn);
}

static int cmd_poweron(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88PowerOn,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88PowerOn failed: 0x%x\n", kr);
        return 1;
    }
    printf("PowerOn issued.\n");
    return 0;
}

static int cmd_poweroff(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88PowerOff,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88PowerOff failed: 0x%x\n", kr);
        return 1;
    }
    printf("PowerOff issued.\n");
    return 0;
}

static int cmd_scan(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88Scan,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88Scan failed: 0x%x\n", kr);
        return 1;
    }
    printf("Scan issued (async -- runManualScan() runs on a thread_call;\n"
           "poll `ctl_rtw88 state` or watch console log for scanDone()).\n");
    return 0;
}

static int cmd_connect(io_connect_t conn, const char *ssid, const char *password)
{
    if (strlen(ssid) >= sizeof(((struct RTW88ConnectArgs *)0)->ssid)) {
        fprintf(stderr, "SSID too long (max %zu bytes)\n",
                sizeof(((struct RTW88ConnectArgs *)0)->ssid) - 1);
        return 1;
    }
    if (strlen(password) >= sizeof(((struct RTW88ConnectArgs *)0)->password)) {
        fprintf(stderr, "Password too long (max %zu bytes)\n",
                sizeof(((struct RTW88ConnectArgs *)0)->password) - 1);
        return 1;
    }

    struct RTW88ConnectArgs args;
    memset(&args, 0, sizeof(args));
    strncpy(args.ssid, ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);

    size_t outSize = 0;
    kern_return_t kr = IOConnectCallStructMethod(conn, kRTW88Connect,
                                                  &args, sizeof(args),
                                                  NULL, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88Connect failed: 0x%x\n", kr);
        return 1;
    }
    printf("Connect issued for SSID \"%s\".\n", ssid);
    return 0;
}

static int cmd_disconnect(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTW88Disconnect,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTW88Disconnect failed: 0x%x\n", kr);
        return 1;
    }
    printf("Disconnect issued.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s state\n"
        "  %s poweron\n"
        "  %s poweroff\n"
        "  %s scan\n"
        "  %s connect <ssid> <password>\n"
        "  %s disconnect\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    io_connect_t conn;
    if (open_connection(&conn) != 0)
        return 1;

    int rc = 1;

    if (strcmp(argv[1], "state") == 0) {
        rc = cmd_state(conn);
    } else if (strcmp(argv[1], "poweron") == 0) {
        rc = cmd_poweron(conn);
    } else if (strcmp(argv[1], "poweroff") == 0) {
        rc = cmd_poweroff(conn);
    } else if (strcmp(argv[1], "scan") == 0) {
        rc = cmd_scan(conn);
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc != 4) {
            fprintf(stderr, "connect requires <ssid> <password>\n");
            usage(argv[0]);
            rc = 1;
        } else {
            rc = cmd_connect(conn, argv[2], argv[3]);
        }
    } else if (strcmp(argv[1], "disconnect") == 0) {
        rc = cmd_disconnect(conn);
    } else {
        usage(argv[0]);
        rc = 1;
    }

    IOServiceClose(conn);
    return rc;
}
