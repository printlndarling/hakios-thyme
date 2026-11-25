/* SPDX-License-Identifier: BSD-2-Clause
 * CS voice milestone 1: observe, never change modem policy. Included by the
 * bridge; all state except switch-file sampling belongs to bb_lock. The modem
 * thread alone sends the three read-only NAS queries on its own QRTR port.
 * See VOICE-CS-umi.md Implementation for wire layouts and remaining gates.
 */
#include <sys/vnode.h>
#include <sys/vnode_if.h>

#define VOICE_PORT 0x4202u /* outside ABM, GNSS (0x4200) and cell-info (0x4201) */
#define VOICE_LOG(...) hakios_tl("voice: " __VA_ARGS__)
static const uint16_t voice_queries[] = { 0x34, 0x24, 0x4d }; /* Get prefs, serving system, system info */
static struct {
    bool enabled, requested, failed;
    uint32_t step, node, port, sent_at;
    uint16_t sequence, pending;
} voice;
static thread_call_t voice_switch_call;
static bool voice_switch_wanted;
#include "umi_voice_audio_control.inc"

/* Filesystem work runs on a thread call: neither bb_lock nor the modem's
 * MHI/QRTR/data service thread may wait for the persistent /var filesystem. */
static void voice_check_switch(thread_call_param_t, thread_call_param_t)
{
    vnode_t node;
    bool enabled = false;
    if (!vnode_lookup("/var/db/hakios-voice/on", 0, &node, vfs_context_kernel())) {
        enabled = vnode_vtype(node) == VREG;
        vnode_put(node);
    }
    IOLockLock(bb_lock);
    voice_switch_wanted = enabled;
    IOLockUnlock(bb_lock);
    uva_work();
}

static void voice_schedule_switch(void)
{
    static uint32_t sampled;
    static bool known;
    uint32_t now = bb_now_ms();
    if (!voice_switch_call || (known && now - sampled < 1000)) return;
    known = true; sampled = now;
    thread_call_enter(voice_switch_call);
}

/* A missing optional field is distinct from zero. Only fixed, non-identity
 * scalars are ever read for logging; unknown TLVs and all number fields stay
 * opaque. The caller validates the complete SDU before using these helpers. */
static int voice_scalar(const uint8_t *p, uint32_t n, uint32_t type, uint32_t bytes)
{
    uint32_t len = 0;
    const uint8_t *v = cbd_find(p, n, type, &len);
    if (!v || len != bytes) return -1;
    return bytes == 1 ? v[0] : bytes == 2 ? cbd_rd16(v) : (int)cbd_rd32(v);
}

static void voice_mask(const uint8_t *p, uint32_t n, uint32_t type, const char *name)
{
    uint32_t len = 0;
    const uint8_t *v = cbd_find(p, n, type, &len);
    if (v && len == 8)
        VOICE_LOG("NAS %s mask 0x%08x%08x\n", name, cbd_rd32(v + 4), cbd_rd32(v));
}

static void voice_nas(const uint8_t *p, uint32_t n)
{
    uint32_t message = cbd_rd16(p + 3), len = 0;
    const uint8_t *v;
    if (message == 0x33 || message == 0x34) {
        if ((message == 0x33 && p[0] != 0) || (message == 0x34 && p[0] == 0)) return;
        bool set = message == 0x33;
        VOICE_LOG("NAS %s mode %d service-domain %d usage %d voice-domain %d duration %d (-1 absent)\n",
            set ? "set" : "current", voice_scalar(p, n, 0x11, 2), voice_scalar(p, n, 0x18, 4),
            voice_scalar(p, n, set ? 0x21 : 0x1f, 4), voice_scalar(p, n, set ? 0x23 : 0x20, 4),
            set ? voice_scalar(p, n, 0x17, 1) : -1);
        voice_mask(p, n, 0x12, "legacy-band");
        voice_mask(p, n, 0x15, "LTE-band");
    } else if (message == 0x24 && p[0] != 0) {
        v = cbd_find(p, n, 0x01, &len);
        if (!v || len < 5u || len != 5u + v[4]) return;
        VOICE_LOG("NAS serving reg %u CS-attach %u PS-attach %u network %u RAT-count %u\n",
            v[0], v[1], v[2], v[3], v[4]);
        for (uint32_t i = 0; i < v[4]; i++) VOICE_LOG("NAS serving RAT %u\n", v[5 + i]);
    } else if ((message == 0x4d && p[0] == 2) || (message == 0x4e && p[0] == 4)) {
        for (uint32_t rat = 0; rat < 2; rat++) {
            const char *name = rat ? "LTE" : "GSM";
            v = cbd_find(p, n, rat ? 0x14 : 0x12, &len);
            if (v && len == 3)
                VOICE_LOG("NAS %s service %u true-service %u preferred %u\n", name, v[0], v[1], v[2]);
            v = cbd_find(p, n, rat ? 0x19 : 0x17, &len);
            if (v && len >= 6)
                VOICE_LOG("NAS %s domain %d capability %d roaming %d (-1 invalid)\n", name,
                    v[0] ? v[1] : -1, v[2] ? v[3] : -1, v[4] ? v[5] : -1);
        }
        VOICE_LOG("NAS LTE voice-support %d voice-domain %d registration-domain %d (-1 absent)\n",
            voice_scalar(p, n, 0x21, 1), voice_scalar(p, n, 0x2a, 4), voice_scalar(p, n, 0x31, 4));
    }
}

/* Observe only successfully queued requests / validated-source packets being
 * delivered to ABM. A malformed diagnostic never changes the delivered SDU. */
static void voice_note(const char *direction, const struct cbq_result &r, const uint8_t *p, uint32_t n)
{
    if (!voice.enabled || (r.service != 3 && r.service != 9) || !cbd_sdu_ok(p, n)) return;
    uint32_t message = cbd_rd16(p + 3);
    if (r.service == 3) { voice_nas(p, n); return; }
    VOICE_LOG("%s ch %u cid %u port 0x%x type %u msg 0x%x txn %u error 0x%x t %u\n",
        direction, r.channel, r.cid, r.local_port, p[0], message, cbd_rd16(p + 1),
        p[0] == 2 ? cbd_error(p, n) : 0u, bb_now_ms());
    if (p[0] == 0 && message == 0x20) voice.requested = true;
    if (p[0] == 2 && message == 0x20)
        VOICE_LOG("dial call-id %d error 0x%x\n", voice_scalar(p, n, 0x10, 1), cbd_error(p, n));
    if (p[0] == 0 && (message == 0x21 || message == 0x22))
        VOICE_LOG("%s call-id %d\n", message == 0x21 ? "end" : "answer", voice_scalar(p, n, 0x01, 1));
    if (p[0] != 4 || message != 0x2e) return;
    voice.requested = true;
    uint32_t len = 0, ends_len = 0;
    const uint8_t *calls = cbd_find(p, n, 0x01, &len);
    /* Standard Voice Call End Reason array. 0x10/0x16/0x18/0x19 contain
     * telephone numbers and must never be decoded by this logger. */
    const uint8_t *ends = cbd_find(p, n, 0x14, &ends_len);
    if (!calls || !len || len != 1u + 7u * calls[0] ||
        (ends && (!ends_len || ends_len != 1u + 3u * ends[0]))) {
        VOICE_LOG("all-call-status malformed metadata; forwarded unchanged\n");
        return;
    }
    VOICE_LOG("all-call-status calls %u to-host ch %u cid %u\n", calls[0], r.channel, r.cid);
    for (uint32_t i = 0; i < calls[0]; i++) {
        const uint8_t *c = calls + 1u + i * 7u;
        int reason = -1;
        if (ends) for (uint32_t j = 0; j < ends[0]; j++)
            if (ends[1u + j * 3u] == c[0]) reason = cbd_rd16(ends + 2u + j * 3u);
        VOICE_LOG("call-id %u state %u type %u direction %u RAT %u end-reason %d (-1 absent)\n",
            c[0], c[1], c[2], c[3], c[4], reason);
        uva_call_state(c);
    }
}

static void voice_receive(uint32_t node, uint32_t port, const uint8_t *p, uint32_t n)
{
    if (!voice.enabled || !voice.pending || node != voice.node || port != voice.port || !cbd_sdu_ok(p, n) ||
        p[0] != 2 || cbd_rd16(p + 1) != voice.pending || cbd_rd16(p + 3) != voice_queries[voice.step]) return;
    uint32_t len = 0;
    const uint8_t *result = cbd_find(p, n, 0x02, &len);
    if (!result || len != 4 || cbd_rd16(result)) {
        VOICE_LOG("snapshot failed msg 0x%x error %d; toggle switch to retry\n", voice_queries[voice.step],
            result && len == 4 ? (int)cbd_rd16(result + 2) : -1);
        voice.failed = true;
    } else {
        VOICE_LOG("snapshot msg 0x%x txn %u from %u:%u\n", voice_queries[voice.step], voice.pending, node, port);
        voice_nas(p, n);
        voice.step++;
    }
    voice.pending = 0;
}

/* No indication registration, attach, Set preference, NV or call commands on
 * this endpoint. A failure stops the snapshot until an explicit off/on. */
static uint32_t voice_poll(bool enabled, uint8_t *packet, uint32_t *node, uint32_t *port, bool *release)
{
    *release = false;
    if (enabled != voice.enabled) {
        *release = !enabled && voice.port;
        voice.enabled = enabled; voice.requested = enabled; voice.failed = false;
        voice.pending = 0; voice.step = 3; voice.port = 0;
        VOICE_LOG("%s; passive voice sessions and exclusive AFE route\n", enabled ? "enabled" : "disabled");
    }
    if (!enabled || voice.failed) return 0;
    uint32_t now = bb_now_ms();
    if (voice.pending) {
        if (now - voice.sent_at >= 5000) {
            VOICE_LOG("snapshot timeout msg 0x%x; toggle switch to retry\n", voice_queries[voice.step]);
            voice.pending = 0; voice.failed = true;
        }
        return 0;
    }
    if (voice.step == 3) {
        if (!voice.requested) return 0;
        voice.requested = false; voice.step = 0;
    }
    const struct cbq_server *server = cbq_server_find(&bb_qmux, 3);
    if (!server) return 0;
    voice.node = server->node; voice.port = server->port;
    if (!++voice.sequence) ++voice.sequence;
    voice.pending = voice.sequence; voice.sent_at = now;
    packet[0] = 0;
    cbd_wr16(packet + 1, voice.pending);
    cbd_wr16(packet + 3, voice_queries[voice.step]);
    cbd_wr16(packet + 5, 0);
    *node = voice.node; *port = voice.port;
    return 7;
}
