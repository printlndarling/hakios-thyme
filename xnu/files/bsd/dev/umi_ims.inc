/* SPDX-License-Identifier: BSD-2-Clause
 * IMS bearer/readiness glue for CommCenter.  The file switch is sampled on a
 * thread call; ordinary data and SMS keep the old forwarding path. */
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <netinet/in_var.h>
#include <netinet6/in6_var.h>
#include "umi_ims_protocol.h"

#define IMS_LOG(...) hakios_tl("ims: " __VA_ARGS__)
static bool ims_enabled, ims_switch_wanted;
/* Route B (P1): /var/db/hakios-voice/modem-ims lets the modem run IMS while the bridge does the
 * QMI housekeeping. Mutually exclusive with ims-on, which keeps the AP as the registration owner. */
static bool modem_ims_enabled, modem_ims_wanted;
static bool ims_modem_ignored_logged;

/* Whichever IMS owner is on: ims-on (the AP stack) or modem-ims (route B). Both flags are written
 * under bb_lock. One predicate for everything both owners need alike: the synthesized WDS 0x5561
 * DAD complete and the private QMI media stubs (IMSRTP 0x28, AUDIO 0xe1); every other use below
 * names its route on purpose. */
static bool ims_owner_on(void)
{
    return ims_enabled || modem_ims_enabled;
}
static thread_call_t ims_switch_call;
static uint8_t ims_fix[2048];
static struct {
    uint8_t cid, channel;
    struct umi_ims_bearer bearer;
} ims_owner[CBQ_MAX_CLIENTS];
static struct {
    uint32_t node, port, sent_at, checked_at, next_at;
    uint16_t sequence, txn, message;
    bool pending, release, bound;
    uint8_t indications, timeout_logged;
    int bind_result, registration;
    int err, err_text;   /* IMSA 0x20 error code (TLV 0x11, u16) and error-text length (0x13); -1 absent */
} ims_audit = { 0, 0, 0, 0, 0, 0, 0, 0, false, false, false, 0, 0, -2, -1, -1, -1 };
/* IMSS 0x8f: sent once per switch-on, never retried after a timeout or a failed result. Before it,
 * the vendor IMS stack's preamble runs once (v29i experiment): the client's subscription bind, which
 * messages and 0x8f fields the modem supports, and the Config Ind Register that qcril performs before
 * it reads the enable state; then the read-only Get IMS Services Enabled (0x90). Each step waits for
 * its own answer; 0x8f goes out only after 0x2a's. All of it is client-local -- no NV, EFS, PDC or MBN. */
#define IMSS_STEP_NONE 0u
#define IMSS_STEP_MSGS 1u      /* 0x1e Get Supported Messages */
#define IMSS_STEP_BIND 2u      /* 0x98 Set Bind Subscription, only if 0x1e advertises it */
#define IMSS_STEP_FIELDS 3u    /* 0x1f Get Supported Fields, msg_id 0x8f */
#define IMSS_STEP_INDREG 4u    /* 0x2a Config Ind Register */
#define IMSS_STEP_GET 5u       /* 0x90 Get IMS Services Enabled */
#define IMSS_STEP_POLICY 6u    /* 0x48 Get IMS Policy Manager Settings (read-only, once per boot) */
#define IMSS_STEP_ENABLE 7u    /* 0x8f Set IMS Services Enabled */
#define IMSS_STEP_DONE 8u
static struct {
    uint32_t node, port, sent_at;
    uint16_t sequence, txn, message;
    bool pending, release, sent;
    int result;                 /* -2 not sent, -1 timeout, else the QMI result code */
    uint8_t step;               /* IMSS_STEP_*: the step this switch session is on */
    uint8_t bind_supported;     /* 0x98 sits in the modem's own Get Supported Messages bitmap */
} ims_imss = { 0, 0, 0, 0, 0, 0, false, false, false, -2, IMSS_STEP_NONE, 0 };
/* Boot-lifetime facts: the preamble has been answered (later sessions re-send only 0x8f), a preamble
 * step timed out (nothing more this boot), and the one-line answers already reported (bits 1..8). */
static uint8_t ims_imss_chain, ims_imss_stalled, ims_imss_logged;
/* IMSA indications: the modem's own registration and the reason it is not there, one log line per
 * change. Error text is an identity carrier, so only its length is kept. */
static struct {
    int registration, technology, voice;
    int err, err_text;   /* 0x23 TLV 0x10 error code (u16), TLV 0x12 error-text length; -1 absent */
} ims_modem = { -1, -1, -1, -1, -1 };
static uint8_t ims_nas_logged, ims_nas_rewritten;   /* the first 0x33's TLV types / its 0x23 rewrite */

/* ---- Route B probe: IMSDCM (QRTR service 770 = 0x302 v1 instance 0), observe only -------------
 * The modem's IMS stack does not build its own IMS PDN: it asks a host-side service for one
 * (81voltd's imsd.qmi; on Android imsdatadaemon hosts 770 on the AP node). P1 announces the
 * service and logs what arrives; nothing is answered on 0x20, so no bearer is created and the
 * boot only answers "does the X55 ask, and with which APN/profile/family" (report
 * INVESTIGATION-routeB-x55-noreg.md §3.2). Bounded: one line per distinct request, 30 per boot. */
#define UMI_DCM_LINES 30u
#define UMI_DCM_SEEN 8u
static struct {
    bool announced;
    uint8_t lines, seen_n;
    uint64_t seen[UMI_DCM_SEEN];
    uint8_t reply[32];
    uint32_t reply_len, reply_node, reply_port, reply_local;
} ims_dcm;
static uint8_t ims_wds_logged;   /* iOS's WDS 0x31/0xC2 values, one line per switch-on */

/* ---- stub IMSRTP (QMI service 0x28): Bambi's baseband RTP stack --------------------------------
 * SipStack's media init waits for RTPManager, whose default (baseband) stack creates a QMI client of
 * service 0x28 and sends INITIALIZE_REQ (0x0020). The X55 has no 0x28, so it answered CTL
 * GetClientId with QMI_ERR_INVALID_SERVICE_TYPE, the driver reported "QMI IMSRTP client creation
 * failed", Bambi went fatal and the 5 s MediaInitTimer took the IMS PDN down (v29i
 * INVESTIGATION-bambi-media). With the IMS switch on, the bridge allocates that client itself
 * (coachz_bb_qmux.h) and this answers its requests: INITIALIZE succeeds with the response's only
 * mandatory TLV (0x02 result, rtp-wire.h), which is all the driver needs to report the stack
 * controller initialized; UNINITIALIZE (0x0021) succeeds for symmetry; every other message is a
 * well-formed QMI_ERR_NOT_SUPPORTED, so a later call phase fails where it is logged instead of
 * pretending to have media. Transactions are the request's. One line per event, never per packet. */
#define IMS_RTP_INITIALIZE 0x0020u
#define IMS_RTP_UNINITIALIZE 0x0021u
#define IMS_RTP_REGISTER_SERVICES 0x0030u
#define IMS_RTP_UNINITIALIZE_ALL 0x003Du
#define IMS_RTP_SESSION_INITIALIZE 0x0022u   /* session lifecycle: answered only with umi_rtpok=1 */
#define IMS_RTP_SESSION_UNINITIALIZE 0x0023u
#define IMS_RTP_SESSION_CONFIGURE 0x0024u
#define IMS_RTP_SESSION_START 0x0025u
#define IMS_RTP_SESSION_STOP 0x0026u
#define IMS_RTP_CONFIGURE_RTCP_REPORTS 0x0029u       /* per-session link settings, indication-carried */
#define IMS_RTP_CONFIGURE_RTP_LINK_MONITOR 0x002Au
#define IMS_RTP_CONFIGURE_RTCP_LINK_MONITOR 0x002Bu
static uint8_t ims_rtp_out[CBQ_HDR + CBQ_SDU_HDR + 16u];   /* response 7, UNINITIALIZE_ALL indication 15 */
static uint8_t ims_rtp_session_out[CBQ_HDR + CBQ_SDU_HDR + 24u];   /* session indication: 22 TLV bytes at most */
static uint8_t ims_rtp_alloc[256];   /* client ids whose "allocated" line went out (cleared on release) */
static uint8_t ims_rtp_init[256];    /* client ids whose "INITIALIZE ok" line went out */
static uint8_t ims_rtp_uninit_ind[256];   /* client ids whose "0x003d indication sent" line went out */
static bool ims_rtpok_on;            /* boot argument umi_rtpok=1: answer the session lifecycle */
static uint8_t ims_rtp_session[256]; /* per client: the last session id handed to iOS (0 = none) */
static uint64_t ims_rtp_session_seen[2];   /* session message ids already reported */
static uint64_t ims_rtp_seen[2];     /* message ids already reported: one bit per id below 128 */

/* ---- one round trip for the synthetic 0x28/0xe1 answers ----------------------------------------
 * CommCenter's RTP stack arms its request timer after its QMI client's send returns and cancels it
 * from the answer. A real modem answers milliseconds later, so that is safe; this bridge answered
 * from inside the sending syscall and the two threads raced. The v29v call (logs/v29-snap/
 * v29v-call-1604.log 81175/81187) shows both outcomes of the race: "No timer found for request
 * SESSION_START" (the answer was processed before the timer existed) and "Timer for request
 * CONFIGURE_RTCP_REPORTS already exists" (a timer that was never cancelled). The SessionStart
 * timer then fired 10 s after SESSION_START (83189) and took the call down (BambiClient fatal,
 * 83190), even though every answer had been success.
 *
 * Hold each synthetic frame for a few ms and push it from a thread call, so the app always
 * finishes arming its timer before it sees the answer: one round trip, like the hardware. The
 * frames of one request keep their order (response first, then its indication). Nothing reaches
 * the modem either way; this only changes when CommCenter is told. */
#define IMS_RTP_TRIP_MS 5u
#define IMS_RTP_TRIP_SLOTS 8u
#define IMS_RTP_TRIP_MAX (CBQ_HDR + CBQ_SDU_HDR + 40u)
struct ims_rtp_trip { uint8_t frame[IMS_RTP_TRIP_MAX]; uint32_t len; uint8_t channel; };
static struct ims_rtp_trip ims_rtp_trip[IMS_RTP_TRIP_SLOTS];
static uint8_t ims_rtp_trip_head, ims_rtp_trip_n;
static thread_call_t ims_rtp_trip_call;

/* Hand every held frame to CommCenter, oldest first. Under bb_lock; the host test and the timer
 * body call it directly, and the switch drains what is pending before it changes behaviour. */
static void ims_rtp_trip_flush(void)
{
    while (ims_rtp_trip_n) {
        struct ims_rtp_trip *t = &ims_rtp_trip[ims_rtp_trip_head];
        bb_push(t->channel, t->frame, t->len);
        t->len = 0;
        ims_rtp_trip_head = (uint8_t)((ims_rtp_trip_head + 1u) % IMS_RTP_TRIP_SLOTS);
        ims_rtp_trip_n--;
    }
}

static void ims_rtp_trip_fire(thread_call_param_t, thread_call_param_t)
{
    if (!bb_lock) return;
    IOLockLock(bb_lock);
    ims_rtp_trip_flush();
    IOLockUnlock(bb_lock);
}

/* Queue one synthetic frame and arm its delivery. Under bb_lock. Without a worker (the host test)
 * or with the queue full it falls back to the immediate push, i.e. the pre-fix behaviour. */
static void ims_rtp_send(uint32_t channel, const uint8_t *frame, uint32_t len)
{
    if (!ims_rtp_trip_call || len > IMS_RTP_TRIP_MAX || ims_rtp_trip_n == IMS_RTP_TRIP_SLOTS) {
        bb_push(channel, frame, len);
        return;
    }
    struct ims_rtp_trip *t = &ims_rtp_trip[(ims_rtp_trip_head + ims_rtp_trip_n) % IMS_RTP_TRIP_SLOTS];
    memcpy(t->frame, frame, len);
    t->len = len;
    t->channel = (uint8_t)channel;
    if (ims_rtp_trip_n++ == 0) {
        uint64_t deadline;
        clock_interval_to_deadline(IMS_RTP_TRIP_MS, kMillisecondScale, &deadline);
        thread_call_enter_delayed(ims_rtp_trip_call, deadline);
    }
}

static void ims_rtp_note(uint32_t message, bool ok)
{
    if (message < 128u) {
        const uint64_t bit = 1ull << (message & 63u);
        if (ims_rtp_seen[message >> 6] & bit) return;
        ims_rtp_seen[message >> 6] |= bit;
    }
    IMS_LOG("imsrtp stub: msg 0x%04x answered %s\n", message, ok ? "ok" : "unsupported");
}

/* ---- session-level lifecycle, boot argument umi_rtpok=1 (default off) ---------------------------
 * Media init succeeds (above), so iOS dials, and SipStack then asks the RTP stack for a session:
 * SESSION_INITIALIZE (0x0022) with the RTP ports it wants in the SDP. The bridge answered
 * QMI_ERR_NOT_SUPPORTED, RTPManager reported rtp::SessionInitialize::Error, the call went
 * kCallDisconnectionReasonMediaError and the SIP session was hung up in the NULL state: the INVITE
 * was never sent (v29t INVESTIGATION-volte-call-1402 §0.2, 93803-93882). With umi_rtpok=1 the
 * session lifecycle is answered like the service-level messages are: result 0 in the response (the
 * response's only field, rtp-wire.h) and, after it, the matching indication, because the driver's
 * session state changes on indications (v29m §7.2 for 0x003D). Neither side carries media: the
 * bridge does not touch RTP, so a call can ring and be answered with no audio, and nothing here
 * reaches the modem. umi_rtpok=0 keeps today's byte-identical NOT_SUPPORTED answers.
 *
 * TLV ids (the generated tables in rtp-wire.c: each message's array carries its TLV id, so they are
 * read, not guessed): initialize ind status 0x01 (4-byte enum, 0 = IMSRTP_SUCCESS), app_id 0x02
 * (mandatory), session_id 0x10, cookie 0x11; uninitialize ind 0x01 status, 0x02 session_id, 0x10
 * app_id; configure ind 0x01, 0x02, 0x12 app_id, 0x11 cookie; start/stop ind 0x01, 0x02, 0x11
 * app_id; configure RTCP reports ind 0x01, 0x02, 0x10 app_id; both link monitors ind 0x01, 0x02,
 * 0x10 app_id. In the requests session_id is 0x01, app_id 0x10 (0x0023) / 0x11 (0x0025, 0x0026,
 * 0x002A, 0x002B) / 0x12 (0x0029) / 0x40 (0x0024), cookie 0x18 (0x0022) / 0x23 (0x0024). iOS sends
 * no app_id in any of these (the start request is session_id + 8-byte direction only), so the app
 * echo is there for completeness and omitted, as the IDL allows. The session id is the bridge's
 * own: iOS echoes it in every later session request, and it is copied back into that message's
 * indication. Direction is optional and omitted. One line per message id, never per packet. */
static uint32_t ims_rtp_session_tlvs(uint32_t message, uint8_t client, const uint8_t *sdu, uint32_t n, uint8_t *t)
{
    uint32_t at = 0, len = 0;
    const uint8_t *v;
    t[at] = 0x01; t[at + 1] = 4; t[at + 2] = 0;   /* status = IMSRTP_SUCCESS */
    t[at + 3] = t[at + 4] = t[at + 5] = t[at + 6] = 0;
    at += 7u;
    if (message == IMS_RTP_SESSION_INITIALIZE) {
        uint8_t app = 0xffu;   /* SipStack's configured default, as the 0x003D indication uses */
        if ((v = umi_ims_find(sdu, n, 0x01u, &len)) && len == 1u) app = v[0];
        const uint8_t session = ims_rtp_session[client] = (uint8_t)(ims_rtp_session[client] % 254u) + 1u;
        t[at] = 0x02; t[at + 1] = 1; t[at + 2] = 0; t[at + 3] = app; at += 4u;              /* app_id */
        t[at] = 0x10; t[at + 1] = 1; t[at + 2] = 0; t[at + 3] = session; at += 4u;          /* session_id */
        if ((v = umi_ims_find(sdu, n, 0x18u, &len)) && len == 4u) {                         /* cookie */
            t[at] = 0x11; t[at + 1] = 4; t[at + 2] = 0;
            memcpy(t + at + 3, v, 4);
            at += 7u;
        }
        return at;
    }
    if ((v = umi_ims_find(sdu, n, 0x01u, &len)) && len == 1u) {                             /* session_id */
        t[at] = 0x02; t[at + 1] = 1; t[at + 2] = 0; t[at + 3] = v[0]; at += 4u;
    } else if (ims_rtp_session[client]) {
        t[at] = 0x02; t[at + 1] = 1; t[at + 2] = 0; t[at + 3] = ims_rtp_session[client]; at += 4u;
    }
    uint32_t in, out;
    if (message == IMS_RTP_SESSION_UNINITIALIZE) {          /* app_id: request 0x10, indication 0x10 */
        in = out = 0x10u;
    } else if (message == IMS_RTP_SESSION_CONFIGURE) {      /* request 0x40, indication 0x12 */
        in = 0x40u; out = 0x12u;
    } else if (message == IMS_RTP_CONFIGURE_RTCP_REPORTS) { /* request 0x12, indication 0x10 */
        in = 0x12u; out = 0x10u;
    } else if (message == IMS_RTP_CONFIGURE_RTP_LINK_MONITOR ||
               message == IMS_RTP_CONFIGURE_RTCP_LINK_MONITOR) {   /* request 0x11, indication 0x10 */
        in = 0x11u; out = 0x10u;
    } else {                                                /* start/stop: request 0x11, indication 0x11 */
        in = out = 0x11u;
    }
    if ((v = umi_ims_find(sdu, n, in, &len)) && len == 1u) {                                /* app_id */
        t[at] = (uint8_t)out;
        t[at + 1] = 1; t[at + 2] = 0; t[at + 3] = v[0];
        at += 4u;
    }
    if (message == IMS_RTP_SESSION_CONFIGURE && (v = umi_ims_find(sdu, n, 0x23u, &len)) && len == 4u) {
        t[at] = 0x11; t[at + 1] = 4; t[at + 2] = 0;                                         /* cookie */
        memcpy(t + at + 3, v, 4);
        at += 7u;
    }
    return at;
}

static void ims_rtp_session_note(uint32_t message)
{
    if (message < 128u) {
        const uint64_t bit = 1ull << (message & 63u);
        if (ims_rtp_session_seen[message >> 6] & bit) return;
        ims_rtp_session_seen[message >> 6] |= bit;
    }
    IMS_LOG("imsrtp stub: session 0x%04x answered ok (umi_rtpok)\n", message);
}

/* One line per link-monitor message id whose answer is the response alone (0x002A/0x002B): their
 * indication is a link state event, so it is never synthesized. Dedupes like ims_rtp_note(). */
static void ims_rtp_link_note(uint32_t message)
{
    if (message < 128u) {
        const uint64_t bit = 1ull << (message & 63u);
        if (ims_rtp_seen[message >> 6] & bit) return;
        ims_rtp_seen[message >> 6] |= bit;
    }
    IMS_LOG("imsrtp stub: 0x%04x response only (link monitor)\n", message);
}

/* A request to 0x28 (CBQ_PRIVATE, either IMS owner): answer it, same transaction. 1 when answered. */
static int ims_rtp_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
    if (!ims_owner_on() || r.service != CBQ_SVC_IMSRTP) return 0;
    const uint32_t message = umi_ims_rd16(sdu + 3);
    const bool session = message >= IMS_RTP_SESSION_INITIALIZE && message <= IMS_RTP_SESSION_STOP;
    /* umi_rtpok=1: the per-session link settings sent between SESSION_START and the answer
     * (0x0027..0x002F: ConfigureRTPLinkMonitor 0x002a, ConfigureRTCPLinkMonitor 0x002b, 0x0029 ...).
     * NOT_SUPPORTED there ends the call as kMediaTimeout right after SIP 183 (v29u 15:45:20,
     * logs/v29-snap/v29u-call-1545.log 94823-94887). Response-only success: nothing is monitored.
     *
     * Only 0x0029 also gets its indication. It has one in the IDL and the driver's
     * CONFIGURE_RTCP_REPORTS request timer is cancelled from it: v29v logged "Timer for request
     * CONFIGURE_RTCP_REPORTS already exists" while two 0x0029 requests a millisecond apart each got
     * only a response (logs/v29-snap/v29v-call-1604.log 81187). The driver logs it as
     * "imsrtpConfigureRTCP: Indicator" and carries on.
     *
     * 0x002A/0x002B must NOT get one: their indication is a link monitor EVENT, not a config ack.
     * The driver turned each one straight into a media error on the live v32 call
     * (logs/v29-snap/v32-rtpok-ims-oslog-0825.txt, 08:25:27.729/27.736: "rtp::ConfigureRTPLinkMonitor: Success" then
     * handleMediaSessionMediaError error=5, "rtp::ConfigureRTCPLinkMonitor: Success" then error=6,
     * CANCEL "Reason: SIP;cause=200;text=\"RTP Timeout\""): the indication's status enum is the
     * RTP/RTCP link state (0x07 TX only active, 0x08 RX only active, ...), so a synthesized success
     * event reads as a timeout. Their requests arm no request timer at all (no startTimerForRequest
     * line for them; 0x0029's is requestId 41), so the response alone is complete. Response only,
     * one line per id.
     *
     * 0x0027/0x0028 and 0x002C..0x002F have no indication in the IDL, so a response is all they get. */
    /* Parse IMSRTP session/media parameters and publish to /dev/hakios-imsrtp for userland RTP engine. */
    static struct hakios_imsrtp_event ims_rtp_active;
    if (ims_rtpok_on && (session || message == 0x0027u || message == 0x0028u)) {
        const uint32_t body = umi_ims_rd16(sdu + 5);
        uint32_t tl = 0;
        const uint8_t *v = nullptr;
        if (message == IMS_RTP_SESSION_INITIALIZE) {
            memset(&ims_rtp_active, 0, sizeof ims_rtp_active);
            ims_rtp_active.kind = HAKIOS_IMSRTP_INIT;
            ims_rtp_active.session_id = (uint8_t)((ims_rtp_session[r.cid] % 254u) + 1u);
            ims_rtp_active.ptime_ms = 20;
            ims_rtp_active.mode_set = 0xff;
            if ((v = umi_ims_find(sdu, r.len, 0x02u, &tl)) && tl >= 2u)
                ims_rtp_active.local_port = (uint16_t)(v[0] | (v[1] << 8));
            if ((v = umi_ims_find(sdu, r.len, 0x03u, &tl)) && tl >= 6u) {
                uint32_t ver = (uint32_t)v[0] | ((uint32_t)v[1] << 8);
                ims_rtp_active.ip_version = (ver == 1u) ? 6u : 4u;
                uint32_t slen = v[4];
                if (5u + slen <= tl) {
                    if (slen >= sizeof(ims_rtp_active.local_ip)) slen = sizeof(ims_rtp_active.local_ip) - 1u;
                    memcpy(ims_rtp_active.local_ip, v + 5, slen);
                    ims_rtp_active.local_ip[slen] = 0;
                }
            }
            IMS_LOG("imsrtp media INIT sid=%u local=[%s]:%u (v%u)\n",
                    ims_rtp_active.session_id, ims_rtp_active.local_ip, ims_rtp_active.local_port, ims_rtp_active.ip_version);
            imsrtp_dev_push(&ims_rtp_active);
        } else if (message == IMS_RTP_SESSION_CONFIGURE) {
            ims_rtp_active.kind = HAKIOS_IMSRTP_CONFIG;
            if ((v = umi_ims_find(sdu, r.len, 0x01u, &tl)) && tl >= 1u) ims_rtp_active.session_id = v[0];
            if ((v = umi_ims_find(sdu, r.len, 0x10u, &tl)) && tl >= 2u)
                ims_rtp_active.local_port = (uint16_t)(v[0] | (v[1] << 8));
            if ((v = umi_ims_find(sdu, r.len, 0x12u, &tl)) && tl >= 6u) {
                uint32_t ver = (uint32_t)v[0] | ((uint32_t)v[1] << 8);
                ims_rtp_active.ip_version = (ver == 1u) ? 6u : 4u;
                uint32_t slen = v[4];
                if (5u + slen <= tl) {
                    if (slen >= sizeof(ims_rtp_active.remote_ip)) slen = sizeof(ims_rtp_active.remote_ip) - 1u;
                    memcpy(ims_rtp_active.remote_ip, v + 5, slen);
                    ims_rtp_active.remote_ip[slen] = 0;
                }
            }
            if ((v = umi_ims_find(sdu, r.len, 0x13u, &tl)) && tl >= 2u)
                ims_rtp_active.remote_port = (uint16_t)(v[0] | (v[1] << 8));
            if ((v = umi_ims_find(sdu, r.len, 0x21u, &tl)) && tl >= 2u)
                ims_rtp_active.remote_rtcp = (uint16_t)(v[0] | (v[1] << 8));
            if ((v = umi_ims_find(sdu, r.len, 0x14u, &tl)) && tl >= 8u)
                ims_rtp_active.codec = v[4];
            if ((v = umi_ims_find(sdu, r.len, 0x15u, &tl)) && tl >= 9u) {
                ims_rtp_active.rx_pt = v[0];
                ims_rtp_active.tx_pt = v[1];
                ims_rtp_active.ptime_ms = (uint16_t)(v[2] | (v[3] << 8));
                ims_rtp_active.clock_rate = (uint16_t)(v[6] | (v[7] << 8));
                ims_rtp_active.dtmf_pt = v[8];
            }
            if ((v = umi_ims_find(sdu, r.len, 0x16u, &tl)) && tl >= 4u) {
                ims_rtp_active.octet_align = v[0];
                ims_rtp_active.mode_set = v[3];
            }
            IMS_LOG("imsrtp media CONFIG sid=%u [%s]:%u <-> [%s]:%u codec=%u rate=%u pt=%u/%u dtmf=%u oa=%u\n",
                    ims_rtp_active.session_id, ims_rtp_active.local_ip, ims_rtp_active.local_port,
                    ims_rtp_active.remote_ip, ims_rtp_active.remote_port, ims_rtp_active.codec,
                    ims_rtp_active.clock_rate, ims_rtp_active.rx_pt, ims_rtp_active.tx_pt,
                    ims_rtp_active.dtmf_pt, ims_rtp_active.octet_align);
            imsrtp_dev_push(&ims_rtp_active);
        } else if (message == IMS_RTP_SESSION_START) {
            ims_rtp_active.kind = HAKIOS_IMSRTP_START;
            if ((v = umi_ims_find(sdu, r.len, 0x01u, &tl)) && tl >= 1u) ims_rtp_active.session_id = v[0];
            if ((v = umi_ims_find(sdu, r.len, 0x10u, &tl)) && tl >= 1u) ims_rtp_active.direction = v[0];
            IMS_LOG("imsrtp media START sid=%u dir=0x%x\n", ims_rtp_active.session_id, ims_rtp_active.direction);
            imsrtp_dev_push(&ims_rtp_active);
        } else if (message == IMS_RTP_SESSION_STOP) {
            struct hakios_imsrtp_event ev = ims_rtp_active;
            ev.kind = HAKIOS_IMSRTP_STOP;
            IMS_LOG("imsrtp media STOP sid=%u\n", ev.session_id);
            imsrtp_dev_push(&ev);
        } else if (message == IMS_RTP_SESSION_UNINITIALIZE) {
            struct hakios_imsrtp_event ev = ims_rtp_active;
            ev.kind = HAKIOS_IMSRTP_UNINIT;
            IMS_LOG("imsrtp media UNINIT sid=%u\n", ev.session_id);
            imsrtp_dev_push(&ev);
        } else if (message == 0x0027u) {
            struct hakios_imsrtp_event ev = ims_rtp_active;
            ev.kind = HAKIOS_IMSRTP_DTMF;
            ev.dtmf_duration = 160;
            ev.dtmf_volume = 10;
            if ((v = umi_ims_find(sdu, r.len, 0x10u, &tl)) && tl >= 1u) ev.dtmf_digit = v[0];
            else if ((v = umi_ims_find(sdu, r.len, 0x02u, &tl)) && tl >= 1u) ev.dtmf_digit = v[0];
            if ((v = umi_ims_find(sdu, r.len, 0x11u, &tl)) && tl >= 2u) ev.dtmf_duration = (uint16_t)(v[0] | (v[1] << 8));
            if ((v = umi_ims_find(sdu, r.len, 0x12u, &tl)) && tl >= 1u) ev.dtmf_volume = v[0];
            IMS_LOG("imsrtp media DTMF sid=%u digit=%u dur=%u body=%u\n", ev.session_id, ev.dtmf_digit, ev.dtmf_duration, body);
            imsrtp_dev_push(&ev);
        }
    }
    const bool link = ims_rtpok_on && message >= 0x0027u && message <= 0x002Fu;
    const bool link_ind = link && message == IMS_RTP_CONFIGURE_RTCP_REPORTS;
    const bool ok = message == IMS_RTP_INITIALIZE || message == IMS_RTP_UNINITIALIZE ||
                    message == IMS_RTP_REGISTER_SERVICES || message == IMS_RTP_UNINITIALIZE_ALL ||
                    (session && ims_rtpok_on) || link;
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, ok ? 0u : CBQ_ERR_NOT_SUPPORTED, nullptr, 0,
                                          ims_rtp_out, sizeof ims_rtp_out);
    if (!n) return 0;
    ims_rtp_send(channel, ims_rtp_out, n);
    if (message == IMS_RTP_UNINITIALIZE_ALL) {
        /* The 0x3D response only arms a 10 s UninitializeAllTimer; the controller advances on the
         * UNINITIALIZE_ALL indication (type 0x04, txn 0) and only then sends RegisterServices
         * (v29m INVESTIGATION-rtp-stack-init §7.2). Same channel and cid as the response, after it;
         * ims_rtp_send holds its own copy, so the buffer is reused.
         *
         * The app id travels in TLV 0x01, not TLV 0x10. The driver's indication dispatcher
         * (0x1b7d6ead4) parses TLV 0x01 into one struct and finds TLV 0x10 into another, and the
         * handler (0x1b7d78864) uses the TLV 0x10 byte (0x1b7d788c4) only for the "<N> sessions
         * dropped" log, while the TLV 0x01 byte (0x1b7d78948) becomes the appId passed to
         * 0x1b7b961f4 -> RegisterServices -> 0x1b7b95fa8 -> RTPManager::stackControllerInitialized
         * -> BambiClient::handleRTPManagerInitializationStatus, which compares it with the
         * SipStack's app id (0xff, applyConfig) and silently drops the callback on a mismatch. Our
         * old echo into TLV 0x10 therefore only produced "255 sessions dropped" and appId=0, and
         * the 5 s MediaInitTimer still fired (v29o IMS log, 2026-11-13). Echo the request's TLV
         * 0x01 app id in TLV 0x01 instead. TLV 0x02 is the indication's mandatory status
         * (imsrtp_uninitialize_all_rtp_session_ind_v01, a 32-bit enum: 0 = IMSRTP_SUCCESS), and
         * TLV 0x10 is the sessions-dropped count: this bridge has no sessions, so 0. */
        uint8_t app = 0xffu;   /* SipStack's configured default; every request carries TLV 0x01 */
        bool echoed = false;
        const uint32_t body = umi_ims_rd16(sdu + 5);
        for (uint32_t at = 0; at + 3u <= body; ) {
            const uint8_t *t = sdu + 7u + at;
            const uint32_t l = umi_ims_rd16(t + 1);
            if (at + 3u + l > body) break;
            if (t[0] == 0x01u && l >= 1u) { app = t[3]; echoed = true; break; }
            at += 3u + l;
        }
        const uint8_t tlvs[15] = {
            0x01, 0x01, 0x00, app,                  /* app_id */
            0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,   /* status = IMSRTP_SUCCESS */
            0x10, 0x01, 0x00, 0x00,                 /* 0 sessions dropped */
        };
        const uint32_t m = cbq_private_indication(message, r.service, r.cid, tlvs, sizeof tlvs,
                                                  ims_rtp_out, sizeof ims_rtp_out);
        if (m) {
            ims_rtp_send(channel, ims_rtp_out, m);
            if (!ims_rtp_uninit_ind[r.cid]) {
                ims_rtp_uninit_ind[r.cid] = 1;
                IMS_LOG("imsrtp stub: 0x003d indication sent (app 0x%02x%s dropped 0)\n",
                    app, echoed ? "" : " default");
            }
        }
    }
    if ((session || link_ind) && ok) {
        /* umi_rtpok=1: response first, then the indication the session state machine advances on
         * (same channel and cid; ims_rtp_send holds its own copy). No media: the RTP stream
         * itself is not touched, so audio stays silent and nothing reaches the modem. */
        uint8_t tlvs[24];
        const uint32_t k = ims_rtp_session_tlvs(message, r.cid, sdu, r.len, tlvs);
        const uint32_t m = cbq_private_indication(message, r.service, r.cid, tlvs, k,
                                                  ims_rtp_session_out, sizeof ims_rtp_session_out);
        if (m) ims_rtp_send(channel, ims_rtp_session_out, m);
        ims_rtp_session_note(message);
    } else if (message == IMS_RTP_INITIALIZE) {
        if (ims_rtp_init[r.cid]) return 1;
        ims_rtp_init[r.cid] = 1;
        IMS_LOG("imsrtp stub: INITIALIZE ok\n");
    } else if (link && (message == IMS_RTP_CONFIGURE_RTP_LINK_MONITOR ||
                        message == IMS_RTP_CONFIGURE_RTCP_LINK_MONITOR)) {
        ims_rtp_link_note(message);   /* response only: the indication is a link event, not an ack */
    } else {
        ims_rtp_note(message, ok);
    }
    return 1;
}

/* ---- AUDIO 0xe1 / msg 0x33: the ARI carrier ------------------------------------------------
 * LegacyQMIRTPStackController::initialize first sends AUDIO 0xe1 msg 0x33 with TLV 0x01 =
 * { event, call type, session id } (the builder at ipt.dis:849990 stores three bytes: the event
 * index 0..8, the call type 2 = kVoIP / 9 = kEmergency, and the session id) and returns on any
 * error - its completion block reads only qmi::Error - so the whole media-init chain
 * (UninitializeAll, RegisterServices, ServiceInitialize) never starts (v29l
 * INVESTIGATION-rtp-stack-init §1.4).
 *
 * The old gate compared the first two bytes with 0x0208, which only ever matched the boot
 * message's {event 8, kVoIP, session 0}. Every CallEvent a live call sends carries another
 * event/type/session triple, so it was refused: v29v at 16:04:44.055 and again 10 s later logged
 * "audio::SendAudioCallEvent: Error QMI_ERR_NOT_SUPPORTED" followed by "Received a media error
 * notification for unknown qmi session ID 0" (logs/v29-snap/v29v-call-1604.log 79504-79598,
 * 83191-83195). The gate is the event enum, not a magic 16-bit id: any valid event (0..8) with
 * call type kVoIP is acknowledged. kEmergency (and every other call type) is deliberately left
 * NOT_SUPPORTED, exactly as today: this bridge has no media, so an emergency call must keep
 * failing fast instead of appearing connected and silent. Every other AUDIO message keeps today's
 * NOT_SUPPORTED path. No local AUDIO IDL header exists; the response has no field the caller reads
 * beyond the result TLV. */
#define IMS_AUDIO_CALL_EVENT 0x33u
#define IMS_ARI_CALL_EVENT 0x0208u   /* the boot triple's first two bytes, as the old gate read it */
#define IMS_ARI_EVENT_MAX 8u
#define IMS_ARI_CALL_TYPE_VOIP 2u
#define IMS_ARI_CALL_TYPE_EMERGENCY 9u
static uint8_t ims_audio_out[CBQ_HDR + CBQ_SDU_HDR + 8u];
static uint16_t ims_audio_seen;   /* event index (0..8) whose one line went out */

/* An AUDIO message the media-init window needs answered (either IMS owner): 1 when answered. Under bb_lock. */
static int ims_audio_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
    if (!ims_owner_on() || r.service != 0xe1u || umi_ims_rd16(sdu + 3) != IMS_AUDIO_CALL_EVENT) return 0;
    uint32_t len = 0;
    const uint8_t *ari = umi_ims_find(sdu, r.len, 0x01, &len);
    if (!ari || len != 3u) return 0;
    const uint8_t event = ari[0], type = ari[1];
    const uint8_t session = ari[2];
    if (event > IMS_ARI_EVENT_MAX || type != IMS_ARI_CALL_TYPE_VOIP) return 0;
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, ims_audio_out, sizeof ims_audio_out);
    if (!n) return 0;
    ims_rtp_send(channel, ims_audio_out, n);
    if (!(ims_audio_seen & (1u << event))) {
        ims_audio_seen |= (uint16_t)(1u << event);
        IMS_LOG("audio 0xe1 0x33 call event %u (voip, session %u) answered ok\n", event, session);
    }
    return 1;
}

/* Synthesized WDS "DAD complete" indications. CommCenter's QMIDataContextIP keeps an IPv6-only IMS
 * context in model state 2 until this Apple-vendor indication arrives; the X55 never sends it. Each
 * indication carries TLV 0x01 = 16-byte IPv6 address || 1-byte prefix length.
 *
 * CommCenter reads the interface address exactly once: in handleSettingsResponse, when the WDS 0x2d
 * (Get Runtime Settings) reply is delivered, via its delegate's getInterfaceAddress(IPv6), and keeps
 * it in fIPv6ConfigAddress (v29i INVESTIGATION-dad-address-source §1.1). That read is a plain
 * interface-address lookup, while the pdp_ip's SLAAC global address only arrives ~57 ms after the
 * reply, so the expected value was empty and every 0x5561 was ignored. The handler then compares
 * std::string equality over 17 binary bytes: address + prefix length (§2.2). An IMS context's 0x2d
 * reply is therefore held (not delivered under bb_lock, never blocking the modem RX path) until its
 * pdp_ip has a global IPv6 address, bounded by IMS_DAD_HOLD_MS; ims_dad_hold_run_locked() hands it
 * over from the modem thread or the timer. The addresses present at that delivery instant are what
 * CommCenter read, so they become candidates (d->t0) with their ifaddr prefix.
 *
 * An activation's reply also yields every distinct global candidate - TLV 0x25 (16+1 prefix), then
 * 0x27 / 0x28 (16 or 17; a missing prefix byte borrows 0x25's, else 64) - plus up to two global IPv6
 * ifaddrs of the matching pdp_ip (prefix from the ifaddr netmask) both at delivery (t0) and at send
 * time, deduplicated by address AND prefix (the 17th byte is compared by CommCenter); at most
 * IMS_DAD_MAX_CANDIDATES indications go out ~1 s after that reply. If the interface has no address
 * then, the lookup is retried up to IMS_DAD_IFRETRY_MAX times, 1 s apart, inside CommCenter's ~10 s
 * DAD window, and only candidates not already sent go out; if none ever appear one line per
 * activation says so. Extra indications are harmless: the handler walks its own address table and
 * cancels the timer if any entry matches. Transactions are nonzero and increment like the X55's own
 * WDS indications (v29g noted txn 0 as the only wire difference). Only IMS contexts get them: the
 * reply's TLV 0x2e (P-CSCF list) has length > 1 for IMS and 1 for the internet PDN; when 0x2e is
 * absent entirely the old send-everything fallback is kept and the log line says so. XNU's address
 * ioctl is NOT a precondition (it only ever runs for an interface's first address, which the IMS
 * unit never gets); it is kept as a diagnostic. One send round per activation per WDS client, gated
 * by ims_owner_on(): both IMS owners need it, the AP stack (ims-on) and the modem's (route B). One
 * "0x2d held" line and one hash-only line per activation report the hold and
 * whether the delivery-time address went out (v29i report §4.1); addresses are never logged. */
#define IMS_DAD_MESSAGE 0x5561u
#define IMS_DAD_TLV 0x01u
#define IMS_DAD_DELAY_MS 1000u
#define IMS_DAD_IFADDR_MS 1000u     /* interface-address snapshot / retry spacing */
#define IMS_DAD_IFRETRY_MAX 5u      /* interface-address retries per activation (~10 s DAD window) */
#define IMS_DAD_MAX_CANDIDATES 7u   /* 0x2d TLVs (3) + delivery-time and send-time pdp_ip ifaddrs (4) */
#define IMS_DAD_MAX_IFADDR 2u
/* Holding 0x2d was tried in v29j and waits for nothing: CommCenter brings pdp_ipN up only after
 * it has the reply, so no address can appear first (the 500 ms always ran out), and the DAD
 * error it aimed at had already gone with v29i's 0x5561. Off; the host tests turn it on. */
#ifndef IMS_DAD_HOLD_ENABLE
#define IMS_DAD_HOLD_ENABLE 0
#endif
#define IMS_DAD_HOLD_MS 500u        /* a held 0x2d reply is delivered at the latest this long after it arrived */
#define IMS_DAD_HOLD_POLL_MS 20u    /* interface-address snapshot spacing while a reply is held */
#define IMS_DAD_HOLD_MAX 1024u      /* the largest 0x2d reply held (field replies are ~150 bytes) */
#define IMS_DAD_HOLD_SLOTS 2u       /* one hold per IMS context; one exists, a second is never lost */
static thread_call_t ims_dad_call, ims_dad_hold_call;
struct ims_dad_state {
    uint8_t cand[IMS_DAD_MAX_CANDIDATES][17];  /* distinct global IPv6 candidates from the 0x2d reply */
    uint8_t cand_n;
    uint8_t cfg[IMS_DAD_MAX_CANDIDATES][17];   /* snapshot taken when arming, re-checked at the deadline */
    uint8_t cfg_n;
    uint8_t t0[IMS_DAD_MAX_IFADDR][17];  /* pdp_ip ifaddrs at the moment the 0x2d reply was delivered */
    uint8_t t0_n, t0_sent;               /* ... and how many of them made it into the sent candidates */
    uint8_t tlv[3][16];         /* raw TLV 0x25/0x27/0x28, for the one hash line only */
    uint8_t tlv_len[3];         /* 0 absent, else the TLV length (16 or 17) */
    uint8_t seen[3];            /* TLV 0x25/0x27/0x28 were present in that reply (diagnostic only) */
    uint8_t ims;                /* TLV 0x2e: 0 absent (fallback), 1 length <= 1 (internet), 2 length > 1 (IMS) */
    uint32_t armed_at, ifretry_at;
    uint16_t start_txn;         /* the Start request whose TLV list was already logged */
    bool armed, sent;           /* timer pending / this activation already fired its first send */
    uint8_t ifretries;          /* interface-address retries made for this activation */
    bool ifretry;               /* interface-address retry pending */
    bool hash_logged;           /* the one hash line has been printed */
    bool ifnone_logged;         /* the one "no interface address" line has been printed */
    bool closed;                /* teardown seen: do not arm until a new Start */
    bool noted, start_logged;   /* one "not armed" line and one Start TLV list per activation */
    bool held;                  /* this activation's 0x2d reply sits in ims_dad_hold, undelivered */
    bool hold_logged;           /* the one "0x2d held" line has been printed */
};
static struct ims_dad_state ims_dad[CBQ_MAX_CLIENTS];
struct ims_dad_ifaddr { uint8_t addr[IMS_DAD_MAX_IFADDR][17]; uint8_t n; };  /* address + prefix length */
static struct ims_dad_ifaddr ims_dad_ifaddrs[CBP_INTERFACES];
/* One WDS runtime-settings reply per IMS context may be held (this port has one). The copy is
 * static: no allocation in the modem RX path. */
struct ims_dad_hold {
    uint8_t frame[IMS_DAD_HOLD_MAX];
    uint32_t len, at;
    uint8_t channel, slot, cid;
    bool used;
};
static struct ims_dad_hold ims_dad_hold[IMS_DAD_HOLD_SLOTS];

static uint32_t ims_dad_ifaddr_at;   /* bb_now_ms() of the last interface-address snapshot (under bb_lock) */
/* Bumped under bb_lock by every activation boundary (switch, Start, teardown) and by the start of
 * every refresh. A walk stores its snapshot only while the generation it started with still holds,
 * so a snapshot taken for an earlier activation, or superseded by a newer walk, is never written
 * back. */
static uint32_t ims_dad_ifaddr_gen;
static bool ims_dad_ioctl_ok[CBP_INTERFACES];   /* diagnostic only: XNU called the address ioctl */
static volatile uint8_t ims_dad_ioctl_flag[CBP_INTERFACES];   /* set from the driver ioctl path, read under bb_lock */
static uint8_t ims_dad_out[CBQ_HDR + CBQ_SDU_HDR + 3u + 17u];
static void ims_dad_switch(bool enabled);
static bool ims_dad_ifaddr_wanted(void);
static void ims_dad_ifaddr_refresh(bool force);
static void ims_dad_hold_run_locked(void);
static void ims_dad_hold_release_all(void);

/* Drop a slot's held 0x2d reply without delivering it (a new activation). Under bb_lock. */
static void ims_dad_hold_clear(uint32_t slot)
{
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++)
        if (ims_dad_hold[i].used && ims_dad_hold[i].slot == slot) ims_dad_hold[i].used = false;
    ims_dad[slot].held = false;
}

static void ims_prune(void)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++) {
        const struct cbq_client *c = &bb_qmux.client[i];
        if (c->used && c->service == UMI_IMS_WDS && c->cid == ims_owner[i].cid && c->channel == ims_owner[i].channel) continue;
        bzero(&ims_owner[i], sizeof ims_owner[i]);
        if (c->used && c->service == UMI_IMS_WDS) {
            ims_owner[i].cid = c->cid;
            ims_owner[i].channel = c->channel;
        }
    }
}

static bool ims_switch_present(const char *path)
{
    vnode_t node;
    if (vnode_lookup(path, 0, &node, vfs_context_kernel())) return false;
    const bool present = vnode_vtype(node) == VREG;
    vnode_put(node);
    return present;
}

/* Read both switch files now: the wanted flags, under bb_lock afterwards. VFS, so the caller's
 * thread - never the modem thread (umi_bb_poll queues it through ims_schedule_switch) and never
 * under bb_lock. A CTL request that can create or list a private service calls it directly, so it
 * never answers with a sample up to a second old (v29i: 0x28 refused at client allocation). */
static void ims_sample_switch(void)
{
    const bool enabled = ims_switch_present("/var/db/hakios-voice/ims-on");
    const bool modem = ims_switch_present("/var/db/hakios-voice/modem-ims");
    IOLockLock(bb_lock);
    ims_switch_wanted = enabled;
    /* One registration owner. ims-on keeps the AP stack, so route B's modem IMS stays off; the
     * line is emitted on the transition only, never every sample. */
    modem_ims_wanted = !enabled && modem;
    if (enabled && modem) {
        if (!ims_modem_ignored_logged) {
            ims_modem_ignored_logged = true;
            IMS_LOG("modem-ims ignored: ims-on present\n");
        }
    } else {
        ims_modem_ignored_logged = false;
    }
    IOLockUnlock(bb_lock);
}

static void ims_check_switch(thread_call_param_t, thread_call_param_t)
{
    ims_sample_switch();
}

static void ims_schedule_switch(void)
{
    static uint32_t sampled;
    static bool known;
    const uint32_t now = bb_now_ms();
    if (!ims_switch_call || (known && now - sampled < 1000u)) return;
    known = true;
    sampled = now;
    thread_call_enter(ims_switch_call);
}

/* Does this host frame need today's switch before it is answered? CTL GetClientId may allocate
 * 0x28 (private only while an IMS owner is on) and GetVersionInfo lists it; both are answered on
 * the caller's thread, which may sample the files again first. Parsed like cbq_from_host: one
 * QMUX frame, control flags 0, service 0. */
static bool ims_ctl_wants_switch(const uint8_t *f, uint32_t n)
{
    if (n < CBQ_HDR + CBQ_CTL_HDR || f[0] != CBQ_IF_TYPE || cbq_rd16(f + 1) + 1u != n || f[3] != 0u || f[4] != 0u)
        return false;
    const uint32_t id = cbq_rd16(f + CBQ_HDR + 2);
    return id == CBQ_CTL_GET_CLIENT_ID || id == CBQ_CTL_GET_VERSION_INFO;
}

static void ims_state(bool enabled)
{
    if (enabled == ims_enabled) return;
    ims_enabled = enabled;
    bb_qmux.imsrtp = ims_owner_on() ? 1u : 0u;   /* 0x28 is private while either owner is on */
    bzero(ims_rtp_alloc, sizeof ims_rtp_alloc);
    bzero(ims_rtp_init, sizeof ims_rtp_init);
    bzero(ims_rtp_uninit_ind, sizeof ims_rtp_uninit_ind);
    bzero(ims_rtp_session, sizeof ims_rtp_session);
    bzero(ims_rtp_session_seen, sizeof ims_rtp_session_seen);
    ims_rtp_trip_flush();   /* answers already promised to CommCenter still belong to it */
    ims_audio_seen = 0;
    IMS_LOG("switch %s; IPv6-only IMS bearer and address ioctls %s\n", enabled ? "on" : "off", enabled ? "enabled" : "disabled");
    bzero(ims_owner, sizeof ims_owner);
    ims_dad_switch(ims_owner_on());   /* the other owner's switch, if still on, keeps the DAD state */
    ims_audit.registration = -1;
    ims_audit.bind_result = -2;
    ims_audit.err = ims_audit.err_text = -1;
    ims_audit.timeout_logged = 0;
    ims_audit.next_at = bb_now_ms();
    if (enabled)
        IMS_LOG("Apple owns SIP; modem IMS configuration blocked; RTP svc28 %s svc52 %s\n",
            cbq_server_find(&bb_qmux, 0x28u) ? "announced" : "absent",
            cbq_server_find(&bb_qmux, 0x52u) ? "announced" : "absent");
}

/* Route B edge. ims_enabled (Apple's stack) stays off, so the Apple-stack bookkeeping above stays
 * untouched: only one IMS registration owner exists, and it is the modem. What both owners need is
 * shared, never route-specific: the synthesized 0x5561 DAD indication, and the private QMI media
 * stubs (0x28, 0xe1) the AP-side SIP stack runs in either route - CommCenter's media init fails
 * with the 0x28 client refused and then tears the IMS PDN down every 5 s (v31 §1.2, §5.2). */
static void modem_ims_state(bool enabled)
{
    if (enabled == modem_ims_enabled) return;
    modem_ims_enabled = enabled;
    /* TODO(route B P3, INVESTIGATION-routeB-x55-noreg.md §4 P3): when the modem IMS stack is the
     * registration owner for real, this gate (and the AUDIO 0xe1 stub's) should be ims_enabled
     * only, so iOS's SIP stack stops registering in parallel. Not changed in this P1 probe: the
     * supervisor decides; v32 must stay byte-comparable. */
    bb_qmux.imsrtp = ims_owner_on() ? 1u : 0u;   /* 0x28 is private while either owner is on */
    bzero(&ims_dcm, sizeof ims_dcm);   /* a new session announces 770 again */
    ims_wds_logged = 0;
    ims_dad_switch(ims_owner_on());   /* route B needs the synthesized 0x5561 as much as ims-on does */
    if (ims_imss.port) ims_imss.release = true;   /* its QRTR port is released by the next poll */
    ims_imss.node = ims_imss.port = ims_imss.sent_at = 0;
    ims_imss.pending = ims_imss.sent = false;
    ims_imss.result = -2;
    ims_imss.step = IMSS_STEP_NONE;
    ims_imss.message = 0;
    ims_modem.registration = ims_modem.technology = ims_modem.voice = -1;
    ims_modem.err = ims_modem.err_text = -1;
    ims_audit.indications = 0;
    ims_audit.err = ims_audit.err_text = -1;   /* the new session's first 0x20 error is an event again */
    ims_audit.bind_result = -2;
    ims_audit.timeout_logged = 0;
    if (enabled) {
        ims_audit.next_at = bb_now_ms();   /* bind and register indications at once */
        ims_nas_logged = ims_nas_rewritten = 0;   /* one diagnostic line set per switch-on */
    }
    IMS_LOG("modem-ims switch %s; IMSS 0x8f from bridge\n", enabled ? "on" : "off");
}

/* The sampled switches -> the owners everything else asks (bb_lock held). Each edge is a no-op when
 * nothing changed, so this runs on every poll pass and again before a host frame is answered. */
static void ims_apply_switch(void)
{
    ims_state(ims_switch_wanted);
    modem_ims_state(modem_ims_wanted);
}

static void ims_note(const char *direction, const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
    if (!ims_enabled || !umi_ims_tlvs_ok(sdu, n)) return;
    const uint32_t message = umi_ims_rd16(sdu + 3);
    if (r.service == UMI_IMS_WDS) {
        ims_prune();
        const int slot = cbq_client_find(&bb_qmux, r.channel, r.service, r.cid);
        if (slot < 0) return;
        struct umi_ims_bearer *b = &ims_owner[slot].bearer;
        umi_ims_observe(b, sdu, n);
        if (b->selected && (message == UMI_IMS_START || message == UMI_IMS_STOP || message == UMI_IMS_STATUS ||
                           message == UMI_IMS_GET_SETTINGS || (sdu[0] == 4u && message == 1u))) {
            IMS_LOG("WDS %s type %u msg 0x%x result %d family %u bearer %u pcscf-list %u\n",
                direction, sdu[0], message, sdu[0] == 2u ? umi_ims_result(sdu, n) : -1,
                b->family, b->active, b->pcscf);
        }
        if (sdu[0] == 2u && message == UMI_IMS_GET_SETTINGS) {
            // Schema metadata only: lets the next run identify X55's runtime-settings fields (the
            // IMS/internet difference) without dumping an address, identity or payload. Not gated
            // by the APN check: CommCenter's Start request never matches it on this firmware.
            for (uint32_t at = 7u; at < n; at += 3u + umi_ims_rd16(sdu + at + 1))
                IMS_LOG("WDS settings field 0x%x bytes %u\n", sdu[at], umi_ims_rd16(sdu + at + 1));
        }
    } else if (r.service == 0x0bu && message == 0x34u) {
        IMS_LOG("UIM authenticate %s type %u result %d\n", direction, sdu[0], sdu[0] == 2u ? umi_ims_result(sdu, n) : -1);
    } else if (r.service == 0x28u || r.service == 0x52u) {
        IMS_LOG("RTP %s svc 0x%x msg 0x%x type %u result %d\n", direction, r.service, message, sdu[0],
            sdu[0] == 2u ? umi_ims_result(sdu, n) : -1);
    }
}

static void ims_note_private(uint32_t service, uint32_t message)
{
    if (ims_owner_on() && (service == 0xe1u || service == 0xe6u || service == 0xe7u))
        IMS_LOG("private svc 0x%x msg 0x%x unsupported by bridge/X55\n", service, message);
}

/* ---- synthesized WDS "DAD complete" indication (Apple vendor 0x5561) ---------------- */

static bool ims_dad_armed;   /* one of the two owners holds the DAD state */

static void ims_dad_switch(bool enabled)
{
    /* ims-on and modem-ims are exclusive per sample, but both edges land in one poll pass
     * (ims_state, then modem_ims_state): a repeated state is a no-op, so neither edge can wipe the
     * activation the other just armed. */
    if (enabled == ims_dad_armed) return;
    ims_dad_armed = enabled;
    ims_dad_hold_release_all();   /* off: CommCenter still waits for that reply, deliver it */
    for (uint32_t slot = 0; slot < CBQ_MAX_CLIENTS; slot++) bzero(&ims_dad[slot], sizeof ims_dad[slot]);
    bzero(ims_dad_ifaddrs, sizeof ims_dad_ifaddrs);
    bzero(ims_dad_hold, sizeof ims_dad_hold);
    ims_dad_ifaddr_at = 0;
    ims_dad_ifaddr_gen++;   /* a walk started before the switch must not store its snapshot back */
    /* an address XNU installed stays installed: ims_dad_ioctl_ok[] is kept across switches */
}

/* Everything arming actually needs on the wire, per WDS client slot: an active IPv6 context whose
 * 0x2d reply for THIS activation carried at least one global IPv6 candidate. No request-side
 * APN/family check: neither exists in CommCenter's request, and the indication is harmless for a
 * context whose model is not waiting in state 2. The address-ioctl record is diagnostic only. */
static bool ims_dad_eligible(uint32_t slot)
{
    const struct cbq_client *q = &bb_qmux.client[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const struct ims_dad_state *d = &ims_dad[slot];
    return q->used && q->service == UMI_IMS_WDS && q->channel < CBQ_CHANNELS &&
        bb_pdp_owners[slot] == 1u + q->cid + ((uint32_t)q->channel << 8) &&
        c->mux >= 1u && c->mux <= CBP_INTERFACES && c->family == 6u && c->active &&
        !d->closed && d->cand_n > 0u;
}

static int ims_dad_addr_same(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 16u; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* CommCenter compares 17 bytes, the prefix included, so two candidates are the same only when both
 * match: an indication with the right address but the wrong prefix never matches (§2.2). */
static int ims_dad_addr17_same(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 17u; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* The pdp_ip unit a WDS client's mux maps to, or CBP_INTERFACES when unknown. */
static uint32_t ims_dad_unit(const struct cbp_client *c)
{
    return c->mux >= 1u && c->mux <= CBP_INTERFACES ? c->mux - 1u : CBP_INTERFACES;
}

/* The pdp_ip addresses at the instant the 0x2d reply is handed to CommCenter. Its
 * getInterfaceAddress(IPv6) reads the interface once, right then (v29i report §1.1), so these are
 * what it compares the 0x5561 against; they go out as candidates too. Under bb_lock. */
static void ims_dad_capture_t0(struct ims_dad_state *d, uint32_t unit)
{
    d->t0_n = 0;
    for (uint32_t i = 0; unit < CBP_INTERFACES && i < ims_dad_ifaddrs[unit].n && i < IMS_DAD_MAX_IFADDR; i++, d->t0_n++)
        for (unsigned k = 0; k < 17u; k++) d->t0[d->t0_n][k] = ims_dad_ifaddrs[unit].addr[i][k];
}

/* Hand one held reply to CommCenter now, keeping the addresses present at this instant as the
 * delivery-time candidates. `log` prints the activation's one "0x2d held" line; a retransmission
 * hands the older answer over without one, so the line describes the answer actually waited on.
 * Under bb_lock. */
static void ims_dad_hold_release(struct ims_dad_hold *h, bool log)
{
    struct ims_dad_state *d = &ims_dad[h->slot];
    const uint32_t unit = ims_dad_unit(&bb_pdp_clients[h->slot]);
    const uint32_t ms = bb_now_ms() - h->at;
    const bool have = unit < CBP_INTERFACES && ims_dad_ifaddrs[unit].n > 0u;
    if (have) ims_dad_capture_t0(d, unit);
    bb_push(h->channel, h->frame, h->len);
    d->held = false;
    h->used = false;
    if (log && !d->hold_logged) {
        d->hold_logged = true;
        IMS_LOG("0x2d held %u ms (ifaddr=%s) cid %u\n", ms, have ? "yes" : "timeout", h->cid);
    }
}

/* The switch went off with replies still held: they are real X55 answers CommCenter waits for, so
 * deliver rather than drop them. Under bb_lock. */
static void ims_dad_hold_release_all(void)
{
    if (!bb_lock) return;
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++)
        if (ims_dad_hold[i].used) ims_dad_hold_release(&ims_dad_hold[i], true);
}

/* Some reply is held: the snapshot must be re-taken at IMS_DAD_HOLD_POLL_MS. Lock-free hint. */
static bool ims_dad_hold_pending(void)
{
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++) if (ims_dad_hold[i].used) return true;
    return false;
}

/* Hint for the modem thread (umi_bb_poll): an activation waiting for its send or for its held 0x2d
 * reply - the first one armed, an interface-address retry, or a reply in ims_dad_hold - needs the
 * snapshot. Read without bb_lock (like ims_enabled); the refresh re-checks the throttle and the
 * generation under the lock. */
static bool ims_dad_ifaddr_wanted(void)
{
    if (ims_dad_hold_pending()) return true;
    for (uint32_t slot = 0; slot < CBQ_MAX_CLIENTS; slot++)
        if (ims_dad[slot].armed || ims_dad[slot].ifretry) return true;
    return false;
}

/* Diagnostic only, v29i report §5: CommCenter takes fIPv6ConfigAddress from its delegate rather
 * than from the 0x2d reply, so print whether the candidate sets agree. FNV-1a over the 16-byte
 * address; "--------" marks an absent TLV. `match` counts interface addresses equal to one of the
 * reply's TLV candidates. `t0` is the pdp_ip address set at 0x2d delivery, as hash/prefix, and
 * `t0sent` how many of those entries went out as indications (0 when the hold timed out without an
 * address). One line per activation, never an address. */
static uint32_t ims_dad_hash(const uint8_t *a)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 16u; i++) { h ^= a[i]; h *= 16777619u; }
    return h;
}

static const char *ims_dad_hash_str(char out[9], const uint8_t *a, uint8_t len)
{
    static const char hx[] = "0123456789abcdef";
    if (!len) return "--------";
    const uint32_t h = ims_dad_hash(a);
    for (unsigned i = 0; i < 8u; i++) out[i] = hx[(h >> (28u - 4u * i)) & 0xfu];
    out[8] = 0;
    return out;
}

static void ims_dad_log_hashes(uint32_t slot)
{
    const struct ims_dad_state *d = &ims_dad[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const uint32_t unit = ims_dad_unit(c);
    char h25[9], h27[9], h28[9], hx[9], ifbuf[24], t0buf[40];
    uint32_t used = 0, match = 0, t0used = 0;
    ifbuf[0] = t0buf[0] = 0;
    for (uint32_t i = 0; unit < CBP_INTERFACES && i < ims_dad_ifaddrs[unit].n && used + 10u <= sizeof ifbuf; i++) {
        const uint8_t *a = ims_dad_ifaddrs[unit].addr[i];
        const char *str = ims_dad_hash_str(hx, a, 16u);
        if (used) ifbuf[used++] = ',';
        for (unsigned k = 0; k < 8u; k++) ifbuf[used++] = str[k];
        ifbuf[used] = 0;
        for (uint32_t j = 0; j < 3u; j++)
            if (d->tlv_len[j] && ims_dad_addr_same(a, d->tlv[j])) { match++; break; }
    }
    for (uint32_t i = 0; i < d->t0_n && t0used + 13u <= sizeof t0buf; i++) {
        const uint8_t *a = d->t0[i];
        const char *str = ims_dad_hash_str(hx, a, 16u);
        const uint8_t plen = a[16];
        if (t0used) t0buf[t0used++] = ',';
        for (unsigned k = 0; k < 8u; k++) t0buf[t0used++] = str[k];
        t0buf[t0used++] = '/';
        if (plen >= 100u) t0buf[t0used++] = (char)('0' + plen / 100u);
        if (plen >= 10u) t0buf[t0used++] = (char)('0' + plen / 10u % 10u);
        t0buf[t0used++] = (char)('0' + plen % 10u);
        t0buf[t0used] = 0;
    }
    IMS_LOG("DAD addr hashes cid %u mux %u: tlv25=%s tlv27=%s tlv28=%s if=[%s] match=%u t0=[%s] t0sent=%u\n",
        bb_qmux.client[slot].cid, c->mux, ims_dad_hash_str(h25, d->tlv[0], d->tlv_len[0]),
        ims_dad_hash_str(h27, d->tlv[1], d->tlv_len[1]), ims_dad_hash_str(h28, d->tlv[2], d->tlv_len[2]),
        ifbuf, match, t0buf, d->t0_sent);
}

/* The interface lookup ended this activation (retries used up, or the context closed under a
 * pending retry). Print the one hash line if it has not gone out yet - so `if=[] match=0` is never
 * the only trace - and, when the snapshot really has no address, exactly one plain line saying so,
 * which tells a too-early snapshot apart from sets that genuinely disagree. Under bb_lock. */
static void ims_dad_note_no_ifaddr(uint32_t slot)
{
    struct ims_dad_state *d = &ims_dad[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const uint32_t unit = ims_dad_unit(c);
    if (!d->hash_logged) {
        d->hash_logged = true;
        ims_dad_log_hashes(slot);
    }
    if (unit >= CBP_INTERFACES || ims_dad_ifaddrs[unit].n > 0u) return;   /* no lookup to report */
    if (d->ifnone_logged) return;
    d->ifnone_logged = true;
    IMS_LOG("DAD no interface address on pdp_ip%u after %u tries\n", unit, d->ifretries);
}

/* Candidate set captured at arm time versus the current one (a later 0x2d reply may replace it).
 * Includes the prefix byte: it is part of the indication payload. */
static bool ims_dad_cand_same(const struct ims_dad_state *d)
{
    if (d->cfg_n != d->cand_n) return false;
    for (uint32_t i = 0; i < d->cfg_n; i++)
        for (unsigned k = 0; k < 17u; k++) if (d->cfg[i][k] != d->cand[i][k]) return false;
    return true;
}

/* Keep one distinct global address per prefix; link-local (fe80::/10) and unspecified are dropped,
 * exactly as CommCenter ignores a link-local DAD. The prefix byte is stored last and dedupe counts
 * it: the same address with another prefix is a different 17-byte value to CommCenter. */
static void ims_dad_add_candidate(struct ims_dad_state *d, const uint8_t *addr, uint8_t prefix)
{
    if (d->cand_n >= IMS_DAD_MAX_CANDIDATES || !cbp_v6_global(addr)) return;
    for (uint32_t i = 0; i < d->cand_n; i++)
        if (ims_dad_addr_same(d->cand[i], addr) && d->cand[i][16] == prefix) return;
    for (unsigned i = 0; i < 16u; i++) d->cand[d->cand_n][i] = addr[i];
    d->cand[d->cand_n][16] = prefix;
    d->cand_n++;
}

/* Every 16-byte IPv6-shaped field of a runtime-settings reply can be the one CommCenter calls
 * fIPv6ConfigAddress, so collect them all. Also read the 0x2e P-CSCF list: its length is the IMS /
 * internet discriminator the v29g report found (length 1 = empty = internet; > 1 = P-CSCFs = IMS). */
static void ims_dad_capture(struct ims_dad_state *d, const uint8_t *sdu, uint32_t n)
{
    uint32_t l25 = 0, l27 = 0, l28 = 0, l2e = 0;
    const uint8_t *v25 = umi_ims_find(sdu, n, 0x25u, &l25);
    const uint8_t *v27 = umi_ims_find(sdu, n, 0x27u, &l27);
    const uint8_t *v28 = umi_ims_find(sdu, n, 0x28u, &l28);
    const uint8_t *v2e = umi_ims_find(sdu, n, 0x2eu, &l2e);
    const uint8_t prefix = v25 && l25 == 17u ? v25[16] : 64u;
    d->cand_n = 0;
    d->seen[0] = v25 != nullptr;
    d->seen[1] = v27 != nullptr;
    d->seen[2] = v28 != nullptr;
    d->ims = !v2e ? 0u : l2e > 1u ? 2u : 1u;
    /* Keep the raw reply values for the one hash line; never sent and never printed. */
    d->tlv_len[0] = v25 && (l25 == 16u || l25 == 17u) ? (uint8_t)l25 : 0u;
    d->tlv_len[1] = v27 && (l27 == 16u || l27 == 17u) ? (uint8_t)l27 : 0u;
    d->tlv_len[2] = v28 && (l28 == 16u || l28 == 17u) ? (uint8_t)l28 : 0u;
    for (unsigned k = 0; k < 16u; k++) {
        if (d->tlv_len[0]) d->tlv[0][k] = v25[k];
        if (d->tlv_len[1]) d->tlv[1][k] = v27[k];
        if (d->tlv_len[2]) d->tlv[2][k] = v28[k];
    }
    if (v25 && l25 == 17u) ims_dad_add_candidate(d, v25, v25[16]);
    if (v27 && (l27 == 16u || l27 == 17u)) ims_dad_add_candidate(d, v27, l27 == 17u ? v27[16] : prefix);
    if (v28 && (l28 == 16u || l28 == 17u)) ims_dad_add_candidate(d, v28, l28 == 17u ? v28[16] : prefix);
}

/* Once per activation: the Start request's TLV types and lengths, never values. CommCenter's Start
 * request on this firmware carries no APN string TLV, so this list is what a later IMS discriminator
 * (the one-byte TLV the IMS request has and the internet one lacks) would have to come from. */
static void ims_dad_log_start(uint32_t slot, const uint8_t *sdu)
{
    static const char hx[] = "0123456789abcdef";
    char fields[96];
    uint32_t used = 0;
    const uint32_t end = 7u + umi_ims_rd16(sdu + 5);
    fields[0] = 0;
    for (uint32_t at = 7u; at + 3u <= end && used + 13u < sizeof fields; at += 3u + umi_ims_rd16(sdu + at + 1)) {
        const uint32_t type = sdu[at], len = umi_ims_rd16(sdu + at + 1);
        fields[used++] = '0';
        fields[used++] = 'x';
        fields[used++] = hx[type >> 4];
        fields[used++] = hx[type & 0xfu];
        fields[used++] = '/';
        fields[used++] = '0';
        fields[used++] = 'x';
        fields[used++] = hx[(len >> 12) & 0xfu];
        fields[used++] = hx[(len >> 8) & 0xfu];
        fields[used++] = hx[(len >> 4) & 0xfu];
        fields[used++] = hx[len & 0xfu];
        fields[used++] = ' ';
        fields[used] = 0;
    }
    IMS_LOG("WDS start cid %u fields %s\n", bb_qmux.client[slot].cid, fields);
}

/* A 0x2d reply arrived but this activation cannot get an indication: say why, once. */
static void ims_dad_note_not_armed(uint32_t slot)
{
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const uint32_t unit = c->mux >= 1u && c->mux <= CBP_INTERFACES ? c->mux - 1u : CBP_INTERFACES;
    IMS_LOG("DAD not armed cid %u mux %u: family %u active %u settings %u v6 %u ioctl %u closed %u worker %u\n",
        bb_qmux.client[slot].cid, c->mux, c->family, c->active, c->settings, cbp_v6_global(c->v6cfg),
        unit < CBP_INTERFACES ? ims_dad_ioctl_ok[unit] : 0u, ims_dad[slot].closed, ims_dad_call != nullptr);
}

/* Arm the ~1 s timer for every eligible slot that has not used its attempt for this activation. */
static void ims_dad_try_locked(void)
{
    if (!ims_owner_on() || !ims_dad_call) return;
    for (uint32_t slot = 0; slot < CBQ_MAX_CLIENTS; slot++) {
        struct ims_dad_state *d = &ims_dad[slot];
        if (d->sent || d->armed || !ims_dad_eligible(slot)) continue;
        for (uint32_t i = 0; i < d->cand_n; i++)
            for (unsigned k = 0; k < 17u; k++) d->cfg[i][k] = d->cand[i][k];
        d->cfg_n = d->cand_n;
        d->armed = true;
        d->armed_at = bb_now_ms();
        uint64_t deadline;
        clock_interval_to_deadline(IMS_DAD_DELAY_MS, kMillisecondScale, &deadline);
        thread_call_enter_delayed(ims_dad_call, deadline);
    }
}

/* The per-activation line. `sent` reports the transaction range actually used; a failed send uses
 * 0..0 and says why. `count` is how many indications this attempt carried. Only cid, mux, counts,
 * TLV presence and transactions are ever printed. */
static void ims_dad_report(uint32_t slot, bool sent, uint16_t first, uint16_t last, const char *why, uint32_t count)
{
    const struct ims_dad_state *d = &ims_dad[slot];
    if (sent)
        IMS_LOG("DAD complete indication cid %u mux %u candidates %u (tlv 0x25/0x27/0x28 present: %u/%u/%u) txn %u..%u sent%s\n",
            bb_qmux.client[slot].cid, bb_pdp_clients[slot].mux, count, d->seen[0], d->seen[1], d->seen[2],
            first, last, d->ims == 2u ? "" : " (no 0x2e field: fallback send)");
    else
        IMS_LOG("DAD complete indication cid %u mux %u candidates %u (tlv 0x25/0x27/0x28 present: %u/%u/%u) txn 0..0 not sent: %s\n",
            bb_qmux.client[slot].cid, bb_pdp_clients[slot].mux, count, d->seen[0], d->seen[1], d->seen[2], why);
}

/* WDS indications from the X55 count 1, 2, 3, ... (not QMI's usual 0); wrap past 0 the same way. */
static uint16_t ims_dad_txn;
static uint16_t ims_dad_next_txn(void)
{
    if (++ims_dad_txn == 0u) ++ims_dad_txn;
    return ims_dad_txn;
}

/* Has this 17-byte candidate already been queued for this activation - in the list being built or
 * in the TLV snapshot the first send took? */
static bool ims_dad_candidate_sent(const struct ims_dad_state *d, const uint8_t (*list)[17], uint32_t n, const uint8_t *a)
{
    for (uint32_t i = 0; i < n; i++) if (ims_dad_addr17_same(list[i], a)) return true;
    for (uint32_t i = 0; i < d->cfg_n; i++) if (ims_dad_addr17_same(d->cfg[i], a)) return true;
    return false;
}

/* The timer expired: re-check under bb_lock and make this activation's attempt(s): one indication
 * per distinct global candidate - the 0x2d TLV snapshot and the delivery-time interface addresses
 * on the first send, then up to two global IPv6 ifaddrs of the matching pdp_ip on every send.
 * Candidates are deduplicated over all 17 bytes on each send, so a retry never repeats what the
 * first one already put on the wire. */
static void ims_dad_send_locked(uint32_t slot, bool retry)
{
    struct ims_dad_state *d = &ims_dad[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const uint32_t unit = ims_dad_unit(c);
    const uint8_t cid = bb_qmux.client[slot].cid;
    if (retry) d->ifretry = false; else d->sent = true;
    if (d->ims == 1u) {
        if (!retry) ims_dad_report(slot, false, 0, 0, "internet context (0x2e len <= 1)", d->cand_n);
        return;
    }
    if (!ims_dad_eligible(slot) || (!retry && !ims_dad_cand_same(d))) {
        if (!retry) ims_dad_report(slot, false, 0, 0, "state changed", d->cand_n);
        else ims_dad_note_no_ifaddr(slot);   /* the retries end here: one line says how they went */
        return;
    }
    const bool have_ifaddr = unit < CBP_INTERFACES && ims_dad_ifaddrs[unit].n > 0u;
    uint8_t list[IMS_DAD_MAX_CANDIDATES][17];
    uint32_t count = 0;
    if (!retry) {
        for (uint32_t i = 0; i < d->cfg_n && count < IMS_DAD_MAX_CANDIDATES; i++, count++)
            for (unsigned k = 0; k < 17u; k++) list[count][k] = d->cfg[i][k];
        /* What CommCenter read from the interface when the 0x2d reply was delivered. */
        for (uint32_t i = 0; i < d->t0_n && count < IMS_DAD_MAX_CANDIDATES; i++)
            if (!ims_dad_candidate_sent(d, list, count, d->t0[i])) {
                for (unsigned k = 0; k < 17u; k++) list[count][k] = d->t0[i][k];
                count++;
            }
        d->t0_sent = 0;
        for (uint32_t i = 0; i < d->t0_n; i++) d->t0_sent += ims_dad_candidate_sent(d, list, count, d->t0[i]);
    }
    if (unit < CBP_INTERFACES)
        for (uint32_t i = 0; i < ims_dad_ifaddrs[unit].n && count < IMS_DAD_MAX_CANDIDATES; i++)
            if (!ims_dad_candidate_sent(d, list, count, ims_dad_ifaddrs[unit].addr[i])) {
                for (unsigned k = 0; k < 17u; k++) list[count][k] = ims_dad_ifaddrs[unit].addr[i][k];
                count++;
            }
    if (!count) {
        if (!retry) ims_dad_report(slot, false, 0, 0, "no candidates", d->cand_n);
    } else {
        uint8_t tlvs[3u + 17u];
        uint16_t first = 0, last = 0;
        for (uint32_t i = 0; i < count; i++) {
            tlvs[0] = IMS_DAD_TLV;
            umi_ims_wr16(tlvs + 1, 17u);
            for (unsigned k = 0; k < 17u; k++) tlvs[3u + k] = list[i][k];
            const uint16_t txn = ims_dad_next_txn();
            const uint32_t n = cbq_private_message(CBQ_QMI_INDICATION, txn, IMS_DAD_MESSAGE, UMI_IMS_WDS, cid, 0,
                tlvs, sizeof tlvs, ims_dad_out, sizeof ims_dad_out);
            if (!n) {
                ims_dad_report(slot, false, 0, 0, "no room", count);
                return;
            }
            bb_push(bb_qmux.client[slot].channel ? CBH_QMUX2 : CBH_QMUX1, ims_dad_out, n);
            if (!first) first = txn;
            last = txn;
        }
        ims_dad_report(slot, true, first, last, nullptr, count);
    }
    /* One hash line per activation: with the interface addresses when they exist, otherwise on the
     * last retry (ims_dad_note_no_ifaddr() also covers a context that closes first). */
    if (!d->hash_logged && (retry || have_ifaddr || unit >= CBP_INTERFACES)) {
        d->hash_logged = true;
        ims_dad_log_hashes(slot);
    }
    /* No interface address: look again IMS_DAD_IFADDR_MS later through the same thread call, up to
     * IMS_DAD_IFRETRY_MAX times - together with the first send that spans CommCenter's ~10 s DAD
     * window. A retry sends only interface addresses not already sent as TLVs. The ifnet walk stays
     * outside bb_lock (ims_dad_finish()); a teardown clears ifretry in ims_dad_poll(). */
    if (unit < CBP_INTERFACES && !have_ifaddr) {
        if (d->ifretries < IMS_DAD_IFRETRY_MAX) {
            d->ifretries++;
            d->ifretry = true;
            d->ifretry_at = bb_now_ms();
            uint64_t deadline;
            clock_interval_to_deadline(IMS_DAD_IFADDR_MS, kMillisecondScale, &deadline);
            thread_call_enter_delayed(ims_dad_call, deadline);
        } else {
            ims_dad_note_no_ifaddr(slot);   /* retries used up: one line, never a silent if=[] */
        }
    }
}

/* Re-check the activation under bb_lock, then send. Called from the timer and from the poll. */
static void ims_dad_run_locked(void)
{
    const uint32_t now = bb_now_ms();
    for (uint32_t slot = 0; slot < CBQ_MAX_CLIENTS; slot++) {
        struct ims_dad_state *d = &ims_dad[slot];
        if (d->armed && now - d->armed_at >= IMS_DAD_DELAY_MS) {
            d->armed = false;
            ims_dad_send_locked(slot, false);
            continue;   /* a retry armed just now is due later, not in this pass */
        }
        if (d->ifretry && now - d->ifretry_at >= IMS_DAD_IFADDR_MS) ims_dad_send_locked(slot, true);
    }
    ims_dad_try_locked();
}

static void ims_dad_finish(void)
{
    if (!ims_owner_on() || !bb_lock) return;
    ims_dad_ifaddr_refresh(true);   /* the send or hold deadline is the moment the addresses matter */
    IOLockLock(bb_lock);
    ims_dad_hold_run_locked();      /* a due hold is released before anything is sent for it */
    ims_dad_run_locked();
    IOLockUnlock(bb_lock);
}

static void ims_dad_fire(thread_call_param_t, thread_call_param_t) { ims_dad_finish(); }
/* The hold's own deadline: fires the forced snapshot + release, independent of the send/retry call. */
static void ims_dad_hold_fire(thread_call_param_t, thread_call_param_t) { ims_dad_finish(); }

/* The WDS client's IPv6 context is still up; when it is not, the next activation may try again. */
static bool ims_dad_slot_live(uint32_t slot)
{
    const struct cbq_client *q = &bb_qmux.client[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    return q->used && q->service == UMI_IMS_WDS && q->channel < CBQ_CHANNELS &&
        c->mux >= 1u && c->mux <= CBP_INTERFACES && c->family == 6u && c->active;
}

/* Snapshot the matching pdp_ip interfaces' global IPv6 ifaddrs (at most IMS_DAD_MAX_IFADDR each,
 * address + prefix length from the netmask). Called with bb_lock RELEASED: ifnet_get_address_list_
 * family() takes the ifnet/ifa locks and allocates with Z_WAITOK, so the walk must neither run under
 * our lock nor in an interrupt. `force` is a send or hold deadline (ims_dad_finish()); the
 * non-forced call from umi_bb_poll() only runs while an activation waits for its send or its held
 * reply (ims_dad_ifaddr_wanted()) and is throttled to once a second - or to IMS_DAD_HOLD_POLL_MS
 * while a 0x2d reply is held, so it is delivered right after the address appears.
 *
 * Consistency: every activation boundary (switch, Start, teardown) and the start of every refresh
 * bumps ims_dad_ifaddr_gen under bb_lock. The walk records the generation it started with and stores
 * its local snapshot only if that generation still holds, so a walk taken for a previous activation -
 * or one already superseded by a newer walk - is dropped instead of being written back. Every store
 * is a whole snapshot under bb_lock; every reader holds bb_lock. */
static void ims_dad_ifaddr_refresh(bool force)
{
    if (!ims_owner_on() || !bb_lock) return;
    if (!force && !ims_dad_ifaddr_wanted()) return;
    const uint32_t now = bb_now_ms();
    const uint32_t interval = ims_dad_hold_pending() ? IMS_DAD_HOLD_POLL_MS : IMS_DAD_IFADDR_MS;
    IOLockLock(bb_lock);
    if (!force && ims_dad_ifaddr_at && now - ims_dad_ifaddr_at < interval) {
        IOLockUnlock(bb_lock);
        return;
    }
    const uint32_t gen = ++ims_dad_ifaddr_gen;
    ims_dad_ifaddr_at = now;
    IOLockUnlock(bb_lock);
    struct ims_dad_ifaddr snap[CBP_INTERFACES];
    bzero(snap, sizeof snap);
    for (uint32_t unit = 0; unit < CBP_INTERFACES; unit++) {
        ifnet_t ifp = bb_pdp_ifaces[unit].ifp;   /* set once at init, never cleared */
        ifaddr_t *list = nullptr;
        if (!ifp || ifnet_get_address_list_family(ifp, &list, AF_INET6)) continue;
        struct ims_dad_ifaddr *s = &snap[unit];
        for (uint32_t i = 0; list[i] && s->n < IMS_DAD_MAX_IFADDR; i++) {
            struct sockaddr_in6 sin6 = {}, mask = {};
            uint8_t *a = s->addr[s->n];
            uint8_t prefix = 64u;
            if (ifaddr_address(list[i], (struct sockaddr *)&sin6, sizeof sin6) || sin6.sin6_family != AF_INET6) continue;
            for (unsigned k = 0; k < 16u; k++) a[k] = sin6.sin6_addr.s6_addr[k];
            if (!cbp_v6_global(a)) continue;
            if (!ifaddr_netmask(list[i], (struct sockaddr *)&mask, sizeof mask) && mask.sin6_family == AF_INET6) {
                prefix = 0;
                for (unsigned k = 0; k < 16u; k++)
                    for (uint8_t b = 0x80u; b && (mask.sin6_addr.s6_addr[k] & b); b >>= 1) prefix++;
            }
            a[16] = prefix;
            s->n++;
        }
        ifnet_free_address_list(list);
    }
    IOLockLock(bb_lock);
    if (ims_dad_ifaddr_gen == gen)
        for (uint32_t unit = 0; unit < CBP_INTERFACES; unit++) ims_dad_ifaddrs[unit] = snap[unit];
    IOLockUnlock(bb_lock);
}

/* Deliver a held 0x2d reply as soon as its pdp_ip has a global IPv6 address, or at the
 * IMS_DAD_HOLD_MS bound. The addresses present at this instant are what CommCenter's
 * getInterfaceAddress(IPv6) reads, so they are kept as candidates (t0) for the 0x5561. A context
 * that closed first drops the reply: it must never arrive after its teardown. The modem thread calls
 * this every pass, and the dedicated ims_dad_hold_call timer fires at the bound, so a re-armed
 * send/retry timer can never push the deadline out. Under bb_lock. */
static void ims_dad_hold_run_locked(void)
{
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++) {
        struct ims_dad_hold *h = &ims_dad_hold[i];
        if (!h->used) continue;
        struct ims_dad_state *d = &ims_dad[h->slot];
        const uint32_t unit = ims_dad_unit(&bb_pdp_clients[h->slot]);
        const uint32_t ms = bb_now_ms() - h->at;
        const bool have = unit < CBP_INTERFACES && ims_dad_ifaddrs[unit].n > 0u;
        if (!ims_dad_slot_live(h->slot) || d->closed || bb_qmux.client[h->slot].cid != h->cid) {
            d->held = false;   /* teardown: the reply is dropped, never delivered late */
            h->used = false;
            continue;
        }
        if (!have && ms < IMS_DAD_HOLD_MS) continue;
        ims_dad_hold_release(h, true);
    }
}

/* Take a WDS runtime-settings (0x2d) reply for an IMS context instead of pushing it to CommCenter.
 * Returns true when the frame was taken (ims_dad_hold_run_locked delivers it later) and false when
 * it must go out immediately: every other context and service is untouched. Under bb_lock. */
static bool ims_dad_hold_response(uint32_t channel, uint32_t slot, const uint8_t *sdu, uint32_t n,
                                  const uint8_t *frame, uint32_t len)
{
    if (!IMS_DAD_HOLD_ENABLE || !ims_owner_on() || !bb_lock || slot >= CBQ_MAX_CLIENTS || n < 7u || sdu[0] != 2u ||
        umi_ims_rd16(sdu + 3) != UMI_IMS_GET_SETTINGS) return false;
    struct ims_dad_state *d = &ims_dad[slot];
    const uint32_t unit = ims_dad_unit(&bb_pdp_clients[slot]);
    /* pdp_ip0 is the internet PDN: never delay it, whatever its TLV 0x2e happens to contain. */
    if (unit == 0u || unit >= CBP_INTERFACES || d->ims != 2u || !ims_dad_slot_live(slot)) return false;
    if (len > IMS_DAD_HOLD_MAX) return false;
    /* A retransmitted 0x2d for this context: the older answer goes now, it is the one CommCenter
     * waits for. Another context's hold is kept (one entry each). */
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++) {
        if (!ims_dad_hold[i].used || ims_dad_hold[i].slot != slot) continue;
        IMS_LOG("0x2d retransmit; older answer handed over cid %u\n", ims_dad_hold[i].cid);
        ims_dad_hold_release(&ims_dad_hold[i], false);
    }
    const bool have = ims_dad_ifaddrs[unit].n > 0u;
    if (have) {
        ims_dad_capture_t0(d, unit);
        if (!d->hold_logged) {
            d->hold_logged = true;
            IMS_LOG("0x2d not held (ifaddr present) cid %u\n", bb_qmux.client[slot].cid);
        }
        return false;   /* an address is already there: CommCenter can read it now */
    }
    uint32_t at = IMS_DAD_HOLD_SLOTS;
    for (uint32_t i = 0; i < IMS_DAD_HOLD_SLOTS; i++)
        if (!ims_dad_hold[i].used) { at = i; break; }
    if (at == IMS_DAD_HOLD_SLOTS) {   /* more IMS contexts than entries: the oldest is handed over */
        at = 0;
        for (uint32_t i = 1; i < IMS_DAD_HOLD_SLOTS; i++)
            if ((int32_t)(ims_dad_hold[i].at - ims_dad_hold[at].at) < 0) at = i;
        ims_dad_hold_release(&ims_dad_hold[at], true);
    }
    struct ims_dad_hold *h = &ims_dad_hold[at];
    for (uint32_t i = 0; i < len; i++) h->frame[i] = frame[i];
    h->len = len;
    h->channel = (uint8_t)channel;
    h->slot = (uint8_t)slot;
    h->cid = bb_qmux.client[slot].cid;
    h->at = bb_now_ms();
    h->used = true;
    d->held = true;
    /* Its own timer: the shared send/retry call being re-armed can never move this bound. */
    if (ims_dad_hold_call) {
        uint64_t deadline;
        clock_interval_to_deadline(IMS_DAD_HOLD_MS, kMillisecondScale, &deadline);
        thread_call_enter_delayed(ims_dad_hold_call, deadline);
    }
    return true;
}

/* Under bb_lock, once per modem-thread pass: absorb successful address ioctls (diagnostic only),
 * drop state of a client whose IPv6 context is gone, release a held 0x2d reply when its address is
 * there or its bound expired, and send whatever is due (the timer is a hint; this is the
 * guarantee). */
static void ims_dad_poll(void)
{
    for (uint32_t unit = 0; unit < CBP_INTERFACES; unit++)
        if (__atomic_exchange_n(&ims_dad_ioctl_flag[unit], 0u, __ATOMIC_RELAXED)) ims_dad_ioctl_ok[unit] = true;
    if (!ims_owner_on()) return;
    ims_dad_hold_run_locked();
    for (uint32_t slot = 0; slot < CBQ_MAX_CLIENTS; slot++) {
        struct ims_dad_state *d = &ims_dad[slot];
        if ((d->armed || d->sent || d->ifretry) && !ims_dad_slot_live(slot)) {
            d->armed = false;
            d->sent = false;
            if (d->ifretry) ims_dad_note_no_ifaddr(slot);   /* closed before an address appeared */
            d->ifretry = false;
        }
    }
    ims_dad_run_locked();
}

/* XNU installed an address on pdp_ip `unit` and called the driver's address ioctl (bb_pdp_ioctl).
 * Recorded even with the switch off so an address installed before ims-on appeared still shows up;
 * diagnostic only, never a precondition. Lock-free: the modem thread absorbs it in ims_dad_poll(). */
static void ims_address_installed(uint32_t unit)
{
    if (unit < CBP_INTERFACES) __atomic_store_n(&ims_dad_ioctl_flag[unit], 1u, __ATOMIC_RELAXED);
}

/* WDS lifecycle bookkeeping: a Start request opens an activation, a teardown closes it, and the
 * 0x2d reply carrying at least one global IPv6 candidate makes it eligible. No address is logged. */
static void ims_dad_observe(uint32_t slot, const uint8_t *sdu, uint32_t n)
{
    if (!ims_owner_on() || n < 7u) return;
    struct ims_dad_state *d = &ims_dad[slot];
    const struct cbp_client *c = &bb_pdp_clients[slot];
    const uint32_t message = umi_ims_rd16(sdu + 3);
    if (sdu[0] == 0u && message == UMI_IMS_START) {
        /* A Start request opens a fresh activation: one attempt and one diagnostic set. */
        ims_dad_hold_clear(slot);   /* a reply still held for an older activation is moot now */
        d->armed = false;
        d->sent = false;
        d->ifretry = false;
        d->ifretries = 0;
        d->hash_logged = false;
        d->ifnone_logged = false;
        d->closed = false;
        d->hold_logged = false;
        d->t0_n = d->t0_sent = 0;
        d->ims = 0;                 /* a reused slot must not inherit the previous context's class */
        ims_dad_ifaddr_gen++;   /* an interface snapshot from before the Start is not this activation's */
        /* The previous activation's snapshot says nothing about this one: an address it saw is
         * usually gone by now, and believing it would skip the hold and send a stale t0 (v29i review
         * B1). The next poll re-takes it for this unit. */
        const uint32_t unit = ims_dad_unit(c);
        if (unit < CBP_INTERFACES) bzero(&ims_dad_ifaddrs[unit], sizeof ims_dad_ifaddrs[unit]);
        ims_dad_ifaddr_at = 0;
        d->cand_n = d->cfg_n = 0;
        d->noted = false;
        const uint16_t txn = umi_ims_rd16(sdu + 1);
        if (!d->start_logged || d->start_txn != txn) {
            d->start_logged = true;
            d->start_txn = txn;
            ims_dad_log_start(slot, sdu);
        }
    } else if (message == UMI_IMS_STOP || (message == UMI_IMS_STATUS && !c->active)) {
        ims_dad_hold_clear(slot);   /* never deliver a reply into a context that is gone */
        d->armed = false;
        d->sent = false;
        d->ifretry = false;
        d->ifretries = 0;
        d->closed = true;
        d->cand_n = d->cfg_n = 0;
        d->t0_n = d->t0_sent = 0;
        d->ims = 0;
        d->noted = false;
        d->start_logged = false;
        d->hash_logged = false;
        d->ifnone_logged = false;
        d->hold_logged = false;
        ims_dad_ifaddr_gen++;   /* the activation is over: nothing it snapshotted may be stored */
        const uint32_t unit = ims_dad_unit(c);
        if (unit < CBP_INTERFACES) bzero(&ims_dad_ifaddrs[unit], sizeof ims_dad_ifaddrs[unit]);
        ims_dad_ifaddr_at = 0;
    } else if (sdu[0] == 2u && message == UMI_IMS_GET_SETTINGS) {
        ims_dad_capture(d, sdu, n);
        if (d->armed && !ims_dad_cand_same(d)) d->armed = false;   /* the candidate set changed: arm again */
    }
    ims_dad_try_locked();
    if (sdu[0] == 2u && message == UMI_IMS_GET_SETTINGS && !d->sent && !d->armed && !d->noted) {
        d->noted = true;
        ims_dad_note_not_armed(slot);
    }
}

/* The WDS lifecycle parser the bridge already ran, then the DAD bookkeeping on top of it. */
static int ims_pdp_observe(uint32_t slot, const uint8_t *sdu, uint32_t n)
{
    if (!cbp_observe(&bb_pdp_clients[slot], sdu, n)) return 0;
    ims_dad_observe(slot, sdu, n);
    return 1;
}

/* A missing QRTR service fails at client allocation, before any RTP request
 * reaches ims_note(). Record that actual CTL result, without dumping TLVs; while
 * either IMS owner is on, 0x28 is allocated here, so this is where its stub's
 * client lifecycle begins (one allocation line per client id, cleared on release). */
static void ims_note_ctl(const uint8_t *request, uint32_t n, const uint8_t *response, uint32_t len)
{
    if (!ims_owner_on() || n < CBQ_HDR + 6u || len < CBQ_HDR + 6u || request[4]) return;
    const uint32_t id = umi_ims_rd16(request + CBQ_HDR + 2);
    uint32_t size;
    const uint8_t *service = cbq_tlv(request + CBQ_HDR + 6, n - CBQ_HDR - 6, 1, &size);
    if (!service || size < 1u) return;
    if (id == CBQ_CTL_RELEASE_CLIENT_ID) {   /* service + client id: the next allocation is a new event */
        if (size == 2u && service[0] == CBQ_SVC_IMSRTP)
            ims_rtp_alloc[service[1]] = ims_rtp_init[service[1]] = ims_rtp_uninit_ind[service[1]] = 0;
        return;
    }
    if (id != CBQ_CTL_GET_CLIENT_ID) return;
    if (*service != 0x28u && *service != 0x52u && *service != 0xe1u && *service != 0xe6u && *service != 0xe7u) return;
    const uint8_t *result = cbq_tlv(response + CBQ_HDR + 6, len - CBQ_HDR - 6, 2, &size);
    if (!result || size != 4u) return;
    IMS_LOG("media client svc 0x%x allocation result %u error %u\n", *service,
        umi_ims_rd16(result), umi_ims_rd16(result + 2));
    if (*service != CBQ_SVC_IMSRTP || umi_ims_rd16(result) || umi_ims_rd16(result + 2)) return;
    const uint8_t *info = cbq_tlv(response + CBQ_HDR + 6, len - CBQ_HDR - 6, 1, &size);
    if (!info || size != 2u || ims_rtp_alloc[info[1]]) return;
    ims_rtp_alloc[info[1]] = 1;
    IMS_LOG("imsrtp stub: client %u allocated\n", info[1]);
}

static bool ims_bearer_allowed(void)
{
    /* Route B: the AP's ims PDN is the bearer the modem IMS stack reuses, so it is never withheld,
     * whatever the audit says. Outside modem-ims this is exactly the old gate. */
    if (modem_ims_enabled) return true;
    return ims_audit.bound && ims_audit.registration == 0 && bb_now_ms() - ims_audit.checked_at < 15000u;
}

/* Independent diagnostic IMSA client: bind this client to the primary
 * subscription before reading status. Binding is client-local; it does not
 * enable registration or change the modem's persistent configuration. */
static uint32_t ims_poll(uint8_t *packet, uint32_t *node, uint32_t *port)
{
    ims_prune();
    const uint32_t now = bb_now_ms();
    const struct cbq_server *s = cbq_server_find(&bb_qmux, UMI_IMS_IMSA);
    if (!ims_owner_on() || !s || (ims_audit.port && (s->node != ims_audit.node || s->port != ims_audit.port))) {
        if (ims_audit.port) ims_audit.release = true;
        ims_audit.port = 0;
        ims_audit.pending = false;
        ims_audit.bound = false;
        ims_audit.indications = 0;
        ims_audit.bind_result = -2;
        ims_audit.registration = -1;
        return 0;
    }
    if (ims_audit.pending) {
        if (now - ims_audit.sent_at < 5000u) return 0;
        ims_audit.pending = false;
        ims_audit.bound = false;
        ims_audit.registration = -1;
        /* Route B retries every 5 s; one line per failure streak, not one per retry. */
        if (!modem_ims_enabled || !ims_audit.timeout_logged) {
            ims_audit.timeout_logged = 1;
            IMS_LOG("modem IMS audit timeout; AP bearer held\n");
        }
        ims_audit.next_at = now + 5000u;
    }
    if ((int32_t)(now - ims_audit.next_at) < 0) return 0;
    if (!++ims_audit.sequence) ++ims_audit.sequence;
    ims_audit.txn = ims_audit.sequence;
    ims_audit.node = *node = s->node;
    ims_audit.port = *port = s->port;
    ims_audit.pending = true;
    ims_audit.sent_at = now;
    if (!ims_audit.bound)
        ims_audit.message = UMI_IMS_BIND_SUBSCRIPTION;
    else if (modem_ims_enabled && !ims_audit.indications)
        ims_audit.message = UMI_IMS_IMSA_INDICATIONS;   /* route B: ask for 0x23/0x24 once */
    else
        ims_audit.message = UMI_IMS_GET_REGISTRATION;
    packet[0] = 0;
    umi_ims_wr16(packet + 1, ims_audit.txn);
    umi_ims_wr16(packet + 3, ims_audit.message);
    uint32_t length = 7u;
    if (!ims_audit.bound) {
        length = 14u;
        packet[7] = 0x10;
        umi_ims_wr16(packet + 8, 4);
        bzero(packet + 10, 4); // primary subscription, u32 enum
    } else if (ims_audit.message == UMI_IMS_IMSA_INDICATIONS) {
        length = 15u;
        packet[7] = 0x10;   /* IMS Registration Status Changed = true */
        umi_ims_wr16(packet + 8, 1);
        packet[10] = 1;
        packet[11] = 0x11;  /* IMS Services Status Changed = true */
        umi_ims_wr16(packet + 12, 1);
        packet[14] = 1;
    }
    umi_ims_wr16(packet + 5, length - 7u);
    return length;
}

static void ims_receive(uint32_t node, uint32_t port, const uint8_t *sdu, uint32_t n)
{
    if (!umi_ims_tlvs_ok(sdu, n)) return;
    /* Route B: IMSA 0x0023/0x0024 indications, one line per actual change, never per query. */
    if (modem_ims_enabled && ims_audit.port && node == ims_audit.node && port == ims_audit.port && sdu[0] == 4u) {
        uint32_t len = 0, text_len = 0;
        int registration = ims_modem.registration, technology = ims_modem.technology, voice = ims_modem.voice;
        int err = ims_modem.err, err_text = ims_modem.err_text;
        const uint8_t *v;
        if (umi_ims_rd16(sdu + 3) == UMI_IMS_IMSA_REGISTRATION_IND) {
            v = umi_ims_find(sdu, n, 0x11u, &len);
            if (v && len == 4u) registration = (int)umi_ims_rd32(v);
            v = umi_ims_find(sdu, n, 0x13u, &len);
            if (v && len == 4u) technology = (int)umi_ims_rd32(v);
            /* Error TLVs (qmi-service-imsa.json 0x0023): 0x10 u16 code, 0x12 error text. */
            v = umi_ims_find(sdu, n, 0x10u, &len);
            err = v && len == 2u ? (int)umi_ims_rd16(v) : -1;
            v = umi_ims_find(sdu, n, 0x12u, &text_len);
            err_text = text_len ? (int)text_len : -1;
        } else if (umi_ims_rd16(sdu + 3) == UMI_IMS_IMSA_SERVICES_IND) {
            v = umi_ims_find(sdu, n, 0x11u, &len);
            if (v && len == 4u) voice = (int)umi_ims_rd32(v);
        } else {
            return;
        }
        if (registration == ims_modem.registration && technology == ims_modem.technology && voice == ims_modem.voice &&
            err == ims_modem.err && err_text == ims_modem.err_text) return;
        ims_modem.registration = registration;
        ims_modem.technology = technology;
        ims_modem.voice = voice;
        ims_modem.err = err;
        ims_modem.err_text = err_text;
        IMS_LOG("modem registration %d technology %d; voice service %d; error %d text %d bytes\n",
            registration, technology, voice, err, err_text < 0 ? 0 : err_text);
        return;
    }
    if (!ims_audit.pending || node != ims_audit.node || port != ims_audit.port || !umi_ims_tlvs_ok(sdu, n) ||
        sdu[0] != 2u || umi_ims_rd16(sdu + 1) != ims_audit.txn || umi_ims_rd16(sdu + 3) != ims_audit.message) return;
    ims_audit.pending = false;
    ims_audit.timeout_logged = 0;
    if (ims_audit.message == UMI_IMS_IMSA_INDICATIONS) {
        const int result = umi_ims_result(sdu, n);
        ims_audit.indications = result == 0;
        ims_audit.next_at = bb_now_ms() + (ims_audit.indications ? 0u : 5000u);
        IMS_LOG("imsa 0x22 register indications result %d\n", result);
        return;
    }
    if (ims_audit.message == UMI_IMS_BIND_SUBSCRIPTION) {
        const int result = umi_ims_result(sdu, n);
        ims_audit.bound = result == 0;
        ims_audit.registration = -1;
        ims_audit.next_at = bb_now_ms() + (ims_audit.bound ? 0u : 5000u);
        /* A rejected bind is retried every 5 s; route B keeps one line per streak. */
        if (!modem_ims_enabled || result != ims_audit.bind_result)
            IMS_LOG("modem IMS audit bind result %d; registration unchanged\n", result);
        ims_audit.bind_result = result;
        return;
    }
    ims_audit.next_at = bb_now_ms() + 10000u;
    ims_audit.checked_at = bb_now_ms();
    const int registration = umi_ims_registration(sdu, n);
    const bool changed = registration != ims_audit.registration;
    ims_audit.registration = registration;
    /* IMSA 0x20 error TLVs (qmi-service-imsa.json 0x0020): 0x11 u16 code, 0x13 error text. The code
     * is the modem's own "why not registered"; the text can carry an identity, so only its length.
     * One line per change, never per query. */
    uint32_t code_len = 0, text_len = 0;
    const uint8_t *code = umi_ims_find(sdu, n, 0x11u, &code_len);
    const uint8_t *text = umi_ims_find(sdu, n, 0x13u, &text_len);
    const int err = code && code_len == 2u ? (int)umi_ims_rd16(code) : -1;
    const int err_text = text_len ? (int)text_len : -1;
    if (err != ims_audit.err || err_text != ims_audit.err_text) {
        ims_audit.err = err;
        ims_audit.err_text = err_text;
        IMS_LOG("modem IMS registration error %d text %d bytes\n", err, err_text < 0 ? 0 : err_text);
    }
    const bool allowed = modem_ims_enabled || ims_audit.registration == 0;
    /* Route B has the indications for change reporting, so its own 10 s query is silent unless the
     * state moved; outside modem-ims every answer keeps its line, exactly as before. */
    if (!modem_ims_enabled || changed)
        IMS_LOG("modem IMS audit result %d registration %d; AP bearer %s\n", umi_ims_result(sdu, n), ims_audit.registration,
            allowed ? "allowed" : "held");
}

/* Route B: IMSS Set IMS Services Enabled Setting (0x008f), bridge-originated on its own QRTR port.
 * Only the two enable bits go out (VoLTE 1, IMS 1); no policy-manager SET, PDC, MBN or NV message
 * is ever sent (0x48 below is the read-only Get). NOTE: the X55 may keep this enable state in its own EFS/NVRAM -- unavoidable without
 * a modem-side query -- so the request is sent once per switch-on, never retried after a failure,
 * and the log says so. Before it, the preamble below runs once to give the modem the same context
 * the vendor stack establishes: supported messages/fields, the enable-config read, 0x2a. */
static uint32_t ims_imss_poll(uint8_t *packet, uint32_t *node, uint32_t *port)
{
    const uint32_t now = bb_now_ms();
    const struct cbq_server *s = cbq_server_find(&bb_qmux, UMI_IMS_IMSS);
    if (!modem_ims_enabled || !s || (ims_imss.port && (s->node != ims_imss.node || s->port != ims_imss.port))) {
        if (ims_imss.port) ims_imss.release = true;
        ims_imss.port = ims_imss.node = 0;
        ims_imss.pending = false;   /* an endpoint change re-sends the outstanding step to the new port */
        return 0;
    }
    if (ims_imss.pending) {
        if (now - ims_imss.sent_at < 5000u) return 0;
        ims_imss.pending = false;
        ims_imss.result = -1;
        ims_imss.step = IMSS_STEP_DONE;
        if (ims_imss.message == UMI_IMS_IMS_SETTINGS_ENABLED)
            IMS_LOG("imss 0x8f timeout; not resent\n");
        else if (ims_imss.message == UMI_IMS_IMSS_GET_POLICY) {
            /* Read-only probe: a modem that does not answer it must still get 0x8f. */
            IMS_LOG("imss 0x48 policy timeout; continuing to 0x8f\n");
            ims_imss.step = IMSS_STEP_ENABLE;
        } else {
            ims_imss_stalled = 1;   /* no answer from IMSS: no further step this boot */
            IMS_LOG("imss 0x%04x timeout; chain stopped, not resent\n", ims_imss.message);
        }
        return 0;
    }
    if (ims_imss_stalled) return 0;
    uint8_t step = ims_imss.step;
    if (step == IMSS_STEP_NONE) step = ims_imss_chain ? IMSS_STEP_ENABLE : IMSS_STEP_MSGS;
    if (step == IMSS_STEP_BIND && !ims_imss.bind_supported) step = IMSS_STEP_FIELDS;
    if (step == IMSS_STEP_DONE || (step == IMSS_STEP_ENABLE && ims_imss.sent)) return 0;
    if (!++ims_imss.sequence) ++ims_imss.sequence;
    ims_imss.step = step;
    ims_imss.txn = ims_imss.sequence;
    ims_imss.node = *node = s->node;
    ims_imss.port = *port = s->port;
    ims_imss.pending = true;
    ims_imss.sent_at = now;
    uint32_t length = 7u;
    switch (step) {
    case IMSS_STEP_MSGS:
        ims_imss.message = UMI_IMS_IMSS_GET_SUPPORTED_MSGS;   /* empty payload */
        break;
    case IMSS_STEP_BIND:
        ims_imss.message = UMI_IMS_IMSS_BIND;
        packet[7] = UMI_IMS_IMSS_BIND_TLV;
        umi_ims_wr16(packet + 8, 4u);
        umi_ims_wr32(packet + 10, UMI_IMS_IMSS_SUBSCRIPTION_PRIMARY);
        length = 14u;
        break;
    case IMSS_STEP_FIELDS:
        ims_imss.message = UMI_IMS_IMSS_GET_SUPPORTED_FIELDS;
        packet[7] = UMI_IMS_IMSS_FIELDS_REQ_TLV;                  /* TLV 0x01, u16 msg_id */
        umi_ims_wr16(packet + 8, 2u);
        umi_ims_wr16(packet + 10, UMI_IMS_IMS_SETTINGS_ENABLED);
        length = 12u;
        break;
    case IMSS_STEP_INDREG:
        ims_imss.message = UMI_IMS_IMSS_CONFIG_IND_REG;
        packet[7] = UMI_IMS_IMSS_TLV_REG_MGR_CONFIG;
        umi_ims_wr16(packet + 8, 1u);
        packet[10] = 1u;
        packet[11] = UMI_IMS_IMSS_TLV_CLIENT_PROVISIONING;
        umi_ims_wr16(packet + 12, 1u);
        packet[14] = 1u;
        packet[15] = UMI_IMS_IMSS_TLV_SERVICE_ENABLE_CONFIG;
        umi_ims_wr16(packet + 16, 1u);
        packet[18] = 1u;
        packet[19] = UMI_IMS_IMSS_TLV_RTT_CONFIG;
        umi_ims_wr16(packet + 20, 1u);
        packet[22] = 1u;
        length = 23u;
        break;
    case IMSS_STEP_GET:
        ims_imss.message = UMI_IMS_IMSS_GET_ENABLED;   /* empty payload; after 0x2a, as in qcril */
        break;
    case IMSS_STEP_POLICY:
        ims_imss.message = UMI_IMS_IMSS_GET_POLICY;    /* empty payload: a read, nothing is set */
        break;
    default:   /* IMSS_STEP_ENABLE */
        ims_imss.message = UMI_IMS_IMS_SETTINGS_ENABLED;
        packet[7] = 0x10;               /* IMS Voice Over LTE Enable */
        umi_ims_wr16(packet + 8, 1u);
        packet[10] = 1u;
        packet[11] = 0x18;              /* IMS Service Enabled */
        umi_ims_wr16(packet + 12, 1u);
        packet[14] = 1u;
        length = 15u;
        ims_imss.sent = true;
        IMS_LOG("imss 0x8f sent (VoLTE 1, IMS 1); X55 may persist this enable in its own EFS\n");
        break;
    }
    packet[0] = 0;
    umi_ims_wr16(packet + 1, ims_imss.txn);
    umi_ims_wr16(packet + 3, ims_imss.message);
    umi_ims_wr16(packet + 5, length - 7u);
    return length;
}

/* Bit `bit` of a Get Supported Messages/Fields bitmap; short or absent bitmaps read as 0. */
static uint32_t ims_imss_bit(const uint8_t *v, uint32_t len, uint32_t bit)
{
    return v && bit / 8u < len ? (v[bit / 8u] >> (bit % 8u)) & 1u : 0u;
}

/* The bitmap behind TLV 0x10 (Get Supported Messages/Fields). QMI IDL prefixes a variable-length
 * array with its element count: two bytes for the 0x1e message bitmap, one for the 0x1f field
 * bitmaps. Returns the bitmap itself and its byte count, or 0 if the TLV is missing or inconsistent. */
static const uint8_t *ims_imss_map(const uint8_t *sdu, uint32_t n, uint32_t prefix, uint32_t *len)
{
    uint32_t raw = 0;
    *len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, UMI_IMS_IMSS_MAP_TLV, &raw);
    if (!v || raw < prefix) return 0;
    const uint32_t count = prefix == 2u ? umi_ims_rd16(v) : v[0];
    if (count > raw - prefix) return 0;
    *len = count;
    return v + prefix;
}

/* Hex of up to UMI_IMS_IMSS_MAP_LOG_MAX bitmap bytes: message/field support only, no identities. */
static void ims_imss_hex(const uint8_t *v, uint32_t len, char *out, uint32_t room)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t used = 0;
    out[0] = 0;
    if (!v) return;
    if (len > UMI_IMS_IMSS_MAP_LOG_MAX) len = UMI_IMS_IMSS_MAP_LOG_MAX;
    for (uint32_t i = 0; i < len; i++) {
        if (room - used < 3u) break;
        out[used++] = digits[v[i] >> 4];
        out[used++] = digits[v[i] & 15u];
    }
    out[used] = 0;
}

/* Compact "id:len" per TLV (decimal length), with "=value" for one-byte TLVs: the enable-config
 * booleans. Truncated into the caller's buffer; this service carries no identities. */
static void ims_imss_summary(const uint8_t *sdu, uint32_t n, char *out, uint32_t room)
{
    uint32_t used = 0;
    out[0] = 0;
    if (!umi_ims_tlvs_ok(sdu, n)) return;
    for (uint32_t at = 7u, end = 7u + umi_ims_rd16(sdu + 5); at < end; at += 3u + umi_ims_rd16(sdu + at + 1)) {
        const uint32_t len = umi_ims_rd16(sdu + at + 1);
        if (room - used < 16u) break;
        if (used) out[used++] = ' ';
        used += (uint32_t)snprintf(out + used, room - used, "%02x:%u", sdu[at], len);
        if (len == 1u) used += (uint32_t)snprintf(out + used, room - used, "=%u", sdu[at + 3]);
    }
}

/* 0x48 Get IMS Policy Manager Settings (qmi-service-ims.json): the read-only answer TLVs, values
 * only. Priorities 0x15..0x18 (ACS/ISIM/NV/PCO, u8), service mask 0x19 (u64), APN list 0x1a (six
 * u8-length-prefixed strings): its count and byte total are logged, never the APN text. */
static void ims_imss_policy(const uint8_t *sdu, uint32_t n, int result)
{
    char tlvs[120];
    uint32_t len = 0;
    const uint8_t *v;
    int err = -1, acs = -1, isim = -1, nv = -1, pco = -1;
    uint64_t mask = 0;
    uint32_t apns = 0, apn_bytes = 0;
    ims_imss_summary(sdu, n, tlvs, sizeof tlvs);
    if ((v = umi_ims_find(sdu, n, 0x10u, &len)) && len == 4u) err = (int)umi_ims_rd32(v);
    if ((v = umi_ims_find(sdu, n, 0x15u, &len)) && len == 1u) acs = v[0];
    if ((v = umi_ims_find(sdu, n, 0x16u, &len)) && len == 1u) isim = v[0];
    if ((v = umi_ims_find(sdu, n, 0x17u, &len)) && len == 1u) nv = v[0];
    if ((v = umi_ims_find(sdu, n, 0x18u, &len)) && len == 1u) pco = v[0];
    if ((v = umi_ims_find(sdu, n, 0x19u, &len)) && len == 8u)
        mask = (uint64_t)umi_ims_rd32(v) | (uint64_t)umi_ims_rd32(v + 4) << 32;
    if ((v = umi_ims_find(sdu, n, 0x1au, &len)) && len) {
        uint32_t at = 0;
        for (unsigned i = 0; i < 6u && at < len; i++) {
            const uint32_t l = v[at++];
            if (l > len - at) break;
            if (l) { apns++; apn_bytes += l; }
            at += l;
        }
    }
    IMS_LOG("imss 0x48 policy result %d tlvs %s; error %d prio acs %d isim %d nv %d pco %d mask %08x%08x apns %u/%u bytes\n",
        result, tlvs, err, acs, isim, nv, pco, (unsigned)(mask >> 32), (unsigned)mask, apns, apn_bytes);
}

static void ims_imss_receive(uint32_t node, uint32_t port, const uint8_t *sdu, uint32_t n)
{
    if (!ims_imss.pending || node != ims_imss.node || port != ims_imss.port || !umi_ims_tlvs_ok(sdu, n) ||
        sdu[0] != 2u || umi_ims_rd16(sdu + 1) != ims_imss.txn || umi_ims_rd16(sdu + 3) != ims_imss.message) return;
    ims_imss.pending = false;
    ims_imss.result = umi_ims_result(sdu, n);
    char tlvs[96];
    uint32_t len = 0;
    const uint8_t *v;
    switch (ims_imss.step) {
    case IMSS_STEP_MSGS: {
        v = ims_imss_map(sdu, n, UMI_IMS_IMSS_MSGS_MAP_PREFIX, &len);   /* bit N = message id N */
        ims_imss.bind_supported = (uint8_t)ims_imss_bit(v, len, UMI_IMS_IMSS_BIND);
        if (!(ims_imss_logged & 1u)) {
            ims_imss_logged |= 1u;
            char map[3u * UMI_IMS_IMSS_MAP_LOG_MAX];
            ims_imss_hex(v, len, map, sizeof map);
            ims_imss_summary(sdu, n, tlvs, sizeof tlvs);
            IMS_LOG("imss supported msgs result %d tlvs %s; 8f=%u 2a=%u 90=%u 98=%u map=%s\n", ims_imss.result, tlvs,
                ims_imss_bit(v, len, UMI_IMS_IMS_SETTINGS_ENABLED), ims_imss_bit(v, len, UMI_IMS_IMSS_CONFIG_IND_REG),
                ims_imss_bit(v, len, UMI_IMS_IMSS_GET_ENABLED), ims_imss.bind_supported, map);
        }
        ims_imss.step = ims_imss.bind_supported ? IMSS_STEP_BIND : IMSS_STEP_FIELDS;
        return;   /* the bind decision replaces the plain step++ */
    }
    case IMSS_STEP_BIND:
        if (!(ims_imss_logged & 16u)) {
            ims_imss_logged |= 16u;
            IMS_LOG("imss 0x98 bind primary result %d\n", ims_imss.result);
        }
        break;
    case IMSS_STEP_FIELDS:
        if (ims_imss_logged & 2u) break;
        ims_imss_logged |= 2u;
        v = ims_imss_map(sdu, n, UMI_IMS_IMSS_FIELDS_MAP_PREFIX, &len);   /* bit N = TLV 0x10+N */
        ims_imss_summary(sdu, n, tlvs, sizeof tlvs);
        IMS_LOG("imss supported fields result %d tlvs %s; 8f 10=%u 11=%u 14=%u 18=%u\n", ims_imss.result, tlvs,
            ims_imss_bit(v, len, 0u), ims_imss_bit(v, len, 1u), ims_imss_bit(v, len, 4u), ims_imss_bit(v, len, 8u));
        break;
    case IMSS_STEP_INDREG:
        if (!(ims_imss_logged & 8u)) {
            ims_imss_logged |= 8u;
            IMS_LOG("imss 0x2a config ind reg result %d\n", ims_imss.result);
        }
        ims_imss_chain = 1;   /* client-local registration done: later sessions go straight to 0x8f */
        break;
    case IMSS_STEP_GET:
        if (ims_imss_logged & 4u) break;
        ims_imss_logged |= 4u;
        ims_imss_summary(sdu, n, tlvs, sizeof tlvs);
        IMS_LOG("imss get enabled result %d tlvs %s\n", ims_imss.result, tlvs);
        break;
    case IMSS_STEP_POLICY:
        if (ims_imss_logged & 32u) break;
        ims_imss_logged |= 32u;
        ims_imss_policy(sdu, n, ims_imss.result);
        break;
    default:   /* IMSS_STEP_ENABLE */
        IMS_LOG("imss 0x8f result %d\n", ims_imss.result);
        break;
    }
    ims_imss.step++;
}

/* Route B: NAS Set System Selection Preference (0x33) carries Voice Domain Preference TLV 0x23
 * (guint32). On LTE the modem has no CS, so a copy of the request is rewritten to 1 (PS only);
 * the SDU length and every other TLV stay exactly as iOS sent them. The first request's TLV type
 * sequence is logged once (types only, never values), and only the 0x23 value is named, because
 * the acceptance log has to show what iOS really sends. With the switch off this returns 0 and
 * writes nothing. */
static uint32_t ims_nas_voice_domain(uint32_t service, const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
    if (!modem_ims_enabled || service != UMI_IMS_NAS || n < 7u || sdu[0] != 0u ||
        umi_ims_rd16(sdu + 3) != UMI_IMS_SYSTEM_SELECTION || !umi_ims_tlvs_ok(sdu, n)) return 0;
    uint32_t len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, UMI_IMS_VOICE_DOMAIN, &len);
    const bool present = v && len == 4u;
    if (!ims_nas_logged) {
        static const char hex[] = "0123456789abcdef";
        char types[3u * 17u];
        uint32_t used = 0;
        for (uint32_t at = 7u, end = 7u + umi_ims_rd16(sdu + 5); at < end && used + 3u < sizeof types;
             at += 3u + umi_ims_rd16(sdu + at + 1)) {
            if (used) types[used++] = ' ';
            types[used++] = hex[sdu[at] >> 4];
            types[used++] = hex[sdu[at] & 0xfu];
        }
        types[used] = 0;
        ims_nas_logged = 1;
        IMS_LOG("NAS 0x33 tlvs %s%s\n", used ? types : "(none)", present ? "" : "; no 0x23, not rewritten");
    }
    if (!present || room < n) return 0;
    if (umi_ims_rd32(v) == 1u) return 0;   /* already PS only: the bytes stay the ones iOS sent */
    for (uint32_t i = 0; i < n; i++) out[i] = sdu[i];
    uint8_t *w = out + (v - sdu);
    umi_ims_wr32(w, 1u);
    if (!ims_nas_rewritten) {
        ims_nas_rewritten = 1;
        IMS_LOG("NAS 0x33 voice domain %u -> PS only\n", umi_ims_rd32(v));
    }
    return n;
}

/* ---- Route B probe: IMSDCM 0x302 -----------------------------------------------------------------
 * Everything here is client-local and gated by modem_ims_enabled; with the switch absent none of
 * it changes a byte, logs a line or answers a packet. */

/* One "small integer" for a log line: length 1/2/4 as decimal, absent, or just the length. */
static void ims_dcm_num(const uint8_t *v, uint32_t len, char *out, uint32_t room)
{
    if (!v) snprintf(out, room, "absent");
    else if (len == 1u) snprintf(out, room, "%u", v[0]);
    else if (len == 2u) snprintf(out, room, "%u", (unsigned)umi_ims_rd16(v));
    else if (len == 4u) snprintf(out, room, "%u", (unsigned)umi_ims_rd32(v));
    else snprintf(out, room, "len %u", len);
}

/* Dedupe one line per distinct request (the transaction differs between retries) and stop after
 * UMI_DCM_LINES, so a modem retry loop cannot flood the serial console. */
static void ims_dcm_line(const char *detail)
{
    uint64_t hash = 1469598103934665603ull;
    for (const char *p = detail; *p; p++) { hash ^= (uint8_t)*p; hash *= 1099511628211ull; }
    for (uint32_t i = 0; i < ims_dcm.seen_n; i++) if (ims_dcm.seen[i] == hash) return;
    if (ims_dcm.seen_n < UMI_DCM_SEEN) ims_dcm.seen[ims_dcm.seen_n++] = hash;
    else {
        for (uint32_t i = 1; i < UMI_DCM_SEEN; i++) ims_dcm.seen[i - 1] = ims_dcm.seen[i];
        ims_dcm.seen[UMI_DCM_SEEN - 1] = hash;
    }
    if (ims_dcm.lines >= UMI_DCM_LINES) return;
    ims_dcm.lines++;
    IMS_LOG("dcm: %s\n", detail);
}

/* The bridge asks the kernel to publish the service once per switch-on (umi_bb_announce does the
 * type-4 NEW_SERVER). Params are those of the Android qrtr-lookup: 770 v1 instance 0. */
static int ims_dcm_announce(uint32_t *service, uint32_t *version, uint32_t *instance, uint32_t *port)
{
    if (!modem_ims_enabled || ims_dcm.announced) return 0;
    ims_dcm.announced = true;
    *service = UMI_IMS_DCM_SERVICE;
    *version = UMI_IMS_DCM_VERSION;
    *instance = UMI_IMS_DCM_INSTANCE;
    *port = UMI_IMS_DCM_PORT;
    return 1;
}

/* A queued QMI CTL answer for the modem (allocated cid or version list). */
static uint32_t ims_dcm_reply(uint8_t *out, uint32_t room, uint32_t *node, uint32_t *port, uint32_t *local)
{
    if (!ims_dcm.reply_len) return 0;
    if (room < ims_dcm.reply_len) { ims_dcm.reply_len = 0; return 0; }
    memcpy(out, ims_dcm.reply, ims_dcm.reply_len);
    *node = ims_dcm.reply_node;
    *port = ims_dcm.reply_port;
    *local = ims_dcm.reply_local;
    const uint32_t n = ims_dcm.reply_len;
    ims_dcm.reply_len = 0;
    return n;
}

/* QMI CTL on the host node: Get Version Info (0x21), Allocate CID (0x22/0xFF22) and Release
 * (0x23/0xFF23). The X55 needs a client id before it can send 0x20 to service 770, and 770 does not
 * fit the u8 Service TLV, hence the QRTR u16 variants. Answering is what keeps the modem talking
 * (report §4 P1 step 2); no state beyond this boot and no bearer results from it. */
static void ims_dcm_control(uint32_t dst_port, uint32_t node, uint32_t port, const uint8_t *sdu, uint32_t n, uint32_t message)
{
    const bool qrtr = message == UMI_IMS_CTL_ALLOCATE_QRTR || message == UMI_IMS_CTL_RELEASE_QRTR;
    uint32_t vlen = 0, service = UMI_IMS_DCM_SERVICE;
    const uint8_t *v = umi_ims_find(sdu, n, 0x01u, &vlen);
    if (v) service = qrtr ? (vlen >= 2u ? umi_ims_rd16(v) : 0u) : (vlen >= 1u ? v[0] : 0u);
    uint8_t *out = ims_dcm.reply;
    out[0] = 2u;   /* response */
    umi_ims_wr16(out + 1, umi_ims_rd16(sdu + 1));
    umi_ims_wr16(out + 3, (uint16_t)message);
    uint32_t o = 7u;
    out[o] = 0x02u; umi_ims_wr16(out + o + 1, 4u);   /* Operation Result: success */
    out[o + 3] = out[o + 4] = out[o + 5] = out[o + 6] = 0;
    o += 7u;
    if (message == UMI_IMS_CTL_GET_VERSION) {
        out[o] = 0x01u; umi_ims_wr16(out + o + 1, 6u);   /* count, then {u8 service, u16 major, u16 minor} */
        out[o + 3] = 1u;
        out[o + 4] = (uint8_t)UMI_IMS_DCM_SERVICE;
        umi_ims_wr16(out + o + 5, UMI_IMS_DCM_VERSION);
        umi_ims_wr16(out + o + 7, 0u);
        o += 9u;
    } else if (message == UMI_IMS_CTL_ALLOCATE || message == UMI_IMS_CTL_ALLOCATE_QRTR) {
        const uint32_t size = qrtr ? 3u : 2u;
        out[o] = 0x01u; umi_ims_wr16(out + o + 1, (uint16_t)size);
        if (qrtr) { umi_ims_wr16(out + o + 3, (uint16_t)service); out[o + 5] = 1u; }
        else { out[o + 3] = (uint8_t)service; out[o + 4] = 1u; }
        o += 3u + size;
    } else {   /* Release: echo the request's {service, cid} */
        const uint32_t size = qrtr ? 3u : 2u;
        out[o] = 0x01u; umi_ims_wr16(out + o + 1, (uint16_t)size);
        if (v && vlen >= size) memcpy(out + o + 3, v, size);
        else if (qrtr) { umi_ims_wr16(out + o + 3, (uint16_t)service); out[o + 5] = 1u; }
        else { out[o + 3] = (uint8_t)service; out[o + 4] = 1u; }
        o += 3u + size;
    }
    umi_ims_wr16(out + 5, o - 7u);
    ims_dcm.reply_len = o;
    ims_dcm.reply_node = node;
    ims_dcm.reply_port = port;
    ims_dcm.reply_local = dst_port;   /* answer from the port the request was addressed to */
    char line[96];
    snprintf(line, sizeof line, "ctl msg 0x%04x node %u port %u svc %#x -> cid 1", message, node, port, service);
    ims_dcm_line(line);
}

/* TLV ids and lengths, as ims_imss_summary does; this service carries no identities. */
static void ims_dcm_tlvs(const uint8_t *sdu, uint32_t n, char *out, uint32_t room)
{
    uint32_t used = 0;
    out[0] = 0;
    if (!umi_ims_tlvs_ok(sdu, n)) return;
    for (uint32_t at = 7u, end = 7u + umi_ims_rd16(sdu + 5); at < end; at += 3u + umi_ims_rd16(sdu + at + 1)) {
        if (room - used < 12u) break;
        used += (uint32_t)snprintf(out + used, room - used, "%s%02x:%u", used ? " " : "", sdu[at], umi_ims_rd16(sdu + at + 1));
    }
}

/* TLV 0x01 of IMSDCM 0x20 (qmi-service-imsdcm.json): a sequence {APN string, APN type, RAT,
 * IP family, WDS profile number}. The APN is the modem's own request (the report asks for it); it
 * is not an identity. Non-printable bytes are shown as '.'. */
static void ims_dcm_start_detail(const uint8_t *sdu, uint32_t n, char *out, uint32_t room)
{
    uint32_t len = 0;
    const uint8_t *v = umi_ims_find(sdu, n, 0x01u, &len);
    if (!v || !len) { snprintf(out, room, "tlv 0x01 absent"); return; }
    const uint32_t apn_len = v[0];
    if (1u + apn_len + 16u > len) { snprintf(out, room, "tlv 0x01 len %u malformed", len); return; }
    char apn[32];
    uint32_t used = 0;
    for (uint32_t i = 0; i < apn_len && used + 1u < sizeof apn; i++) {
        const uint8_t c = v[1u + i];
        apn[used++] = c >= 0x20u && c < 0x7fu ? (char)c : '.';
    }
    apn[used] = 0;
    const uint8_t *fields = v + 1u + apn_len;
    snprintf(out, room, "apn \"%s\" apn_type %u rat %u ip_family %u profile %u", apn,
        (unsigned)umi_ims_rd32(fields), (unsigned)umi_ims_rd32(fields + 4),
        (unsigned)umi_ims_rd32(fields + 8), (unsigned)umi_ims_rd32(fields + 12));
}

/* An IMSDCM request for this boot's host service. 0x20 (PDP Activate Request) is logged and
 * deliberately NOT answered: P1 must not create an IMS bearer, and the report's plan is to see the
 * full request first (a synthetic error could make the modem back off before we learn the set). */
static int ims_dcm_receive(uint32_t dst_port, uint32_t node, uint32_t port, const uint8_t *sdu, uint32_t n)
{
    if (!modem_ims_enabled) return 0;
    if (dst_port != UMI_IMS_DCM_PORT && dst_port != UMI_IMS_QRTR_CTRL) return 0;
    if (n < 7u || sdu[0] != 0u || !umi_ims_tlvs_ok(sdu, n)) return 0;
    const uint32_t message = umi_ims_rd16(sdu + 3);
    if (message == UMI_IMS_CTL_GET_VERSION || message == UMI_IMS_CTL_ALLOCATE || message == UMI_IMS_CTL_RELEASE ||
        message == UMI_IMS_CTL_ALLOCATE_QRTR || message == UMI_IMS_CTL_RELEASE_QRTR) {
        ims_dcm_control(dst_port, node, port, sdu, n, message);
        return 1;
    }
    if (dst_port != UMI_IMS_DCM_PORT) return 0;   /* the control port carries only QMI CTL */
    char tlvs[112], extra[160], detail[288];
    ims_dcm_tlvs(sdu, n, tlvs, sizeof tlvs);
    extra[0] = 0;
    if (message == UMI_IMS_DCM_START) {
        char start[128];
        ims_dcm_start_detail(sdu, n, start, sizeof start);
        snprintf(extra, sizeof extra, "; %s", start);
    }
    /* The remaining 0x20 TLVs: sequence 0x10, subscription 0x11, slot 0x12, instance 0x13. */
    static const uint8_t ids[4] = { 0x10u, 0x11u, 0x12u, 0x13u };
    static const char *names[4] = { "seq", "sub", "slot", "inst" };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t len = 0, room = sizeof extra - strlen(extra);
        const uint8_t *v = umi_ims_find(sdu, n, ids[i], &len);
        if (!v || len != 4u) continue;
        char num[12];
        ims_dcm_num(v, len, num, sizeof num);
        snprintf(extra + strlen(extra), room, " %s %s", names[i], num);
    }
    snprintf(detail, sizeof detail, "msg 0x%04x node %u port %u len %u tlvs %s%s", message, node, port,
        7u + umi_ims_rd16(sdu + 5), tlvs, extra);
    ims_dcm_line(detail);
    return 1;
}

/* The report's §3.2b: iOS's IMS Start Network carries no APN string, only TLV 0x31 (profile index
 * 3GPP, u8) and vendor TLV 0xC2; the internet PDN's Start has no 0xC2 (v32:25745/25749). One line
 * per switch-on, values only, so the modem's own 0x20 profile can be matched against it. */
static void ims_wds_profile_probe(uint8_t cid, uint32_t service, const uint8_t *sdu, uint32_t n)
{
    if (!modem_ims_enabled || ims_wds_logged || service != UMI_IMS_WDS || !umi_ims_tlvs_ok(sdu, n) ||
        sdu[0] != 0u || umi_ims_rd16(sdu + 3) != UMI_IMS_START) return;
    uint32_t plen = 0, vlen = 0;
    const uint8_t *profile = umi_ims_find(sdu, n, 0x31u, &plen);
    const uint8_t *vendor = umi_ims_find(sdu, n, 0xc2u, &vlen);
    if (!vendor) return;
    char p[12], v[12];
    ims_dcm_num(profile, plen, p, sizeof p);
    ims_dcm_num(vendor, vlen, v, sizeof v);
    ims_wds_logged = 1;
    IMS_LOG("WDS start cid %u TLV 0x31 %s TLV 0xC2 %s (iOS IMS profile probe)\n", cid, p, v);
}

static struct umi_ims_request_fix ims_prepare(uint32_t service, const uint8_t *sdu, uint32_t n)
{
    struct umi_ims_request_fix f = { 0, 0, 0, 0, 0 };
    /* Route B: the modem IMS stack reuses this bearer, which may need IPv4 (P-CSCF), so the
     * family TLV is left exactly as sent. */
    if (modem_ims_enabled) return f;
    if (!ims_enabled) return f;
    f = umi_ims_prepare(service, sdu, n, ims_fix, sizeof ims_fix);
    if (f.is_ims)
        IMS_LOG("WDS start IMS family %u%s; APN and identities redacted\n", f.family_before, f.family_changed ? " -> IPv6" : "");
    return f;
}

/* Driver notifications: XNU already owns address and flag changes. DLIL sends
 * SIOCSIFFLAGS with NULL; SIOCSIFADDR can carry an ifaddr, not an ifreq.
 * Never reenter protocol control from its own driver callback. */
static errno_t ims_pdp_ioctl(ifnet_t, unsigned long cmd, void *)
{
    switch (cmd) {
    case SIOCSIFADDR: case SIOCSIFDSTADDR: case SIOCAIFADDR:
    case SIOCSIFADDR_IN6: case SIOCSIFDSTADDR_IN6: case SIOCSIFNETMASK_IN6: case SIOCAIFADDR_IN6:
    case SIOCSIFFLAGS:
        return 0;
    default:
        return -1;
    }
}
