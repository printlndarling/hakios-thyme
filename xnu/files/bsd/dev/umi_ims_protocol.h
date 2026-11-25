/* SPDX-License-Identifier: BSD-2-Clause */
/* Portable IMS WDS request/response helpers.  No APN, address or identity is
 * returned to callers; the caller decides what may be logged. */
#ifndef UMI_IMS_PROTOCOL_H
#define UMI_IMS_PROTOCOL_H

#include <stdint.h>

#define UMI_IMS_WDS 0x01u
#define UMI_IMS_START 0x0020u
#define UMI_IMS_STOP 0x0021u
#define UMI_IMS_GET_SETTINGS 0x002du
#define UMI_IMS_STATUS 0x0022u
#define UMI_IMS_SET_FAMILY 0x004du
#define UMI_IMS_APN_TLV 0x14u
#define UMI_IMS_FAMILY_TLV 0x19u
#define UMI_IMS_IMSA 0x21u
#define UMI_IMS_GET_REGISTRATION 0x20u
#define UMI_IMS_BIND_SUBSCRIPTION 0x33u
#define UMI_IMS_AUDIT_PORT 0x4203u

/* Route B (modem-ims): the modem's own IMS stack, configured by the bridge. */
#define UMI_IMS_IMSS 0x12u
#define UMI_IMS_IMSS_AUDIT_PORT 0x4204u
#define UMI_IMS_IMS_SETTINGS_ENABLED 0x008fu
/* Route B probe: IMSDCM (QRTR service 770 = 0x302), the host-side "IMS data connection manager".
 * The modem's IMS stack asks the AP for its IMS bearer here (libqmi qmi-service-imsdcm.json); on
 * Android imsdatadaemon hosts 770 v1 instance 0 on the AP node (work/v29i/android-ims-20261114/
 * qrtr.txt: "770 1 0 1"). P1 announces the service and observes only -- no bearer is created. */
#define UMI_IMS_DCM_SERVICE 0x0302u
#define UMI_IMS_DCM_VERSION 1u
#define UMI_IMS_DCM_INSTANCE 0u
#define UMI_IMS_DCM_PORT 0x4205u
#define UMI_IMS_DCM_START 0x0020u   /* PDP Activate Request: TLV 0x01 {APN, APN type, RAT, family, profile} */
#define UMI_IMS_DCM_STOP 0x0021u
/* QMI CTL, the node-local service a QMI client allocates its client id through. Over QRTR the
 * u16-service variants 0xFF22/0xFF23 exist because 0x302 does not fit the u8 Service TLV. */
#define UMI_IMS_QRTR_CTRL 0xfffffffeu
#define UMI_IMS_CTL_GET_VERSION 0x0021u
#define UMI_IMS_CTL_ALLOCATE 0x0022u
#define UMI_IMS_CTL_RELEASE 0x0023u
#define UMI_IMS_CTL_ALLOCATE_QRTR 0xff22u
#define UMI_IMS_CTL_RELEASE_QRTR 0xff23u
/* The vendor IMSS client's preamble, which the X55 expects before it accepts 0x8f, in the order
 * qcril runs it (0x1e, 0x1f, 0x2a, get) plus the per-client subscription bind qcril sends first:
 * Set Bind Subscription (0x98), Get Supported Messages (0x1e), Get Supported Fields (0x1f),
 * Config Ind Register (0x2a), and the read-only Get IMS Services Enabled Setting (0x90). */
#define UMI_IMS_IMSS_GET_SUPPORTED_MSGS 0x001eu
#define UMI_IMS_IMSS_GET_SUPPORTED_FIELDS 0x001fu
#define UMI_IMS_IMSS_CONFIG_IND_REG 0x002au
#define UMI_IMS_IMSS_GET_ENABLED 0x0090u
/* Read-only Get IMS Policy Manager Settings (libqmi qmi-service-ims.json 0x48): what the modem
 * currently believes the IMS APN/parameter source order is. Sent once per boot, right before 0x8f. */
#define UMI_IMS_IMSS_GET_POLICY 0x0048u
/* Set Bind Subscription: libqmi's "Bind" (0x98). Payload is TLV 0x01 with a u32 subscription type
 * (qcril's ims_set_bind_subscription_req_msg_v01); qcril's ImssModemEndPoint sends it once per
 * client before the preamble, primary only for the stack-0 instance. Client/session state, no NV. */
#define UMI_IMS_IMSS_BIND 0x0098u
#define UMI_IMS_IMSS_BIND_TLV 0x01u
#define UMI_IMS_IMSS_SUBSCRIPTION_PRIMARY 0u
/* Get Supported Messages answers TLV 0x10: a uint16 element count (QMI IDL SZ_IS_16; the X55's
 * max message id is 0x00a9, i.e. 22 bitmap bytes) and then one bit per message id, LSB first.
 * Get Supported Fields answers TLV 0x10 (request_fields; 0x11/0x12 are response/indication
 * fields) with the same bit order behind a one-byte count. Its request is TLV 0x01 carrying
 * msg_id as a u16 -- not a bare u16 in the message body. */
#define UMI_IMS_IMSS_MAP_TLV 0x10u
#define UMI_IMS_IMSS_FIELDS_REQ_TLV 0x01u
#define UMI_IMS_IMSS_MSGS_MAP_PREFIX 2u   /* the count field of the 0x1e bitmap */
#define UMI_IMS_IMSS_FIELDS_MAP_PREFIX 1u /* the count field of the 0x1f bitmaps */
#define UMI_IMS_IMSS_MAP_LOG_MAX 24u      /* bitmap bytes shown by the one-shot log line */
/* Config Ind Register booleans, by field number of ims_settings_config_ind_reg_req_msg_v01:
 * reg_mgr_config=17, client_provisioning_config=31, ims_service_enable_config_enabled=45,
 * rtt_config_enabled=49; an all-optional request's first field (16) is TLV 0x10, so TLV = field. */
#define UMI_IMS_IMSS_TLV_REG_MGR_CONFIG 0x11u
#define UMI_IMS_IMSS_TLV_CLIENT_PROVISIONING 0x1fu
#define UMI_IMS_IMSS_TLV_SERVICE_ENABLE_CONFIG 0x2du
#define UMI_IMS_IMSS_TLV_RTT_CONFIG 0x31u
#define UMI_IMS_IMSA_INDICATIONS 0x0022u
#define UMI_IMS_IMSA_REGISTRATION_IND 0x0023u
#define UMI_IMS_IMSA_SERVICES_IND 0x0024u
#define UMI_IMS_NAS 0x03u
#define UMI_IMS_SYSTEM_SELECTION 0x0033u
#define UMI_IMS_VOICE_DOMAIN 0x23u

struct umi_ims_request_fix {
    uint32_t len;
    uint8_t is_ims;
    uint8_t family_before;
    uint8_t family_after;
    uint8_t family_changed;
};

static inline uint16_t umi_ims_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void umi_ims_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void umi_ims_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t umi_ims_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline int umi_ims_tlvs_ok(const uint8_t *sdu, uint32_t n)
{
    /* (the SDU's declared length is a floor, as cbd_sdu_ok() reads it: a reply may carry padding past the
     * last TLV. Each TLV's own bounds are still held to the declared end, not to n) */
    if (n < 7u || 7u + umi_ims_rd16(sdu + 5) > n) return 0;
    uint32_t at = 7u, end = 7u + umi_ims_rd16(sdu + 5);
    while (at < end) {
        if (end - at < 3u || at + 3u + umi_ims_rd16(sdu + at + 1) > end) return 0;
        at += 3u + umi_ims_rd16(sdu + at + 1);
    }
    return 1;
}

static inline const uint8_t *umi_ims_find(const uint8_t *sdu, uint32_t n, uint32_t type, uint32_t *len)
{
    if (!umi_ims_tlvs_ok(sdu, n)) return 0;
    for (uint32_t at = 7u, end = 7u + umi_ims_rd16(sdu + 5); at < end; at += 3u + umi_ims_rd16(sdu + at + 1))
        if (sdu[at] == type) { *len = umi_ims_rd16(sdu + at + 1); return sdu + at + 3; }
    return 0;
}

static inline int umi_ims_apn(const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, UMI_IMS_APN_TLV, &len);
    return v && len == 3u && ((v[0] == 'i' || v[0] == 'I') && (v[1] == 'm' || v[1] == 'M') && (v[2] == 's' || v[2] == 'S'));
}

/* Copy a WDS Start Network request and select IPv6-only for the IMS APN.  The
 * modem has already demonstrated that its IPv4 IMS attempt fails with 3GPP
 * #51; this prevents CommCenter's IPv4 half from holding the baseband reset
 * timer open. */
static inline struct umi_ims_request_fix umi_ims_prepare(uint32_t service, const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
    struct umi_ims_request_fix f = { 0, 0, 0, 0, 0 };
    if (service != UMI_IMS_WDS || n < 7u || sdu[0] != 0 || umi_ims_rd16(sdu + 3) != UMI_IMS_START || !umi_ims_apn(sdu, n)) return f;
    f.is_ims = 1;
    if (room < n || !umi_ims_tlvs_ok(sdu, n)) return f;
    for (uint32_t i = 0; i < n; i++) out[i] = sdu[i];
    uint32_t len = 0;
    const uint8_t *family = umi_ims_find(out, n, UMI_IMS_FAMILY_TLV, &len);
    if (family && len == 1u) {
        f.family_before = family[0];
        f.family_after = family[0] == 3u ? 6u : family[0];
        f.family_changed = family[0] == 3u;
        if (f.family_changed) {
            const uint32_t offset = (uint32_t)(family - out);
            out[offset] = 6u;
        }
    }
    f.len = n;
    return f;
}

static inline int umi_ims_has_pcscf(const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    /* WDS GetCurrentSettings: 0x23 = u8 count + IPv4 addresses;
     * 0x2e = u8 count + 16-byte IPv6 addresses;
     * 0x24 = u8 count + {u16 length, domain} entries.  In particular 0x25
     * is OUR IPv6 address/prefix, and 0x22 only requests discovery via PCO.
     * Neither establishes a P-CSCF. Unknown firmware extensions pass through. */
    if (n < 7u || sdu[0] != 2u || umi_ims_rd16(sdu + 3) != UMI_IMS_GET_SETTINGS) return 0;
    const uint8_t *result = umi_ims_find(sdu, n, 0x02u, &len);
    if (!result || len != 4u || umi_ims_rd16(result)) return 0;
    const uint8_t *v = umi_ims_find(sdu, n, 0x23u, &len);
    if (v && len > 1u && v[0] && len == 1u + 4u * v[0]) {
        for (uint32_t at = 1u; at < len; at += 4u)
            if (v[at] || v[at + 1] || v[at + 2] || v[at + 3]) return 1;
    }
    v = umi_ims_find(sdu, n, 0x2eu, &len);
    if (v && len > 1u && v[0] && len == 1u + 16u * v[0]) {
        for (uint32_t at = 1u; at < len; at++)
            if (v[at]) return 1;
    }
    v = umi_ims_find(sdu, n, 0x24u, &len);
    if (!v || !len || !v[0]) return 0;
    uint32_t at = 1u;
    for (uint32_t i = 0; i < v[0]; i++) {
        if (len - at < 2u) return 0;
        const uint32_t bytes = umi_ims_rd16(v + at);
        at += 2u;
        if (!bytes || bytes > len - at) return 0;
        at += bytes;
    }
    return at == len;
}

/* A missing result is not success. Used for diagnostics and readiness only;
 * all response bytes still go to Apple's parser unchanged. */
static inline int umi_ims_result(const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, 0x02u, &len);
    if (!v || len != 4u) return -1;
    return umi_ims_rd16(v) ? (umi_ims_rd16(v + 2) ? umi_ims_rd16(v + 2) : -1) : 0;
}

struct umi_ims_bearer {
    uint16_t start_txn;
    uint8_t selected, active, pcscf, family;
};

static inline void umi_ims_observe(struct umi_ims_bearer *b, const uint8_t *sdu, uint32_t n)
{
    if (!umi_ims_tlvs_ok(sdu, n)) return;
    const uint32_t msg = umi_ims_rd16(sdu + 3);
    uint32_t len = 0;
    const uint8_t *v;
    if (sdu[0] == 0u && msg == UMI_IMS_START) {
        b->selected = (uint8_t)umi_ims_apn(sdu, n);
        b->active = b->pcscf = b->family = 0;
        b->start_txn = umi_ims_rd16(sdu + 1);
        v = umi_ims_find(sdu, n, UMI_IMS_FAMILY_TLV, &len);
        if (b->selected && v && len == 1u) b->family = v[0];
    }
    if (!b->selected) return;
    if (sdu[0] == 2u && msg == UMI_IMS_START && umi_ims_rd16(sdu + 1) == b->start_txn) {
        v = umi_ims_find(sdu, n, 0x01u, &len);
        b->active = umi_ims_result(sdu, n) == 0 && v && len == 4u && umi_ims_rd32(v) != 0u;
        b->pcscf = 0;
    } else if (sdu[0] == 2u && msg == UMI_IMS_GET_SETTINGS) {
        b->pcscf = b->active && umi_ims_has_pcscf(sdu, n);
        v = umi_ims_find(sdu, n, 0x2bu, &len);
        if (umi_ims_result(sdu, n) == 0 && v && len == 1u) b->family = v[0];
    } else if (sdu[0] == 2u && msg == UMI_IMS_STOP && umi_ims_result(sdu, n) == 0) {
        b->active = b->pcscf = 0;
    } else if ((sdu[0] == 2u && msg == UMI_IMS_STATUS) || (sdu[0] == 4u && msg == 0x01u)) {
        v = umi_ims_find(sdu, n, 0x01u, &len);
        if (v && len && v[0] == 1u) b->active = b->pcscf = 0;
    }
}

/* Apple owns registration. No host may configure the Android IMS stack while
 * this mode is enabled. IMSA GetRegistrationStatus remains a read-only probe. */
static inline int umi_ims_modem_registration_command(uint32_t service, uint32_t message)
{
    return service == 0x12u || (service == UMI_IMS_IMSA && message != UMI_IMS_GET_REGISTRATION);
}

/* IMSA's original registration flag is TLV 0x10 (boolean). Newer versions
 * additionally return TLV 0x12 (u32 registration state). Only zero means idle;
 * registering/registered/limited service all exclude an AP registration. */
static inline int umi_ims_registration(const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || sdu[0] != 2u || umi_ims_rd16(sdu + 3) != UMI_IMS_GET_REGISTRATION || umi_ims_result(sdu, n)) return -1;
    uint32_t len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, 0x12u, &len);
    if (v && len == 4u) return umi_ims_rd32(v) != 0u;
    v = umi_ims_find(sdu, n, 0x10u, &len);
    return v && len == 1u && v[0] <= 1u ? v[0] : -1;
}

#endif
