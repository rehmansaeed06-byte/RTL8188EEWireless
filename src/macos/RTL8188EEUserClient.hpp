/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTL8188EEUserClient.hpp — IOUserClient for rtw88ctl IPC
 *
 * Selector numbers must match ctl/main.c RTL8188EE_CMD_* defines.
 */
#pragma once

#include <IOKit/IOUserClient.h>

class RTL8188EEPCIDevice;

/* ------------------------------------------------------------------ */
/*  Selector constants — keep in sync with ctl/main.c                  */
/* ------------------------------------------------------------------ */
enum RTL8188EEUserClientSelector {
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
    kRTL8188EENumSelectors
};

/* Structures passed through IOConnectCallStructMethod */
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
class RTL8188EEUserClient : public IOUserClient {
    OSDeclareDefaultStructors(RTL8188EEUserClient)

public:
    static RTL8188EEUserClient *create(RTL8188EEPCIDevice *dev, task_t owningTask);

    bool     init(OSDictionary *props) override;
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool     initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    void     free() override;

    /* IOUserClient */
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;

private:
    /* Dispatch table */
    static IOReturn sScan(RTL8188EEUserClient *target, void *ref,
                          IOExternalMethodArguments *args);
    static IOReturn sConnect(RTL8188EEUserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sDisconnect(RTL8188EEUserClient *target, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sGetState(RTL8188EEUserClient *target, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sGetBSSList(RTL8188EEUserClient *target, void *ref,
                                IOExternalMethodArguments *args);
    static IOReturn sGetRSSI(RTL8188EEUserClient *target, void *ref,
                              IOExternalMethodArguments *args);
    static IOReturn sSetDebug(RTL8188EEUserClient *target, void *ref,
                               IOExternalMethodArguments *args);
    static IOReturn sGetLog(RTL8188EEUserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sPowerOn(RTL8188EEUserClient *target, void *ref,
                             IOExternalMethodArguments *args);
    static IOReturn sPowerOff(RTL8188EEUserClient *target, void *ref,
                              IOExternalMethodArguments *args);

    static const IOExternalMethodDispatch sMethods[kRTL8188EENumSelectors];

    RTL8188EEPCIDevice *_provider    = nullptr;
    task_t          _owningTask  = nullptr;
};
