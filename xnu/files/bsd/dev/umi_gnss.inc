/* SPDX-License-Identifier: BSD-2-Clause
 * Included by hakios.umi_baseband.cpp. bb_lock protects state; only the modem
 * thread calls the transport. A dedicated QRTR endpoint never enters ABM's
 * client pool, reset paths, pending table or frame queues.
 */
#include <sys/sysctl.h>
#include <sys/kauth.h>
#include "umi_gnss_qmi.h"
#define GNSS_LOG(...) hakios_tl("gnss: " __VA_ARGS__)
enum { GNSS_IDLE, GNSS_REGISTER, GNSS_START, GNSS_RUNNING, GNSS_STOP, GNSS_UNREGISTER, GNSS_FAILED };
static struct {
    uint32_t phase, wanted, heartbeat, sent_at, started_at, received_at;
    uint32_t node, port, sequence, fixes, view, view_at, sv_logs, malformed;
    uint16_t txn, pending_txn, pending_message;
    uint8_t session;
    bool failed, view_known;
    struct umi_gnss_fix latest;
} gnss;

static int gnss_enable_sysctl(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req)
{
    if (kauth_cred_getuid(kauth_cred_get()) != 0) return EPERM;
    if (!bb_published) return ENXIO;
    IOLockLock(bb_lock);
    int value = (int)gnss.wanted;
    IOLockUnlock(bb_lock);
    int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value != 0 && value != 1) return EINVAL;
    IOLockLock(bb_lock);
    if (value && !gnss.wanted) {
        gnss.failed = false;
        if (gnss.phase == GNSS_FAILED) gnss.phase = GNSS_IDLE;
        GNSS_LOG("enabled by helper; waiting for LOC session\n");
    }
    gnss.wanted = (uint32_t)value;
    gnss.heartbeat = bb_now_ms();
    if (!value) bzero(&gnss.latest, sizeof gnss.latest);
    IOLockUnlock(bb_lock);
    umi_modem_wake();
    return 0;
}
static int gnss_fix_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    if (kauth_cred_getuid(kauth_cred_get()) != 0) return EPERM;
    if (req->newptr) return EPERM;
    if (!bb_published) return ENXIO;
    struct umi_gnss_fix fix;
    IOLockLock(bb_lock);
    fix = gnss.latest;
    fix.version = UMI_GNSS_ABI;
    if (fix.flags & UMI_GNSS_POSITION) fix.age_ms = bb_now_ms() - gnss.received_at;
    IOLockUnlock(bb_lock);
    return SYSCTL_OUT(req, &fix, sizeof fix);
}
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, gnss_enable, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    nullptr, 0, gnss_enable_sysctl, "I", "Root GNSS lease: 1 renews for 15 seconds, 0 stops");
SYSCTL_PROC(_kern_hakios, OID_AUTO, gnss_fix, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    nullptr, 0, gnss_fix_sysctl, "S,umi_gnss_fix", "Root-only latest GNSS fix; contains private location");

/* Under bb_lock. Called before ABM routing, only for our dedicated port. */
static void gnss_receive(uint32_t node, uint32_t port, const uint8_t *p, uint32_t n)
{
    if (node != gnss.node || port != gnss.port || !ugl_valid(p, n)) return;
    uint32_t now = bb_now_ms(), message = ugl_u16(p + 3);
    if (p[0] == 2 && gnss.pending_message == message && ugl_u16(p + 1) == gnss.pending_txn) {
        int result = ugl_result(p, n);
        GNSS_LOG("response msg 0x%x result %d (0 success, positive QMI error+1)\n", message, result);
        gnss.pending_message = 0;
        if (result) gnss.failed = true;
        switch (gnss.phase) {
        case GNSS_REGISTER: gnss.phase = result ? GNSS_FAILED : GNSS_START; break;
        case GNSS_START:
            gnss.phase = result ? GNSS_STOP : GNSS_RUNNING;
            if (!result) GNSS_LOG("session started; periodic 1000 ms, simulated delivery by umi_gnssd\n");
            break;
        case GNSS_STOP: gnss.phase = GNSS_UNREGISTER; break;
        case GNSS_UNREGISTER:
            gnss.phase = gnss.failed ? GNSS_FAILED : GNSS_IDLE;
            GNSS_LOG("session stopped; fixes %u malformed %u%s\n", gnss.fixes, gnss.malformed,
                     gnss.failed ? "; cleanup/error: toggle switch to retry" : "");
            break;
        }
        return;
    }
    if (!gnss.wanted || (gnss.phase != GNSS_RUNNING && gnss.phase != GNSS_START) || p[0] != 4) return;
    if (message == UGL_POSITION) {
        struct umi_gnss_fix fix = {};
        int result = ugl_position(p, n, gnss.session, &fix);
        if (result < 0) { gnss.malformed++; return; }
        if (!result) {
            if (fix.version && fix.session_status != gnss.latest.session_status) {
                GNSS_LOG("position status %u (0 success, 1 searching); fixes %u\n", fix.session_status, gnss.fixes);
                gnss.latest.session_status = fix.session_status;
            }
            return;
        }
        if (gnss.view_known && now - gnss.view_at < 10000) {
            fix.satellites_view = gnss.view; fix.flags |= UMI_GNSS_VIEW;
        }
        fix.sequence = ++gnss.sequence;
        gnss.latest = fix; gnss.received_at = now;
        gnss.fixes++;
        if (gnss.fixes == 1) GNSS_LOG("first fix after %u ms\n", now - gnss.started_at);
        if (gnss.fixes <= 3 || gnss.fixes % 30 == 0)
            GNSS_LOG("fix count %u accuracy %u m satellites view %u/%u used %u/%u (count/known) age 0 ms\n", gnss.fixes,
                     ugl_accuracy_m(fix.accuracy_bits), fix.satellites_view, !!(fix.flags & UMI_GNSS_VIEW),
                     fix.satellites_used, !!(fix.flags & UMI_GNSS_USED));
    } else if (message == UGL_SV) {
        uint32_t view;
        if (!ugl_satellites(p, n, &view)) { gnss.malformed++; return; }
        gnss.view = view; gnss.view_at = now; gnss.view_known = true;
        if (gnss.sv_logs++ % 30 == 0) GNSS_LOG("satellites in view %u; fixes %u\n", view, gnss.fixes);
    }
}
/* Produce at most one small LOC command, separately from ABM's queue. Errors
 * latch until switch off/on. Pending start is settled before its matching stop.
 */
static uint32_t gnss_poll(uint8_t *packet, uint32_t *node, uint32_t *port)
{
    uint32_t now = bb_now_ms();
    if (gnss.wanted && now - gnss.heartbeat >= 15000) {
        gnss.wanted = 0;
        bzero(&gnss.latest, sizeof gnss.latest);
        GNSS_LOG("helper lease expired; stopping session\n");
    }
    const struct cbq_server *server = cbq_server_find(&bb_qmux, UGL_SERVICE);
    if (!server || (gnss.port && (gnss.node != server->node || gnss.port != server->port))) {
        if (gnss.port) {
            GNSS_LOG("LOC endpoint lost; latest fix cleared\n");
            gnss.port = 0; gnss.phase = GNSS_IDLE; gnss.pending_message = 0;
            bzero(&gnss.latest, sizeof gnss.latest);
        }
        return 0;
    }
    if (gnss.pending_message) {
        if (now - gnss.sent_at < 8000) return 0;
        GNSS_LOG("timeout msg 0x%x; toggle switch to retry\n", gnss.pending_message);
        gnss.failed = true; gnss.pending_message = 0;
        if (gnss.phase == GNSS_START) gnss.phase = GNSS_STOP;
        else if (gnss.phase == GNSS_STOP || gnss.phase == GNSS_REGISTER) gnss.phase = GNSS_UNREGISTER;
        else gnss.phase = GNSS_FAILED;
    }
    if (gnss.phase == GNSS_IDLE && gnss.wanted && !gnss.failed) {
        gnss.node = server->node; gnss.port = server->port;
        if (!++gnss.session) ++gnss.session;
        gnss.fixes = gnss.malformed = gnss.sv_logs = 0;
        gnss.view_known = false; gnss.started_at = now;
        gnss.phase = GNSS_REGISTER;
        GNSS_LOG("session start requested; LOC on independent QRTR port 0x%x\n", UGL_PORT);
    }
    if (!gnss.wanted && gnss.phase == GNSS_START) gnss.phase = GNSS_UNREGISTER;
    if (!gnss.wanted && gnss.phase == GNSS_RUNNING) gnss.phase = GNSS_STOP;
    uint16_t message = gnss.phase == GNSS_REGISTER || gnss.phase == GNSS_UNREGISTER ? UGL_REGISTER :
                       gnss.phase == GNSS_START ? UGL_START : gnss.phase == GNSS_STOP ? UGL_STOP : 0;
    if (!message) return 0;
    if (!++gnss.txn) ++gnss.txn;
    gnss.pending_txn = gnss.txn; gnss.pending_message = message; gnss.sent_at = now;
    *node = gnss.node; *port = gnss.port;
    return ugl_request(packet, gnss.txn, message, gnss.session, gnss.phase == GNSS_REGISTER);
}
