// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// RTL8188EEKext.cpp — top-level IOService, delegates to PCI or USB device classes

#include "RTL8188EEKext.hpp"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(RTL8188EEKext, IOService)

bool RTL8188EEKext::init(OSDictionary *props)
{
    IOLog("rtw88: RTL8188EEKext::init\n");
    return super::init(props);
}

IOService *RTL8188EEKext::probe(IOService *provider, SInt32 *score)
{
    IOLog("rtw88: RTL8188EEKext::probe\n");
    return super::probe(provider, score);
}

bool RTL8188EEKext::start(IOService *provider)
{
    IOLog("rtw88: RTL8188EEKext::start\n");
    if (!super::start(provider)) return false;
    registerService();
    return true;
}

void RTL8188EEKext::stop(IOService *provider)
{
    IOLog("rtw88: RTL8188EEKext::stop\n");
    super::stop(provider);
}

void RTL8188EEKext::free()
{
    super::free();
}
