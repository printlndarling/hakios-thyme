/* umi_modem=2: what the CommCenter bridge (iokit/Kernel/hakios.umi_baseband.cpp) owes ABM. Portable, host-tested
 * (tools/modem-tests/bb.c); no kernel code.
 *
 * Every QMI request ABM writes must be answered: when an answer is lost, CommCenter resets the baseband after its ATCS
 * timeout ("reset: reason=ATCS_TIMEOUT on message for svc=... client=... (txid=... msgid=...)", v23) and does it again.
 * So each request forwarded to the X55 is remembered until the X55 answers it; one it does not answer within
 * UBP_ANSWER_MS is answered here with QMI_ERR_INTERNAL. The bridge answers locally whatever it cannot forward.
 *
 * DMS Set Operating Mode (0x002e, TLV 0x01, one byte) to 4 (reset) or 5 (shutting down) makes the X55 restart its modem
 * (v23: after CommCenter's "Resetting baseband after pushing files", EFS sync, then no QMI answer ever again), and only
 * umi_modem.c could boot it again. A reset is ABM's to do through AppleBaseband, which the bridge plays: the X55 itself is
 * taken to low power (1), as a baseband coming out of reset is, and CommCenter brings it online again as after one.
 */
#ifndef UMI_BB_PENDING_H
#define UMI_BB_PENDING_H

#include <stdint.h>

#define UBP_MAX 128u
#define UBP_ANSWER_MS 8000u
#define UBP_ERR_INTERNAL 3u
#define UBP_SVC_DMS 0x02u
#define UBP_DMS_SET_OPERATING_MODE 0x002eu
#define UBP_MODE_LOW_POWER 1u
#define UBP_MODE_RESET 4u
#define UBP_MODE_SHUTTING_DOWN 5u

struct ubp_entry { uint8_t used, channel, cid, service; uint16_t txn, message; uint32_t port, sent_ms; };
struct ubp { struct ubp_entry e[UBP_MAX]; uint32_t answered, expired, overflow; };

/* A request forwarded from local QRTR `port`. 0 when the table is full (the caller answers it at once). */
static inline int ubp_add(struct ubp *p, uint32_t port, uint32_t channel, uint32_t cid, uint32_t service, uint32_t txn,
                          uint32_t message, uint32_t now_ms)
{
    for (uint32_t i = 0; i < UBP_MAX; i++) {
        struct ubp_entry *e = &p->e[i];
        if (e->used) continue;
        *e = (struct ubp_entry){ 1, (uint8_t)channel, (uint8_t)cid, (uint8_t)service, (uint16_t)txn, (uint16_t)message, port, now_ms };
        return 1;
    }
    p->overflow++;
    return 0;
}
/* The X55's answer (QMI type 2) to `port` with transaction `txn`: 1 and the request in *out when it was owed. */
static inline int ubp_answer(struct ubp *p, uint32_t port, uint32_t txn, struct ubp_entry *out)
{
    for (uint32_t i = 0; i < UBP_MAX; i++) {
        struct ubp_entry *e = &p->e[i];
        if (!e->used || e->port != port || e->txn != (uint16_t)txn) continue;
        *out = *e;
        e->used = 0;
        p->answered++;
        return 1;
    }
    return 0;
}
/* One request the X55 left unanswered for UBP_ANSWER_MS, taken from the table: 1 and *out. */
static inline int ubp_expired(struct ubp *p, uint32_t now_ms, struct ubp_entry *out)
{
    for (uint32_t i = 0; i < UBP_MAX; i++) {
        struct ubp_entry *e = &p->e[i];
        if (!e->used || now_ms - e->sent_ms < UBP_ANSWER_MS) continue;
        *out = *e;
        e->used = 0;
        p->expired++;
        return 1;
    }
    return 0;
}
/* A client released (CTL release, sync): nothing more is owed to it. */
static inline void ubp_forget(struct ubp *p, uint32_t port)
{
    for (uint32_t i = 0; i < UBP_MAX; i++)
        if (p->e[i].used && p->e[i].port == port) p->e[i].used = 0;
}

/* DMS Set Operating Mode to reset or shutting down: the mode byte rewritten in `sdu` (a request the bridge owns) to low
 * power. Returns the mode asked for when it was rewritten, else 0. */
static inline uint32_t ubp_dms_mode_fix(uint32_t service, uint8_t *sdu, uint32_t n)
{
    if (service != UBP_SVC_DMS || n < 11u || sdu[0] != 0u || (uint32_t)(sdu[3] | sdu[4] << 8) != UBP_DMS_SET_OPERATING_MODE) return 0;
    const uint32_t end = 7u + (uint32_t)(sdu[5] | sdu[6] << 8);
    if (end > n) return 0;
    for (uint32_t at = 7u; at + 3u <= end;) {
        const uint32_t len = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
        if (at + 3u + len > end) return 0;
        if (sdu[at] == 0x01 && len == 1u) {
            const uint32_t mode = sdu[at + 3];
            if (mode != UBP_MODE_RESET && mode != UBP_MODE_SHUTTING_DOWN) return 0;
            sdu[at + 3] = UBP_MODE_LOW_POWER;
            return mode;
        }
        at += 3u + len;
    }
    return 0;
}

/*
 * WDS Start Network (0x0020) refused with a 3GPP (verbose type 6) or Mobile IP (1) call-end reason: CommCenter's
 * QMIDataContextIP::handleActivateResponse only logs "wds::kErrorTypeMobileIP or wds::kErrorType3GPP received handling those
 * in indications" and waits for the WDS Packet Service Status indication (0x0022) that Apple's baseband sends with the reason
 * (libCommCenterMCommandDrivers 0x1f0ea8f70..0x1f0ea8fd8; any other type fails the activation from the response). The X55 sends
 * no such indication for a call that never connected (v25: IMS IPv4 refused with #51 "PDN type IPv6 only allowed" at boot; the
 * context waited 360 s for it and CommCenter reset the baseband). Returns the TLVs of that indication for the refused client:
 * 0x01 connection status {1 disconnected, 0}, the response's own call-end reason (0x10) and verbose reason (0x11), and the
 * client's IP family (0x12). 0 when the response is anything else.
 */
#define UBP_SVC_WDS 0x01u
#define UBP_WDS_START_NETWORK 0x0020u
#define UBP_WDS_PACKET_SERVICE_STATUS 0x0022u
static inline const uint8_t *ubp_tlv(const uint8_t *sdu, uint32_t n, uint32_t type, uint32_t *len)
{
    if (n < 7u) return 0;
    const uint32_t end = 7u + (uint32_t)(sdu[5] | sdu[6] << 8);
    if (end > n) return 0;
    for (uint32_t at = 7u; at + 3u <= end;) {
        const uint32_t l = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
        if (at + 3u + l > end) return 0;
        if (sdu[at] == type) { *len = l; return sdu + at + 3; }
        at += 3u + l;
    }
    return 0;
}
static inline uint32_t ubp_start_failure_indication(uint32_t service, const uint8_t *rsp, uint32_t n, uint32_t family, uint8_t *tlvs,
                                                    uint32_t room)
{
    uint32_t len = 0, rlen = 0, vlen = 0;
    if (service != UBP_SVC_WDS || n < 7u || rsp[0] != 2u || (uint32_t)(rsp[3] | rsp[4] << 8) != UBP_WDS_START_NETWORK) return 0;
    const uint8_t *result = ubp_tlv(rsp, n, 0x02, &len);
    if (!result || len < 4u || !(result[0] | result[1] << 8)) return 0;
    const uint8_t *reason = ubp_tlv(rsp, n, 0x10, &rlen), *verbose = ubp_tlv(rsp, n, 0x11, &vlen);
    if (!verbose || vlen != 4u) return 0;
    const uint32_t type = (uint32_t)(verbose[0] | verbose[1] << 8);
    if (type != 1u && type != 6u) return 0;
    const uint32_t need = 5u + (reason && rlen == 2u ? 5u : 0u) + 7u + (family == 4u || family == 6u ? 4u : 0u);
    if (room < need) return 0;
    uint32_t w = 0;
    tlvs[w++] = 0x01; tlvs[w++] = 2; tlvs[w++] = 0; tlvs[w++] = 1; tlvs[w++] = 0;
    if (reason && rlen == 2u) { tlvs[w++] = 0x10; tlvs[w++] = 2; tlvs[w++] = 0; tlvs[w++] = reason[0]; tlvs[w++] = reason[1]; }
    tlvs[w++] = 0x11; tlvs[w++] = 4; tlvs[w++] = 0;
    for (uint32_t i = 0; i < 4u; i++) tlvs[w++] = verbose[i];
    if (family == 4u || family == 6u) { tlvs[w++] = 0x12; tlvs[w++] = 1; tlvs[w++] = 0; tlvs[w++] = (uint8_t)family; }
    return w;
}

#endif /* UMI_BB_PENDING_H */
