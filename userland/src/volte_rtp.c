/*
 * volte_rtp: real-time two-way VoLTE RTP media engine for HakiOS on Xiaomi Mi 10S (thyme).
 *
 * Reads session lifecycle events from /dev/hakios-imsrtp (published by umi_ims.inc when
 * IPTelephony.framework runs IMSRTP 0x0022/0x0024/0x0025/0x0026/0x0023/0x0027 over QMUX),
 * configures the kernel pdp_ip1 UDP tap/injector on /dev/hakios-imsrtp-data (avoiding
 * EADDRINUSE against CommCenter's placeholder UDP socket on local_port), decodes incoming
 * RFC 4867 AMR-NB / AMR-WB (and PCMU/PCMA) voice packets to 48 kHz stereo PCM on
 * /dev/hakios-audio, and captures 48 kHz mono mic PCM from /dev/hakios-mic, encoding and
 * sending RFC 4867 RTP + RFC 4733 DTMF + periodic RTCP RR back to the IMS media gateway.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hakios.audio.h"
#include "hakios_imsrtp_abi.h"
#include "opencore-amrnb/interf_dec.h"
#include "opencore-amrnb/interf_enc.h"
#include "opencore-amrwb/dec_if.h"
#include "vo-amrwbenc/enc_if.h"

#define LOG(fmt, ...) do { printf("volte_rtp: " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

/* ---- RFC 4867 AMR-NB & AMR-WB bit-length tables (speech bits, excluding 4b CMR & 6b TOC) ---- */
static const uint16_t amrnb_bits[16] = {
    95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0
};
static const uint16_t amrwb_bits[16] = {
    132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 0, 0, 0, 0, 0, 0
};

static inline uint32_t get_bits(const uint8_t *buf, size_t len, size_t bitpos, int nbits) {
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++) {
        size_t p = bitpos + (size_t)i;
        size_t byte = p >> 3;
        int shift = 7 - (int)(p & 7u);
        uint32_t b = (byte < len) ? ((buf[byte] >> shift) & 1u) : 0u;
        v = (v << 1) | b;
    }
    return v;
}

static inline void put_bits(uint8_t *buf, size_t cap, size_t *bitpos, uint32_t val, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        size_t p = (*bitpos)++;
        size_t byte = p >> 3;
        int shift = 7 - (int)(p & 7u);
        if (byte < cap) {
            if ((val >> i) & 1u) buf[byte] |= (uint8_t)(1u << shift);
            else buf[byte] &= (uint8_t)~(1u << shift);
        }
    }
}

static size_t rfc4867_unpack(const uint8_t *pay, size_t plen, bool is_wb, bool octet_align,
                             uint8_t *out, size_t out_cap, uint8_t *out_ft) {
    if (plen < 2 || out_cap < 64) return 0;
    const uint16_t *bits_tbl = is_wb ? amrwb_bits : amrnb_bits;
    uint8_t ft = 15, q = 1;

    if (octet_align) {
        size_t toc_idx = 1;
        uint8_t toc0 = pay[1];
        ft = (toc0 >> 3) & 0x0fu;
        q = (toc0 >> 2) & 0x01u;
        while ((pay[toc_idx] & 0x80u) && toc_idx + 1 < plen) toc_idx++;
        size_t data_off = toc_idx + 1;
        uint16_t nb = bits_tbl[ft];
        size_t nbytes = (size_t)(nb + 7u) / 8u;
        if (ft == 15 || nb == 0 || data_off + nbytes > plen) return 0;
        out[0] = (uint8_t)((ft << 3) | (q << 2));
        memcpy(out + 1, pay + data_off, nbytes);
        if (out_ft) *out_ft = ft;
        return 1u + nbytes;
    } else {
        size_t bp = 4;
        uint32_t f = get_bits(pay, plen, bp, 1); bp += 1;
        ft = (uint8_t)get_bits(pay, plen, bp, 4); bp += 4;
        q  = (uint8_t)get_bits(pay, plen, bp, 1); bp += 1;
        while (f && bp + 6 <= plen * 8) {
            f = get_bits(pay, plen, bp, 1);
            bp += 6;
        }
        size_t speech_bitpos = bp;
        uint16_t nb = bits_tbl[ft];
        size_t nbytes = (size_t)(nb + 7u) / 8u;
        if (ft == 15 || nb == 0 || speech_bitpos + nb > plen * 8) return 0;
        memset(out, 0, 1u + nbytes);
        out[0] = (uint8_t)((ft << 3) | (q << 2));
        for (uint16_t i = 0; i < nb; i++) {
            uint32_t b = get_bits(pay, plen, speech_bitpos + i, 1);
            if (b) out[1u + (i >> 3)] |= (uint8_t)(1u << (7 - (i & 7)));
        }
        if (out_ft) *out_ft = ft;
        return 1u + nbytes;
    }
}

static size_t rfc4867_pack(const uint8_t *storage, size_t slen, bool is_wb, bool octet_align,
                           uint8_t *out, size_t out_cap) {
    if (slen < 1 || out_cap < 64) return 0;
    const uint16_t *bits_tbl = is_wb ? amrwb_bits : amrnb_bits;
    uint8_t ft = (storage[0] >> 3) & 0x0fu;
    uint8_t q  = (storage[0] >> 2) & 0x01u;
    uint8_t cmr = 15;
    uint16_t nb = bits_tbl[ft];
    size_t nbytes = (size_t)(nb + 7u) / 8u;
    if (1u + nbytes > slen) return 0;

    if (octet_align) {
        out[0] = (uint8_t)(cmr << 4);
        out[1] = (uint8_t)((ft << 3) | (q << 2));
        if (nbytes > 0) memcpy(out + 2, storage + 1, nbytes);
        return 2u + nbytes;
    } else {
        size_t total_bits = 4u + 6u + nb;
        size_t total_bytes = (total_bits + 7u) / 8u;
        if (total_bytes > out_cap) return 0;
        memset(out, 0, total_bytes);
        size_t bp = 0;
        put_bits(out, out_cap, &bp, cmr, 4);
        put_bits(out, out_cap, &bp, 0, 1);
        put_bits(out, out_cap, &bp, ft, 4);
        put_bits(out, out_cap, &bp, q, 1);
        for (uint16_t i = 0; i < nb; i++) {
            uint32_t b = (storage[1u + (i >> 3)] >> (7 - (i & 7))) & 1u;
            put_bits(out, out_cap, &bp, b, 1);
        }
        return total_bytes;
    }
}

static int16_t ulaw_decode(uint8_t u) {
    u = (uint8_t)~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= ((unsigned)u & 0x70) >> 4;
    return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}
static int16_t alaw_decode(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0f) << 4;
    int seg = ((unsigned)a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else t = (t + 0x108) << (seg - 1);
    return (a & 0x80) ? (int16_t)t : (int16_t)(-t);
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

typedef struct {
    struct hakios_imsrtp_event cfg;
    volatile int running;
    int data_fd;
    int audio_fd;
    int mic_fd;
    uint32_t tx_ssrc;
    uint16_t tx_seq;
    uint32_t tx_ts;
    pthread_t rx_tid;
    pthread_t tx_tid;
    int rx_started;
    int tx_started;

    pthread_mutex_t dtmf_mtx;
    int dtmf_pending;
    uint8_t dtmf_digit;
    uint16_t dtmf_dur;
    uint8_t dtmf_vol;

    uint64_t rx_pkts;
    uint64_t rx_decoded;
    uint64_t rx_bad;
    uint64_t rx_audio_writes;
    int32_t  rx_peak;
    uint32_t rx_last_ssrc;
    uint64_t tx_pkts;
    uint64_t tx_err;
    uint64_t tx_mic_reads;
    int32_t  tx_mic_peak;
} CallSession;

static CallSession g_call;

static ssize_t send_ims_pkt(int fd, uint8_t flags, const uint8_t *payload, size_t len) {
    if (fd < 0 || len == 0 || len > 1400) return -1;
    uint8_t frame[1404];
    frame[0] = flags;
    memcpy(frame + 1, payload, len);
    return write(fd, frame, 1u + len);
}

static void *rx_thread_fn(void *arg) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CallSession *s = (CallSession *)arg;
    bool is_wb = (s->cfg.codec == 8 || s->cfg.clock_rate == 16000);
    bool is_pcmu = (s->cfg.codec == 0);
    bool is_pcma = (s->cfg.codec == 1);
    bool oa = (s->cfg.octet_align != 0);

    void *amrnb_dec = NULL;
    void *amrwb_dec = NULL;
    if (is_wb) amrwb_dec = D_IF_init();
    else if (!is_pcmu && !is_pcma) amrnb_dec = Decoder_Interface_init();

    uint8_t pkt[1500];
    uint8_t storage[64];
    int16_t pcm_in[320];
    int16_t stereo48[960 * 2];
    int16_t prev_sample = 0;
    uint64_t last_log_pkts = 0;

    LOG("RX thread started: codec=%s rate=%u pt=%u oa=%u data_fd=%d audio_fd=%d",
        is_wb ? "AMR-WB" : (is_pcmu ? "PCMU" : (is_pcma ? "PCMA" : "AMR-NB")),
        s->cfg.clock_rate, s->cfg.rx_pt, oa ? 1u : 0u, s->data_fd, s->audio_fd);

    while (s->running) {
        ssize_t n = read(s->data_fd, pkt, sizeof(pkt));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break; /* EOF when SET_ROUTE(active=0) */
        }
        if (n < 13) continue;
        uint8_t ver = (pkt[0] >> 6) & 0x03u;
        if (ver != 2) continue;
        uint8_t cc = pkt[0] & 0x0fu;
        bool has_ext = (pkt[0] & 0x10u) != 0;
        bool has_pad = (pkt[0] & 0x20u) != 0;
        uint8_t pt = pkt[1] & 0x7fu;

        size_t hdr = 12u + (size_t)cc * 4u;
        if ((size_t)n <= hdr) continue;
        if (has_ext) {
            if ((size_t)n < hdr + 4u) continue;
            uint16_t ext_words = (uint16_t)((pkt[hdr + 2] << 8) | pkt[hdr + 3]);
            hdr += 4u + (size_t)ext_words * 4u;
        }
        size_t end = (size_t)n;
        if (has_pad && end > hdr) {
            uint8_t pad = pkt[end - 1];
            if (pad > 0 && end - hdr >= pad) end -= pad;
        }
        if (end <= hdr) continue;

        if (s->cfg.dtmf_pt && pt == s->cfg.dtmf_pt) continue;
        s->rx_pkts++;
        s->rx_last_ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                          ((uint32_t)pkt[10] << 8) | pkt[11];

        const uint8_t *pay = pkt + hdr;
        size_t plen = end - hdr;
        int nsamples = 0;

        if (is_pcmu) {
            nsamples = (int)(plen > 160 ? 160 : plen);
            for (int i = 0; i < nsamples; i++) pcm_in[i] = ulaw_decode(pay[i]);
        } else if (is_pcma) {
            nsamples = (int)(plen > 160 ? 160 : plen);
            for (int i = 0; i < nsamples; i++) pcm_in[i] = alaw_decode(pay[i]);
        } else {
            uint8_t ft = 15;
            size_t slen = rfc4867_unpack(pay, plen, is_wb, oa, storage, sizeof(storage), &ft);
            if (slen == 0 && !oa) {
                slen = rfc4867_unpack(pay, plen, is_wb, true, storage, sizeof(storage), &ft);
            }
            if (slen == 0) {
                s->rx_bad++;
                continue;
            }
            if (is_wb && amrwb_dec) {
                D_IF_decode(amrwb_dec, storage, pcm_in, 0);
                nsamples = 320;
            } else if (!is_wb && amrnb_dec) {
                Decoder_Interface_Decode(amrnb_dec, storage, pcm_in, 0);
                nsamples = 160;
            }
        }

        if (nsamples <= 0) continue;
        s->rx_decoded++;

        int32_t fpeak = 0;
        for (int i = 0; i < nsamples; i++) {
            int32_t a = pcm_in[i] < 0 ? -pcm_in[i] : pcm_in[i];
            if (a > fpeak) fpeak = a;
        }
        if (fpeak > s->rx_peak) s->rx_peak = fpeak;

        int factor = (nsamples == 320) ? 3 : 6;
        int out_frames = nsamples * factor;
        if (out_frames > 960) out_frames = 960;
        for (int i = 0; i < nsamples && i * factor < out_frames; i++) {
            int32_t cur = pcm_in[i];
            int32_t prev = prev_sample;
            for (int k = 0; k < factor; k++) {
                int32_t smp = prev + ((cur - prev) * (k + 1)) / factor;
                if (smp > 32767) smp = 32767;
                if (smp < -32768) smp = -32768;
                int idx = (i * factor + k) * 2;
                stereo48[idx] = (int16_t)smp;
                stereo48[idx + 1] = (int16_t)smp;
            }
            prev_sample = (int16_t)cur;
        }

        if (s->audio_fd >= 0) {
            ssize_t w = write(s->audio_fd, stereo48, (size_t)out_frames * HAKIOS_AUDIO_FRAME);
            if (w > 0) s->rx_audio_writes++;
        }

        if (s->rx_pkts == 1 || s->rx_pkts - last_log_pkts >= 200) {
            last_log_pkts = s->rx_pkts;
            LOG("RX stats: pkts=%llu decoded=%llu bad=%llu writes=%llu peak=%d frame_peak=%d",
                (unsigned long long)s->rx_pkts, (unsigned long long)s->rx_decoded,
                (unsigned long long)s->rx_bad, (unsigned long long)s->rx_audio_writes,
                s->rx_peak, fpeak);
        }
    }

    if (amrnb_dec) Decoder_Interface_exit(amrnb_dec);
    if (amrwb_dec) D_IF_exit(amrwb_dec);
    LOG("RX thread exit: pkts=%llu decoded=%llu bad=%llu writes=%llu peak=%d",
        (unsigned long long)s->rx_pkts, (unsigned long long)s->rx_decoded,
        (unsigned long long)s->rx_bad, (unsigned long long)s->rx_audio_writes, s->rx_peak);
    return NULL;
}

static void send_rtcp_rr(CallSession *s) {
    if (s->data_fd < 0) return;
    uint8_t rr[8];
    rr[0] = 0x80;
    rr[1] = 201;
    rr[2] = 0x00;
    rr[3] = 0x01;
    rr[4] = (uint8_t)(s->tx_ssrc >> 24);
    rr[5] = (uint8_t)(s->tx_ssrc >> 16);
    rr[6] = (uint8_t)(s->tx_ssrc >> 8);
    rr[7] = (uint8_t)(s->tx_ssrc);
    (void)send_ims_pkt(s->data_fd, HAKIOS_IMSRTP_DATA_RTCP, rr, sizeof(rr));
}

static void send_dtmf_rfc4733(CallSession *s, uint8_t digit, uint16_t dur_ms, uint8_t vol, bool is_wb) {
    if (!s->cfg.dtmf_pt || s->data_fd < 0) return;
    uint32_t ts = s->tx_ts;
    uint32_t step = is_wb ? 320u : 160u;
    uint32_t total_dur = is_wb ? (uint32_t)dur_ms * 16u : (uint32_t)dur_ms * 8u;
    if (total_dur < step) total_dur = step;
    for (int i = 0; i < 6 && s->running; i++) {
        uint8_t pkt[16];
        bool end = (i >= 3);
        uint16_t seq = s->tx_seq++;
        uint32_t d = end ? total_dur : (uint32_t)(i + 1) * step;
        if (d > total_dur) d = total_dur;
        pkt[0] = 0x80;
        pkt[1] = (uint8_t)((i == 0 ? 0x80u : 0x00u) | (s->cfg.dtmf_pt & 0x7fu));
        pkt[2] = (uint8_t)(seq >> 8);
        pkt[3] = (uint8_t)(seq);
        pkt[4] = (uint8_t)(ts >> 24);
        pkt[5] = (uint8_t)(ts >> 16);
        pkt[6] = (uint8_t)(ts >> 8);
        pkt[7] = (uint8_t)(ts);
        pkt[8] = (uint8_t)(s->tx_ssrc >> 24);
        pkt[9] = (uint8_t)(s->tx_ssrc >> 16);
        pkt[10] = (uint8_t)(s->tx_ssrc >> 8);
        pkt[11] = (uint8_t)(s->tx_ssrc);
        pkt[12] = digit;
        pkt[13] = (uint8_t)((end ? 0x80u : 0x00u) | (vol & 0x3fu));
        pkt[14] = (uint8_t)(d >> 8);
        pkt[15] = (uint8_t)(d);
        (void)send_ims_pkt(s->data_fd, 0, pkt, sizeof(pkt));
        usleep(20000);
    }
    s->tx_ts += total_dur;
    LOG("TX sent RFC4733 DTMF digit=%u pt=%u dur=%ums", digit, s->cfg.dtmf_pt, dur_ms);
}

static void *tx_thread_fn(void *arg) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CallSession *s = (CallSession *)arg;
    bool is_wb = (s->cfg.codec == 8 || s->cfg.clock_rate == 16000);
    bool oa = (s->cfg.octet_align != 0);
    void *amrnb_enc = NULL;
    void *amrwb_enc = NULL;

    if (is_wb) amrwb_enc = E_IF_init();
    else amrnb_enc = Encoder_Interface_init(0);

    int16_t mic48[960];
    int16_t pcm_enc[320];
    uint8_t storage[64];
    uint8_t rtp[128];
    uint64_t rtcp_tick = 0;
    uint64_t gate_start = now_us();

    LOG("TX thread started: codec=%s rate=%u pt=%u oa=%u mic_fd=%d (waiting for RX media gate)",
        is_wb ? "AMR-WB" : "AMR-NB", s->cfg.clock_rate, s->cfg.tx_pt, oa ? 1u : 0u, s->mic_fd);

    /* Wait for QCI-1 / MGW media gate (first 5 RX RTP packets, or 6 s fallback) before sending uplink RTP */
    while (s->running && s->rx_pkts < 5 && (now_us() - gate_start) < 6000000ULL) {
        usleep(20000);
    }
    if (s->running) {
        LOG("TX gate open after %llu ms (rx_pkts=%llu)",
            (unsigned long long)((now_us() - gate_start) / 1000ULL),
            (unsigned long long)s->rx_pkts);
    }
    uint64_t next_tick = now_us();

    while (s->running) {
        int do_dtmf = 0;
        uint8_t d_digit = 0, d_vol = 10;
        uint16_t d_dur = 160;
        pthread_mutex_lock(&s->dtmf_mtx);
        if (s->dtmf_pending) {
            do_dtmf = 1;
            d_digit = s->dtmf_digit;
            d_dur = s->dtmf_dur;
            d_vol = s->dtmf_vol;
            s->dtmf_pending = 0;
        }
        pthread_mutex_unlock(&s->dtmf_mtx);
        if (do_dtmf) {
            send_dtmf_rfc4733(s, d_digit, d_dur, d_vol, is_wb);
            next_tick = now_us() + 20000ULL;
            continue;
        }

        uint64_t t = now_us();
        if (next_tick > t) {
            uint64_t wait_us = next_tick - t;
            if (wait_us > 40000ULL) wait_us = 20000ULL;
            usleep((useconds_t)wait_us);
        }
        next_tick += 20000ULL;
        if (next_tick + 60000ULL < now_us()) next_tick = now_us() + 20000ULL;
        if (!s->running) break;

        size_t need = sizeof(mic48);
        size_t got = 0;
        memset(mic48, 0, sizeof(mic48));
        if (s->mic_fd >= 0) {
            for (int tries = 0; tries < 4 && got < need && s->running; tries++) {
                ssize_t r = read(s->mic_fd, ((uint8_t *)mic48) + got, need - got);
                if (r > 0) {
                    got += (size_t)r;
                    s->tx_mic_reads++;
                } else if (r == 0) {
                    if (got == 0) break;
                    usleep(2000);
                } else {
                    if (errno == EINTR) continue;
                    break;
                }
            }
        }
        if (!s->running) break;

        int factor = is_wb ? 3 : 6;
        int nsamples = is_wb ? 320 : 160;
        for (int i = 0; i < nsamples; i++) {
            int32_t sum = 0;
            for (int k = 0; k < factor; k++) sum += mic48[i * factor + k];
            int16_t smp = (int16_t)(sum / factor);
            pcm_enc[i] = smp;
            int32_t a = smp < 0 ? -smp : smp;
            if (a > s->tx_mic_peak) s->tx_mic_peak = a;
        }

        int slen = 0;
        if (is_wb && amrwb_enc) {
            slen = E_IF_encode(amrwb_enc, 8, pcm_enc, storage, 0);
        } else if (!is_wb && amrnb_enc) {
            slen = Encoder_Interface_Encode(amrnb_enc, MR122, pcm_enc, storage, 0);
        }
        if (slen <= 0) continue;

        size_t plen = rfc4867_pack(storage, (size_t)slen, is_wb, oa, rtp + 12, sizeof(rtp) - 12);
        if (plen == 0) continue;

        uint16_t seq = s->tx_seq++;
        uint32_t ts = s->tx_ts;
        s->tx_ts += (uint32_t)nsamples;

        rtp[0] = 0x80;
        rtp[1] = (uint8_t)((s->tx_pkts == 0 ? 0x80u : 0x00u) | (s->cfg.tx_pt & 0x7fu));
        rtp[2] = (uint8_t)(seq >> 8);
        rtp[3] = (uint8_t)(seq);
        rtp[4] = (uint8_t)(ts >> 24);
        rtp[5] = (uint8_t)(ts >> 16);
        rtp[6] = (uint8_t)(ts >> 8);
        rtp[7] = (uint8_t)(ts);
        rtp[8] = (uint8_t)(s->tx_ssrc >> 24);
        rtp[9] = (uint8_t)(s->tx_ssrc >> 16);
        rtp[10] = (uint8_t)(s->tx_ssrc >> 8);
        rtp[11] = (uint8_t)(s->tx_ssrc);

        ssize_t sent = send_ims_pkt(s->data_fd, 0, rtp, 12u + plen);
        if (sent > 0) s->tx_pkts++;
        else s->tx_err++;

        if ((rtcp_tick++ % 250u) == 50u) send_rtcp_rr(s);
    }

    if (amrnb_enc) Encoder_Interface_exit(amrnb_enc);
    if (amrwb_enc) E_IF_exit(amrwb_enc);
    LOG("TX thread exit: pkts=%llu err=%llu mic_reads=%llu mic_peak=%d",
        (unsigned long long)s->tx_pkts, (unsigned long long)s->tx_err,
        (unsigned long long)s->tx_mic_reads, s->tx_mic_peak);
    return NULL;
}

static void call_stop(void) {
    if (!g_call.running && g_call.data_fd < 0 && g_call.audio_fd < 0 && g_call.mic_fd < 0) return;
    g_call.running = 0;
    if (g_call.data_fd >= 0) {
        struct hakios_imsrtp_route off = {0};
        (void)ioctl(g_call.data_fd, HAKIOS_IMSRTP_SET_ROUTE, &off);
    }
    if (g_call.rx_started) {
        pthread_join(g_call.rx_tid, NULL);
        g_call.rx_started = 0;
    }
    if (g_call.tx_started) {
        pthread_join(g_call.tx_tid, NULL);
        g_call.tx_started = 0;
    }
    if (g_call.data_fd >= 0) { close(g_call.data_fd); g_call.data_fd = -1; }
    if (g_call.audio_fd >= 0) { close(g_call.audio_fd); g_call.audio_fd = -1; }
    if (g_call.mic_fd >= 0) { close(g_call.mic_fd); g_call.mic_fd = -1; }
    LOG("call stopped (rx=%llu/%llu writes=%llu peak=%d tx=%llu mic_peak=%d)",
        (unsigned long long)g_call.rx_decoded, (unsigned long long)g_call.rx_pkts,
        (unsigned long long)g_call.rx_audio_writes, g_call.rx_peak,
        (unsigned long long)g_call.tx_pkts, g_call.tx_mic_peak);
}

static void call_start(const struct hakios_imsrtp_event *ev) {
    call_stop();
    memset(&g_call, 0, sizeof(g_call));
    pthread_mutex_init(&g_call.dtmf_mtx, NULL);
    g_call.data_fd = g_call.audio_fd = g_call.mic_fd = -1;
    g_call.cfg = *ev;
    g_call.tx_ssrc = (uint32_t)(time(NULL) ^ (getpid() << 16) ^ 0x5a3c1234u);
    g_call.tx_seq = 1;
    g_call.tx_ts = 160;

    if (!ev->local_port || !ev->remote_port || !ev->local_ip[0] || !ev->remote_ip[0]) {
        LOG("cannot start: incomplete config local=[%s]:%u remote=[%s]:%u",
            ev->local_ip, ev->local_port, ev->remote_ip, ev->remote_port);
        return;
    }

    struct hakios_imsrtp_route rt = {0};
    rt.active = 1;
    rt.ip_version = (ev->ip_version == 6 || strchr(ev->local_ip, ':') != NULL) ? 6u : 4u;
    rt.local_port = ev->local_port;
    rt.remote_port = ev->remote_port;
    rt.remote_rtcp = ev->remote_rtcp ? ev->remote_rtcp : (uint16_t)(ev->remote_port + 1u);
    int af = (rt.ip_version == 6u) ? AF_INET6 : AF_INET;
    if (inet_pton(af, ev->local_ip, rt.local_ip) != 1 ||
        inet_pton(af, ev->remote_ip, rt.remote_ip) != 1) {
        LOG("bad IP addresses local='%s' remote='%s'", ev->local_ip, ev->remote_ip);
        return;
    }

    g_call.data_fd = open(HAKIOS_IMSRTP_DATA_DEVICE, O_RDWR);
    if (g_call.data_fd < 0) {
        LOG("open %s failed: %s", HAKIOS_IMSRTP_DATA_DEVICE, strerror(errno));
        return;
    }
    if (ioctl(g_call.data_fd, HAKIOS_IMSRTP_SET_ROUTE, &rt) < 0) {
        LOG("ioctl SET_ROUTE failed: %s", strerror(errno));
        close(g_call.data_fd);
        g_call.data_fd = -1;
        return;
    }

    g_call.audio_fd = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
    if (g_call.audio_fd < 0) LOG("open %s failed: %s", HAKIOS_AUDIO_DEVICE, strerror(errno));
    g_call.mic_fd = open("/dev/hakios-mic", O_RDONLY);
    if (g_call.mic_fd < 0) LOG("open /dev/hakios-mic failed: %s", strerror(errno));

    g_call.running = 1;
    if (pthread_create(&g_call.rx_tid, NULL, rx_thread_fn, &g_call) == 0) g_call.rx_started = 1;
    if (pthread_create(&g_call.tx_tid, NULL, tx_thread_fn, &g_call) == 0) g_call.tx_started = 1;
    LOG("ACTIVE VoLTE RTP via %s: [%s]:%u <-> [%s]:%u codec=%u rate=%u pt=%u/%u oa=%u",
        HAKIOS_IMSRTP_DATA_DEVICE, ev->local_ip, ev->local_port, ev->remote_ip, ev->remote_port,
        ev->codec, ev->clock_rate, ev->rx_pt, ev->tx_pt, ev->octet_align);
}

static int run_selftest(void) {
    int16_t in_nb[160], out_nb[160], in_wb[320], out_wb[320];
    uint8_t st[64], rtp_pay[64], st2[64];
    for (int oa = 0; oa <= 1; oa++) {
        void *nb_e = Encoder_Interface_init(0), *nb_d = Decoder_Interface_init();
        void *wb_e = E_IF_init(), *wb_d = D_IF_init();
        int sl = 0, peak = 0, match = 1;
        for (int f = 0; f < 5; f++) {
            for (int i = 0; i < 160; i++)
                in_nb[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * (f * 160 + i) / 8000.0));
            sl = Encoder_Interface_Encode(nb_e, MR122, in_nb, st, 0);
            size_t pl = rfc4867_pack(st, (size_t)sl, false, oa != 0, rtp_pay, sizeof(rtp_pay));
            uint8_t ft = 15;
            size_t ul = rfc4867_unpack(rtp_pay, pl, false, oa != 0, st2, sizeof(st2), &ft);
            if (ul != (size_t)sl || memcmp(st, st2, (size_t)sl) != 0) match = 0;
            Decoder_Interface_Decode(nb_d, st2, out_nb, 0);
            for (int i = 0; i < 160; i++) { int v = abs(out_nb[i]); if (v > peak) peak = v; }
            if (f == 4) LOG("selftest AMR-NB oa=%d sl=%d pl=%zu ul=%zu ft=%u match=%d peak=%d", oa, sl, pl, ul, ft, match, peak);
        }
        peak = 0; match = 1;
        for (int f = 0; f < 5; f++) {
            for (int i = 0; i < 320; i++)
                in_wb[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 440.0 * (f * 320 + i) / 16000.0));
            sl = E_IF_encode(wb_e, 8, in_wb, st, 0);
            size_t pl = rfc4867_pack(st, (size_t)sl, true, oa != 0, rtp_pay, sizeof(rtp_pay));
            uint8_t ft = 15;
            size_t ul = rfc4867_unpack(rtp_pay, pl, true, oa != 0, st2, sizeof(st2), &ft);
            if (ul != (size_t)sl || memcmp(st, st2, (size_t)sl) != 0) match = 0;
            D_IF_decode(wb_d, st2, out_wb, 0);
            for (int i = 0; i < 320; i++) { int v = abs(out_wb[i]); if (v > peak) peak = v; }
            if (f == 4) LOG("selftest AMR-WB oa=%d sl=%d pl=%zu ul=%zu ft=%u match=%d peak=%d", oa, sl, pl, ul, ft, match, peak);
        }
        Encoder_Interface_exit(nb_e); Decoder_Interface_exit(nb_d);
        E_IF_exit(wb_e); D_IF_exit(wb_d);
    }
    return 0;
}

static int run_tone_test(void) {
    int fd = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
    if (fd < 0) {
        LOG("tone: open %s failed: %s", HAKIOS_AUDIO_DEVICE, strerror(errno));
        return 1;
    }
    int16_t buf[960 * 2];
    int total_frames = 0;
    uint64_t next = now_us();
    for (int chunk = 0; chunk < 50; chunk++) {
        for (int i = 0; i < 960; i++) {
            double t = (double)(chunk * 960 + i) / 48000.0;
            int16_t s = (int16_t)(16000.0 * sin(2.0 * M_PI * 523.25 * t));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        ssize_t w = write(fd, buf, sizeof(buf));
        if (w > 0) total_frames += (int)(w / HAKIOS_AUDIO_FRAME);
        next += 20000ULL;
        uint64_t now = now_us();
        if (next > now) usleep((useconds_t)(next - now));
    }
    close(fd);
    LOG("tone: wrote %d frames (%.2f s) to %s", total_frames, total_frames / 48000.0, HAKIOS_AUDIO_DEVICE);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) return run_selftest();
    if (argc >= 2 && strcmp(argv[1], "tone") == 0) return run_tone_test();

    signal(SIGPIPE, SIG_IGN);
    memset(&g_call, 0, sizeof(g_call));
    g_call.data_fd = g_call.audio_fd = g_call.mic_fd = -1;
    pthread_mutex_init(&g_call.dtmf_mtx, NULL);

    int dev = -1;
    for (int tries = 0; tries < 60 && dev < 0; tries++) {
        dev = open(HAKIOS_IMSRTP_DEVICE, O_RDONLY);
        if (dev < 0) sleep(1);
    }
    if (dev < 0) {
        LOG("cannot open %s: %s (non-cellular boot?)", HAKIOS_IMSRTP_DEVICE, strerror(errno));
        return 0;
    }
    LOG("listening on %s", HAKIOS_IMSRTP_DEVICE);

    struct hakios_imsrtp_event latest = {0};
    for (;;) {
        struct hakios_imsrtp_event ev;
        ssize_t r = read(dev, &ev, sizeof(ev));
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG("read %s failed: %s", HAKIOS_IMSRTP_DEVICE, strerror(errno));
            usleep(200000);
            continue;
        }
        if (r != (ssize_t)sizeof(ev)) continue;

        switch (ev.kind) {
        case HAKIOS_IMSRTP_INIT:
            latest = ev;
            LOG("event INIT sid=%u local=[%s]:%u", ev.session_id, ev.local_ip, ev.local_port);
            break;
        case HAKIOS_IMSRTP_CONFIG:
            latest = ev;
            LOG("event CONFIG sid=%u [%s]:%u <-> [%s]:%u codec=%u rate=%u pt=%u/%u oa=%u",
                ev.session_id, ev.local_ip, ev.local_port, ev.remote_ip, ev.remote_port,
                ev.codec, ev.clock_rate, ev.rx_pt, ev.tx_pt, ev.octet_align);
            if (g_call.running) call_start(&latest);
            break;
        case HAKIOS_IMSRTP_START:
            if (ev.remote_port) latest = ev;
            else latest.direction = ev.direction;
            call_start(&latest);
            break;
        case HAKIOS_IMSRTP_STOP:
        case HAKIOS_IMSRTP_UNINIT:
            call_stop();
            break;
        case HAKIOS_IMSRTP_DTMF:
            pthread_mutex_lock(&g_call.dtmf_mtx);
            g_call.dtmf_digit = ev.dtmf_digit;
            g_call.dtmf_dur = ev.dtmf_duration ? ev.dtmf_duration : 160;
            g_call.dtmf_vol = ev.dtmf_volume ? ev.dtmf_volume : 10;
            g_call.dtmf_pending = 1;
            pthread_mutex_unlock(&g_call.dtmf_mtx);
            break;
        default:
            break;
        }
    }
    return 0;
}
