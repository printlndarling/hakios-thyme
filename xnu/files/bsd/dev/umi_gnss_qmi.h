/* SPDX-License-Identifier: BSD-2-Clause
 * LOC v2 wire definitions checked against Debian libqmi 1.36.0 (libqmi-glib.so.5.11.0):
 * qmi_client_loc_{register_events,start,stop}, position_report_indication_parse,
 * gnss_sv_info_indication_parse and qmi_loc_* enum tables. See LOCATION-umi.md.
 * Byte operations only; used by the actual kernel client and host tests.
 */
#ifndef UMI_GNSS_QMI_H
#define UMI_GNSS_QMI_H
#include "umi_gnss_abi.h"
#define UGL_SERVICE 0x10u
#define UGL_REGISTER 0x21u
#define UGL_START 0x22u
#define UGL_STOP 0x23u
#define UGL_POSITION 0x24u
#define UGL_SV 0x25u
#define UGL_PORT 0x4200u /* outside ABM 0x4100..0x415f and kernel/TFTP 0x4000.. */
static uint32_t ugl_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t ugl_u32(const uint8_t *p) { return ugl_u16(p) | ugl_u16(p + 2) << 16; }
static uint64_t ugl_u64(const uint8_t *p) { return ugl_u32(p) | (uint64_t)ugl_u32(p + 4) << 32; }
static void ugl_w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
/* Strict framing, including malformed unknown TLVs and duplicates. */
static int ugl_valid(const uint8_t *p, uint32_t n)
{
    uint8_t seen[32] = {0};
    if (n < 7 || ugl_u16(p + 5) != n - 7) return 0;
    for (uint32_t at = 7; at < n;) {
        if (n - at < 3) return 0;
        uint32_t t = p[at], len = ugl_u16(p + at + 1);
        if (len > n - at - 3 || (seen[t / 8] & (1u << (t % 8)))) return 0;
        seen[t / 8] |= (uint8_t)(1u << (t % 8));
        at += 3 + len;
    }
    return 1;
}
static const uint8_t *ugl_tlv(const uint8_t *p, uint32_t n, uint32_t type, uint32_t *len)
{
    for (uint32_t at = 7; at < n; at += 3 + ugl_u16(p + at + 1)) {
        if (p[at] == type) { *len = ugl_u16(p + at + 1); return p + at + 3; }
    }
    *len = 0; return 0;
}
static int ugl_field(const uint8_t *p, uint32_t n, uint32_t type, uint32_t size, uint64_t *out)
{
    uint32_t len;
    const uint8_t *v = ugl_tlv(p, n, type, &len);
    if (!v || len != size) return 0;
    *out = size == 8 ? ugl_u64(v) : size == 4 ? ugl_u32(v) : v[0];
    return 1;
}
/* Returns -1 malformed, 0 valid report without a usable fix, 1 complete success.
 * Do not merge optional fields with an older position. A partial fix is not a fix.
 */
static int ugl_position(const uint8_t *p, uint32_t n, uint8_t session, struct umi_gnss_fix *out)
{
    struct umi_gnss_fix f = {0};
    uint64_t v, id;
    if (!ugl_valid(p, n) || p[0] != 4 || ugl_u16(p + 3) != UGL_POSITION ||
        !ugl_field(p, n, 1, 4, &v) || !ugl_field(p, n, 2, 1, &id)) return -1;
    if (id != session) return 0;
    f.version = UMI_GNSS_ABI; f.session_status = (uint32_t)v;
    *out = f;
    if (v != 0) return 0; /* success=0, in-progress=1, all other statuses unusable */
    if (!ugl_field(p, n, 0x10, 8, &f.latitude_bits) || !ugl_field(p, n, 0x11, 8, &f.longitude_bits)) return 0;
    /* IEEE positive magnitude ordering validates finite values and geographic range without FP. */
    if ((f.latitude_bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x4056800000000000) ||
        (f.longitude_bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x4066800000000000)) return -1;
    /* Use circular uncertainty, or the ellipse's semi-major axis. Both are metres. */
    if (!ugl_field(p, n, 0x12, 4, &v) && !ugl_field(p, n, 0x14, 4, &v)) return 0;
    if (!v || v >= 0x7f800000u) return -1; /* rejects negative, zero, NaN, infinity */
    f.accuracy_bits = (uint32_t)v; f.flags = UMI_GNSS_POSITION;
    if (ugl_field(p, n, 0x24, 8, &f.utc_ms)) f.flags |= UMI_GNSS_UTC;
    /* CoreLocation expects altitude above mean sea level, not ellipsoid. */
    uint64_t alt, vertical;
    if (ugl_field(p, n, 0x1c, 4, &alt) && ugl_field(p, n, 0x1d, 4, &vertical) &&
        (alt & 0x7fffffffu) < 0x7f800000u && vertical && vertical < 0x7f800000u) {
        f.altitude_bits = (uint32_t)alt; f.vertical_accuracy_bits = (uint32_t)vertical;
        f.flags |= UMI_GNSS_ALTITUDE;
    }
    uint32_t len;
    const uint8_t *used = ugl_tlv(p, n, 0x2c, &len);
    if (used) {
        if (!len || len != 1u + 2u * used[0]) return -1;
        f.satellites_used = used[0]; f.flags |= UMI_GNSS_USED;
    }
    *out = f; return 1;
}
static int ugl_satellites(const uint8_t *p, uint32_t n, uint32_t *view)
{
    if (!ugl_valid(p, n) || p[0] != 4 || ugl_u16(p + 3) != UGL_SV) return 0;
    uint64_t altitude_assumed;
    if (!ugl_field(p, n, 1, 1, &altitude_assumed)) return 0;
    uint32_t len;
    const uint8_t *list = ugl_tlv(p, n, 0x10, &len);
    if (!list || !len || len != 1u + 28u * list[0]) return 0;
    *view = list[0]; return 1;
}
static int ugl_result(const uint8_t *p, uint32_t n)
{
    uint32_t len;
    if (!ugl_valid(p, n) || p[0] != 2) return -1;
    const uint8_t *v = ugl_tlv(p, n, 2, &len);
    if (!v || len != 4) return -1;
    return ugl_u16(v) == 0 ? 0 : (int)ugl_u16(v + 2) + 1;
}
/* Register mask bit0 position + bit1 SV, no NMEA (coordinates must never reach logs).
 * Start: session, recurrence=periodic(1), intermediate reports=disable(2), interval=1000ms.
 * Only volatile session commands; no operation mode, lock, SUPL, NV or EFS writes.
 */
static uint32_t ugl_request(uint8_t *p, uint16_t txn, uint16_t message, uint8_t session, int enabled)
{
    uint32_t n = 7;
    p[0] = 0; ugl_w16(p + 1, txn); ugl_w16(p + 3, message);
    if (message == UGL_REGISTER) {
        p[n++] = 1; ugl_w16(p + n, 8); n += 2;
        for (unsigned i = 0; i < 8; i++) p[n++] = i == 0 && enabled ? 3 : 0;
    } else {
        p[n++] = 1; ugl_w16(p + n, 1); n += 2; p[n++] = session;
        if (message == UGL_START) {
            const uint8_t tlvs[] = {0x10,4,0,1,0,0,0, 0x12,4,0,2,0,0,0, 0x13,4,0,0xe8,3,0,0};
            for (unsigned i = 0; i < sizeof tlvs; i++) p[n++] = tlvs[i];
        }
    }
    ugl_w16(p + 5, n - 7); return n;
}
/* Ceiling of a positive finite float in metres, for coordinate-free kernel logs. */
static uint32_t ugl_accuracy_m(uint32_t bits)
{
    int e = (int)(bits >> 23) - 127;
    uint32_t m = (bits & 0x7fffffu) | 0x800000u;
    if (e < 0) return 1;
    if (e > 22) return e > 30 ? UINT32_MAX : m << (e - 23);
    return (m + ((1u << (23 - e)) - 1)) >> (23 - e);
}
#endif
