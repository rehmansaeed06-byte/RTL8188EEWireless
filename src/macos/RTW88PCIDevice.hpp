/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTW88PCIDevice.hpp — IOEthernetController subclass for PCIe rtw88 chips.
 *
 * Approach mirrors itlwm: present a transparent Ethernet interface to macOS
 * while doing 802.11 management internally.  The 802.11 state machine lives
 * in RTW88IEEE80211; this class handles IOKit life-cycle and the Ethernet
 * framing visible to macOS network stack.
 */
#pragma once

/* Pull in mbuf_t and related kernel types before any IOKit network headers */
#include <sys/kernel_types.h>

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IOMbufMemoryCursor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>

/* Forward declarations */
class RTW88IEEE80211;
class RTW88UserClient;

/* RTW88PCIDevice --------------------------------------------------------- */
class RTW88PCIDevice : public IOEthernetController {
    OSDeclareDefaultStructors(RTW88PCIDevice)

    friend class RTW88UserClient;
    friend class RTW88IEEE80211;

public:
    /* IOService */
    bool     init(OSDictionary *props) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    void     free() override;
    IOReturn powerStateWillChangeTo(IOPMPowerFlags flags, unsigned long state,
                                     IOService *actor) override;

    /* IONetworkController */
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;
    IOReturn enable(IONetworkInterface *iface) override;
    IOReturn disable(IONetworkInterface *iface) override;
    IOReturn setMaxPacketSize(UInt32 maxSize) override;
    IOReturn getMaxPacketSize(UInt32 *maxSize) const override;
    IOReturn selectMedium(const IONetworkMedium *medium) override;
    bool     configureInterface(IONetworkInterface *iface) override;
    UInt32   outputPacket(mbuf_t m, void *param) override;
    /* Provide a gated output queue so outputPacket() is serialized through the
     * work loop.  Without this the stack calls outputPacket() concurrently,
     * racing rtw_pci_tx_write_data's BD fill (which reads ring->r.wp before the
     * irq_lock) and leaving a zeroed descriptor that stalls the TX DMA engine. */
    IOOutputQueue *createOutputQueue() override;

    /* IOEthernetController */
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn setHardwareAddress(const IOEthernetAddress *addr) override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
    IOReturn setPromiscuousMode(bool active) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;

    /* IOUserClient creation */
    IOReturn newUserClient(task_t owningTask, void *securityID,
                           UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;

    /* Called from interrupt handler */
    void handleInterrupt(IOInterruptEventSource *src, int count);

    /* Resume a flow-control-stalled output queue once the IRQ bottom-half has
     * freed BE ring slots.  Called via a C trampoline from the compat layer. */
    void resumeTxIfStalled();

    /* Called from RTW88IEEE80211 to deliver RX frames to macOS */
    void injectRxFrame(mbuf_t m);
    /* The workloop the RX/interrupt path runs on. RTW88IEEE80211 attaches its
     * RX reorder flush timer here so all frame delivery is serialized on one
     * thread (injectRxFrame's queue+flush is not safe against concurrent
     * callers). */
    IOWorkLoop *getRxWorkLoop() const { return _workLoop; }
    /* Allocate an input mbuf via the IONetworkController allocator (sets
     * m_len and pkthdr.len consistently — required for inputPacket). */
    mbuf_t allocateInputPacket(uint32_t len);

    /* DMA helpers — used by Linux compat dma_alloc_coherent */
    void *allocCoherent(size_t size, IOPhysicalAddress *phys);
    void  freeCoherent(size_t size, void *virt, IOPhysicalAddress phys);
    void  freeCoherentByPhys(IOPhysicalAddress phys);
    /* Bounce buffer helpers for dma_map_single / dma_sync_single_for_cpu */
    void  setBounceOrigVA(IOPhysicalAddress phys, void *orig_va);
    void  syncBounceForCpu(IOPhysicalAddress dma, size_t size);

    /* PCI config space — used by Linux compat pci_read/write_config_* */
    UInt8  pciReadByte(int offset);
    UInt16 pciReadWord(int offset);
    UInt32 pciReadDword(int offset);
    void   pciWriteByte(int offset, UInt8 val);
    void   pciWriteWord(int offset, UInt16 val);
    void   pciWriteDword(int offset, UInt32 val);
    int    pciFindCapability(int cap);

    /* 802.11 state machine accessors */
    RTW88IEEE80211 *get80211() { return _ieee80211; }

    /* MMIO base — used by compat ioremap shim */
    volatile void *mmioBase() const { return _mmioBase; }

private:
    bool     attachDevice();
    bool     setupInterrupt();
    bool     setupDMA();
    void     teardown();
    const char *chipDisplayName() const;
    void     publishHardwareIdentity();

    bool     setupMediumDict();
    void     addMedium(OSDictionary *mediums, IOMediumType type, UInt64 speed);

    static void interruptOccurred(OSObject *owner,
                                   IOInterruptEventSource *src, int count);

    void debugTimerFired(IOTimerEventSource *src);

    IOPCIDevice            *_pciDev       = nullptr;
    IOMemoryMap            *_mmioMap      = nullptr;
    volatile void          *_mmioBase     = nullptr;
    IOWorkLoop             *_workLoop     = nullptr;
    IOCommandGate          *_cmdGate      = nullptr;
    IOInterruptEventSource *_intrSrc      = nullptr;
    IOTimerEventSource     *_debugTimer   = nullptr;
    IOEthernetInterface    *_iface        = nullptr;
    IOGatedOutputQueue     *_txQueue      = nullptr;

    RTW88IEEE80211         *_ieee80211    = nullptr;
    RTW88UserClient        *_userClient   = nullptr;

    IOEthernetAddress       _macAddr;
    bool                    _enabled      = false;
    bool                    _initialized  = false;

    /* TX flow control: set when outputPacket() stalls the gated queue because
     * the BE ring is nearly full; cleared when the IRQ completion path frees
     * slots and resumes the queue.  See createOutputQueue()/outputPacket(). */
    volatile bool           _txStalled    = false;

    /* Linked-list of allocated DMA buffers for cleanup */
    struct DMAEntry {
        IOBufferMemoryDescriptor *desc;
        void    *virt;        /* kernel VA of the bounce/coherent buffer */
        IOPhysicalAddress phys;
        size_t   size;
        void    *orig_va;     /* original CPU VA for bounce mappings (NULL=coherent) */
        DMAEntry *next;
    };
    DMAEntry *_dmaList        = nullptr;
    IOSimpleLock *_dmaLock    = nullptr;

    /* Entries whose desc->complete()/release() was deferred because
     * preemption was disabled at free time (TX-completion interrupt path).
     * Drained by drainPendingFree() from preemption-enabled contexts. */
    DMAEntry     *_dmaPendingFree    = nullptr;
    IOSimpleLock *_pendingFreeLock   = nullptr;
    void          drainPendingFree();

    /* Back-pointer passed to compat layer */
    struct pci_dev *_compatPciDev = nullptr;
};
