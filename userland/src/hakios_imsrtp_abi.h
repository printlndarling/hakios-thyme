/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef HAKIOS_IMSRTP_ABI_H
#define HAKIOS_IMSRTP_ABI_H

#include <stdint.h>
#include <sys/ioccom.h>

#define HAKIOS_IMSRTP_DEVICE      "/dev/hakios-imsrtp"
#define HAKIOS_IMSRTP_DATA_DEVICE "/dev/hakios-imsrtp-data"

enum hakios_imsrtp_kind {
    HAKIOS_IMSRTP_INIT   = 1,
    HAKIOS_IMSRTP_CONFIG = 2,
    HAKIOS_IMSRTP_START  = 3,
    HAKIOS_IMSRTP_STOP   = 4,
    HAKIOS_IMSRTP_UNINIT = 5,
    HAKIOS_IMSRTP_DTMF   = 6,
};

struct hakios_imsrtp_event {
    uint32_t kind;
    uint8_t  session_id;
    uint8_t  ip_version;     /* 4 or 6 */
    uint8_t  codec;          /* 7=AMR-NB, 8=AMR-WB, 0=PCMU, 1=PCMA */
    uint8_t  octet_align;    /* 0=bandwidth-efficient, 1=octet-aligned */
    uint8_t  rx_pt;
    uint8_t  tx_pt;
    uint8_t  dtmf_pt;
    uint8_t  mode_set;       /* AMR mode_set bitmask (0xff = all) */
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t remote_rtcp;
    uint16_t clock_rate;     /* 8000 or 16000 */
    uint16_t ptime_ms;       /* 20 */
    uint8_t  direction;      /* bit 0 = TX, bit 1 = RX */
    uint8_t  dtmf_digit;     /* RFC 4733 event code (0..15) */
    uint16_t dtmf_duration;  /* ms */
    uint8_t  dtmf_volume;
    uint8_t  pad;
    char     local_ip[64];
    char     remote_ip[64];
};

/* Kernel pdp_ip1 UDP tap/inject route for /dev/hakios-imsrtp-data */
struct hakios_imsrtp_route {
    uint8_t  active;         /* 1 = tap RX & enable TX on pdp_ip1, 0 = stop */
    uint8_t  ip_version;     /* 4 or 6 */
    uint16_t local_port;     /* host byte order */
    uint16_t remote_port;    /* host byte order */
    uint16_t remote_rtcp;    /* host byte order (0 = remote_port + 1) */
    uint8_t  local_ip[16];   /* network byte order (4B for IPv4, 16B for IPv6) */
    uint8_t  remote_ip[16];  /* network byte order */
};

#define HAKIOS_IMSRTP_SET_ROUTE _IOW('I', 1, struct hakios_imsrtp_route)
#define HAKIOS_IMSRTP_DATA_RTCP 0x01u /* flags byte bit 0 on write(): 0=RTP, 1=RTCP */

#endif /* HAKIOS_IMSRTP_ABI_H */
