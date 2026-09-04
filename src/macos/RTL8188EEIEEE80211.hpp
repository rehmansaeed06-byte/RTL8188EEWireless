/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * RTL8188EEIEEE80211.hpp — 802.11 state machine for rtw88 macOS port.
 *
 * Responsibilities:
 *  - Drives the compiled-in rtlwifi driver (hw->ops->start/stop/tx)
 *  - Manages scan, authenticate, associate, 4-way handshake
 *  - Converts between mbuf_t and sk_buff for the driver
 *  - Delivers decrypted data frames as Ethernet to RTL8188EEPCIDevice
 *  - Accepts Ethernet output frames and wraps them as 802.11 data frames
 */
#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <sys/mbuf.h>
#include <net/ethernet.h>
#include <kern/thread_call.h>

/* Opaque C driver handle */
struct rtl_priv;
/* struct pci_dev / struct pci_device_id: real, complete definitions —
 * NOT forward declarations — pulled from src/compat/linux/pci.h.
 * Bucket B, findings.md Section 81.2/83.2/85: RTL8188EEIEEE80211.cpp::start()
 * dereferences _pcidev->device/->vendor and builds a real
 * `const struct pci_device_id fake_id = {...}` designated-initializer
 * value, both of which need the complete type — a forward declaration
 * alone (what used to be here) compiles the pointer but not the member
 * access or the struct literal. No prior file in this project's
 * `#include` chain actually pulls in linux/pci.h (confirmed by grep —
 * this project's own rtlwifi_compat.h does not), so this
 * incomplete-type error was real, not a chain-ordering accident.
 */
#include "../compat/linux/pci.h"
struct ieee80211_hw;
struct ieee80211_vif;
struct ieee80211_sta;
struct ieee80211_channel;
struct sk_buff;

class RTL8188EEPCIDevice;

/* ------------------------------------------------------------------ */
/*  BSS descriptor (scan result)                                        */
/* ------------------------------------------------------------------ */
struct RTL8188EEBSS {
    char   ssid[33];
    uint8_t ssid_len;
    uint8_t bssid[6];
    int16_t rssi;
    uint16_t freq;
    uint8_t  channel;
    uint32_t capabilities;
    uint32_t cipher;       /* selected pairwise WLAN_CIPHER_SUITE_* */
    uint32_t group_cipher; /* selected group WLAN_CIPHER_SUITE_* */
    uint32_t akm;
    uint32_t last_seen_scan;
    /* Raw IE data for association */
    uint8_t  ies[512];
    uint16_t ies_len;
    RTL8188EEBSS *next;
};

/* ------------------------------------------------------------------ */
/*  Connection state                                                     */
/* ------------------------------------------------------------------ */
enum RTL8188EEState {
    RTL8188EE_STATE_IDLE = 0,
    RTL8188EE_STATE_SCANNING,
    RTL8188EE_STATE_AUTHENTICATING,
    RTL8188EE_STATE_ASSOCIATING,
    RTL8188EE_STATE_HANDSHAKING,
    RTL8188EE_STATE_CONNECTED,
    RTL8188EE_STATE_DISCONNECTING,
};

/* ------------------------------------------------------------------ */
/*  RTL8188EEIEEE80211                                                       */
/* ------------------------------------------------------------------ */
class RTL8188EEIEEE80211 : public OSObject {
    OSDeclareDefaultStructors(RTL8188EEIEEE80211)

public:
    static RTL8188EEIEEE80211 *create(RTL8188EEPCIDevice *dev, struct pci_dev *pci);

    bool      init(RTL8188EEPCIDevice *dev, struct pci_dev *pci);
    void      free() override;

    /* Called by RTL8188EEPCIDevice */
    IOReturn  start();       /* probe: chip info, efuse, register hw */
    void      stop();        /* full teardown */
    IOReturn  powerOn();     /* enable: hw->ops->start() */
    void      powerOff();    /* disable: rtw_core_stop */
    void      handleInterrupt();
    UInt32    outputPacket(mbuf_t m);
    void      getMACAddress(uint8_t *mac);

    /* Called from compat layer (ieee80211_rx_irqsafe) */
    void      rxFrame(struct sk_buff *skb);
    void      txStatus(struct sk_buff *skb);
    void      scanDone(bool aborted);

    /* Control interface — called from RTL8188EEUserClient */
    IOReturn  cmdScan();
    IOReturn  cmdConnect(const char *ssid, const char *password);
    IOReturn  cmdDisconnect();
    IOReturn  cmdPowerOn();
    IOReturn  cmdPowerOff();
    IOReturn  cmdGetState(struct RTL8188EEStateResult *result);
    IOReturn  cmdGetBSSList(uint8_t *buf, uint32_t *len);
    IOReturn  cmdGetRSSI(int *rssi);

private:
    /* State machine internals */
    void      processRxMgmt(struct sk_buff *skb);
    void      processRxData(struct sk_buff *skb);
    void      processScanResult(struct sk_buff *skb);
    void      processAssocResponse(struct sk_buff *skb);

    void      doAuthenticate();
    void      doAssociate();
    void      setConnectedChandef(struct ieee80211_channel *chan);
    void      doHandshake(const uint8_t *eapol, uint32_t len);
    void      doDisconnect();
    void      clearKeys();
    void      releaseSta();
    bool      abortActiveScan(bool waitForIdle);
    void      restoreConnectedChannel();
    bool      installKey(struct ieee80211_key_conf **slot, bool pairwise,
                         uint8_t keyidx, uint32_t cipher,
                         const uint8_t *tk, uint8_t tk_len);

    bool      buildAssocReq(uint8_t *buf, uint32_t *len);
    bool      buildAuthReq(uint8_t *buf, uint32_t *len);

    /* WPA2 4-way handshake */
    void      handleEAPOL(const uint8_t *data, uint32_t len);
    bool      deriveKeys(const uint8_t *anonce, const uint8_t *snonce);
    void      sendEAPOLKey(int step, const uint8_t *replay_counter,
                            bool install, bool ack, bool mic);

    /* A-MPDU BlockAck (aggregation) negotiation */
    bool      htAllowed() const;   /* HT/VHT/A-MPDU usable on this link? */
    void      startTxAggregation();
    void      sendAddbaRequest(uint8_t tid);
    void      sendAddbaResponse(uint8_t tid, uint8_t dialog,
                                uint16_t req_param, uint16_t ba_timeout);
    void      handleBackAction(const uint8_t *b, uint32_t len);

    /* RX A-MPDU reorder + delivery */
    void      deliverDataFrame(struct sk_buff *skb);   /* strip 802.11, inject */
    void      deAmsdu(const uint8_t *data, uint32_t len);
    void      deliverEthernet(const uint8_t *da, const uint8_t *sa,
                              uint16_t ethertype,
                              const uint8_t *payload, uint32_t paylen);
    void      rxBaSetup(uint8_t tid, uint16_t ssn, uint16_t bufsize);
    void      rxBaTeardown(uint8_t tid);
    void      rxBaTeardownAll();
    void      rxReorderInput(uint8_t tid, struct sk_buff *skb, uint16_t sn);
    void      rxReorderArmTimer();
    void      rxReorderFlushStale();
    static void reorderTimerFired(OSObject *owner, IOTimerEventSource *t);

    /* Frame transmission helpers */
    bool      txMgmtFrame(const uint8_t *frame, uint32_t len);
    bool      txNullFunc(bool powerSave);
    bool      txProbeRequest();
    bool      txDataFrame(mbuf_t m);
    struct sk_buff *mbufToSkb(mbuf_t m);
    mbuf_t    skbToMbuf(struct sk_buff *skb);

    /* Driver callbacks installed in rtw_dev */
    static void compat_rx_frame(void *kext_hw, struct sk_buff *skb);
    static void compat_tx_status(void *kext_hw, struct sk_buff *skb);
    static void compat_scan_done(void *kext_hw, bool aborted);

    /* Timer callback for state machine timeouts */
    static void timerFired(OSObject *owner, IOTimerEventSource *timer);
    void        onTimer();

    /* Connect thread_call — runs doAuthenticate off the IOUserClient thread */
    static void connectTCFn(thread_call_param_t self, thread_call_param_t);
    thread_call_t _connectTC = nullptr;

    /* Manual passive scan fallback for chips/firmware without scan offload */
    static void manualScanTCFn(thread_call_param_t self, thread_call_param_t);
    void        runManualScan();
    thread_call_t _manualScanTC = nullptr;

    /* ---------------------------------------------------------------- */
    RTL8188EEPCIDevice    *_parent        = nullptr;
    struct rtl_priv   *_rtwdev        = nullptr;
    struct ieee80211_hw *_hw          = nullptr;
    struct ieee80211_vif *_vif        = nullptr;
    struct ieee80211_sta *_sta        = nullptr;
    size_t              _staAllocSize = 0;
    struct pci_dev     *_pcidev       = nullptr;

    IOWorkLoop         *_wl           = nullptr;
    IOCommandGate      *_gate         = nullptr;
    IOTimerEventSource *_timer        = nullptr;
    IOLock             *_lock         = nullptr;

    RTL8188EEState          _state        = RTL8188EE_STATE_IDLE;
    RTL8188EEState          _scanReturnState = RTL8188EE_STATE_IDLE;
    bool                _powered      = false;
    uint8_t             _macAddr[6]   = {};
    uint32_t            _timeoutMs    = 0;

    /* Scan results */
    RTL8188EEBSS           *_bssList      = nullptr;
    uint32_t            _bssCount     = 0;
    IOLock             *_bssLock      = nullptr;
    uint32_t            _scanGeneration = 0;
    struct ieee80211_channel *_manualScanChannels[256] = {};
    uint32_t            _manualScanChannelCount = 0;
    volatile bool       _manualScanAbort = false;
    volatile bool       _manualScanOnHomeChannel = false;
    bool                _manualScanFallbackLogged = false;

    /* Target BSS for connection */
    RTL8188EEBSS            _targetBSS    = {};
    char                _password[64] = {};

    /* WPA2 key material */
    uint8_t  _pmk[32]  = {};
    uint8_t  _ptk[64]  = {};   /* PTK = KCK|KEK|TK */
    uint8_t  _gtk[32]  = {};
    struct ieee80211_key_conf *_ptkConf = nullptr;
    struct ieee80211_key_conf *_gtkConf = nullptr;
    uint8_t  _anonce[32] = {};
    uint8_t  _snonce[32] = {};
    uint8_t  _replayCtr[8] = {};
    uint8_t  _ccmpTxPn[6] = {};
    bool     _rxCcmpIvSkipLogged = false;
    bool     _wpa2     = false;

    /* Sequence number for TX frames */
    uint16_t _txSeq    = 0;
    /* Separate SN space for QoS data (TID 0) so the BlockAck window is gap-free */
    uint16_t _dataSeq  = 0;
    uint16_t _assocAID = 0;

    /* A-MPDU aggregation (BlockAck) state */
    bool     _txBaActive = false;  /* uplink TX BlockAck agreement established */
    uint8_t  _baTid      = 0;      /* TID carrying aggregated data (BE)        */
    uint8_t  _connChanWidth = 20;  /* negotiated operating width: 20/40/80 MHz */
    uint8_t  _baDialog   = 0;      /* rolling ADDBA-request dialog token       */
    uint16_t _baBufSize  = 64;     /* advertised BlockAck buffer/window size   */

    /* RX A-MPDU reorder buffer — one per TID with an active downlink BA.
     * Touched from the RX workloop (frame input) and the IEEE80211 workloop
     * (flush timer, disconnect teardown), so guarded by _rxBaLock.  Frames are
     * always delivered with the lock dropped to avoid holding it across the
     * network-stack input path. */
    static const uint16_t kRxBaMaxBuf   = 64;
    static const uint8_t  kRxBaNumTid   = 8;   /* QoS data TIDs 0-7 */
    static const uint32_t kReorderTimeoutMs = 60;
    struct RxReorder {
        bool      active;
        uint16_t  headSn;          /* next expected SN (12-bit, mod 4096) */
        uint16_t  bufSize;         /* reorder window size */
        uint32_t  stored;          /* frames currently buffered */
        struct sk_buff *buf[kRxBaMaxBuf];   /* indexed by SN % bufSize */
    };
    RxReorder *_rxBa[kRxBaNumTid] = {};
    IOLock    *_rxBaLock      = nullptr;
    IOTimerEventSource *_reorderTimer = nullptr;

    /* RSSI tracking */
    int      _rssi     = -100;

    /* Diagnostics: periodic logging of RX activity during scan */
    uint32_t _rxFrameCount = 0;
    /* findings.md Section 103: distinguishes "no RX at all during scan"
     * from "RX happened but none were beacon/probe-response frames" --
     * incremented only on the same two frame types processScanResult()
     * actually handles (stype 0x0080 beacon, 0x0050 probe-resp), same
     * reset points as _rxFrameCount (scan start). */
    uint32_t _rxScanRelevantCount = 0;

    /* Diagnostics: RX visibility during the auth/assoc window.
     * findings.md TWENTY-SECOND UPDATE follow-up: bsslist now works, but
     * `connect` times out on real hardware with zero insight into whether
     * any frame at all is arriving after the auth request is sent -- the
     * existing [rxdiag] log above is scan-gated only. Separate counter
     * (not _rxFrameCount) so this doesn't interfere with the scan
     * diagnostic's own reset points; reset when entering the
     * authenticating/associating state in doAuthenticate(). */
    uint32_t _rxAuthFrameCount = 0;
};
