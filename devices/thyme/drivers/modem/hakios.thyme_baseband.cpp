// SPDX-License-Identifier: BSD-2-Clause
/*
 * umi_modem=2: iOS's own CommCenter drives the Mi 10's X55 (devices/xiaomi-mi-10/COMMCENTER-umi.md).
 *
 * The IOKit side of Apple's PCIe Qualcomm baseband as iPhone10,3's CommCenter/ABM/BBUpdater expect it: AppleBaseband (board
 * methods), AppleBasebandPCIControl, one AppleBasebandPCIInterface per libPCITransport channel (ABPInterfaceProtocol = pbl,
 * sahara, ..., qmux1, qmux2, ...), the Mav PDP manager and AppleIPAppender, each with the user-client class name CommCenter's
 * sandbox knows. QMUX on qmux1/qmux2 goes to the X55's QMI services over QRTR (coachz_bb_qmux.h); the sahara channel is a
 * device end that takes BBUpdater's images and drops them (coachz_bb_sahara.h): the X55 runs its own firmware, loaded by
 * umi_modem.c. Apple-private QMI (BSP activation, carrier files, chip info) is answered here (coachz_bb_data.h), the rest of
 * it QMI_ERR_NOT_SUPPORTED. pdp_ip0..3 carry QMAP muxes 1..4 of IP_HW0; CommCenter owns WDS and their addresses, this file
 * only watches WDS to know which mux carries which family.
 *
 * Ported from UniXNU/QcomBaseband dd0199cf, baseband/coachz_baseband.cpp and coachz_bb_pdp.cpp: Copyright (c) 2026,
 * Trogdor-ipadOS contributors (HoshinoYumeka, @chiba233), BSD-2-Clause. Differences: no eSIM (VINYL answers unsupported); no
 * IPA/DPM port setup, and a bind that names only a mux gets the X55's IP_HW0 endpoint {PCIe 3, 4}; every request is answered
 * (umi_bb_pending.h), and DMS reset/shutdown is taken to low power: the X55's bring-up, and after a link fault its bounded
 * restart, are umi_modem.c's service thread (umi_baseband.h), which also pulls uplink packets; services are published
 * before the modem is up and SAHARA's HELLO waits for it; Mav17's chip id.
 *
 * Threads: the modem's service thread owns MHI and QRTR and calls every umi_bb_* function; user threads only queue and wake
 * it. bb_lock covers the bridge, bb_pdp_lock the interfaces' queues; user memory is copied outside them and asynchronous
 * reads complete on a thread call. Logged: QMI header fields and counts, never TLV contents (no IMEI, IMSI, ICCID, number,
 * APN or address).
 */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>

#include "../../bsd/dev/coachz_bb_qmux.h"
#include "../../bsd/dev/coachz_bb_sahara.h"
#include "../../bsd/dev/coachz_bb_control.h"
#include "../../bsd/dev/coachz_bb_channels.h"
#include "../../bsd/dev/coachz_bb_data.h"
#include "../../bsd/dev/hakios.uim_status.h"
#include "../../bsd/dev/coachz_bb_pdp.h"
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include "../../bsd/dev/hakios_imsrtp_abi.h"
#include "../../bsd/dev/umi_baseband.h"
#include "../../bsd/dev/umi_bb_pending.h"

extern "C" void hakios_tl(const char *format, ...) __printflike(1, 2);
#define BB_LOG(...) hakios_tl("THYME BB: " __VA_ARGS__)

#define BB_FRAME_MAX (64u * 1024u)       // one read or write: a QMUX frame, a SAHARA packet or a READ_DATA answer
// Frames waiting for ABM, per channel. With 64 the reference lost boot-time indications and CommCenter reset the baseband.
#define BB_QUEUE 1024u
#define BB_READS 8u                      // asynchronous reads waiting for a frame, per channel
#define BB_TX 128u                       // QMI messages waiting for the modem's thread
#define BB_SYNC_READ_SECONDS 30u
#define BB_SVC_UIM 0x0bu
#define BB_SVC_WDA 0x1au
#define BB_SVC_VINYL 0xe5u
#define BB_QMAP_MAX 32768u               // umi_modem_mhi.inc MD_PACKET: one IP_HW0 transfer
#define BB_PDP_QUEUE 64u

// The SAHARA image the device end asks for: SBL1 (Qualcomm image id 0x15), the first a boot loader requests.
static const uint32_t bb_sahara_images[] = { 0x15 };

namespace {
struct bb_frame { uint32_t len; uint8_t *data; };
struct bb_read { OSAsyncReference64 reference; IOMemoryDescriptor *memory; uint32_t len; void *owner; };
struct bb_channel {
  bb_frame frames[BB_QUEUE];
  uint32_t head, count;
  bb_read reads[BB_READS];
  uint32_t reads_waiting;
  void *owner;
  uint32_t opens, frames_in, frames_out, frames_dropped, bytes_in, bytes_out;
};
struct bb_tx { uint32_t local_port, node, port, len; uint8_t *data; };
struct bb_pdp_if {
  ifnet_t ifp;
  mbuf_t head, tail;
  uint32_t queued, families, blocked;
  uint64_t tx, rx, dropped;
};
}

static IOLock *bb_lock, *bb_pdp_lock;
static thread_call_t bb_complete_call;
static struct cbq_bridge bb_qmux;
static struct cbs_device bb_sahara;
static struct cbc_state bb_control;
static bb_channel bb_ch[CBH_PUBLISHED];
static bb_tx bb_txq[BB_TX];
static uint32_t bb_tx_head, bb_tx_count, bb_tx_dropped;
static int bb_published;
static uint32_t bb_private_seen[64], bb_private_count, bb_ctl_logged, bb_activations;
static uint8_t bb_out[BB_FRAME_MAX];     // answers built under bb_lock
static struct cbp_client bb_pdp_clients[CBQ_MAX_CLIENTS];
static uint32_t bb_pdp_owners[CBQ_MAX_CLIENTS];
static uint8_t bb_wds_sub_bound[CBQ_MAX_CLIENTS];
static bb_pdp_if bb_pdp_ifaces[CBP_INTERFACES];
static uint32_t bb_pdp_next;
// IP_HW0 is raw IP over QMAP (5): set by the kernel (umi_bb_link) and kept unless ABM's own WDA request changes it.
static bool bb_link_ok;
// BBUpdater pushes SBL with sendImage on pbl, then opens sahara and waits 40 s for the loaded SBL's HELLO. Here the HELLO
// also waits for the X55's QRTR services: a booted baseband is one whose QMI answers.
static bool bb_boot_image_sent, bb_hello_wanted, bb_modem_ready;
// The X55's transport died (umi_bb_modem_lost) and has not come back (umi_bb_modem_ready): ABM's QMUX
// requests are dropped without a reply so its own ATCS timer fires and CommCenter resets the baseband
// (v29t: 25 s to "Reset requested"; with a synthetic error answer it never does). Never derived from
// bb_modem_ready, which is false during every boot: only a fault sets this.
static bool bb_transport_lost;
static uint32_t bb_lost_drops;           // host QMUX frames dropped because of it
static struct ubp bb_pending;            // requests forwarded to the X55 and not answered yet
static uint8_t bb_fix[2048];             // a request as the X55 gets it, when the bridge changed it
static uint8_t bb_nas[256];              // NAS 0x33 with the modem-ims voice domain rewritten
static uint32_t bb_endpoint_fixes, bb_req_lines;
#define BB_REQ_LINES 800u                // per-request trace budget (req, rsp, local answers, timeouts)

static uint32_t bb_now_ms(void)
{
  uint64_t ticks, ns;
  clock_get_uptime(&ticks);
  absolutetime_to_nanoseconds(ticks, &ns);
  return (uint32_t)(ns / 1000000u);
}

#include "../../bsd/dev/umi_gnss.inc"
#include "../../bsd/dev/umi_voice.inc"
static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len); /* used by umi_ims.inc */

// ------------------------------------------------------------------ /dev/hakios-imsrtp (+ -data) for userland VoLTE RTP media
#define IMSRTP_RING_LEN    32u
#define IMSRTP_RX_RING_LEN 64u
#define IMSRTP_RX_PKT_MAX  1460u

struct imsrtp_rx_slot {
  uint16_t len;
  uint8_t  data[IMSRTP_RX_PKT_MAX];
};

static IOLock *imsrtp_dev_lock;
static struct hakios_imsrtp_event imsrtp_ring[IMSRTP_RING_LEN];
static uint32_t imsrtp_ring_head, imsrtp_ring_count;
static struct hakios_imsrtp_route imsrtp_route;
static volatile uint32_t imsrtp_route_active_fast;
static struct imsrtp_rx_slot *imsrtp_rx_ring;
static uint32_t imsrtp_rx_head, imsrtp_rx_count;
static uint32_t imsrtp_rx_tapped, imsrtp_rx_dropped, imsrtp_tx_sent, imsrtp_tx_err;
static uint16_t imsrtp_ipv4_id;
static int imsrtp_dev_ready;

static errno_t bb_pdp_output(ifnet_t ifp, mbuf_t m);

static void imsrtp_dev_push(const struct hakios_imsrtp_event *ev)
{
  if (!imsrtp_dev_lock || !ev) return;
  IOLockLock(imsrtp_dev_lock);
  if (imsrtp_ring_count == IMSRTP_RING_LEN) {
    imsrtp_ring_head = (imsrtp_ring_head + 1u) % IMSRTP_RING_LEN;
    imsrtp_ring_count--;
  }
  imsrtp_ring[(imsrtp_ring_head + imsrtp_ring_count) % IMSRTP_RING_LEN] = *ev;
  imsrtp_ring_count++;
  IOLockWakeup(imsrtp_dev_lock, &imsrtp_ring, false);
  IOLockUnlock(imsrtp_dev_lock);
}

static bool imsrtp_rx_try_tap(uint32_t version, const uint8_t *ip, uint32_t n)
{
  if (!imsrtp_route_active_fast || !imsrtp_dev_lock || !imsrtp_rx_ring) return false;
  uint32_t hdr_len = 0;
  uint16_t dport = 0, ulen = 0;
  if (version == 6u) {
    if (n < 48u || ip[6] != 17u) return false;
    hdr_len = 40u;
    dport = (uint16_t)(((uint16_t)ip[42] << 8) | ip[43]);
    ulen  = (uint16_t)(((uint16_t)ip[44] << 8) | ip[45]);
  } else if (version == 4u) {
    if (n < 28u || ip[9] != 17u) return false;
    hdr_len = (uint32_t)(ip[0] & 0x0fu) * 4u;
    if (hdr_len < 20u || n < hdr_len + 8u) return false;
    dport = (uint16_t)(((uint16_t)ip[hdr_len + 2u] << 8) | ip[hdr_len + 3u]);
    ulen  = (uint16_t)(((uint16_t)ip[hdr_len + 4u] << 8) | ip[hdr_len + 5u]);
  } else {
    return false;
  }

  IOLockLock(imsrtp_dev_lock);
  if (!imsrtp_route.active || imsrtp_route.local_port == 0) {
    IOLockUnlock(imsrtp_dev_lock);
    return false;
  }
  const uint16_t lp = imsrtp_route.local_port;
  if (dport != lp) {
    IOLockUnlock(imsrtp_dev_lock);
    return false;
  }
  {
    const uint32_t avail = n - (hdr_len + 8u);
    uint32_t pay_len = (ulen >= 8u && hdr_len + ulen <= n) ? (uint32_t)(ulen - 8u) : avail;
    if (pay_len > IMSRTP_RX_PKT_MAX) pay_len = IMSRTP_RX_PKT_MAX;
    if (pay_len > 0) {
      if (imsrtp_rx_count == IMSRTP_RX_RING_LEN) {
        imsrtp_rx_head = (imsrtp_rx_head + 1u) % IMSRTP_RX_RING_LEN;
        imsrtp_rx_count--;
        imsrtp_rx_dropped++;
      }
      struct imsrtp_rx_slot *slot = &imsrtp_rx_ring[(imsrtp_rx_head + imsrtp_rx_count) % IMSRTP_RX_RING_LEN];
      slot->len = (uint16_t)pay_len;
      memcpy(slot->data, ip + hdr_len + 8u, pay_len);
      imsrtp_rx_count++;
      imsrtp_rx_tapped++;
      IOLockWakeup(imsrtp_dev_lock, &imsrtp_rx_ring, false);
    }
  }
  IOLockUnlock(imsrtp_dev_lock);
  return true;
}

static uint32_t imsrtp_csum_add(uint32_t sum, const uint8_t *buf, uint32_t len)
{
  uint32_t i = 0;
  for (; i + 1u < len; i += 2u) sum += ((uint32_t)buf[i] << 8) | buf[i + 1u];
  if (i < len) sum += (uint32_t)buf[i] << 8;
  return sum;
}

static uint16_t imsrtp_csum_finish(uint32_t sum)
{
  while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
  return (uint16_t)(~sum & 0xffffu);
}

static int imsrtp_dev_open(dev_t, int, int, struct proc *) { return 0; }

static int imsrtp_dev_close(dev_t dev, int, int, struct proc *)
{
  if (minor(dev) == 1 && imsrtp_dev_lock) {
    IOLockLock(imsrtp_dev_lock);
    if (imsrtp_route.active) {
      BB_LOG("imsrtp-data: closed while active (rx_tap=%u drop=%u tx=%u err=%u)\n",
             imsrtp_rx_tapped, imsrtp_rx_dropped, imsrtp_tx_sent, imsrtp_tx_err);
      bzero(&imsrtp_route, sizeof imsrtp_route);
      imsrtp_route_active_fast = 0;
      imsrtp_rx_head = imsrtp_rx_count = 0;
      IOLockWakeup(imsrtp_dev_lock, &imsrtp_rx_ring, false);
    }
    IOLockUnlock(imsrtp_dev_lock);
  }
  return 0;
}

static int imsrtp_dev_read(dev_t dev, struct uio *uio, int ioflag)
{
  if (!imsrtp_dev_lock) return ENXIO;
  if (minor(dev) == 1) {
    if (!imsrtp_rx_ring) return ENXIO;
    uint8_t pkt[IMSRTP_RX_PKT_MAX];
    uint16_t plen = 0;
    IOLockLock(imsrtp_dev_lock);
    while (imsrtp_rx_count == 0) {
      if (!imsrtp_route.active) {
        IOLockUnlock(imsrtp_dev_lock);
        return 0; /* clean EOF when call stops */
      }
      if (ioflag & IO_NDELAY) {
        IOLockUnlock(imsrtp_dev_lock);
        return EWOULDBLOCK;
      }
      int w = IOLockSleep(imsrtp_dev_lock, &imsrtp_rx_ring, THREAD_ABORTSAFE);
      if (w == THREAD_INTERRUPTED) {
        IOLockUnlock(imsrtp_dev_lock);
        return EINTR;
      }
    }
    struct imsrtp_rx_slot *slot = &imsrtp_rx_ring[imsrtp_rx_head];
    plen = slot->len;
    memcpy(pkt, slot->data, plen);
    imsrtp_rx_head = (imsrtp_rx_head + 1u) % IMSRTP_RX_RING_LEN;
    imsrtp_rx_count--;
    IOLockUnlock(imsrtp_dev_lock);
    if ((user_ssize_t)plen > uio_resid(uio)) plen = (uint16_t)uio_resid(uio);
    return uiomove((caddr_t)pkt, (int)plen, uio);
  }

  if (uio_resid(uio) < (user_ssize_t)sizeof(struct hakios_imsrtp_event)) return EINVAL;
  struct hakios_imsrtp_event ev;
  IOLockLock(imsrtp_dev_lock);
  while (imsrtp_ring_count == 0) {
    if (ioflag & IO_NDELAY) {
      IOLockUnlock(imsrtp_dev_lock);
      return EWOULDBLOCK;
    }
    int w = IOLockSleep(imsrtp_dev_lock, &imsrtp_ring, THREAD_ABORTSAFE);
    if (w == THREAD_INTERRUPTED) {
      IOLockUnlock(imsrtp_dev_lock);
      return EINTR;
    }
  }
  ev = imsrtp_ring[imsrtp_ring_head];
  imsrtp_ring_head = (imsrtp_ring_head + 1u) % IMSRTP_RING_LEN;
  imsrtp_ring_count--;
  IOLockUnlock(imsrtp_dev_lock);
  return uiomove((caddr_t)&ev, (int)sizeof(ev), uio);
}

static int imsrtp_dev_write(dev_t dev, struct uio *uio, int)
{
  if (minor(dev) != 1 || !imsrtp_dev_lock) return ENODEV;
  const user_ssize_t req = uio_resid(uio);
  if (req < 2 || req > 1401) return EINVAL;
  uint8_t ubuf[1404];
  int err = uiomove((caddr_t)ubuf, (int)req, uio);
  if (err) return err;

  struct hakios_imsrtp_route r;
  IOLockLock(imsrtp_dev_lock);
  r = imsrtp_route;
  IOLockUnlock(imsrtp_dev_lock);
  if (!r.active || !r.local_port || !r.remote_port) return ENETDOWN;
  if (!bb_pdp_ifaces[1].ifp) return ENETDOWN;

  const bool is_rtcp = (ubuf[0] & HAKIOS_IMSRTP_DATA_RTCP) != 0;
  const uint8_t *pay = ubuf + 1;
  const uint32_t pay_len = (uint32_t)(req - 1);
  const uint16_t sport = is_rtcp ? (uint16_t)(r.local_port + 1u) : r.local_port;
  const uint16_t dport = is_rtcp ? (r.remote_rtcp ? r.remote_rtcp : (uint16_t)(r.remote_port + 1u)) : r.remote_port;
  const uint16_t ulen = (uint16_t)(8u + pay_len);

  uint8_t pkt[1500];
  uint32_t total = 0;
  if (r.ip_version == 6u) {
    bzero(pkt, 48u);
    pkt[0] = 0x60; pkt[1] = 0x00; /* IPv6, TC=0 Flow=0 */
    pkt[4] = (uint8_t)(ulen >> 8); pkt[5] = (uint8_t)ulen;
    pkt[6] = 17u; pkt[7] = 64u;
    memcpy(pkt + 8, r.local_ip, 16u);
    memcpy(pkt + 24, r.remote_ip, 16u);
    pkt[40] = (uint8_t)(sport >> 8); pkt[41] = (uint8_t)sport;
    pkt[42] = (uint8_t)(dport >> 8); pkt[43] = (uint8_t)dport;
    pkt[44] = (uint8_t)(ulen >> 8);  pkt[45] = (uint8_t)ulen;
    memcpy(pkt + 48, pay, pay_len);
    uint32_t sum = 0;
    sum = imsrtp_csum_add(sum, r.local_ip, 16u);
    sum = imsrtp_csum_add(sum, r.remote_ip, 16u);
    sum += (uint32_t)ulen + 17u;
    sum = imsrtp_csum_add(sum, pkt + 40, ulen);
    uint16_t csum = imsrtp_csum_finish(sum);
    if (csum == 0) csum = 0xffffu;
    pkt[46] = (uint8_t)(csum >> 8); pkt[47] = (uint8_t)csum;
    total = 48u + pay_len;
  } else if (r.ip_version == 4u) {
    const uint16_t tlen = (uint16_t)(28u + pay_len);
    const uint16_t id = ++imsrtp_ipv4_id;
    bzero(pkt, 28u);
    pkt[0] = 0x45; pkt[1] = 0xb8;
    pkt[2] = (uint8_t)(tlen >> 8); pkt[3] = (uint8_t)tlen;
    pkt[4] = (uint8_t)(id >> 8);   pkt[5] = (uint8_t)id;
    pkt[6] = 0x40; pkt[8] = 64u;   pkt[9] = 17u;
    memcpy(pkt + 12, r.local_ip, 4u);
    memcpy(pkt + 16, r.remote_ip, 4u);
    uint16_t hcsum = imsrtp_csum_finish(imsrtp_csum_add(0, pkt, 20u));
    pkt[10] = (uint8_t)(hcsum >> 8); pkt[11] = (uint8_t)hcsum;
    pkt[20] = (uint8_t)(sport >> 8); pkt[21] = (uint8_t)sport;
    pkt[22] = (uint8_t)(dport >> 8); pkt[23] = (uint8_t)dport;
    pkt[24] = (uint8_t)(ulen >> 8);  pkt[25] = (uint8_t)ulen;
    memcpy(pkt + 28, pay, pay_len);
    uint32_t sum = 0;
    sum = imsrtp_csum_add(sum, r.local_ip, 4u);
    sum = imsrtp_csum_add(sum, r.remote_ip, 4u);
    sum += 17u + (uint32_t)ulen;
    sum = imsrtp_csum_add(sum, pkt + 20, ulen);
    uint16_t csum = imsrtp_csum_finish(sum);
    if (csum == 0) csum = 0xffffu;
    pkt[26] = (uint8_t)(csum >> 8); pkt[27] = (uint8_t)csum;
    total = 28u + pay_len;
  } else {
    return EINVAL;
  }

  mbuf_t m = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, total, nullptr, &m) || !m) {
    imsrtp_tx_err++;
    return ENOBUFS;
  }
  if (mbuf_copyback(m, 0, total, pkt, MBUF_DONTWAIT)) {
    mbuf_freem(m);
    imsrtp_tx_err++;
    return ENOBUFS;
  }
  errno_t rc = bb_pdp_output(bb_pdp_ifaces[1].ifp, m);
  if (rc) { imsrtp_tx_err++; return rc; }
  if (imsrtp_tx_sent == 0) {
    BB_LOG("imsrtp-data: tx #1 v%u total=%u %u->%u csum=%02x%02x rtp=%02x%02x%02x%02x\n",
           r.ip_version, total, sport, dport,
           r.ip_version == 6u ? pkt[46] : pkt[26], r.ip_version == 6u ? pkt[47] : pkt[27],
           pay_len > 0 ? pay[0] : 0, pay_len > 1 ? pay[1] : 0,
           pay_len > 2 ? pay[2] : 0, pay_len > 3 ? pay[3] : 0);
  }
  imsrtp_tx_sent++;
  return 0;
}

static int imsrtp_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, int, struct proc *)
{
  if (minor(dev) != 1 || !imsrtp_dev_lock) return ENOTTY;
  if (cmd != HAKIOS_IMSRTP_SET_ROUTE || !data) return ENOTTY;
  const struct hakios_imsrtp_route *nr = (const struct hakios_imsrtp_route *)data;
  IOLockLock(imsrtp_dev_lock);
  if (nr->active) {
    imsrtp_route = *nr;
    imsrtp_rx_head = imsrtp_rx_count = 0;
    imsrtp_rx_tapped = imsrtp_rx_dropped = imsrtp_tx_sent = imsrtp_tx_err = 0;
    imsrtp_route_active_fast = 1;
    BB_LOG("imsrtp-data: route ON v%u local_port=%u remote_port=%u rtcp=%u\n",
           nr->ip_version, nr->local_port, nr->remote_port, nr->remote_rtcp);
  } else {
    BB_LOG("imsrtp-data: route OFF (rx_tap=%u drop=%u tx=%u err=%u)\n",
           imsrtp_rx_tapped, imsrtp_rx_dropped, imsrtp_tx_sent, imsrtp_tx_err);
    bzero(&imsrtp_route, sizeof imsrtp_route);
    imsrtp_route_active_fast = 0;
    imsrtp_rx_head = imsrtp_rx_count = 0;
    IOLockWakeup(imsrtp_dev_lock, &imsrtp_rx_ring, false);
  }
  IOLockUnlock(imsrtp_dev_lock);
  return 0;
}

static const struct cdevsw imsrtp_cdevsw = {
  .d_open = imsrtp_dev_open,
  .d_close = imsrtp_dev_close,
  .d_read = imsrtp_dev_read,
  .d_write = imsrtp_dev_write,
  .d_ioctl = imsrtp_dev_ioctl,
  .d_stop = eno_stop,
  .d_reset = eno_reset,
  .d_ttys = nullptr,
  .d_select = eno_select,
  .d_mmap = eno_mmap,
  .d_strategy = eno_strat,
  .d_type = 0,
};

static void imsrtp_dev_init(void)
{
  if (imsrtp_dev_ready) return;
  imsrtp_dev_lock = IOLockAlloc();
  if (!imsrtp_dev_lock) return;
  imsrtp_rx_ring = static_cast<struct imsrtp_rx_slot *>(
      IOMallocZero(sizeof(struct imsrtp_rx_slot) * IMSRTP_RX_RING_LEN));
  int major = cdevsw_add(-1, &imsrtp_cdevsw);
  if (major >= 0 &&
      devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-imsrtp") != nullptr &&
      devfs_make_node(makedev(major, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-imsrtp-data") != nullptr) {
    imsrtp_dev_ready = 1;
    BB_LOG("/dev/hakios-imsrtp + /dev/hakios-imsrtp-data ready for VoLTE RTP media daemon\n");
  }
}

#include "../../bsd/dev/umi_ims.inc"

// ------------------------------------------------- LTE VoPS override (boot argument umi_vops=1)
/* CommCenter's "Ims Voice Support on LTE" capability has one input: TLV 0x21 ("LTE Voice Support", u8)
 * of the NAS System Info indication (0x4E) or of a Get System Info response (0x4D). The X55 reports 0,
 * so CommCenter keeps IMS voice out of Bambi's services mask and registers sms only. With umi_vops=1
 * -- off unless the boot argument says so -- the bridge rewrites that one value byte to 1 in its own
 * copy of the frame on the way to iOS (cbq_from_modem's copy, not the modem's SDU). Only NAS, only
 * while the message carries the LTE system/service info (TLV 0x14/0x19, i.e. an LTE camp) and only
 * when TLV 0x21 is present; lengths, every other TLV (emergency ones included) and the modem side are
 * untouched. One line the first time it applies; bb_report carries the count. */
static bool bb_vops_on, bb_vops_logged;
static uint32_t bb_vops_flips;

static void bb_vops_claim(uint32_t service, uint8_t *frame, uint32_t len)
{
  if (!bb_vops_on || service != CBD_SVC_NAS || len < CBQ_HDR + 7u) return;
  uint8_t *sdu = frame + CBQ_HDR;
  const uint32_t n = len - CBQ_HDR;
  const uint32_t message = umi_ims_rd16(sdu + 3);
  if (!((sdu[0] == CBQ_QMI_INDICATION && message == 0x004eu) ||
        (sdu[0] == CBQ_QMI_RESPONSE && message == 0x004du)))
    return;
  uint32_t other = 0;
  // LTE System Info (0x14) or LTE Service Info (0x19): no LTE camp, no voice claim.
  if (!umi_ims_find(sdu, n, 0x14u, &other) && !umi_ims_find(sdu, n, 0x19u, &other)) return;
  uint32_t tlv_len = 0;
  uint8_t *v = (uint8_t *)umi_ims_find(sdu, n, 0x21u, &tlv_len);
  if (!v || tlv_len != 1u || v[0] == 1u) return;
  const uint32_t was = v[0];
  v[0] = 1u;
  bb_vops_flips++;
  if (!bb_vops_logged) {
    bb_vops_logged = true;
    hakios_tl("umi_vops: forcing LTE VoPS supported (msg 0x%04x tlv 0x%02x was %u)\n", message, 0x21u, was);
  }
}

// ------------------------------------------------------------------ queues (bb_lock held)
static void bb_complete(thread_call_param_t, thread_call_param_t);

// Probe: QMI header fields of each message, never TLV contents. UIM, WDA and WDS print every message; other services the
// first 6 of a kind, then each power of two. A budget keeps the log from filling.
#define BB_TRACE_KINDS 512u
#define BB_TRACE_BUDGET 4000u
static uint64_t bb_trace_key[BB_TRACE_KINDS];
static uint32_t bb_trace_count[BB_TRACE_KINDS], bb_trace_lines, bb_trace_held;
static void bb_trace(const char *dir, uint32_t channel, uint32_t service, uint32_t cid, const uint8_t *sdu, uint32_t n)
{
  if (!service || n < 7u) return;   // CTL has its own SDU layout and its own lines
  const uint32_t type = sdu[0], txn = cbd_rd16(sdu + 1), message = cbd_rd16(sdu + 3);
  const uint32_t error = type == 2u ? (cbd_sdu_ok(sdu, n) ? cbd_error(sdu, n) : 0xfffeu) : 0u;
  const uint64_t key = 1u + ((uint64_t)((uint32_t)dir[0] << 8 | (uint32_t)dir[1]) << 48 | (uint64_t)service << 40 |
    (uint64_t)type << 32 | (uint64_t)message << 16 | error);
  uint32_t count = 0;
  for (uint32_t i = 0, at = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 55) % BB_TRACE_KINDS; i < BB_TRACE_KINDS;
       i++, at = (at + 1) % BB_TRACE_KINDS) {
    if (bb_trace_key[at] != key && bb_trace_key[at]) continue;
    bb_trace_key[at] = key;
    count = ++bb_trace_count[at];
    break;
  }
  const bool always = service == BB_SVC_UIM || service == BB_SVC_WDA || service == CBD_SVC_WDS;
  if (!always && count > 6u && (count & (count - 1u))) return;
  if (bb_trace_lines >= BB_TRACE_BUDGET) { bb_trace_held++; return; }
  bb_trace_lines++;
  BB_LOG("qmi: %s ch %u svc 0x%x cid %u type %u msg 0x%x txn %u len %u err 0x%x n %u t %u\n",
         dir, channel, service, cid, type, message, txn, n, error, count, bb_now_ms());
}
// A QMUX frame (I/F type 1, length, flags, service, client id, then the SDU).
static void bb_trace_frame(const char *dir, uint32_t channel, const uint8_t *frame, uint32_t len)
{
  if (len >= CBQ_HDR + 7u && frame[0] == 1u) bb_trace(dir, channel, frame[4], frame[5], frame + CBQ_HDR, len - CBQ_HDR);
}

static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len)
{
  if (channel == CBH_QMUX1 || channel == CBH_QMUX2) bb_trace_frame("to-host", channel == CBH_QMUX1 ? 1u : 2u, data, len);
  bb_channel *c = &bb_ch[channel];
  uint8_t *copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || c->count == BB_QUEUE) {
    if (copy) IOFreeData(copy, len);
    if (c->frames_dropped < 32u)
      BB_LOG("dropped frame for ABM on channel %u (%s, %u queued, %u bytes) svc 0x%x type %u msg 0x%x\n", channel,
             copy ? "queue full" : "no memory", c->count, len, len > 4 ? data[4] : 0u, len > CBQ_HDR ? data[CBQ_HDR] : 0u,
             len > CBQ_HDR + 4 ? (uint32_t)(data[CBQ_HDR + 3] | data[CBQ_HDR + 4] << 8) : 0u);
    c->frames_dropped++;
    return;
  }
  memcpy(copy, data, len);
  c->frames[(c->head + c->count++) % BB_QUEUE] = { len, copy };
  c->frames_out++;
  c->bytes_out += len;
  IOLockWakeup(bb_lock, c, false);
  if (c->reads_waiting && bb_complete_call) thread_call_enter(bb_complete_call);
}

static bb_frame bb_pop(uint32_t channel)
{
  bb_channel *c = &bb_ch[channel];
  bb_frame f = { 0, nullptr };
  if (!c->count) return f;
  f = c->frames[c->head];
  c->head = (c->head + 1) % BB_QUEUE;
  c->count--;
  return f;
}

static void bb_flush(uint32_t channel)
{
  for (bb_frame f = bb_pop(channel); f.data; f = bb_pop(channel)) IOFreeData(f.data, f.len);
}

// A QMI message for the X55, sent by the modem's thread from umi_bb_poll.
static bool bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len)
{
  uint8_t *copy = len ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || bb_tx_count == BB_TX) {
    if (copy) IOFreeData(copy, len);
    bb_tx_dropped++;
    return false;
  }
  memcpy(copy, data, len);
  bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = { local_port, node, port, len, copy };
  return true;
}

// ------------------------------------------------------------------ every request answered (bb_lock held)
static uint32_t bb_qmux_channel(uint32_t qmux) { return qmux == 0 ? CBH_QMUX1 : CBH_QMUX2; }

// The answer ABM gets for a request nobody else answers: its transaction and message, result `error`.
static void bb_answer_error(uint32_t qmux, uint32_t service, uint32_t cid, uint32_t txn, uint32_t message, uint32_t error, const char *why)
{
  const uint32_t n = cbq_private_message(CBQ_QMI_RESPONSE, txn, message, service, cid, error, nullptr, 0, bb_out, sizeof bb_out);
  if (n) bb_push(bb_qmux_channel(qmux), bb_out, n);
  if (bb_req_lines++ < BB_REQ_LINES)
    BB_LOG("req svc 0x%x msg 0x%x txn %u cid %u answered here: error 0x%x (%s)\n", service, message, txn, cid, error, why);
}

#include "../../bsd/dev/umi_cellinfo.inc"

// A request for the X55: remembered until it answers, or answered here when it cannot be sent.
static void bb_forward(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  const uint32_t txn = cbd_rd16(sdu + 1), message = cbd_rd16(sdu + 3);
  if (!ubp_add(&bb_pending, r.local_port, r.channel, r.cid, r.service, txn, message, bb_now_ms())) {
    bb_answer_error(r.channel, r.service, r.cid, txn, message, UBP_ERR_INTERNAL, "too many requests in flight");
    return;
  }
  if (!bb_queue_tx(r.local_port, r.node, r.port, sdu, n)) {
    struct ubp_entry e;
    ubp_answer(&bb_pending, r.local_port, txn, &e);
    bb_answer_error(r.channel, r.service, r.cid, txn, message, UBP_ERR_INTERNAL, "send queue full");
    return;
  }
  voice_note("tx", r, sdu, n);
  if (bb_req_lines++ < BB_REQ_LINES)
    BB_LOG("req svc 0x%x msg 0x%x txn %u cid %u -> X55 node %u port %u from 0x%x\n", r.service, message, txn, r.cid, r.node, r.port,
           r.local_port);
}

// A request cbq_from_host could not route (a client ABM no longer has, a service the X55 does not announce): answered.
static void bb_answer_unrouted(uint32_t qmux, const uint8_t *f, uint32_t len)
{
  if (len < CBQ_HDR + CBQ_SDU_HDR || f[0] != CBQ_IF_TYPE || f[3] != 0 || !f[4] || f[CBQ_HDR] != CBQ_QMI_REQUEST) return;
  const uint8_t *sdu = f + CBQ_HDR;
  const bool known = cbq_client_find(&bb_qmux, qmux, f[4], f[5]) >= 0;
  bb_answer_error(qmux, f[4], f[5], cbd_rd16(sdu + 1), cbd_rd16(sdu + 3), known ? CBQ_ERR_INVALID_SERVICE_TYPE : CBQ_ERR_INVALID_CLIENT_ID,
                  known ? "service not announced by the X55" : "unknown client");
}

static void bb_sahara_restart(void)
{
  struct cbs_out o;
  bb_flush(CBH_SAHARA);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  cbs_start(&bb_sahara, &o);
  for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
  BB_LOG("sahara hello\n");
}

static void bb_private_note(uint32_t service, uint32_t message)
{
  const uint32_t key = service << 16 | (message & 0xffff);
  for (uint32_t i = 0; i < bb_private_count; i++)
    if (bb_private_seen[i] == key) return;
  if (bb_private_count < 64) bb_private_seen[bb_private_count++] = key;
  BB_LOG("private QMI service 0x%x message 0x%x answered not supported\n", service, message);
}

// ------------------------------------------------------------------ data: which mux carries what (bb_lock held)
// WDS is entirely CommCenter's. Observe only lifecycle facts, never credentials or addresses.
static void bb_pdp_note(uint32_t local_port, uint32_t cid, uint32_t channel, const uint8_t *sdu, uint32_t n)
{
  const uint32_t slot = local_port - CBQ_PORT_BASE;
  if (slot >= CBQ_MAX_CLIENTS || !cbd_sdu_ok(sdu, n)) return;
  const uint32_t owner = 1u + cid + (channel << 8);
  if (bb_pdp_owners[slot] != owner) {
    bzero(&bb_pdp_clients[slot], sizeof bb_pdp_clients[slot]);
    bb_pdp_clients[slot].family = 4; // WDS default family, overridden by Set IP Family or Start Network
    bb_pdp_owners[slot] = owner;
  }
  cbp_client &c = bb_pdp_clients[slot];
  if (ims_pdp_observe(slot, sdu, n))
    BB_LOG("pdp: WDS type %u message 0x%x txn %u mux %u family %u active %u error %u address-fields %u\n",
      sdu[0], cbd_rd16(sdu + 3), cbd_rd16(sdu + 1), c.mux, c.family, c.active, sdu[0] == 2 ? cbd_error(sdu, n) : 0, c.settings);
  const bool start_failed = sdu[0] == 2 && cbd_rd16(sdu + 3) == CBP_START && cbd_error(sdu, n);
  if (start_failed || (sdu[0] == 4 && cbd_rd16(sdu + 3) == CBP_STATUS && !c.active)) {
    uint32_t reason_size = 0, verbose_size = 0;
    const uint8_t *reason = cbd_find(sdu, n, 0x10, &reason_size);
    const uint8_t *verbose = cbd_find(sdu, n, 0x11, &verbose_size);
    BB_LOG("pdp: %s cid %u channel %u mux %u family %u reason %u verbose type %u code %d\n",
      start_failed ? "start failed" : "disconnected", cid, channel, c.mux, c.family,
      reason && reason_size == 2u ? cbd_rd16(reason) : 0u, verbose && verbose_size == 4u ? cbd_rd16(verbose) : 0u,
      verbose && verbose_size == 4u ? static_cast<int16_t>(cbd_rd16(verbose + 2)) : 0);
  }
}

// ABM sets the data format itself (libBasebandManager "Success setting DataFormat"). Name what it asked for and what the
// X55 took; umi_modem_data.inc frames basic QMAP (5) only, so anything else stops the data path with a line saying so.
static void bb_wda_request(const uint8_t *sdu, uint32_t n)
{
  uint32_t len = 0;
  if (sdu[0] != 0u || cbd_rd16(sdu + 3) != CBD_WDA_SET_DATA_FORMAT) return;
  const uint8_t *ep = cbd_find(sdu, n, 0x17, &len);
  BB_LOG("data format asked by ABM: link %d uplink %d downlink %d datagrams %d size %d endpoint %d/%d\n",
    (int)cbd_u32(sdu, n, 0x11), (int)cbd_u32(sdu, n, 0x12), (int)cbd_u32(sdu, n, 0x13), (int)cbd_u32(sdu, n, 0x15),
    (int)cbd_u32(sdu, n, 0x16), ep && len == 8u ? (int)cbd_rd32(ep) : -1, ep && len == 8u ? (int)cbd_rd32(ep + 4) : -1);
}
static void bb_wda_answer(const uint8_t *sdu, uint32_t n)
{
  if (sdu[0] != 2u || cbd_rd16(sdu + 3) != CBD_WDA_SET_DATA_FORMAT || cbd_error(sdu, n)) return;
  const uint32_t llp = cbd_u32(sdu, n, 0x11), ul = cbd_u32(sdu, n, 0x12), dl = cbd_u32(sdu, n, 0x13), size = cbd_u32(sdu, n, 0x16);
  bb_link_ok = llp == CBD_LLP_RAW_IP && ul == CBD_DAP_QMAP && dl == CBD_DAP_QMAP && size && size <= BB_QMAP_MAX;
  BB_LOG("data format now link %d uplink %d downlink %d size %d: %s\n", (int)llp, (int)ul, (int)dl, (int)size,
    bb_link_ok ? "QMAP, data path kept" : "not basic QMAP, data path stopped");
}

// ------------------------------------------------------------------ activation and SIM identity (bb_lock held)
// The activation indication (coachz_bb_data.h) to the BSP client that registered, with slot one's ICCID and IMSI once read.
static uint8_t bb_iccid[CBD_ICCID_BYTES], bb_imsi[CBD_IMSI_BYTES], bb_iccid_known, bb_imsi_known, bb_act_known, bb_act_channel, bb_act_cid;
static void bb_activation_indicate(void)
{
  if (!bb_act_known) return;
  uint8_t tlvs[CBD_ACTIVATION_SIM_TLVS];
  const uint32_t t = cbd_activation_state(tlvs, sizeof tlvs, bb_iccid_known ? bb_iccid : nullptr, bb_imsi_known ? bb_imsi : nullptr);
  const uint32_t n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, bb_act_cid, tlvs, t, bb_out, sizeof bb_out);
  if (!n) return;
  bb_push(bb_act_channel, bb_out, n);
  static uint32_t sent;
  if (sent++ < 8) BB_LOG("activation mode disabled (0x15=1) sent\n");
}

// CommCenter's own EF_ICCID and EF_IMSI reads: client, transaction and file, so the answer can be recognized. Never logged.
struct bb_sim_read { uint8_t used, channel, cid; uint16_t txn, file; };
static bb_sim_read bb_sim_reads[8];
static uint32_t bb_sim_next;
static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service != CBD_SVC_UIM) return;
  const uint32_t file = cbd_uim_read_file(sdu, r.len);
  if (file != CBD_EF_ICCID && file != CBD_EF_IMSI) return;
  bb_sim_reads[bb_sim_next++ % 8] = { 1, r.channel, r.cid, cbd_rd16(sdu + 1), (uint16_t)file };
}
static void bb_sim_answer(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  if (r.service != CBD_SVC_UIM || !cbd_sdu_ok(sdu, n) || sdu[0] != CBQ_QMI_RESPONSE) return;
  const uint32_t message = cbd_rd16(sdu + 3);
  if (message == 0x002fu && cbd_error(sdu, n) == 0) {
    uint32_t length = 0;
    const uint8_t *status = cbd_find(sdu, n, 0x10, &length);
    struct uim_inventory inventory;
    if (status && uim_status_inventory(status, length, &inventory)) {
      BB_LOG("UIM inventory: %u reported slots, %u cards present, %u ready SIMs, primary GW index 0x%x, secondary GW index 0x%x\n",
             inventory.slots, inventory.present, inventory.ready, inventory.primary_gw, inventory.secondary_gw);
    } else {
      BB_LOG("UIM inventory: no complete card status TLV (%u bytes)\n", length);
    }
  }
  if (message != CBD_UIM_READ_TRANSPARENT) return;
  for (auto &q : bb_sim_reads) {
    if (!q.used || q.channel != r.channel || q.cid != r.cid || q.txn != cbd_rd16(sdu + 1)) continue;
    q.used = 0;
    uint8_t ef[16], value[CBD_ICCID_BYTES];
    const uint32_t len = cbd_uim_read_content(sdu, n, ef, sizeof ef);
    const bool iccid = q.file == CBD_EF_ICCID;
    if (iccid ? !cbd_iccid_from_ef(ef, len, value) : !cbd_imsi_from_ef(ef, len, value)) return;
    uint8_t *have = iccid ? bb_iccid : bb_imsi, *known = iccid ? &bb_iccid_known : &bb_imsi_known;
    const uint32_t size = iccid ? CBD_ICCID_BYTES : CBD_IMSI_BYTES;
    if (*known && !memcmp(value, have, size)) return;
    memcpy(have, value, size); *known = 1;
    BB_LOG("slot one %s read from the card, activation indication %s\n", iccid ? "ICCID" : "IMSI",
           bb_act_known ? "sent again with it" : "waits for registration");
    bb_activation_indicate();
    return;
  }
}

// A private request the bridge answers itself: 1 when it did.
static int bb_private_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
  const uint32_t message = (uint32_t)(sdu[3] | sdu[4] << 8);
  if (r.service != CBD_SVC_BSP) return 0;
  // Carrier bundle files: accepted and dropped, each segment acknowledged with the next segment number.
  if (message == CBD_BSP_SEND_FILE) {
    struct cbd_file_segment f;
    if (!cbd_bsp_file_segment(sdu, r.len, &f)) return 0;
    uint8_t tlvs[8];
    const uint32_t t = cbd_bsp_file_answer(&f, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    if (f.segment + 1u == f.count) {
      // The file is complete: report it received as an Apple modem does (FileTransferStatus), or CommCenter times out after 22 s.
      uint8_t done[12];
      const uint32_t d = cbd_bsp_file_done(&f, done, sizeof done);
      const uint32_t m = cbq_private_indication(CBD_BSP_FILE_TRANSFER_STATUS, r.service, r.cid, done, d, bb_out, sizeof bb_out);
      if (m) bb_push(channel, bb_out, m);
      BB_LOG("carrier file type %u taken (%u segments, last %u bytes), dropped, transfer status sent\n", f.type, f.count, f.bytes);
    }
    return 1;
  }
  if (message == CBD_BSP_CHIP_INFO) {
    uint8_t tlvs[12];
    const uint32_t t = cbd_bsp_chip_info(CBS_CHIP_ID, CBS_SERIAL, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    return 1;
  }
  if (message == CBD_BSP_SET_CONFIG) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    BB_LOG("baseband config (SetConfig) taken\n");
    return 1;
  }
  if (message == CBD_BSP_ACTIVATION_REGISTER) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    bb_act_known = 1; bb_act_channel = (uint8_t)channel; bb_act_cid = r.cid;
    bb_activation_indicate();
    if (bb_activations++ < 4)
      BB_LOG("activation register answered, state activated, slot one ICCID %s, IMSI %s\n", bb_iccid_known ? "included" : "not read yet",
             bb_imsi_known ? "included" : "not read yet");
    return 1;
  }
  return 0;
}

// China Broadnet (PLMN 46015) uses Apple's signed CMCC_cn.bundle (same MOCN VoLTE/IMS stack),
// whose default Internet APN is "cmnet"; rewrite the 5-byte "cmnet" APN in-place to "cbnet" on
// WDS Create/Modify Profile (0x27/0x28/0xc7) and Start Network (0x20) when slot one's IMSI is 46015.
static bool bb_cbn_apn_fix(uint32_t service, uint8_t *sdu, uint32_t n)
{
  if (service != CBD_SVC_WDS || !bb_imsi_known || bb_imsi[0] != 0x46 || bb_imsi[1] != 0x01 || (bb_imsi[2] >> 4) != 5u) return false;
  if (!cbd_sdu_ok(sdu, n) || sdu[0] != 0u) return false;
  const uint32_t msg = cbd_rd16(sdu + 3);
  if (msg != 0x0020u && msg != 0x0027u && msg != 0x0028u && msg != 0x00c7u) return false;
  const uint32_t end = 7u + cbd_rd16(sdu + 5);
  bool changed = false;
  for (uint32_t at = 7u; at + 3u <= end;) {
    const uint32_t type = sdu[at], len = cbd_rd16(sdu + at + 1);
    if (at + 3u + len > end) break;
    if ((type == 0x14u || type == 0xa1u) && len == 5u) {
      uint8_t *v = sdu + at + 3;
      if ((v[0] | 0x20) == 'c' && (v[1] | 0x20) == 'm' && (v[2] | 0x20) == 'n' && (v[3] | 0x20) == 'e' && (v[4] | 0x20) == 't') {
        v[1] = (v[1] >= 'A' && v[1] <= 'Z') ? 'B' : 'b';
        changed = true;
        static uint32_t rewrites;
        if (rewrites++ < 16) BB_LOG("46015 CBN: WDS msg 0x%x tlv 0x%x APN cmnet -> cbnet\n", msg, type);
      }
    }
    at += 3u + len;
  }
  return changed;
}

static void bb_wds_tlv_log(uint32_t service, const uint8_t *sdu, uint32_t n)
{
  static uint32_t logged;
  if (service != CBD_SVC_WDS || logged >= 16 || !cbd_sdu_ok(sdu, n) || sdu[0] != 0u) return;
  const uint32_t msg = cbd_rd16(sdu + 3);
  if (msg != 0x0020u && msg != 0x0027u && msg != 0x0028u && msg != 0x00c7u) return;
  char desc[160];
  uint32_t used = 0, end = 7u + cbd_rd16(sdu + 5);
  desc[0] = 0;
  for (uint32_t at = 7u; at + 3u <= end && used + 24u < sizeof desc;) {
    const uint32_t t = sdu[at], l = cbd_rd16(sdu + at + 1);
    if (at + 3u + l > end) break;
    const uint8_t *v = sdu + at + 3;
    if (t == 0x14u && l > 0u && l <= 16u) {
      char apn[17];
      for (uint32_t k = 0; k < l; k++) apn[k] = (v[k] >= 0x20 && v[k] < 0x7f) ? (char)v[k] : '?';
      apn[l] = 0;
      used += (uint32_t)snprintf(desc + used, sizeof desc - used, "%s14='%s'", used ? " " : "", apn);
    } else if (t == 0x01u && l == 2u) {
      used += (uint32_t)snprintf(desc + used, sizeof desc - used, "%s01=%u/%u", used ? " " : "", v[0], v[1]);
    } else if (t == 0x31u && l == 1u) {
      used += (uint32_t)snprintf(desc + used, sizeof desc - used, "%s31=%u", used ? " " : "", v[0]);
    } else {
      used += (uint32_t)snprintf(desc + used, sizeof desc - used, "%s%02x:%u", used ? " " : "", t, l);
    }
    at += 3u + l;
  }
  logged++;
  BB_LOG("WDS req msg 0x%x len %u tlvs=[%s]\n", msg, n, desc);
}

// One write from ABM on a channel, after its bytes were copied in.
static void bb_host_write(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  c->frames_in++;
  c->bytes_in += len;
  if (channel == CBH_SAHARA) {
    const enum cbs_state before = bb_sahara.state;
    for (uint32_t at = 0; at < len && bb_sahara.state != CBS_FAILED;) {
      struct cbs_out o;
      const uint32_t used = cbs_receive(&bb_sahara, data + at, len - at, &o);
      for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
      if (!used) break;
      at += used;
    }
    if (bb_sahara.state != before && bb_sahara.state == CBS_COMMAND)
      BB_LOG("sahara command mode\n");
    if (bb_sahara.state != before && (bb_sahara.state == CBS_FINISHED || bb_sahara.state == CBS_FAILED || bb_sahara.state == CBS_WAIT_DONE))
      BB_LOG("sahara %s, image %u of %u (id 0x%x), host mode %u status %u, last command %u, %u requests, %llu bytes\n",
             bb_sahara.state == CBS_FINISHED ? "finished" : bb_sahara.state == CBS_FAILED ? "failed" : "image read",
             bb_sahara.image_index + 1, bb_sahara.image_count, bb_sahara.images[bb_sahara.image_index], bb_sahara.host_mode,
             bb_sahara.host_status, bb_sahara.last_command, bb_sahara.requests, bb_sahara.bytes);
    return;
  }
  if (channel != CBH_QMUX1 && channel != CBH_QMUX2) return;   // counted above, nothing behind it
  const uint32_t qmux = channel == CBH_QMUX1 ? 0 : 1;
  bb_trace_frame("host", qmux + 1u, data, len);
  // A faulted X55 is not answered at all: ABM's in-flight QMI transaction must stay unanswered so its ATCS
  // timer fires and CommCenter runs its own baseband reset. A live link passes even before umi_bb_modem_ready
  // clears the flag (umi_mdm_restart=1). Boot never reaches this: only a fault sets the flag.
  if (bb_transport_lost && umi_modem_state() != 1) {
    if (!bb_lost_drops)   // once per boot; the periodic summary carries the count
      BB_LOG("transport lost: dropping host QMUX (no reply) so ABM resets (qmux reads waiting %u/%u)\n",
             bb_ch[CBH_QMUX1].reads_waiting, bb_ch[CBH_QMUX2].reads_waiting);
    bb_lost_drops++;
    return;
  }
  // A switch sampled since the last poll pass applies before this frame is answered: the gate a
  // frame sees (0x28 private, the 0xe1/0x28 stub answers) is never older than the last sample.
  ims_apply_switch();
  const struct cbq_result r = cbq_from_host(&bb_qmux, qmux, data, len, bb_out, sizeof bb_out);
  cellinfo_prune();   // CTL release/sync/allocation may change NAS ownership immediately
  ims_prune();
  switch (r.kind) {
  case CBQ_TO_HOST:
    ims_note_ctl(data, len, bb_out, r.len);
    bb_push(channel, bb_out, r.len);
    if (bb_qmux.ctl_other != bb_ctl_logged) {
      bb_ctl_logged = bb_qmux.ctl_other;
      BB_LOG("CTL message 0x%x accepted without action\n", (uint32_t)(data[CBQ_HDR + 2] | data[CBQ_HDR + 3] << 8));
    }
    break;
  case CBQ_TO_MODEM: {
    const uint8_t *sdu = data + CBQ_HDR;
    uint32_t n = r.len;
    // Route B probe (report INVESTIGATION-routeB-x55-noreg.md §3.2b): one line for iOS's IMS Start
    // Network -- TLV 0x31 profile index and vendor 0xC2 -- so it can be matched against the modem's
    // own IMSDCM 0x20 request. Off with the switch; never changes the request.
    ims_wds_profile_probe(r.cid, r.service, sdu, n);
    if (cellinfo_host(r, sdu, n)) break;
    // Route B (modem-ims): NAS voice domain preference PS only; with the switch off this returns 0
    // and the request is forwarded byte for byte.
    const uint32_t nas = ims_nas_voice_domain(r.service, sdu, n, bb_nas, sizeof bb_nas);
    if (nas) { sdu = bb_nas; n = nas; }
    const struct umi_ims_request_fix ims = ims_prepare(r.service, sdu, n);
    if (ims.len) { sdu = ims_fix; n = ims.len; }
    if (ims_enabled && umi_ims_modem_registration_command(r.service, cbd_rd16(sdu + 3))) {
      bb_answer_error(r.channel, r.service, r.cid, cbd_rd16(sdu + 1), cbd_rd16(sdu + 3), CBQ_ERR_NOT_SUPPORTED,
                      "Apple IMS owns registration; modem IMS configuration prohibited");
      break;
    }
    if (ims.is_ims && !ims_bearer_allowed()) {
      IMS_LOG("IMS start held: modem registration %d; audit required\n", ims_audit.registration);
      bb_answer_error(r.channel, r.service, r.cid, cbd_rd16(sdu + 1), cbd_rd16(sdu + 3), 0x5au,
                      "modem IMS registration audit not idle");
      break;
    }
    ims_note("tx", r, sdu, n);
    // Apple's report configuration this firmware does not know (coachz_bb_data.h): accepted here, not forwarded.
    if (cbd_vendor_reporting(r.service, sdu, n)) {
      const uint32_t m = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
      if (m) bb_push(channel, bb_out, m);
      if (bb_req_lines++ < BB_REQ_LINES)
        BB_LOG("req svc 0x%x msg 0x%x txn %u cid %u answered here: success (vendor reporting)\n", r.service, cbd_rd16(sdu + 3),
               cbd_rd16(sdu + 1), r.cid);
      break;
    }
    if (r.service == CBD_SVC_WDS) bb_pdp_note(r.local_port, r.cid, r.channel, sdu, n);
    if (r.service == BB_SVC_WDA) bb_wda_request(sdu, n);
    bb_sim_request(r, sdu);
    // A bind naming only a mux (v23: WDS 0xa2 and QOS 0x2b, answered QMI_ERR_INVALID_ARG 0x30) gets the X55's data port.
    const struct cbd_fix f = cbd_endpoint_fix(r.service, sdu, n, bb_fix, sizeof bb_fix, CBD_EP_PCIE, CBD_EP_IP_HW0);
    if (f.len) {
      if (bb_endpoint_fixes++ < 16)
        BB_LOG("data port: service %u message 0x%x endpoint %u/%u %s %u/%u\n", r.service, cbd_rd16(sdu + 3), f.before_type,
               f.before_interface, f.inserted ? "added as" : "now", CBD_EP_PCIE, CBD_EP_IP_HW0);
      sdu = bb_fix;
      n = f.len;
    } else if (n <= sizeof bb_fix) {
      memcpy(bb_fix, sdu, n);
      if (const uint32_t mode = ubp_dms_mode_fix(r.service, bb_fix, n)) {
        BB_LOG("DMS operating mode %u (%s) sent as low power (1): the X55 is not restarted here\n", mode,
               mode == UBP_MODE_RESET ? "reset" : "shutting down");
        sdu = bb_fix;
      } else if (bb_cbn_apn_fix(r.service, bb_fix, n)) {
        sdu = bb_fix;
      }
    }
    bb_wds_tlv_log(r.service, sdu, n);
    // On dual-SIM X55M with physical slot 2 switched to logical slot 1 (Primary GW provisioned),
    // the modem's EFS default data subscription may still point to subscription 2, causing
    // CM_ACL_FAILURE (verbose 3/1042) on every single-SIM CommCenter WDS Start Network unless
    // each WDS client binds Subscription 1 (PRIMARY) via WDS Bind Subscription (0x00af).
    static const uint8_t wds_bind_sub1[14] = {
      0x00, 0xaf, 0xfe, 0xaf, 0x00, 0x07, 0x00,
      0x01, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    bool wds_bind_after = false;
    if (r.service == CBD_SVC_WDS && r.local_port >= CBQ_PORT_BASE) {
      const uint32_t slot = r.local_port - CBQ_PORT_BASE;
      const uint32_t msg = cbd_sdu_ok(sdu, n) && sdu[0] == 0u ? cbd_rd16(sdu + 3) : 0u;
      if (slot < CBQ_MAX_CLIENTS) {
        if (msg == 0x00afu) {
          bb_wds_sub_bound[slot] = 1;
        } else if (msg == 0x00a2u) {
          // Bind Subscription 1 immediately AFTER Bind Mux Data Port (matches qcrild order: 0x00a2 -> 0x00af -> 0x004d -> 0x0020).
          wds_bind_after = true;
        } else if (!bb_wds_sub_bound[slot]) {
          if (bb_queue_tx(r.local_port, r.node, r.port, wds_bind_sub1, sizeof wds_bind_sub1))
            bb_wds_sub_bound[slot] = 1;
        }
      }
    }
    bb_forward(r, sdu, n);
    if (wds_bind_after) {
      if (bb_queue_tx(r.local_port, r.node, r.port, wds_bind_sub1, sizeof wds_bind_sub1))
        bb_wds_sub_bound[r.local_port - CBQ_PORT_BASE] = 1;
    }
    break;
  }
  case CBQ_PRIVATE: {
    const uint8_t *sdu = data + CBQ_HDR;
    if (bb_private_answer(channel, r, sdu)) break;
    if (ims_audio_answer(channel, r, sdu)) break;   // AUDIO 0xe1 ARI CallEvent: media init's first gate
    if (ims_rtp_answer(channel, r, sdu)) break;   // IMSRTP 0x28 stub while the IMS switch is on
    ims_note_private(r.service, (uint32_t)(sdu[3] | sdu[4] << 8));
    bb_private_note(r.service, (uint32_t)(sdu[3] | sdu[4] << 8));
    const uint32_t n = cbq_private_unsupported(sdu, r.service, r.cid, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    break;
  }
  case CBQ_DROP:
    bb_answer_unrouted(qmux, data, len);
    break;
  }
}

// ------------------------------------------------------------------ asynchronous completions
// IOUserClient::sendAsyncResult64 is a protected static; this never-instantiated subclass only forwards to it.
class UmiBasebandAsync : public IOUserClient {
public:
  static IOReturn send(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *args, UInt32 count)
  {
    return sendAsyncResult64(reference, result, args, count);
  }
};

static void bb_finish_read(bb_read *read, IOReturn result, const uint8_t *data, uint32_t len)
{
  io_user_reference_t args[1] = { 0 };
  if (data && read->memory) {
    const uint32_t n = len < read->len ? len : read->len;
    args[0] = read->memory->writeBytes(0, data, n);
    if (len > read->len) result = kIOReturnOverrun;
  }
  if (read->memory) {
    read->memory->complete();
    read->memory->release();
    read->memory = nullptr;
  }
  UmiBasebandAsync::send(read->reference, result, args, 1);
}

static void bb_complete(thread_call_param_t, thread_call_param_t)
{
  for (;;) {
    bb_read read;
    bb_frame frame = { 0, nullptr };
    bool found = false;
    IOLockLock(bb_lock);
    for (uint32_t c = 0; c < CBH_PUBLISHED && !found; c++) {
      bb_channel *ch = &bb_ch[c];
      if (!ch->reads_waiting || !ch->count) continue;
      read = ch->reads[0];
      memmove(&ch->reads[0], &ch->reads[1], (--ch->reads_waiting) * sizeof ch->reads[0]);
      frame = bb_pop(c);
      found = true;
    }
    IOLockUnlock(bb_lock);
    if (!found) return;
    bb_finish_read(&read, kIOReturnSuccess, frame.data, frame.len);
    IOFreeData(frame.data, frame.len);
  }
}

// Every read a client has waiting ends with `result` (close, abort).
static void bb_abort_reads(uint32_t channel, void *owner, IOReturn result)
{
  bb_read ended[BB_READS];
  uint32_t n = 0;
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[channel];
  for (uint32_t i = 0; i < ch->reads_waiting;) {
    if (!owner || ch->reads[i].owner == owner) {
      ended[n++] = ch->reads[i];
      memmove(&ch->reads[i], &ch->reads[i + 1], (--ch->reads_waiting - i) * sizeof ch->reads[0]);
    } else {
      i++;
    }
  }
  IOLockWakeup(bb_lock, ch, false);
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) bb_finish_read(&ended[i], result, nullptr, 0);
}

// ------------------------------------------------------------------ user memory
static uint8_t *bb_copy_in(task_t task, mach_vm_address_t address, uint32_t len)
{
  if (!len || len > BB_FRAME_MAX) return nullptr;
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, len, kIODirectionOut, task);
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(len));
  bool ok = memory && copy && memory->prepare() == kIOReturnSuccess;
  if (ok) {
    ok = memory->readBytes(0, copy, len) == len;
    memory->complete();
  }
  OSSafeReleaseNULL(memory);
  if (!ok && copy) {
    IOFreeData(copy, len);
    copy = nullptr;
  }
  return copy;
}

static bool bb_async(IOExternalMethodArguments *args) { return args->asyncWakePort != MACH_PORT_NULL && args->asyncReference; }

static void bb_reply(IOExternalMethodArguments *args, IOReturn result, uint32_t count, uint64_t a = 0, uint64_t b = 0)
{
  io_user_reference_t values[2] = { a, b };
  OSAsyncReference64 reference;
  memset(reference, 0, sizeof reference);
  memcpy(reference, args->asyncReference, sizeof(io_user_reference_t) *
         (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
  UmiBasebandAsync::send(reference, result, values, count);
}

// ------------------------------------------------------------------ AppleBasebandPCIInterface
class AppleBasebandPCIUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIUserClient)
  task_t fTask;
  uint32_t fChannel;
public:
  bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
  {
    fTask = task;
    return IOUserClient::initWithTask(task, security, type, properties);
  }
  void setChannel(uint32_t channel) { fChannel = channel; }
  IOReturn clientClose() override
  {
    closeChannel();
    terminate();
    return kIOReturnSuccess;
  }
  void closeChannel()
  {
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    IOLockLock(bb_lock);
    if (bb_ch[fChannel].owner == this) bb_ch[fChannel].owner = nullptr;
    IOLockUnlock(bb_lock);
  }
  IOReturn write(IOExternalMethodArguments *args);
  IOReturn read(IOExternalMethodArguments *args);
  IOReturn readRegister(IOExternalMethodArguments *args);
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIUserClient, IOUserClient)

IOReturn AppleBasebandPCIUserClient::write(IOExternalMethodArguments *args)
{
  uint8_t *copy = nullptr;
  uint32_t len = 0;
  if (bb_async(args)) {
    if (args->scalarInputCount < 2) return kIOReturnBadArgument;
    len = (uint32_t)args->scalarInput[1];
    copy = bb_copy_in(fTask, args->scalarInput[0], len);
  } else if (args->structureInputDescriptor) {
    IOMemoryDescriptor *memory = args->structureInputDescriptor;
    len = (uint32_t)memory->getLength();
    copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    bool ok = copy && memory->prepare() == kIOReturnSuccess;
    if (ok) {
      ok = memory->readBytes(0, copy, len) == len;
      memory->complete();
    }
    if (!ok && copy) {
      IOFreeData(copy, len);
      copy = nullptr;
    }
  } else if (args->structureInput && args->structureInputSize) {
    len = args->structureInputSize;
    copy = len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    if (copy) memcpy(copy, args->structureInput, len);
  }
  if (!copy) return kIOReturnBadArgument;
  // umi_bb_poll samples the switch files at most once a second; a CTL request that allocates a
  // client or lists services (0x28 is private only while an IMS owner is on) samples them again
  // here, on this thread: VFS outside bb_lock, never on the modem thread (v31 §5.2).
  if (ims_ctl_wants_switch(copy, len)) ims_sample_switch();
  IOLockLock(bb_lock);
  bb_host_write(fChannel, copy, len);
  const bool to_modem = bb_tx_count != 0;
  IOLockUnlock(bb_lock);
  IOFreeData(copy, len);
  if (to_modem) umi_modem_wake();
  if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 1, len);
  return kIOReturnSuccess;
}

IOReturn AppleBasebandPCIUserClient::read(IOExternalMethodArguments *args)
{
  if (bb_async(args)) {
    if (args->scalarInputCount < 2 || !args->scalarInput[1]) return kIOReturnBadArgument;
    bb_read r;
    memset(&r, 0, sizeof r);
    memcpy(r.reference, args->asyncReference, sizeof(io_user_reference_t) *
           (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
    r.len = (uint32_t)args->scalarInput[1];
    r.owner = this;
    r.memory = IOMemoryDescriptor::withAddressRange(args->scalarInput[0], r.len, kIODirectionIn, fTask);
    if (!r.memory || r.memory->prepare() != kIOReturnSuccess) {
      OSSafeReleaseNULL(r.memory);
      return kIOReturnVMError;
    }
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool queued = ch->reads_waiting < BB_READS;
    if (queued) ch->reads[ch->reads_waiting++] = r;
    if (queued && ch->count && bb_complete_call) thread_call_enter(bb_complete_call);
    IOLockUnlock(bb_lock);
    if (!queued) {
      r.memory->complete();
      r.memory->release();
      return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
  }
  // synchronous: the next frame, waiting up to BB_SYNC_READ_SECONDS
  uint64_t deadline = 0;
  clock_interval_to_deadline(BB_SYNC_READ_SECONDS, kSecondScale, &deadline);
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[fChannel];
  int waited = THREAD_AWAKENED;
  while (!ch->count && ch->owner == this && waited == THREAD_AWAKENED)
    waited = IOLockSleepDeadline(bb_lock, ch, deadline, THREAD_INTERRUPTIBLE);
  bb_frame frame = bb_pop(fChannel);
  IOLockUnlock(bb_lock);
  if (!frame.data) return waited == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
  IOReturn result = kIOReturnSuccess;
  if (args->structureOutputDescriptor) {
    IOMemoryDescriptor *memory = args->structureOutputDescriptor;
    const uint32_t room = (uint32_t)memory->getLength(), n = frame.len < room ? frame.len : room;
    if (memory->prepare() == kIOReturnSuccess) {
      memory->writeBytes(0, frame.data, n);
      memory->complete();
      args->structureOutputDescriptorSize = n;
    } else {
      result = kIOReturnVMError;
    }
    if (frame.len > room) result = kIOReturnOverrun;
  } else if (args->structureOutput) {
    const uint32_t n = frame.len < args->structureOutputSize ? frame.len : args->structureOutputSize;
    memcpy(args->structureOutput, frame.data, n);
    if (frame.len > args->structureOutputSize) result = kIOReturnOverrun;
    args->structureOutputSize = n;
  } else {
    result = kIOReturnBadArgument;
  }
  IOFreeData(frame.data, frame.len);
  return result;
}

// readRegister (BHI): one scalar, the register, and a structure output of the size asked for (cbs_read_register).
IOReturn AppleBasebandPCIUserClient::readRegister(IOExternalMethodArguments *args)
{
  if (args->scalarInputCount < 1) return kIOReturnBadArgument;
  const uint32_t reg = (uint32_t)args->scalarInput[0];
  static uint32_t seen;
  if (reg < 32 && !(__atomic_fetch_or(&seen, 1u << reg, __ATOMIC_RELAXED) & (1u << reg)))
    BB_LOG("%s register %u read, %u bytes\n", cbh_published[fChannel], reg,
           args->structureOutputDescriptor ? (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize);
  if (args->structureOutput && args->structureOutputSize) {
    cbs_read_register(reg, static_cast<uint8_t *>(args->structureOutput), args->structureOutputSize);
    return kIOReturnSuccess;
  }
  if (IOMemoryDescriptor *memory = args->structureOutputDescriptor) {
    const uint32_t size = (uint32_t)memory->getLength();
    if (!size || size > BB_FRAME_MAX) return kIOReturnBadArgument;
    uint8_t *data = static_cast<uint8_t *>(IOMallocData(size));
    if (!data) return kIOReturnNoMemory;
    cbs_read_register(reg, data, size);
    IOReturn result = memory->prepare();
    if (result == kIOReturnSuccess) {
      memory->writeBytes(0, data, size);
      memory->complete();
      args->structureOutputDescriptorSize = size;
    }
    IOFreeData(data, size);
    return result;
  }
  return kIOReturnBadArgument;
}

// Selectors and shapes: coachz_bb_channels.h (libIOABP's IOConnectCall* sites; 20F66's channel table checked identical).
IOReturn AppleBasebandPCIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                                    OSObject *, void *)
{
  if (!args) return kIOReturnBadArgument;
  switch (selector) {
  case 0: {   // open
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    const bool free_channel = !ch->owner || ch->owner == this;
    if (free_channel) {
      ch->owner = this;
      if (!ch->opens++) BB_LOG("%s opened\n", cbh_published[fChannel]);
      if (fChannel == CBH_SAHARA && bb_boot_image_sent) {
        bb_boot_image_sent = false;
        if (bb_modem_ready) bb_sahara_restart();
        else {
          bb_hello_wanted = true;
          BB_LOG("sahara opened before the X55's QMI services: hello held until they are up\n");
        }
      }
    }
    IOLockUnlock(bb_lock);
    return free_channel ? kIOReturnSuccess : kIOReturnExclusiveAccess;
  }
  case 1:     // close
    closeChannel();
    return kIOReturnSuccess;
  case 2:
    return write(args);
  case 3:
    return read(args);
  case 4:     // sendImage (BHI): the X55 already runs its own firmware; the image is accepted and dropped
    BB_LOG("%s sendImage %llu bytes dropped\n", cbh_published[fChannel], args->scalarInputCount > 1 ? args->scalarInput[1] : 0ull);
    IOLockLock(bb_lock);
    if (fChannel == CBH_PBL) bb_boot_image_sent = true;
    IOLockUnlock(bb_lock);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 2, args->scalarInputCount > 1 ? args->scalarInput[1] : 0, 0);
    return kIOReturnSuccess;
  case 5:
    return readRegister(args);
  case 6:     // abort channel: pending reads end
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 7:     // start channel
  case 9:     // map shared memory: nothing here reads it
  case 10:    // unmap
  case 12:    // deregister time event
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 11:    // register time event: none is ever sent
    return kIOReturnSuccess;
  default:    // 8 time sync: no time base behind this channel
    BB_LOG("channel %s selector %u unsupported\n", cbh_published[fChannel], selector);
    return kIOReturnUnsupported;
  }
}

template <class Client>
static IOReturn bb_open(IOService *service, task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out)
{
  if (!out) return kIOReturnBadArgument;
  *out = nullptr;
  Client *client = OSTypeAlloc(Client);
  if (!client) return kIOReturnNoMemory;
  if (!client->initWithTask(task, security, type, properties) || !client->attach(service)) {
    client->release();
    return kIOReturnError;
  }
  if (!client->start(service)) {
    client->detach(service);
    client->release();
    return kIOReturnError;
  }
  *out = client;
  return kIOReturnSuccess;
}

class AppleBasebandPCIInterface : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIInterface)
  uint32_t fChannel;
public:
  bool setup(uint32_t channel)
  {
    fChannel = channel;
    return setProperty(CBH_PROTOCOL_KEY, cbh_published[channel]);
  }
  // libIOABP puts ABPInterfaceProtocol straight in the IOServiceMatching dictionary. IOKit does not compare a key it does not
  // know: the family's matchPropertyTable does, and without it every client gets whichever channel comes first.
  bool matchPropertyTable(OSDictionary *table, SInt32 *score) override
  {
    return IOService::matchPropertyTable(table, score) && matchProtocol(table);
  }
  bool matchPropertyTable(OSDictionary *table) override
  {
    return IOService::matchPropertyTable(table) && matchProtocol(table);
  }
  bool matchProtocol(OSDictionary *table) const
  {
    OSString *wanted = OSDynamicCast(OSString, table->getObject(CBH_PROTOCOL_KEY));
    return !wanted || wanted->isEqualTo(cbh_published[fChannel]);
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) override
  {
    static uint32_t seen;
    if (!(__atomic_fetch_or(&seen, 1u << fChannel, __ATOMIC_RELAXED) & (1u << fChannel)))
      BB_LOG("first user client on %s\n", cbh_published[fChannel]);
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    AppleBasebandPCIUserClient *client = OSTypeAlloc(AppleBasebandPCIUserClient);
    if (!client) return kIOReturnNoMemory;
    client->setChannel(fChannel);
    if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
      client->release();
      return kIOReturnError;
    }
    if (!client->start(this)) {
      client->detach(this);
      client->release();
      return kIOReturnError;
    }
    *out = client;
    return kIOReturnSuccess;
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **out) override
  {
    return newUserClient(task, security, type, nullptr, out);
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, IOService)

// ------------------------------------------------------------------ AppleBasebandPCIControl
class AppleBasebandPCIControlUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIControlUserClient)
  OSAsyncReference64 fLogs;
  bool fLogsPending;
public:
  void endLogs()
  {
    if (!fLogsPending) return;
    fLogsPending = false;
    io_user_reference_t none[1] = { 0 };
    UmiBasebandAsync::send(fLogs, kIOReturnAborted, none, 1);
  }
  IOReturn clientClose() override
  {
    endLogs();
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    switch (selector) {
    case 0:   // open
    case 3:   // logger tune
      return kIOReturnSuccess;
    case 1:   // close
      endLogs();
      return kIOReturnSuccess;
    case 2:   // read logs: there are none; the request waits until close
      if (!bb_async(args) || fLogsPending) return kIOReturnBadArgument;
      memset(fLogs, 0, sizeof fLogs);
      memcpy(fLogs, args->asyncReference, sizeof(io_user_reference_t) *
             (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
      fLogsPending = true;
      return kIOReturnSuccess;
    default:
      return kIOReturnUnsupported;
    }
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControlUserClient, IOUserClient)

class AppleBasebandPCIControl : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIControl)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) BB_LOG("first user client on AppleBasebandPCIControl\n");
    return bb_open<AppleBasebandPCIControlUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControl, IOService)

// ------------------------------------------------------------------ AppleBaseband (board methods, coachz_bb_control.h)
// A baseband reset ends every data call. The X55 is not reset, so the calls ABM started are stopped here (WDS Stop Network with
// the handle its Start Network got), or after the reset ABM's Start Network joins the old call, no Packet Service Status
// indication follows and its context stays activating until CommCenter resets the baseband again (v25, 13:00:03 -> 13:06:03).
// The answers come back to pooled ports and are dropped. bb_lock held.
static uint32_t bb_reset_calls(void)
{
  const struct cbq_server *wds = cbq_server_find(&bb_qmux, CBD_SVC_WDS);
  uint32_t stopped = 0;
  for (uint32_t i = 0; wds && i < CBQ_MAX_CLIENTS; i++) {
    const struct cbq_client &c = bb_qmux.client[i];
    const struct cbp_client &p = bb_pdp_clients[i];
    if (!c.used || c.channel >= CBQ_CHANNELS || c.service != CBD_SVC_WDS || !p.active || !p.handle ||
        bb_pdp_owners[i] != 1u + c.cid + (uint32_t(c.channel) << 8)) continue;
    uint8_t sdu[14], handle[4];
    cbd_wr32(handle, p.handle);
    const uint32_t n = cbd_request(sdu, 0xff00u + i, CBP_STOP, cbd_tlv(sdu + 7, sizeof sdu - 7, 0x01, handle, 4));
    if (bb_queue_tx(CBQ_PORT_BASE + i, wds->node, wds->port, sdu, n)) stopped++;
  }
  if (stopped) umi_modem_wake();
  return stopped;
}

class AppleBasebandUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    uint64_t out[4] = { 0 };
    uint32_t nout = args->scalarOutputCount < 4 ? args->scalarOutputCount : 4;
    IOLockLock(bb_lock);
    const int result = cbc_call(&bb_control, selector, args->scalarInput, args->scalarInputCount, out, &nout);
    // A reset or power-on goes back to before sendImage: nothing greets until the next one. When the transport is up the
    // X55 itself is untouched, but to ABM the baseband restarts: its QMI clients are gone (DEL_CLIENT to the X55 from
    // umi_bb_poll), what was queued for them is dropped, and activation waits for its next registration. When it is not up
    // (umi_modem.c stopped it after a fault) this call is what starts the real bring-up again, and the log below says so.
    if (!result && (selector == CBC_RESET_MODEM || selector == CBC_POWERCYCLE || (selector == CBC_POWER && args->scalarInputCount && args->scalarInput[0]))) {
      bb_boot_image_sent = false;
      bb_hello_wanted = false;
      bb_flush(CBH_SAHARA);
      bb_flush(CBH_QMUX1);
      bb_flush(CBH_QMUX2);
      bb_act_known = 0;
      const uint32_t stopped = bb_reset_calls();
      for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++)
        if (bb_qmux.client[i].used && bb_qmux.client[i].channel < CBQ_CHANNELS) ubp_forget(&bb_pending, CBQ_PORT_BASE + i);
      const uint32_t freed = cbq_reset_host_clients(&bb_qmux);
      cellinfo_prune();
      // Report what is really there: only a live link may be called untouched. umi_modem.c holds PERST after a fault and
      // restarts the X55 itself, so a reset ABM asks for while the transport is dead is what starts (or retries) that
      // bring-up. Refused: kill switch off, the per-boot cap used up, or no service thread left.
      const int transport = umi_modem_state();
      const uint32_t restart = transport == 1 ? 0u : (uint32_t)umi_modem_restart();
      if (transport == 1)
        BB_LOG("AppleBaseband selector %u (reset or power) recorded; the X55 keeps running; %u QMI clients of ABM pooled, %u data calls stopped, %u restarts so far\n",
               selector, freed, stopped, umi_modem_restarts());
      else
        BB_LOG("AppleBaseband selector %u (reset or power) recorded; the X55 is NOT running; full bring-up %s; %u QMI clients of ABM pooled, %u data calls stopped, %u restarts so far\n",
               selector, restart ? "requested" : transport ? "refused (disabled or per-boot limit)" : "impossible (no service thread)",
               freed, stopped, umi_modem_restarts());
    }
    IOLockUnlock(bb_lock);
    if (result == CBC_UNSUPPORTED) BB_LOG("AppleBaseband selector %u unsupported\n", selector);
    // Every board call while ABM boots: what it asked and what it was told (scalars only).
    static uint32_t logged;
    if (logged++ < 64)
      BB_LOG("board call %u: selector %u in %u [0x%llx 0x%llx] -> 0x%x out %u [0x%llx 0x%llx]\n", logged, selector,
             args->scalarInputCount, args->scalarInputCount > 0 ? args->scalarInput[0] : 0ull,
             args->scalarInputCount > 1 ? args->scalarInput[1] : 0ull, result, nout, nout > 0 ? out[0] : 0ull, nout > 1 ? out[1] : 0ull);
    for (uint32_t i = 0; i < nout; i++) args->scalarOutput[i] = out[i];
    args->scalarOutputCount = nout;
    return result;
  }
};
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient)

class AppleBaseband : public IOService {
  OSDeclareDefaultStructors(AppleBaseband)
public:
  // libTelephonyBaseband sets StartDevice / StopDevice (booleans) on the "baseband" service to power the modem: recorded;
  // the X55 is umi_modem.c's. Anything else is named in the log and refused.
  IOReturn setProperties(OSObject *properties) override
  {
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) return kIOReturnBadArgument;
    IOReturn result = kIOReturnSuccess;
    OSCollectionIterator *keys = OSCollectionIterator::withCollection(dict);
    while (OSSymbol *key = keys ? OSDynamicCast(OSSymbol, keys->getNextObject()) : nullptr) {
      const bool known = key->isEqualTo("StartDevice") || key->isEqualTo("StopDevice");
      BB_LOG("AppleBaseband property %s = %s %s\n", key->getCStringNoCopy(), dict->getObject(key) == kOSBooleanTrue ? "true" : "other",
             known ? "taken" : "refused");
      if (!known) result = kIOReturnUnsupported;
    }
    OSSafeReleaseNULL(keys);
    return result;
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) BB_LOG("first user client on AppleBaseband\n");
    return bb_open<AppleBasebandUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBaseband, IOService)

// ------------------------------------------------------------------ pdp_ip0..3: QMAP muxes 1..4 of IP_HW0
static errno_t bb_pdp_demux(ifnet_t, mbuf_t m, char *, protocol_family_t *family)
{
  uint8_t first;
  if (mbuf_copydata(m, 0, 1, &first)) return ENOENT;
  *family = first >> 4 == 4 ? PF_INET : first >> 4 == 6 ? PF_INET6 : 0;
  return *family ? 0 : EPROTONOSUPPORT;
}
static errno_t bb_pdp_add_proto(ifnet_t, protocol_family_t f, const struct ifnet_demux_desc *, uint32_t)
{
  return f == PF_INET || f == PF_INET6 ? 0 : EPROTONOSUPPORT;
}
static errno_t bb_pdp_del_proto(ifnet_t ifp, protocol_family_t family)
{
  BB_LOG("pdp: unplumb unit %u family %u\n", ifnet_unit(ifp), family);
  return 0;
}
static errno_t bb_pdp_input(ifnet_t ifp, protocol_family_t f, mbuf_t m, char *)
{
  if (proto_input(f, m)) { mbuf_freem(m); ifnet_stat_increment_in(ifp, 0, 0, 1); }
  return 0;
}
static errno_t bb_pdp_pre_output(ifnet_t, protocol_family_t, mbuf_t *, const struct sockaddr *, void *, char *, char *)
{
  return 0;  // raw IP: no link header
}
// Plumbed again after CommCenter tears an IP service down (in_domifattach/proto_plumb): utun_attach_proto's contract.
static errno_t bb_pdp_plumb(ifnet_t ifp, protocol_family_t family)
{
  struct ifnet_attach_proto_param proto = {};
  proto.input = bb_pdp_input;
  proto.pre_output = bb_pdp_pre_output;
  const errno_t error = ifnet_attach_protocol(ifp, family, &proto);
  BB_LOG("pdp: plumb unit %u family %u result %d\n", ifnet_unit(ifp), family, error);
  return error;
}
static errno_t bb_pdp_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
  const errno_t ims_error = ims_pdp_ioctl(ifp, cmd, data);
  if (ims_error != -1) {
    const bool address = cmd == SIOCAIFADDR || cmd == SIOCAIFADDR_IN6 || cmd == SIOCSIFADDR || cmd == SIOCSIFADDR_IN6;
    if (ims_enabled && address)
      IMS_LOG("address ioctl 0x%lx result %d; DAD delegated to XNU\n", cmd, ims_error);
    // Diagnostic only: XNU calls this ioctl only for an interface's first address, so the IMS unit
    // never sees one. Arming is driven by the WDS 0x2d reply (see umi_ims.inc).
    if (address && !ims_error) ims_address_installed(ifnet_unit(ifp));
    return ims_error;
  }
  switch (cmd) {
  case SIOCSIFMTU: {
    const int mtu = static_cast<struct ifreq *>(data)->ifr_mtu;
    if (mtu < 576 || mtu > 1500) return EINVAL;
    return ifnet_set_mtu(ifp, static_cast<uint32_t>(mtu));
  }
  case SIOCSIFFLAGS: case SIOCSIFADDR: case SIOCSIFDSTADDR: case SIOCAIFADDR:
    return 0;
  case SIOCGIFMEDIA32: case SIOCGIFMEDIA64:   // configd's media query: a cellular link has none
    return EOPNOTSUPP;
  default: {
    static unsigned long seen[16];
    static uint32_t count;
    bool known = false;
    for (uint32_t i = 0; i < count && !known; i++) known = seen[i] == cmd;
    if (!known && count < 16) {
      seen[count++] = cmd;
      BB_LOG("pdp: ioctl 0x%lx unit %u unsupported\n", cmd, ifnet_unit(ifp));
    }
    return EOPNOTSUPP;
  }
  }
}
static errno_t bb_pdp_output(ifnet_t ifp, mbuf_t m)
{
  auto *p = static_cast<bb_pdp_if *>(ifnet_softc(ifp));
  const size_t len = mbuf_pkthdr_len(m);
  uint8_t first = 0;
  if (!len || len > ifnet_mtu(ifp) || mbuf_copydata(m, 0, 1, &first)) { mbuf_freem(m); return EMSGSIZE; }
  const uint32_t bit = cbp_family_bit(first >> 4);
  IOLockLock(bb_pdp_lock);
  if (!bit || !(p->families & bit) || p->blocked || p->queued == BB_PDP_QUEUE) {
    p->dropped++;
    IOLockUnlock(bb_pdp_lock);
    mbuf_freem(m);
    return ENETDOWN;
  }
  mbuf_setnextpkt(m, nullptr);
  if (p->tail) mbuf_setnextpkt(p->tail, m); else p->head = m;
  p->tail = m;
  p->queued++;
  IOLockUnlock(bb_pdp_lock);
  umi_modem_wake();
  return 0;
}

static void bb_pdp_init(void)
{
  const errno_t ipv4 = proto_register_plumber(PF_INET, IFNET_FAMILY_CELLULAR, bb_pdp_plumb, nullptr);
  const errno_t ipv6 = proto_register_plumber(PF_INET6, IFNET_FAMILY_CELLULAR, bb_pdp_plumb, nullptr);
  BB_LOG("pdp: protocol plumbers IPv4 %d IPv6 %d\n", ipv4, ipv6);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    bb_pdp_if *p = &bb_pdp_ifaces[i];
    struct ifnet_init_eparams init = {};
    init.ver = IFNET_INIT_CURRENT_VERSION;
    init.len = sizeof init;
    init.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    init.name = "pdp_ip";
    init.unit = i;
    init.family = IFNET_FAMILY_CELLULAR;
    init.type = IFT_CELLULAR;
    init.softc = p;
    init.output = bb_pdp_output;
    init.demux = bb_pdp_demux;
    init.add_proto = bb_pdp_add_proto;
    init.del_proto = bb_pdp_del_proto;
    init.ioctl = bb_pdp_ioctl;
    errno_t error = ifnet_allocate_extended(&init, &p->ifp);
    if (!error) {
      ifnet_set_hdrlen(p->ifp, 0);
      ifnet_set_addrlen(p->ifp, 0);
      ifnet_set_mtu(p->ifp, 1500);
      ifnet_set_flags(p->ifp, IFF_POINTOPOINT | IFF_MULTICAST, 0xffff);
      error = ifnet_attach(p->ifp, nullptr);
      if (error) { ifnet_release(p->ifp); p->ifp = nullptr; }
    }
    if (!error) {
      error = bb_pdp_plumb(p->ifp, PF_INET);
      if (!error || error == EEXIST) error = bb_pdp_plumb(p->ifp, PF_INET6);
    }
    BB_LOG("pdp: pdp_ip%u mux %u registered result %d (down until CommCenter starts a context on it)\n", i, i + 1u, error);
  }
}

// Which families each mux carries, from WDS (bb_pdp_clients) and the link format. Modem thread; takes both locks in turn.
static void bb_pdp_update(void)
{
  uint32_t active[CBP_INTERFACES];
  IOLockLock(bb_lock);
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) {
    if (!bb_qmux.client[i].used || bb_qmux.client[i].service != CBD_SVC_WDS ||
        bb_pdp_owners[i] != (1u + bb_qmux.client[i].cid + (uint32_t(bb_qmux.client[i].channel) << 8)))
      bzero(&bb_pdp_clients[i], sizeof bb_pdp_clients[i]);
  }
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) active[i] = bb_link_ok ? cbp_active_families(bb_pdp_clients, CBQ_MAX_CLIENTS, i + 1u) : 0;
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    bb_pdp_if *p = &bb_pdp_ifaces[i];
    if (!p->ifp) continue;
    IOLockLock(bb_pdp_lock);
    const bool changed = p->families != active[i];
    p->families = active[i];
    IOLockUnlock(bb_pdp_lock);
    if (changed) {
      ifnet_set_flags(p->ifp, active[i] ? IFF_RUNNING : 0, IFF_RUNNING);
      BB_LOG("pdp: pdp_ip%u active families %u (1 IPv4, 2 IPv6)\n", i, active[i]);
    }
  }
}

static IOReturn bb_pdp_block(uint32_t unit, uint32_t blocked)
{
  if (unit >= CBP_INTERFACES || !bb_pdp_lock) return kIOReturnBadArgument;
  IOLockLock(bb_pdp_lock);
  bb_pdp_ifaces[unit].blocked = !!blocked;
  IOLockUnlock(bb_pdp_lock);
  BB_LOG("pdp: pdp_ip%u traffic %s by CommCenter\n", unit, blocked ? "blocked" : "allowed");
  return kIOReturnSuccess;
}

extern "C" int umi_bb_data_input(uint32_t mux, const uint8_t *ip, uint32_t n)
{
  if (!bb_pdp_lock || !cbp_mux_ok(mux) || !n) return 0;
  const uint32_t version = ip[0] >> 4;
  bb_pdp_if *p = &bb_pdp_ifaces[mux - 1u];
  IOLockLock(bb_pdp_lock);
  const bool usable = p->ifp && (p->families & cbp_family_bit(version)) && !p->blocked;
  if (!usable) p->dropped++;
  IOLockUnlock(bb_pdp_lock);
  if (!usable || !(ifnet_flags(p->ifp) & IFF_UP)) return 0;
  if (mux == 2u && imsrtp_route_active_fast) {
    static uint32_t rx_dbg;
    if (rx_dbg < 6u) {
      const uint8_t proto = (version == 6u && n >= 7u) ? ip[6] : ((version == 4u && n >= 10u) ? ip[9] : 0);
      uint16_t sp = 0, dp = 0;
      if (version == 6u && proto == 17u && n >= 48u) {
        sp = (uint16_t)(((uint16_t)ip[40] << 8) | ip[41]);
        dp = (uint16_t)(((uint16_t)ip[42] << 8) | ip[43]);
      }
      if (sp != 5060u && dp != 5060u && sp != 5061u && dp != 5061u) {
        rx_dbg++;
        char pay[40];
        uint32_t pos = 0, off = (version == 6u && proto == 17u) ? 48u : 40u;
        uint32_t avail = n > off ? (n - off) : 0u;
        if (avail > 16u) avail = 16u;
        for (uint32_t k = 0; k < avail && pos + 3u < sizeof pay; k++)
          pos += (uint32_t)snprintf(pay + pos, sizeof pay - pos, "%02x", ip[off + k]);
        pay[pos] = 0;
        BB_LOG("imsrtp-data: mux2 rx #%u v%u proto=%u n=%u %u->%u pay=%s\n", rx_dbg, version, proto, n, sp, dp, pay);
      }
    }
    if (imsrtp_rx_try_tap(version, ip, n)) {
      p->rx++;
      return 1;
    }
  }
  mbuf_t m = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, n, nullptr, &m) || !m) return 0;
  if (mbuf_copyback(m, 0, n, ip, MBUF_DONTWAIT)) { mbuf_freem(m); return 0; }
  mbuf_pkthdr_setrcvif(m, p->ifp);
  mbuf_pkthdr_setheader(m, nullptr);
  struct ifnet_stat_increment_param inc = {};
  inc.packets_in = 1;
  inc.bytes_in = n;
  if (ifnet_input(p->ifp, m, &inc)) return 0;   // ifnet_input frees the chain on error
  p->rx++;
  return 1;
}

extern "C" uint32_t umi_bb_data_take(uint8_t *mux, uint8_t *ip, uint32_t room)
{
  if (!bb_pdp_lock) return 0;
  for (uint32_t k = 0; k < CBP_INTERFACES; k++) {
    const uint32_t i = (bb_pdp_next + k) % CBP_INTERFACES;
    bb_pdp_if *p = &bb_pdp_ifaces[i];
    IOLockLock(bb_pdp_lock);
    mbuf_t m = p->head;
    if (m) { p->head = mbuf_nextpkt(m); if (!p->head) p->tail = nullptr; p->queued--; mbuf_setnextpkt(m, nullptr); }
    const uint32_t families = p->blocked ? 0 : p->families;
    IOLockUnlock(bb_pdp_lock);
    if (!m) continue;
    const uint32_t n = (uint32_t)mbuf_pkthdr_len(m);
    const bool ok = n <= room && !mbuf_copydata(m, 0, n, ip) && (families & cbp_family_bit(ip[0] >> 4)) && (ifnet_flags(p->ifp) & IFF_UP);
    mbuf_freem(m);
    if (!ok) {
      IOLockLock(bb_pdp_lock); p->dropped++; IOLockUnlock(bb_pdp_lock);
      ifnet_stat_increment_out(p->ifp, 0, 0, 1);
      continue;
    }
    ifnet_stat_increment_out(p->ifp, 1, n, 0);
    p->tx++;
    bb_pdp_next = i + 1u;
    *mux = (uint8_t)(i + 1u);
    return n;
  }
  return 0;
}

// ------------------------------------------------------------------ the data plane's kernel services
// ABM's data::TransportService calls ApplePDPHelperInterface::getInstance()->enableMetadataTrailer() without a null check, and
// getInstance is null without an AppleIPAppender service to open. ApplePDPHelper picks its variant by which PDP manager is
// registered; this is the Mav one. Every AppleIPAppender method (0 addFlow ... 20 registerFilterRuleCallBack) succeeds with
// zeroed outputs except 16, Block PDP traffic; the first use of each is logged with its shape (sizes only).
class AppleIPAppenderUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleIPAppenderUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    static uint32_t shapes_seen;
    if (selector < 32 && !(__atomic_fetch_or(&shapes_seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      BB_LOG("pdp: appender selector %u scalar-in %u scalar-out %u struct-in %u struct-out %u descriptor-in %llu descriptor-out %llu\n",
        selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize, args->structureOutputSize,
        static_cast<unsigned long long>(args->structureInputDescriptor ? args->structureInputDescriptor->getLength() : 0),
        static_cast<unsigned long long>(args->structureOutputDescriptor ? args->structureOutputDescriptor->getLength() : 0));
    if (selector == 16) {
      if (args->scalarInputCount != 2) return kIOReturnBadArgument;
      return bb_pdp_block(static_cast<uint32_t>(args->scalarInput[0]), static_cast<uint32_t>(args->scalarInput[1]));
    }
    for (uint32_t i = 0; i < args->scalarOutputCount; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    if (args->structureOutputDescriptor) args->structureOutputDescriptorSize = 0;
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, IOUserClient)

class AppleIPAppender : public IOService {
  OSDeclareDefaultStructors(AppleIPAppender)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    return bb_open<AppleIPAppenderUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleIPAppender, IOService)

// libPCITransport's pci::event::PDPListener matches the base class, ApplePDPHelper the leaf.
class AppleBasebandPCIPDPManagerBase : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManagerBase)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManagerBase, IOService)

class AppleBasebandPCIPDPManager : public AppleBasebandPCIPDPManagerBase {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManager, AppleBasebandPCIPDPManagerBase)

class AppleBasebandPCIMAVPDPManager : public AppleBasebandPCIPDPManager {
  OSDeclareDefaultStructors(AppleBasebandPCIMAVPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIMAVPDPManager, AppleBasebandPCIPDPManager)

// ------------------------------------------------------------------ publication and the modem thread's entry points
// `node`: a device-tree node whose name and properties the service takes, as a DT-backed nub would. libTelephonyBaseband
// finds the board by IOServiceNameMatching("baseband") and reads its "config" (_TelephonyBasebandControllerLoadConfigOnce).
template <class Service>
static Service *bb_publish(IOService *provider, uint32_t channel = ~0u, const char *node = nullptr)
{
  Service *service = OSTypeAlloc(Service);
  bool ok = service && service->init();
  if (ok && node) {
    IORegistryEntry *dt = IORegistryEntry::fromPath(node, gIODTPlane);
    OSDictionary *props = dt ? dt->dictionaryWithProperties() : nullptr;
    OSCollectionIterator *keys = props ? OSCollectionIterator::withCollection(props) : nullptr;
    while (OSSymbol *key = keys ? OSDynamicCast(OSSymbol, keys->getNextObject()) : nullptr) service->setProperty(key, props->getObject(key));
    BB_LOG("%s: %u properties from IODeviceTree:%s (config %s)\n", service->getMetaClass()->getClassName(),
           props ? props->getCount() : 0u, node, props && props->getObject("config") ? "present" : "MISSING");
    service->setName(dt ? dt->getName() : "baseband");
    OSSafeReleaseNULL(keys);
    OSSafeReleaseNULL(props);
    OSSafeReleaseNULL(dt);
  }
  if (ok && channel != ~0u) {
    AppleBasebandPCIInterface *interface = OSDynamicCast(AppleBasebandPCIInterface, service);
    ok = interface && interface->setup(channel);
  }
  ok = ok && service->attach(provider);
  if (ok && !service->start(provider)) {
    service->detach(provider);
    ok = false;
  }
  if (!ok) {
    OSSafeReleaseNULL(service);
    return nullptr;
  }
  service->registerService();
  return service;
}

extern "C" void umi_bb_publish(void)
{
  if (bb_published) return;
  bb_lock = IOLockAlloc();
  bb_pdp_lock = IOLockAlloc();
  bb_complete_call = thread_call_allocate(bb_complete, nullptr);
  if (!bb_lock || !bb_pdp_lock || !bb_complete_call) {
    BB_LOG("no memory for the bridge: nothing published\n");
    return;
  }
  cellinfo_switch_call = thread_call_allocate(cellinfo_check_switch, nullptr);
  voice_switch_call = thread_call_allocate(voice_check_switch, nullptr);
  ims_switch_call = thread_call_allocate(ims_check_switch, nullptr);
  ims_dad_call = thread_call_allocate(ims_dad_fire, nullptr);
  ims_dad_hold_call = thread_call_allocate(ims_dad_hold_fire, nullptr);
  ims_rtp_trip_call = thread_call_allocate(ims_rtp_trip_fire, nullptr);   /* IMSRTP 0x28: one round trip */
  if (!voice_switch_call) VOICE_LOG("switch worker unavailable; voice diagnostics disabled\n");
  if (!ims_switch_call) IMS_LOG("switch worker unavailable; IMS bearer changes disabled\n");
  if (!ims_dad_call) IMS_LOG("DAD indication worker unavailable; synthesized DAD complete disabled\n");
  if (!ims_dad_hold_call) IMS_LOG("DAD hold timer unavailable; the modem poll releases held replies\n");
  if (!ims_rtp_trip_call) IMS_LOG("IMSRTP worker unavailable; 0x28 answers are delivered immediately\n");
  int vops = 0;
  bb_vops_on = PE_parse_boot_argn("umi_vops", &vops, sizeof vops) && vops == 1;
  int rtpok = 0;   /* umi_rtpok=1: IMSRTP 0x28 session lifecycle answered success (umi_ims.inc) */
  ims_rtpok_on = PE_parse_boot_argn("umi_rtpok", &rtpok, sizeof rtpok) && rtpok == 1;
  cbq_init(&bb_qmux);
  cbc_init(&bb_control);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  bb_pdp_init();
  imsrtp_dev_init();
  IOService *provider = IOService::getResourceService();
  int made = 0;
  made += provider && bb_publish<AppleBaseband>(provider, ~0u, "/baseband");
  made += provider && bb_publish<AppleBasebandPCIControl>(provider);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) made += provider && bb_publish<AppleBasebandPCIInterface>(provider, c);
  made += provider && bb_publish<AppleBasebandPCIMAVPDPManager>(provider);
  made += provider && bb_publish<AppleIPAppender>(provider);
  bb_published = 1;
  BB_LOG("%d of %u services published (AppleBaseband, AppleBasebandPCIControl, %u channels, the Mav PDP manager, AppleIPAppender)\n",
         made, 4u + CBH_PUBLISHED, CBH_PUBLISHED);
}

extern "C" void umi_bb_modem_ready(void)
{
  if (!bb_published) return;
  IOLockLock(bb_lock);
  uint32_t servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  bb_modem_ready = true;
  bb_transport_lost = false;   // QMI answers again: ABM's next GetClientId must be served
  const bool hello = bb_hello_wanted;
  bb_hello_wanted = false;
  if (hello) bb_sahara_restart();
  IOLockUnlock(bb_lock);
  BB_LOG("X55 QMI is up (%u services known)%s\n", servers, hello ? ": the held sahara hello is sent" : "");
}

// The X55's transport died (umi_modem.c: MHI/SMMU fault, PERST held). Nothing the bridge holds for that link is valid:
// the requests already in flight are answered here, its QMI clients are pooled (their QRTR ports stay, as after a baseband
// reset), and the node-3 service table is emptied so the restarted modem's own NEW_SERVER announcements are the only ones
// CommCenter can see. Every request that comes after this is dropped with no reply at all (bb_host_write): an error answer
// would keep ABM's ATCS timer from expiring, and only its timeout makes CommCenter reset the baseband. The IMS audit,
// cellinfo and voice state drop their port so the next discovery re-binds them. Called on the modem thread before the
// restart runs; the modem thread is not in umi_bb_poll.
extern "C" void umi_bb_modem_lost(void)
{
  if (!bb_published) return;
  IOLockLock(bb_lock);
  bb_modem_ready = false;
  bb_transport_lost = true;    // from here on ABM's QMUX requests are dropped, not answered
  bb_link_ok = false;
  bb_act_known = 0;
  uint32_t answered = 0;
  for (uint32_t i = 0; i < UBP_MAX; i++) {
    struct ubp_entry &e = bb_pending.e[i];
    if (!e.used) continue;
    e.used = 0;
    answered++;
    bb_answer_error(e.channel, e.service, e.cid, e.txn, e.message, UBP_ERR_INTERNAL, "X55 link lost");
  }
  uint32_t queued = 0;
  while (bb_tx_count) {
    IOFreeData(bb_txq[bb_tx_head].data, bb_txq[bb_tx_head].len);
    bb_tx_head = (bb_tx_head + 1) % BB_TX;
    bb_tx_count--;
    queued++;
  }
  uint32_t servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  uint32_t freed = cbq_reset_host_clients(&bb_qmux);
  bzero(bb_qmux.server, sizeof bb_qmux.server);
  bzero(bb_pdp_clients, sizeof bb_pdp_clients);
  bzero(bb_pdp_owners, sizeof bb_pdp_owners);
  bzero(bb_wds_sub_bound, sizeof bb_wds_sub_bound);
  ims_audit.registration = -1; ims_audit.pending = false; ims_audit.bound = false;
  ims_audit.release = false; ims_audit.port = 0; ims_audit.next_at = bb_now_ms();
  ims_imss_chain = 0;   /* the 0x2a control-point registration died with the modem: redo the preamble */
  cellinfo_finish(3); cellinfo.pending = false; cellinfo.port = 0; cellinfo.sampled = false; cellinfo.release = false;
  voice.node = voice.port = 0; voice.pending = 0;
  IOLockUnlock(bb_lock);
  BB_LOG("X55 transport lost: %u QMI services and %u clients dropped, %u requests answered here, %u queued frames dropped\n",
         servers, freed, answered, queued);
  // The modem thread may end right after this (umi_mdm_restart=0): take pdp_ip0..3's IFF_RUNNING down now
  // instead of never. Outside bb_lock -- bb_pdp_update takes it and bb_pdp_lock itself, and sets ifnet flags.
  bb_pdp_update();
}

extern "C" void umi_bb_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
  if (!bb_published) return;
  IOLockLock(bb_lock);
  cbq_server_new(&bb_qmux, service, version_instance, node, port);
  IOLockUnlock(bb_lock);
}

extern "C" void umi_bb_del_server(uint32_t service, uint32_t node, uint32_t port)
{
  if (!bb_published) return;
  IOLockLock(bb_lock);
  cbq_server_del(&bb_qmux, service, node, port);
  if (service == UMI_IMS_IMSA && ims_audit.node == node && ims_audit.port == port) {
    ims_audit.registration = -1; ims_audit.pending = false; ims_audit.bound = false; ims_audit.indications = 0;
    ims_audit.release = true; ims_audit.port = 0;
    ims_audit.next_at = bb_now_ms();
  }
  if (service == UMI_IMS_IMSS && ims_imss.node == node && ims_imss.port == port) {
    ims_imss.pending = false; ims_imss.release = true; ims_imss.node = ims_imss.port = 0;
  }
  if (service == UCI_SERVICE && cellinfo.node == node && cellinfo.port == port) {
    cellinfo_finish(3); cellinfo.pending = false; cellinfo.port = 0; cellinfo.sampled = false; cellinfo.release = true;
  }
  if (service == CBD_SVC_WDS) { bzero(bb_pdp_clients, sizeof bb_pdp_clients); bzero(bb_wds_sub_bound, sizeof bb_wds_sub_bound); }
  IOLockUnlock(bb_lock);
  BB_LOG("X55 service %u left (node %u port %u)\n", service, node, port);
}

extern "C" int umi_bb_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size)
{
  if (dst_port == UMI_IMS_AUDIT_PORT) {
    if (bb_published) {
      IOLockLock(bb_lock);
      ims_receive(src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
    }
    return 1;
  }
  if (dst_port == UMI_IMS_IMSS_AUDIT_PORT) {
    if (bb_published) {
      IOLockLock(bb_lock);
      ims_imss_receive(src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
    }
    return 1;
  }
  // Route B probe: the host-side IMSDCM port (announced only with the switch on) and QMI CTL on
  // the host node, which is where the modem's 770 client id comes from. With the switch off
  // ims_dcm_receive() answers nothing, so an unhandled packet keeps the old unknown-port log.
  if (dst_port == UMI_IMS_DCM_PORT || dst_port == UMI_IMS_QRTR_CTRL) {
    if (bb_published) {
      IOLockLock(bb_lock);
      const int handled = ims_dcm_receive(dst_port, src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
      if (handled) return 1;
    }
    return 0;
  }
  if (dst_port == VOICE_PORT) {
    if (bb_published) {
      IOLockLock(bb_lock);
      voice_receive(src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
    }
    return 1;
  }
  if (dst_port == UCI_PORT) {
    if (bb_published) {
      IOLockLock(bb_lock);
      cellinfo_receive(src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
    }
    return 1;
  }
  if (dst_port == UGL_PORT) {
    if (bb_published) {
      IOLockLock(bb_lock);
      gnss_receive(src_node, src_port, static_cast<const uint8_t *>(body), size);
      IOLockUnlock(bb_lock);
    }
    return 1;
  }
  if (dst_port < CBQ_PORT_BASE || dst_port >= CBQ_PORT_BASE + CBQ_MAX_CLIENTS) return 0;
  if (!bb_published) return 1;
  const uint8_t *sdu = static_cast<const uint8_t *>(body);
  IOLockLock(bb_lock);
  // Only the client's own service may speak to it: a message from another X55 service (a port that changed hands, v25: a WDS
  // 0x22 indication parsed by ABM as another service's, "QMI parsing error … msgId=0x0022", ABM ASSERT, CommCenter SIGTRAP)
  // is dropped.
  const struct cbq_client &owner = bb_qmux.client[dst_port - CBQ_PORT_BASE];
  const struct cbq_server *from = owner.used ? cbq_server_find(&bb_qmux, owner.service) : nullptr;
  if (from && (from->node != src_node || from->port != src_port)) {
    static uint32_t foreign;
    if (foreign++ < 16)
      BB_LOG("X55 message for port 0x%x (svc 0x%x) from %u:%u, not its service's %u:%u: dropped\n", dst_port, owner.service, src_node,
             src_port, from->node, from->port);
    IOLockUnlock(bb_lock);
    return 1;
  }
  const struct cbq_result r = cbq_from_modem(&bb_qmux, dst_port, src_node, sdu, size, bb_out, sizeof bb_out);
  if (r.kind == CBQ_TO_HOST && r.channel < CBQ_CHANNELS && sdu[0] == CBQ_QMI_RESPONSE) {
    struct ubp_entry e;
    const uint32_t txn = cbd_rd16(sdu + 1), message = cbd_rd16(sdu + 3);
    if (r.service == CBD_SVC_WDS && message == 0x00afu && txn == 0xfeafu) {
      static uint32_t bound_logs;
      if (bound_logs++ < 24)
        BB_LOG("WDS auto bind sub 1 port 0x%x cid %u err 0x%x\n", dst_port, r.cid,
               cbd_sdu_ok(sdu, size) ? cbd_error(sdu, size) : 0xfffeu);
      IOLockUnlock(bb_lock);
      return 1;
    }
    if (!ubp_answer(&bb_pending, dst_port, txn, &e)) {
      // Already answered here (timeout) or never asked: ABM must not get a second answer.
      if (bb_req_lines++ < BB_REQ_LINES)
        BB_LOG("rsp svc 0x%x msg 0x%x txn %u cid %u from the X55 owed to nobody: dropped\n", r.service, message, txn, r.cid);
      IOLockUnlock(bb_lock);
      return 1;
    }
    if (r.service == UCI_SERVICE) cellinfo_observe(dst_port - CBQ_PORT_BASE, sdu, size);
    if (bb_req_lines++ < BB_REQ_LINES)
      BB_LOG("rsp svc 0x%x msg 0x%x txn %u cid %u error 0x%x after %u ms\n", r.service, message, txn, r.cid,
             cbd_sdu_ok(sdu, size) ? cbd_error(sdu, size) : 0xfffeu, bb_now_ms() - e.sent_ms);
  }
  if (r.kind == CBQ_TO_HOST && r.channel < CBQ_CHANNELS) {
    voice_note("rx", r, sdu, size);
    ims_note("rx", r, sdu, size);
    if (r.service == CBD_SVC_WDS) bb_pdp_note(dst_port, r.cid, r.channel, sdu, size);
    if (r.service == BB_SVC_WDA) bb_wda_answer(sdu, size);
    // An IMS context's 0x2d (runtime settings) reply is held until its pdp_ip has a global IPv6
    // address, at most IMS_DAD_HOLD_MS: CommCenter reads that interface address once, when this reply
    // arrives, and the address is ~57 ms late (umi_ims.inc ims_dad_hold_response).
    const uint32_t bb_channel = r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2;
    if (!(r.service == CBD_SVC_WDS &&
          ims_dad_hold_response(bb_channel, dst_port - CBQ_PORT_BASE, sdu, size, bb_out, r.len))) {
      bb_vops_claim(r.service, bb_out, r.len);   // umi_vops=1: NAS 0x4E/0x4D TLV 0x21 0 -> 1
      bb_push(bb_channel, bb_out, r.len);
    }
    // A Start Network refused with a 3GPP / Mobile IP reason: the Packet Service Status indication Apple's baseband sends with
    // that reason, which CommCenter waits for (umi_bb_pending.h). The X55's own reason, family and status, nothing invented.
    uint8_t ind[32];
    const uint32_t t = ubp_start_failure_indication(r.service, sdu, size, bb_pdp_clients[dst_port - CBQ_PORT_BASE].family, ind, sizeof ind);
    if (t) {
      const uint32_t m = cbq_private_indication(UBP_WDS_PACKET_SERVICE_STATUS, r.service, r.cid, ind, t, bb_out, sizeof bb_out);
      if (m) {
        bb_pdp_note(dst_port, r.cid, r.channel, bb_out + CBQ_HDR, m - CBQ_HDR);
        bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, m);
        uint32_t vlen = 0;
        const uint8_t *v = ubp_tlv(sdu, size, 0x11, &vlen);
        BB_LOG("pdp: start refused (cid %u family %u, verbose type %u code %d): disconnected indication sent with the X55's reason\n", r.cid,
               bb_pdp_clients[dst_port - CBQ_PORT_BASE].family, v ? (uint32_t)(v[0] | v[1] << 8) : 0u, v ? (int16_t)(v[2] | v[3] << 8) : 0);
      }
    }
    bb_sim_answer(r, sdu, size);   // after the answer itself: bb_activation_indicate reuses bb_out
  }
  IOLockUnlock(bb_lock);
  return 1;
}

extern "C" void umi_bb_link(uint32_t dl_max_size)
{
  if (!bb_published) return;
  IOLockLock(bb_lock);
  bb_link_ok = dl_max_size && dl_max_size <= BB_QMAP_MAX;
  IOLockUnlock(bb_lock);
  BB_LOG("data link: IP_HW0 raw IP over QMAP %s (downlink aggregate %u bytes)\n", bb_link_ok ? "ready" : "NOT ready", dl_max_size);
}

extern "C" unsigned umi_bb_poll(umi_bb_send_t send, umi_bb_release_t release)
{
  if (!bb_published) return 0;
  voice_schedule_switch();
  ims_schedule_switch();
  uint8_t voice_packet[7];
  uint32_t voice_node = 0, voice_port = 0;
  bool voice_release;
  bb_tx batch[16];
  uint32_t n = 0, released[16], r = 0, port;
  uint8_t loc[32];
  uint32_t loc_node = 0, loc_port = 0;
  uint8_t cell[7];
  uint32_t cell_node = 0, cell_port = 0;
  uint8_t ims_packet[16];
  uint32_t ims_node = 0, ims_port = 0;
  uint8_t imss_packet[24];   /* the 0x2a Config Ind Register request is 23 bytes */
  uint32_t imss_node = 0, imss_port = 0;
  uint8_t dcm_packet[32];    /* a QMI CTL answer: Get Version Info 23 bytes at most */
  uint32_t dcm_node = 0, dcm_port = 0, dcm_reply_local = 0;
  uint32_t dcm_service = 0, dcm_version = 0, dcm_instance = 0, dcm_local = 0;
  cellinfo_schedule_switch(); // VFS work is queued to a separate thread
  // pdp_ip ifaddrs for the DAD candidates: only while an activation is waiting for its send, and
  // before bb_lock because the ifnet walk takes ifnet locks and may sleep (Z_WAITOK).
  if (ims_dad_ifaddr_wanted()) ims_dad_ifaddr_refresh(false);
  IOLockLock(bb_lock);
  ims_apply_switch();
  /* D3 (v30 diag): IMSA stops answering 6-20 s into a VoLTE attempt, 60-95 s before the whole PCIe
   * function leaves the bus (v29t/u/v). umi_ims.inc times a request out inside ims_poll; sampling the
   * same deadline just before that call gives one line per run of three consecutive lost audits
   * instead of one per query. Timeouts leave registration at -1, so an answered audit clears the run. */
  static uint32_t imsa_misses;
  const bool imsa_lost = ims_audit.pending && bb_now_ms() - ims_audit.sent_at >= 5000u;
  const uint32_t ims_len = ims_poll(ims_packet, &ims_node, &ims_port);
  if (imsa_lost) {
    if (++imsa_misses == 3)
      BB_LOG("IMSA silent: 3 consecutive audit timeouts (txn %u); the rest of the X55 still answers\n", ims_audit.txn);
  } else if (ims_audit.registration >= 0) imsa_misses = 0;
  const bool ims_release = ims_audit.release;
  ims_audit.release = false;
  const uint32_t imss_len = ims_imss_poll(imss_packet, &imss_node, &imss_port);
  const bool imss_release = ims_imss.release;
  ims_imss.release = false;
  // Route B probe: once per switch-on, ask the kernel to publish IMSDCM 770 (type-4 NEW_SERVER);
  // and drain a queued QMI CTL answer for the modem.
  const bool dcm_announce = ims_dcm_announce(&dcm_service, &dcm_version, &dcm_instance, &dcm_local) != 0;
  const uint32_t dcm_len = ims_dcm_reply(dcm_packet, sizeof dcm_packet, &dcm_node, &dcm_port, &dcm_reply_local);
  ims_dad_poll();   // synthesized WDS DAD complete: absorbs diagnostic ioctls and fires due timers
  const uint32_t cell_len = cellinfo_poll(cellinfo_switch_wanted, cell, &cell_node, &cell_port);
  const bool cell_release = cellinfo.release;
  cellinfo.release = false;
  const uint32_t voice_len = voice_poll(voice_switch_wanted, voice_packet, &voice_node, &voice_port, &voice_release);
  const uint32_t loc_len = gnss_poll(loc, &loc_node, &loc_port);
  while (bb_tx_count && n < 16) {
    batch[n++] = bb_txq[bb_tx_head];
    bb_tx_head = (bb_tx_head + 1) % BB_TX;
    bb_tx_count--;
  }
  while (r < 16 && (port = cbq_next_release(&bb_qmux))) {
    released[r++] = port;
    ubp_forget(&bb_pending, port);
  }
  struct ubp_entry late;
  while (ubp_expired(&bb_pending, bb_now_ms(), &late)) {
    BB_LOG("no answer from the X55 in %u ms: svc 0x%x msg 0x%x txn %u cid %u port 0x%x\n", UBP_ANSWER_MS, late.service, late.message,
           late.txn, late.cid, late.port);
    if (cbq_client_find(&bb_qmux, late.channel, late.service, late.cid) == (int)(late.port - CBQ_PORT_BASE))
      bb_answer_error(late.channel, late.service, late.cid, late.txn, late.message, UBP_ERR_INTERNAL, "X55 silent");
  }
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) {
    send(batch[i].local_port, batch[i].node, batch[i].port, batch[i].data, batch[i].len);
    IOFreeData(batch[i].data, batch[i].len);
  }
  for (uint32_t i = 0; i < r; i++) release(released[i]);
  if (voice_release) release(VOICE_PORT);
  if (voice_len) send(VOICE_PORT, voice_node, voice_port, voice_packet, voice_len);
  if (loc_len) send(UGL_PORT, loc_node, loc_port, loc, loc_len);
  if (cell_release) release(UCI_PORT);
  if (cell_len) send(UCI_PORT, cell_node, cell_port, cell, cell_len);
  if (ims_release) release(UMI_IMS_AUDIT_PORT);
  if (ims_len) send(UMI_IMS_AUDIT_PORT, ims_node, ims_port, ims_packet, ims_len);
  if (imss_release) release(UMI_IMS_IMSS_AUDIT_PORT);
  if (imss_len) send(UMI_IMS_IMSS_AUDIT_PORT, imss_node, imss_port, imss_packet, imss_len);
  if (dcm_announce) umi_bb_announce(dcm_service, dcm_version, dcm_instance, dcm_local);
  if (dcm_len) send(dcm_reply_local, dcm_node, dcm_port, dcm_packet, dcm_len);
  bb_pdp_update();
  return n + r + !!loc_len + !!voice_len + voice_release + !!cell_len + cell_release + !!ims_len + ims_release +
         !!imss_len + imss_release + !!dcm_len + dcm_announce;
}

extern "C" void umi_bb_report(unsigned second)
{
  if (!bb_published) return;
  IOLockLock(bb_pdp_lock);
  for (uint32_t i = 0; i < CBP_INTERFACES; i++) {
    const bb_pdp_if &p = bb_pdp_ifaces[i];
    if (p.families || p.tx || p.rx || p.dropped)
      BB_LOG("pdp: %u s pdp_ip%u families %u blocked %u queued %u tx %llu rx %llu drop %llu\n",
        second, i, p.families, p.blocked, p.queued, p.tx, p.rx, p.dropped);
  }
  IOLockUnlock(bb_pdp_lock);
  IOLockLock(bb_lock);
  uint32_t clients = 0, servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) clients += bb_qmux.client[i].used;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  uint32_t owed = 0;
  for (uint32_t i = 0; i < UBP_MAX; i++) owed += bb_pending.e[i].used;
  BB_LOG("%u s: %u QMI services, %u clients, dropped %u/%u; sahara state %u, %u requests; board calls %u (%u unsupported); %u activations; trace %u lines, %u held; vops flips %u; restarts %u\n",
         second, servers, clients, bb_qmux.dropped, bb_tx_dropped, bb_sahara.state, bb_sahara.requests, bb_control.calls,
         bb_control.unsupported, bb_activations, bb_trace_lines, bb_trace_held, bb_vops_flips, umi_modem_restarts());
  BB_LOG("%u s: requests answered by the X55 %u, by timeout %u, in flight %u, table full %u, dropped on a lost transport %u\n",
         second, bb_pending.answered, bb_pending.expired, owed, bb_pending.overflow, bb_lost_drops);
  BB_LOG("%u s: NAS5G: mode=%d nr5g_dis=%d (set=%d/%d #%u) pref=%u sys=%u reg=%d rat=%d lte_svc=%d vops=%d nr5g_svc=%d dom=%d cap=%d endc=%d dcnr=%d\n",
         second, nas_obs.mode, nas_obs.nr5g_dis, nas_obs.set_mode, nas_obs.set_nr5g_dis, nas_obs.set_seen,
         nas_obs.pref_seen, nas_obs.sys_info_seen, nas_obs.serving_reg, nas_obs.serving_rat,
         nas_obs.lte_svc, nas_obs.lte_vops, nas_obs.nr5g_svc, nas_obs.nr5g_dom, nas_obs.nr5g_cap,
         nas_obs.endc_avail, nas_obs.dc_nr_restr);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) {
    const bb_channel *ch = &bb_ch[c];
    if (ch->opens)
      BB_LOG("%s: opens %u, in %u frames %u bytes, out %u frames %u bytes, dropped %u, queued %u, reads waiting %u\n",
             cbh_published[c], ch->opens, ch->frames_in, ch->bytes_in, ch->frames_out, ch->bytes_out, ch->frames_dropped, ch->count,
             ch->reads_waiting);
  }
  IOLockUnlock(bb_lock);
}
