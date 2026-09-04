/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTL8188EEKext.hpp — top-level IOService provider matching
 */
#pragma once

#include <IOKit/IOService.h>

class RTL8188EEKext : public IOService {
    OSDeclareDefaultStructors(RTL8188EEKext)

public:
    bool init(OSDictionary *props) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOService *probe(IOService *provider, SInt32 *score) override;
};
