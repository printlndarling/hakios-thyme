/* umi_modem=2: the CommCenter bridge (iokit/Kernel/hakios.umi_baseband.cpp) and the X55 driver
 * (umi_modem.c) it rides on. Every function here runs on the modem's service thread unless it says
 * otherwise: that thread alone owns MHI and sends QRTR. See devices/xiaomi-mi-10/COMMCENTER-umi.md. */
#ifndef UMI_BASEBAND_H
#define UMI_BASEBAND_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*umi_bb_send_t)(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length);
typedef void (*umi_bb_release_t)(uint32_t local_port);

/* Publish AppleBaseband, AppleBasebandPCIControl, the AppleBasebandPCIInterface channels, the PDP
 * manager, AppleIPAppender and pdp_ip0..3, before the modem is up: CommCenter must find them. */
void umi_bb_publish(void);
/* QRTR discovery is done: SAHARA may greet BBUpdater, QMUX clients reach real services. */
void umi_bb_modem_ready(void);
void umi_bb_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
void umi_bb_del_server(uint32_t service, uint32_t node, uint32_t port);
/* A QRTR data packet for dst_port: 1 when the port is one of the bridge's (0x4100..0x413f). */
int umi_bb_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
/* Route B probe: publish a host-side QRTR service to the modem's name server (type-4 NEW_SERVER,
 * as the TFTP registration) and re-publish it on every router HELLO. The packet is
 * {4, service, version | instance << 8, host node 1, port}. Safe before the router has said HELLO:
 * the request is kept and sent then. Returns 0 when recorded, an errno when the send failed. */
int umi_bb_announce(uint32_t service, uint32_t version, uint32_t instance, uint32_t port);
/* Send what ABM queued, release what it freed. Returns the number of messages handled. */
unsigned umi_bb_poll(umi_bb_send_t send, umi_bb_release_t release);
/* The kernel set IP_HW0 to raw IP over QMAP (5) with this downlink aggregate limit (0: no link). */
void umi_bb_link(uint32_t dl_max_size);
/* One downlink IP packet of QMAP mux `mux`: 1 when an active pdp_ip took it. */
int umi_bb_data_input(uint32_t mux, const uint8_t *ip, uint32_t length);
/* The next uplink IP packet (at most `room` bytes) and its mux, or 0. */
uint32_t umi_bb_data_take(uint8_t *mux, uint8_t *ip, uint32_t room);
void umi_bb_report(unsigned second);

/* umi_modem.c: wake the service thread (callable from any thread). */
void umi_modem_wake(void);

/* umi_modem.c: the X55 transport itself. 0 = no service thread, 1 = a live link, 2 = the thread is alive but the link is
 * not (bring-up, fault, or between restarts). The bridge must never report a reset as harmless while this is not 1. */
int umi_modem_state(void);
/* Ask for a real bring-up after a fault (PERST cycle, PCIe retrain, MHI/Sahara, QRTR discovery). 1 when taken, 0 when
 * refused: kill switch off (kern.hakios.umi_mdm_restart), per-boot cap reached, link live, or no thread. */
int umi_modem_restart(void);
/* Full restarts performed since boot, for the periodic summary. */
unsigned umi_modem_restarts(void);
/* umi_modem.c: the transport died. Called on the modem thread after it has stopped the link, before the restart: the
 * bridge drops the X55's QRTR services, pools ABM's QMI clients, drops queued frames and answers what it owes ABM, so
 * nothing keeps pointing at a link that no longer exists. From then until umi_bb_modem_ready() every QMUX request from
 * ABM is dropped with no reply, so ABM's ATCS timeout, not a synthetic error, is what starts CommCenter's reset. */
void umi_bb_modem_lost(void);
#ifdef __cplusplus
}
#endif
#endif
