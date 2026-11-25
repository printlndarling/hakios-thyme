/* SPDX-License-Identifier: BSD-2-Clause
 * Included after bridge queue/error helpers. All state is under bb_lock.
 * Filesystem lookup runs on a thread call, never on the modem service thread.
 * QRTR send/release runs on the modem thread outside the lock. NAS registration
 * and all non-5556 traffic remain byte-for-byte ABM's.
 */
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include "umi_cellinfo_qmi.h"
#define CELL_LOG(...) hakios_tl("cellinfo: " __VA_ARGS__)
#define UCI_WAITERS 32u
struct uci_owner {
    uint8_t cid, channel;
    bool subscribed, registering, requested;
    uint16_t register_txn;
    uint32_t register_at;
};
struct uci_waiter { bool used; uint8_t slot; uint16_t txn; };
static struct {
    bool enabled, pending, sampled, release;
    uint32_t node, port, sent_at, next_at, logs;
    uint16_t sequence, txn;
    struct uci_owner owner[CBQ_MAX_CLIENTS];
    struct uci_waiter wait[UCI_WAITERS];
    uint8_t tlvs[UCI_TLVS_MAX];
    uint32_t len;
} cellinfo;

static bool cellinfo_file(const char *path)
{
    vnode_t vp;
    if (vnode_lookup(path,0,&vp,vfs_context_kernel())) return false;
    vnode_put(vp); return true;
}
/* The modem thread must not wait for filesystem I/O, even when disabled. */
static thread_call_t cellinfo_switch_call;
static bool cellinfo_switch_wanted; // bb_lock
static void cellinfo_check_switch(thread_call_param_t, thread_call_param_t)
{
    bool value=cellinfo_file("/var/db/hakios-location/cellinfo-on") &&
               !cellinfo_file("/var/db/hakios-location/cellinfo-off");
    IOLockLock(bb_lock);
    bool changed=cellinfo_switch_wanted!=value;
    cellinfo_switch_wanted=value;
    IOLockUnlock(bb_lock);
    if (changed) umi_modem_wake();
}
/* Default off until the supervisor verifies NAS-client coexistence on the X55.
 * A missing /var or failed optional thread-call allocation sends no queries. */
static void cellinfo_schedule_switch(void)
{
    static uint32_t checked;
    uint32_t now=bb_now_ms();
    if (cellinfo_switch_call && (!checked || now-checked>=2000)) {
        checked=now ? now : 1;
        thread_call_enter(cellinfo_switch_call);
    }
}
static void cellinfo_prune(void)
{
    for (unsigned i=0;i<CBQ_MAX_CLIENTS;i++) {
        const struct cbq_client *c=&bb_qmux.client[i];
        struct uci_owner *o=&cellinfo.owner[i];
        if (c->used && c->service==UCI_SERVICE && c->channel<CBQ_CHANNELS &&
            o->cid==c->cid && o->channel==c->channel) continue;
        if (o->cid) {
            for (unsigned j=0;j<UCI_WAITERS;j++)
                if (cellinfo.wait[j].used && cellinfo.wait[j].slot==i) cellinfo.wait[j].used=false;
            memset(o,0,sizeof *o);
        }
        if (c->used && c->service==UCI_SERVICE && c->channel<CBQ_CHANNELS) {
            o->cid=c->cid; o->channel=c->channel;
        }
    }
}
static void cellinfo_finish(uint32_t error)
{
    for (unsigned i=0;i<UCI_WAITERS;i++) {
        struct uci_waiter *w=&cellinfo.wait[i];
        if (!w->used) continue;
        const struct uci_owner *o=&cellinfo.owner[w->slot];
        uint32_t n=cbq_private_message(CBQ_QMI_RESPONSE,w->txn,UCI_GET,UCI_SERVICE,o->cid,error,
            error ? nullptr : cellinfo.tlvs,error ? 0 : cellinfo.len,bb_out,sizeof bb_out);
        if (n) bb_push(bb_qmux_channel(o->channel),bb_out,n);
        w->used=false;
    }
}
/* Returns true only when 5556 was consumed. Registration is observed even while
 * disabled so enabling the file does not require restarting CommCenter. */
static bool cellinfo_host(const struct cbq_result &r, const uint8_t *p, uint32_t n)
{
    if (r.service!=UCI_SERVICE || n<7 || p[0]!=CBQ_QMI_REQUEST) return false;
    int slot=cbq_client_find(&bb_qmux,r.channel,r.service,r.cid);
    if (slot<0) return false;
    struct uci_owner *o=&cellinfo.owner[slot];
    uint16_t message=uci_r16(p+3),txn=uci_r16(p+1);
    if (message==3 && uci_valid(p,n)) {
        uint32_t len; const uint8_t *v=uci_tlv(p,n,0xa0,&len);
        if (v && len==1 && v[0]<=1) {
            o->registering=true; o->register_txn=txn;
            o->requested=v[0]!=0; o->register_at=bb_now_ms();
        }
    }
    if (!cellinfo.enabled || message!=UCI_GET) return false;
    if (n!=7 || !uci_valid(p,n)) {
        bb_answer_error(r.channel,r.service,r.cid,txn,message,0x30,"cellinfo request has unexpected TLVs");
        return true;
    }
    for (unsigned i=0;i<UCI_WAITERS;i++)
        if (cellinfo.wait[i].used && cellinfo.wait[i].slot==slot && cellinfo.wait[i].txn==txn) return true;
    for (unsigned i=0;i<UCI_WAITERS;i++) if (!cellinfo.wait[i].used) {
        cellinfo.wait[i]={true,(uint8_t)slot,txn}; return true;
    }
    bb_answer_error(r.channel,r.service,r.cid,txn,message,0x05,"cellinfo wait queue full");
    return true;
}
/* Called only after normal forwarding matched/consumed the original request. */
static void cellinfo_observe(uint32_t slot, const uint8_t *p, uint32_t n)
{
    struct uci_owner *o=&cellinfo.owner[slot];
    if (!o->registering || !uci_valid(p,n) || p[0]!=2 || uci_r16(p+3)!=3 ||
        uci_r16(p+1)!=o->register_txn) return;
    o->registering=false;
    if (!uci_result(p,n)) {
        o->subscribed=o->requested;
        if (o->subscribed) cellinfo.next_at=bb_now_ms();
    }
}
static void cellinfo_receive(uint32_t node, uint32_t port, const uint8_t *p, uint32_t n)
{
    if (!cellinfo.pending || node!=cellinfo.node || port!=cellinfo.port ||
        !uci_valid(p,n) || p[0]!=2 || uci_r16(p+3)!=UCI_LOCATION || uci_r16(p+1)!=cellinfo.txn) return;
    cellinfo_prune();
    cellinfo.pending=false; cellinfo.next_at=bb_now_ms()+5000;
    int error=uci_result(p,n);
    struct uci_lte lte;
    int have=error ? -1 : uci_parse(p,n,&lte);
    if (have<0) {
        cellinfo_finish(error>0 ? (uint32_t)error : 3u);
        if (cellinfo.logs++<32) CELL_LOG("query failed error %d; no cell payload delivered\n",error ? error : -1);
        return;
    }
    /* Separate buffer avoids keeping a second snapshot of private cell data.
     * Indications are periodic while subscribed; every successful query replaces
     * the previous observation, including an empty vector when LTE is absent. */
    cellinfo.len=uci_encode(&lte,have,cellinfo.tlvs,sizeof cellinfo.tlvs);
    if (!cellinfo.len) { cellinfo_finish(3); return; }
    cellinfo_finish(0);
    unsigned indications=0;
    for (unsigned i=0;i<CBQ_MAX_CLIENTS;i++) {
        const struct uci_owner *o=&cellinfo.owner[i];
        if (!o->cid || !o->subscribed) continue;
        uint32_t m=cbq_private_indication(UCI_GET,UCI_SERVICE,o->cid,cellinfo.tlvs,cellinfo.len,bb_out,sizeof bb_out);
        if (m) { bb_push(bb_qmux_channel(o->channel),bb_out,m); indications++; }
    }
    if (cellinfo.logs++<32 || cellinfo.logs%64==0)
        CELL_LOG("translated NAS 0x43 -> 0x5556: LTE cells %u measurements %u indications %u\n",
            have ? 1u : 0u,have ? lte.count : 0u,indications);
    memset(&lte,0,sizeof lte);
    memset(cellinfo.tlvs,0,sizeof cellinfo.tlvs); cellinfo.len=0;
}
/* One bounded independent read, no NAS mode/set/register commands of our own. */
static uint32_t cellinfo_poll(bool enabled, uint8_t *packet, uint32_t *node, uint32_t *port)
{
    uint32_t now=bb_now_ms();
    cellinfo_prune();
    if (enabled!=cellinfo.enabled) {
        cellinfo.enabled=enabled;
        CELL_LOG("%s (experimental; file switch, default off)\n",enabled ? "enabled" : "disabled");
        cellinfo.next_at=now;
    }
    const struct cbq_server *s=cbq_server_find(&bb_qmux,UCI_SERVICE);
    if (!enabled || !s || (cellinfo.port && (cellinfo.node!=s->node || cellinfo.port!=s->port))) {
        cellinfo_finish(3); cellinfo.pending=false; cellinfo.sampled=false;
        if (cellinfo.port) cellinfo.release=true;
        cellinfo.port=0; return 0;
    }
    if (cellinfo.pending) {
        if (now-cellinfo.sent_at<8000) return 0;
        cellinfo.pending=false; cellinfo_finish(3); cellinfo.next_at=now+5000;
        if (cellinfo.logs++<32) CELL_LOG("NAS 0x43 timeout; waiting requests answered with error\n");
    }
    bool waiting=false,subscribed=false;
    for (unsigned i=0;i<UCI_WAITERS;i++) waiting |= cellinfo.wait[i].used;
    for (unsigned i=0;i<CBQ_MAX_CLIENTS;i++) {
        struct uci_owner *o=&cellinfo.owner[i];
        if (o->registering && now-o->register_at>=8000) o->registering=false;
        subscribed |= o->cid && o->subscribed;
    }
    if (!waiting && (!subscribed || (cellinfo.sampled && (int32_t)(now-cellinfo.next_at)<0))) return 0;
    if (cellinfo.release) return 0; /* release old dedicated endpoint before reuse */
    cellinfo.node=s->node; cellinfo.port=s->port;
    if (!++cellinfo.sequence) ++cellinfo.sequence;
    cellinfo.txn=cellinfo.sequence; cellinfo.pending=true; cellinfo.sampled=true; cellinfo.sent_at=now;
    packet[0]=0; uci_w16(packet+1,cellinfo.txn); uci_w16(packet+3,UCI_LOCATION); uci_w16(packet+5,0);
    *node=s->node; *port=s->port; return 7;
}
