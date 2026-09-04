// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// RTL8188EEUserClient.cpp — IOUserClient implementation

#include "RTL8188EEUserClient.hpp"
#include "RTL8188EEPCIDevice.hpp"
#include "RTL8188EEIEEE80211.hpp"

#include <IOKit/IOLib.h>
#include <string.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RTL8188EEUserClient, IOUserClient)

/* ------------------------------------------------------------------ */
/*  Dispatch table                                                      */
/* ------------------------------------------------------------------ */

const IOExternalMethodDispatch RTL8188EEUserClient::sMethods[kRTL8188EENumSelectors] = {
    /* kRTL8188EEScan */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sScan,
      0, 0, 0, 0 },
    /* kRTL8188EEConnect: input struct = RTL8188EEConnectArgs */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sConnect,
      0, sizeof(RTL8188EEConnectArgs), 0, 0 },
    /* kRTL8188EEDisconnect */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sDisconnect,
      0, 0, 0, 0 },
    /* kRTL8188EEGetState: output struct = RTL8188EEStateResult */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sGetState,
      0, 0, 0, sizeof(RTL8188EEStateResult) },
    /* kRTL8188EEGetBSSList: output = raw bytes, max 16KB */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sGetBSSList,
      0, 0, 0, kIOUCVariableStructureSize },
    /* kRTL8188EEGetRSSI: output scalar */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sGetRSSI,
      0, 0, 1, 0 },
    /* kRTL8188EESetDebug: input scalar = debug level */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sSetDebug,
      1, 0, 0, 0 },
    /* kRTL8188EEGetLog: output = log bytes */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sGetLog,
      0, 0, 0, kIOUCVariableStructureSize },
    /* kRTL8188EEPowerOn */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sPowerOn,
      0, 0, 0, 0 },
    /* kRTL8188EEPowerOff */
    { (IOExternalMethodAction)&RTL8188EEUserClient::sPowerOff,
      0, 0, 0, 0 },
};

/* ------------------------------------------------------------------ */
/*  Factory                                                             */
/* ------------------------------------------------------------------ */

RTL8188EEUserClient *RTL8188EEUserClient::create(RTL8188EEPCIDevice *dev, task_t owningTask)
{
    RTL8188EEUserClient *uc = new RTL8188EEUserClient;
    if (uc && !uc->init(nullptr)) { uc->release(); return nullptr; }
    if (uc) { uc->_provider = dev; uc->_owningTask = owningTask; }
    return uc;
}

/* ------------------------------------------------------------------ */
/*  IOService lifecycle                                                 */
/* ------------------------------------------------------------------ */

bool RTL8188EEUserClient::init(OSDictionary *props)
{
    return super::init(props);
}

bool RTL8188EEUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties)
{
    _owningTask = owningTask;
    if (!super::initWithTask(owningTask, securityID, type, properties)) {
        IOLog("rtw88: RTL8188EEUserClient::initWithTask(4) super failed\n");
        return false;
    }
    return true;
}

bool RTL8188EEUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    _owningTask = owningTask;
    if (!super::initWithTask(owningTask, securityID, type)) {
        IOLog("rtw88: RTL8188EEUserClient::initWithTask(3) super failed\n");
        return false;
    }
    return true;
}

bool RTL8188EEUserClient::start(IOService *provider)
{
    if (!super::start(provider)) {
        IOLog("rtw88: RTL8188EEUserClient::start() super::start failed\n");
        return false;
    }
    _provider = OSDynamicCast(RTL8188EEPCIDevice, provider);
    if (!_provider) {
        IOLog("rtw88: RTL8188EEUserClient::start() OSDynamicCast to RTL8188EEPCIDevice failed\n");
        return false;
    }
    return true;
}

void RTL8188EEUserClient::stop(IOService *provider)
{
    super::stop(provider);
}

void RTL8188EEUserClient::free()
{
    super::free();
}

IOReturn RTL8188EEUserClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/*  externalMethod dispatch                                             */
/* ------------------------------------------------------------------ */

IOReturn RTL8188EEUserClient::externalMethod(uint32_t selector,
                                          IOExternalMethodArguments *args,
                                          IOExternalMethodDispatch *dispatch,
                                          OSObject *target, void *reference)
{
    if (selector >= kRTL8188EENumSelectors)
        return kIOReturnUnsupported;

    const IOExternalMethodDispatch *d = &sMethods[selector];
    return super::externalMethod(selector, args,
                                  const_cast<IOExternalMethodDispatch *>(d),
                                  this, nullptr);
}

/* ------------------------------------------------------------------ */
/*  Individual selectors                                                */
/* ------------------------------------------------------------------ */

IOReturn RTL8188EEUserClient::sScan(RTL8188EEUserClient *uc, void *ref,
                                  IOExternalMethodArguments *args)
{
    if (!uc->_provider || !uc->_provider->get80211()) return kIOReturnOffline;
    return uc->_provider->get80211()->cmdScan();
}

IOReturn RTL8188EEUserClient::sConnect(RTL8188EEUserClient *uc, void *ref,
                                     IOExternalMethodArguments *args)
{
    IOLog("rtw88: sConnect() ENTERED\n");
    if (!uc->_provider || !uc->_provider->get80211()) return kIOReturnOffline;
    if (!args->structureInput || args->structureInputSize < sizeof(RTL8188EEConnectArgs))
        return kIOReturnBadArgument;

    const RTL8188EEConnectArgs *ca = (const RTL8188EEConnectArgs *)args->structureInput;
    return uc->_provider->get80211()->cmdConnect(ca->ssid, ca->password);
}

IOReturn RTL8188EEUserClient::sDisconnect(RTL8188EEUserClient *uc, void *ref,
                                        IOExternalMethodArguments *args)
{
    if (!uc->_provider || !uc->_provider->get80211()) return kIOReturnOffline;
    return uc->_provider->get80211()->cmdDisconnect();
}

IOReturn RTL8188EEUserClient::sGetState(RTL8188EEUserClient *uc, void *ref,
                                      IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize < sizeof(RTL8188EEStateResult))
        return kIOReturnBadArgument;

    RTL8188EEStateResult *result = (RTL8188EEStateResult *)args->structureOutput;
    memset(result, 0, sizeof(*result));

    if (!uc->_provider || !uc->_provider->get80211()) {
        strlcpy(result->chip_name, "Uninitialized", sizeof(result->chip_name));
        args->structureOutputSize = sizeof(*result);
        return kIOReturnSuccess;
    }

    IOReturn ret = uc->_provider->get80211()->cmdGetState(result);
    if (ret != kIOReturnSuccess) return ret;

    args->structureOutputSize = sizeof(*result);
    return kIOReturnSuccess;
}

IOReturn RTL8188EEUserClient::sGetBSSList(RTL8188EEUserClient *uc, void *ref,
                                        IOExternalMethodArguments *args)
{
    if (!args->structureOutput) return kIOReturnBadArgument;
    if (!uc->_provider || !uc->_provider->get80211()) {
        args->structureOutputSize = 0;
        return kIOReturnSuccess;
    }

    uint32_t len = (uint32_t)args->structureOutputSize;
    IOReturn ret = uc->_provider->get80211()->cmdGetBSSList(
        (uint8_t *)args->structureOutput, &len);
    args->structureOutputSize = len;
    return ret;
}

IOReturn RTL8188EEUserClient::sGetRSSI(RTL8188EEUserClient *uc, void *ref,
                                     IOExternalMethodArguments *args)
{
    if (!args->scalarOutput || args->scalarOutputCount < 1) return kIOReturnBadArgument;
    if (!uc->_provider || !uc->_provider->get80211()) {
        args->scalarOutput[0] = (uint64_t)(int64_t)-100;
        return kIOReturnSuccess;
    }
    int rssi = -100;
    IOReturn ret = uc->_provider->get80211()->cmdGetRSSI(&rssi);
    args->scalarOutput[0] = (uint64_t)(int64_t)rssi;
    return ret;
}

IOReturn RTL8188EEUserClient::sSetDebug(RTL8188EEUserClient *uc, void *ref,
                                      IOExternalMethodArguments *args)
{
    if (args->scalarInputCount >= 1) {
        extern int rtw88_log_level;
        rtw88_log_level = (int)args->scalarInput[0];
    }
    return kIOReturnSuccess;
}

extern "C" {
    uint32_t rtw88_read_log(char *out_buf, uint32_t max_len);
}

IOReturn RTL8188EEUserClient::sGetLog(RTL8188EEUserClient *uc, void *ref,
                                    IOExternalMethodArguments *args)
{
    if (!args->structureOutput || args->structureOutputSize == 0)
        return kIOReturnBadArgument;

    uint32_t max_len = (uint32_t)args->structureOutputSize;
    uint32_t read = rtw88_read_log((char *)args->structureOutput, max_len);
    
    args->structureOutputSize = read;
    return kIOReturnSuccess;
}

IOReturn RTL8188EEUserClient::sPowerOn(RTL8188EEUserClient *uc, void *ref,
                                   IOExternalMethodArguments *args)
{
    if (!uc->_provider || !uc->_provider->get80211()) return kIOReturnOffline;
    return uc->_provider->get80211()->cmdPowerOn();
}

IOReturn RTL8188EEUserClient::sPowerOff(RTL8188EEUserClient *uc, void *ref,
                                    IOExternalMethodArguments *args)
{
    if (!uc->_provider || !uc->_provider->get80211()) return kIOReturnOffline;
    return uc->_provider->get80211()->cmdPowerOff();
}
