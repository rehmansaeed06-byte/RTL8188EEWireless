/*
 * ctl_rtl8188ee.c -- standalone diagnostic/control tool for the rtl8188ee kext.
 *
 * Extends ctl_getstate.c (which only called kRTL8188EEGetState) to cover the
 * full RTL8188EEUserClient selector table confirmed in src/macos/RTL8188EEUserClient.hpp
 * and src/macos/RTL8188EEUserClient.cpp:
 *
 *   kRTL8188EEScan        = 0   -- cmdScan(), no args
 *   kRTL8188EEConnect     = 1   -- cmdConnect(ssid, password), input = RTL8188EEConnectArgs
 *   kRTL8188EEDisconnect  = 2   -- cmdDisconnect(), no args
 *   kRTL8188EEGetState    = 3   -- cmdGetState(), output = RTL8188EEStateResult
 *   kRTL8188EEGetBSSList  = 4   -- cmdGetBSSList(), output = raw packed bytes,
 *                              wired up as `bsslist` -- see cmd_bsslist()'s
 *                              own header comment for the confirmed wire
 *                              format (findings.md Section 102)
 *   kRTL8188EEGetRSSI     = 5   -- not yet wired up here (output scalar; simplest
 *                              to add once needed, see NOTE below)
 *   kRTL8188EESetDebug    = 6   -- not yet wired up here (input scalar)
 *   kRTL8188EEGetLog      = 7   -- not yet wired up here (output = log bytes)
 *   kRTL8188EEPowerOn     = 8   -- cmdPowerOn(), no args
 *   kRTL8188EEPowerOff    = 9   -- cmdPowerOff(), no args
 *
 * This does NOT exist anywhere in the upstream repo, same as
 * ctl_getstate.c before it -- purely a diagnostic/bring-up tool, not
 * part of the committed project.
 *
 * Build:
 *   clang -o ctl_rtl8188ee ctl_rtl8188ee.c -framework IOKit -framework CoreFoundation
 *
 * Run:
 *   ./ctl_rtl8188ee state
 *   ./ctl_rtl8188ee poweron
 *   ./ctl_rtl8188ee scan
 *   ./ctl_rtl8188ee bsslist
 *   ./ctl_rtl8188ee connect "MySSID" "MyPassword"
 *   ./ctl_rtl8188ee disconnect
 *   ./ctl_rtl8188ee poweroff
 *
 * Typical first real association test, in order:
 *   ./ctl_rtl8188ee poweron
 *   ./ctl_rtl8188ee state        # confirm powered=1 before going further
 *   ./ctl_rtl8188ee scan
 *   ./ctl_rtl8188ee bsslist      # confirm real APs were actually found before
 *                             # trying to connect to one
 *   ./ctl_rtl8188ee connect "YourSSID" "YourPassword"
 *   ./ctl_rtl8188ee state        # watch `state` field transition, check bssid/rssi/channel
 *
 * NOTE on selectors 5-7 (GetRSSI / SetDebug / GetLog):
 * their exact I/O shapes weren't confirmed against RTL8188EEUserClient.cpp/
 * .hpp before this tool was written. Adding them is mechanical once
 * confirmed -- see the stubs left below. (GetBSSList, selector 4, WAS
 * in this category until this session -- see cmd_bsslist().)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

enum {
    kRTL8188EEScan        = 0,
    kRTL8188EEConnect     = 1,
    kRTL8188EEDisconnect  = 2,
    kRTL8188EEGetState    = 3,
    kRTL8188EEGetBSSList  = 4,
    kRTL8188EEGetRSSI     = 5,
    kRTL8188EESetDebug    = 6,
    kRTL8188EEGetLog      = 7,
    kRTL8188EEPowerOn     = 8,
    kRTL8188EEPowerOff    = 9,
};

/* Must match RTL8188EEUserClient.hpp exactly */
struct RTL8188EEConnectArgs {
    char ssid[33];
    char password[64];
};

struct RTL8188EEStateResult {
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
    CFMutableDictionaryRef matching = IOServiceMatching("RTL8188EEPCIDevice");
    if (!matching) {
        fprintf(stderr, "IOServiceMatching failed to build dictionary\n");
        return 1;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (!service) {
        fprintf(stderr, "No RTL8188EEPCIDevice service found in the IORegistry.\n");
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
    struct RTL8188EEStateResult result;
    memset(&result, 0, sizeof(result));
    size_t outSize = sizeof(result);

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTL8188EEGetState,
                                                  NULL, 0,
                                                  &result, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEGetState failed: 0x%x\n", kr);
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
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTL8188EEPowerOn,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEPowerOn failed: 0x%x\n", kr);
        return 1;
    }
    printf("PowerOn issued.\n");
    return 0;
}

static int cmd_poweroff(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTL8188EEPowerOff,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEPowerOff failed: 0x%x\n", kr);
        return 1;
    }
    printf("PowerOff issued.\n");
    return 0;
}

static int cmd_scan(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTL8188EEScan,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEScan failed: 0x%x\n", kr);
        return 1;
    }
    printf("Scan issued (async -- runManualScan() runs on a thread_call;\n"
           "poll `ctl_rtl8188ee state` or watch console log for scanDone()).\n");
    return 0;
}

/*
 * cmd_bsslist() -- kRTL8188EEGetBSSList (selector 4), previously left as a
 * stub (see this file's header comment) because the raw-bytes wire
 * format wasn't confirmed against source. Confirmed this session
 * directly against RTL8188EEIEEE80211::cmdGetBSSList() (src/macos/
 * RTL8188EEIEEE80211.cpp): kIOUCVariableStructureSize output, no input.
 *
 * Wire format (all fields packed, no padding, little-endian for the
 * multi-byte ones since this only ever runs on x86_64):
 *   [0:4)   uint32_t total_len   -- total bytes written, INCLUDING this
 *                                   4-byte prefix itself
 *   repeating entries until total_len bytes consumed:
 *     [0:1)     uint8_t  ssid_len
 *     [1:+N)    char     ssid[ssid_len]      -- NOT NUL-terminated on
 *                                                the wire; N = ssid_len
 *     [+0:+6)   uint8_t  bssid[6]
 *     [+0:+2)   int16_t  rssi                -- big-endian on the wire
 *                                                (kext writes high byte
 *                                                first: buf[written++] =
 *                                                (rssi>>8)&0xff, then
 *                                                rssi&0xff -- NOT a
 *                                                straight memcpy of a
 *                                                host-endian int16_t
 *                                                the way bssid/cipher
 *                                                are)
 *     [+0:+1)   uint8_t  channel
 *     [+0:+4)   uint32_t cipher              -- host-endian (straight
 *                                                memcpy on the kext side)
 *
 * Kext caps the whole buffer at 4095 bytes internally
 * (RTL8188EEIEEE80211::cmdGetBSSList: "if (max > 4095) max = 4095;") even
 * if a larger buffer is requested -- request exactly that size here
 * rather than the 16KB this file's header comment originally
 * speculated, since asking for more just wastes a stack buffer with
 * no benefit.
 */
static int cmd_bsslist(io_connect_t conn)
{
    uint8_t buf[4095];
    size_t outSize = sizeof(buf);

    kern_return_t kr = IOConnectCallStructMethod(conn, kRTL8188EEGetBSSList,
                                                  NULL, 0,
                                                  buf, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEGetBSSList failed: 0x%x\n", kr);
        return 1;
    }

    if (outSize < 4) {
        printf("(no BSS list data returned)\n");
        return 0;
    }

    uint32_t total_len;
    memcpy(&total_len, buf, 4);
    if (total_len > outSize)
        total_len = (uint32_t)outSize; /* defensive: never read past what we got */

    uint32_t off = 4;
    int count = 0;

    printf("%-4s %-33s %-17s %6s %4s %10s\n",
           "#", "SSID", "BSSID", "RSSI", "CH", "CIPHER");

    while (off + 1 <= total_len) {
        uint8_t ssid_len = buf[off];
        uint32_t entry_sz = 1 + ssid_len + 6 + 2 + 1 + 4;
        if (off + entry_sz > total_len) {
            fprintf(stderr, "(truncated entry at offset %u, stopping)\n", off);
            break;
        }

        const uint8_t *p = buf + off + 1;
        char ssid[34];
        memcpy(ssid, p, ssid_len);
        ssid[ssid_len] = '\0';
        p += ssid_len;

        uint8_t bssid[6];
        memcpy(bssid, p, 6);
        p += 6;

        /* Big-endian on the wire -- see this function's header comment. */
        int16_t rssi = (int16_t)(((int16_t)p[0] << 8) | p[1]);
        p += 2;

        uint8_t channel = p[0];
        p += 1;

        uint32_t cipher;
        memcpy(&cipher, p, 4);

        printf("%-4d %-33s %02x:%02x:%02x:%02x:%02x:%02x %6d %4u %#10x\n",
               count, ssid,
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
               rssi, channel, cipher);

        off += entry_sz;
        count++;
    }

    if (count == 0)
        printf("(no BSS entries -- scan may not have found anything yet,\n"
               " or hasn't been run: try `ctl_rtl8188ee scan` first and wait\n"
               " a few seconds before checking again)\n");

    return 0;
}

static int cmd_connect(io_connect_t conn, const char *ssid, const char *password)
{
    if (strlen(ssid) >= sizeof(((struct RTL8188EEConnectArgs *)0)->ssid)) {
        fprintf(stderr, "SSID too long (max %zu bytes)\n",
                sizeof(((struct RTL8188EEConnectArgs *)0)->ssid) - 1);
        return 1;
    }
    if (strlen(password) >= sizeof(((struct RTL8188EEConnectArgs *)0)->password)) {
        fprintf(stderr, "Password too long (max %zu bytes)\n",
                sizeof(((struct RTL8188EEConnectArgs *)0)->password) - 1);
        return 1;
    }

    struct RTL8188EEConnectArgs args;
    memset(&args, 0, sizeof(args));
    strncpy(args.ssid, ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);

    size_t outSize = 0;
    kern_return_t kr = IOConnectCallStructMethod(conn, kRTL8188EEConnect,
                                                  &args, sizeof(args),
                                                  NULL, &outSize);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEConnect failed: 0x%x\n", kr);
        return 1;
    }
    printf("Connect issued for SSID \"%s\".\n", ssid);
    return 0;
}

static int cmd_disconnect(io_connect_t conn)
{
    kern_return_t kr = IOConnectCallScalarMethod(conn, kRTL8188EEDisconnect,
                                                  NULL, 0, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "kRTL8188EEDisconnect failed: 0x%x\n", kr);
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
        "  %s bsslist\n"
        "  %s connect <ssid> <password>\n"
        "  %s disconnect\n",
        prog, prog, prog, prog, prog, prog, prog);
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
    } else if (strcmp(argv[1], "bsslist") == 0) {
        rc = cmd_bsslist(conn);
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
