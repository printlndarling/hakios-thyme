/* SPDX-License-Identifier: BSD-2-Clause
 * Portable NAS 0043 -> Apple NAS 5556 LTE translator. Wire evidence and limits:
 * LOCATION-umi.md, Implementation (cell info). No allocation, casts to packed
 * structs, kernel dependencies, or logging of the private payload.
 */
#ifndef UMI_CELLINFO_QMI_H
#define UMI_CELLINFO_QMI_H
#include <stdint.h>
#include <string.h>
#define UCI_SERVICE 3u
#define UCI_PORT 0x4201u
#define UCI_GET 0x5556u
#define UCI_LOCATION 0x0043u
#define UCI_MAX_MEASUREMENTS 32u
#define UCI_TLVS_MAX 1400u
static inline uint16_t uci_r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t uci_r32(const uint8_t *p) { return (uint32_t)uci_r16(p) | (uint32_t)uci_r16(p+2) << 16; }
static inline void uci_w16(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void uci_w32(uint8_t *p, uint32_t v) { uci_w16(p,v); uci_w16(p+2,v>>16); }
/* Strict envelope, including unknown TLVs, duplicate TLVs and trailing bytes. */
static inline int uci_valid(const uint8_t *p, uint32_t n)
{
    if (n < 7 || uci_r16(p+5) != n-7) return 0;
    uint8_t seen[32] = {};
    for (uint32_t at=7; at<n;) {
        if (n-at < 3) return 0;
        uint32_t type=p[at], len=uci_r16(p+at+1);
        if ((seen[type/8] & (1u << (type%8))) || len > n-at-3) return 0;
        seen[type/8] |= (uint8_t)(1u << (type%8)); at += 3+len;
    }
    return 1;
}
/* Call only on a valid envelope. */
static inline const uint8_t *uci_tlv(const uint8_t *p, uint32_t n, uint8_t type, uint32_t *len)
{
    *len=0;
    for (uint32_t at=7; at<n; at+=3+uci_r16(p+at+1))
        if (p[at]==type) { *len=uci_r16(p+at+1); return p+at+3; }
    return 0;
}
static inline int uci_result(const uint8_t *p, uint32_t n)
{
    uint32_t len; const uint8_t *v=uci_tlv(p,n,2,&len);
    if (!v || len!=4 || uci_r16(v)>1) return -1;
    if (!uci_r16(v)) return uci_r16(v+2) ? -1 : 0;
    return uci_r16(v+2) ? (int)uci_r16(v+2) : -1;
}
struct uci_measurement { uint16_t pci; int16_t rsrp, rsrq; };
struct uci_lte {
    uint16_t mcc, mnc, tac, pci;
    uint32_t cell, earfcn;
    uint8_t count;
    struct uci_measurement measurement[UCI_MAX_MEASUREMENTS];
};
static inline int uci_plmn(const uint8_t *p, uint16_t *mcc, uint16_t *mnc)
{
    unsigned a=p[0]&15,b=p[0]>>4,c=p[1]&15,d=p[2]&15,e=p[2]>>4,f=p[1]>>4;
    if (a>9 || b>9 || c>9 || d>9 || e>9 || (f>9 && f!=15)) return 0;
    *mcc=(uint16_t)(100*a+10*b+c);
    *mnc=(uint16_t)(f==15 ? 10*d+e : 100*d+10*e+f);
    return *mcc != 0;
}
/* 1 LTE, 0 no usable LTE, -1 malformed. Result TLV is checked by caller.
 * TLV 13 is the LTE identity + intra-frequency measurements. TLV 1e is
 * its extended EARFCN; 2e is NR's extended EARFCN, NOT another LTE layout.
 */
static inline int uci_parse(const uint8_t *p, uint32_t n, struct uci_lte *out)
{
    memset(out,0,sizeof *out);
    if (!uci_valid(p,n) || p[0]!=2 || uci_r16(p+3)!=UCI_LOCATION) return -1;
    uint32_t len; const uint8_t *v=uci_tlv(p,n,0x13,&len);
    if (!v) return 0;
    if (len<19 || len!=19u+10u*v[18] || v[0]>1) return -1;
    if (!uci_plmn(v+1,&out->mcc,&out->mnc)) return 0;
    out->tac=uci_r16(v+4); out->cell=uci_r32(v+6);
    out->earfcn=uci_r16(v+10); out->pci=uci_r16(v+12);
    uint32_t extlen; const uint8_t *ext=uci_tlv(p,n,0x1e,&extlen);
    if (ext) { if (extlen!=4) return -1; out->earfcn=uci_r32(ext); }
    if (out->tac==0xffff || out->cell>0x0fffffffu || out->pci>503 || out->earfcn>262143) return 0;
    for (uint32_t i=0;i<v[18];i++) {
        const uint8_t *c=v+19+10*i;
        uint16_t pci=uci_r16(c);
        int rsrq=(int16_t)uci_r16(c+2), rsrp=(int16_t)uci_r16(c+4);
        /* Unavailable samples (e.g. -32768) are omitted, never made up. */
        if (pci==out->pci || pci>503 || rsrp < -1400 || rsrp > -440 || rsrq < -200 || rsrq > -30) continue;
        if (out->count==UCI_MAX_MEASUREMENTS) continue;
        struct uci_measurement *m=&out->measurement[out->count++];
        m->pci=pci; m->rsrp=(int16_t)(rsrp/10); m->rsrq=(int16_t)(rsrq/10);
    }
    return 1;
}
static inline uint8_t *uci_add(uint8_t *p, uint8_t type, uint32_t len)
{ p[0]=type; uci_w16(p+1,len); memset(p+3,0,len); return p+3; }
/* All four identity versions are parallel encodings of the SAME observation:
 * CC selects exactly one by personality, rather than trying older TLVs.
 * Coordinates are unavailable: the zero/zero pair is explicitly skipped by
 * locationd 20F66 at 0x10022b350..0x10022b378. This is an absent coordinate,
 * not a reported tower position.
 * Band 0xff becomes model band 0 (unknown). Other unavailable packed members
 * use all-ones, including bandwidth/deployment and V4 extension members.
 */
static inline uint32_t uci_encode(const struct uci_lte *c, int have_lte, uint8_t *out, uint32_t cap)
{
    const uint32_t sizes[4]={26,28,31,50};
    const uint8_t types[4]={0xbb,0xbc,0xd2,0xd3};
    uint32_t count=have_lte ? c->count : 0;
    uint32_t oldcount=have_lte && c->earfcn<=65535 ? count : 0;
    uint32_t need=have_lte ? 151u : 16u; /* identity TLV headers + count + records */
    need+=8u+8u*oldcount+27u*count;
    if (cap<need) return 0;
    uint8_t *p=out;
    for (unsigned version=0;version<4;version++) {
        int fits=have_lte && (version || c->earfcn<=65535);
        uint32_t len=1+(fits ? sizes[version] : 0);
        uint8_t *r=uci_add(p,types[version],len); p+=3+len;
        *r++=(uint8_t)fits;
        if (!fits) continue;
        memset(r,0xff,sizes[version]);
        uci_w16(r,0); uci_w16(r+2,c->mcc); uci_w16(r+4,c->mnc);
        if (version<2) {
            uci_w16(r+7,c->tac); uci_w32(r+9,c->cell);
            if (!version) { uci_w16(r+13,c->earfcn); uci_w16(r+15,c->pci); }
            else { uci_w32(r+13,c->earfcn); uci_w16(r+17,c->pci); }
            uci_w32(r+(version ? 19 : 17),0);
            uci_w32(r+(version ? 23 : 21),0);
        } else {
            uci_w32(r+7,c->tac); uci_w32(r+11,c->cell);
            uci_w32(r+15,c->earfcn); uci_w16(r+19,c->pci);
            uci_w32(r+21,0); uci_w32(r+25,0);
        }
    }
    /* Actual neighbors only. CC obtains serving RSRP/RSRQ from its independent
     * signal model; these neighbor records have no ECGI/PLMN to invent. */
    for (unsigned version=0;version<2;version++) {
        uint32_t num=version ? count : oldcount, stride=version ? 27 : 8;
        uint8_t *r=uci_add(p,version ? 0xd4 : 0xbf,1+stride*num); p+=4+stride*num;
        *r++=(uint8_t)num;
        for (uint32_t i=0;i<num;i++,r+=stride) {
            const struct uci_measurement *m=&c->measurement[i];
            if (version) {
                memset(r,0xff,stride); uci_w32(r,c->earfcn); uci_w16(r+4,m->pci);
                uci_w16(r+6,(uint16_t)m->rsrp); uci_w16(r+8,(uint16_t)m->rsrq);
            } else {
                uci_w16(r,c->earfcn); uci_w16(r+2,m->pci);
                uci_w16(r+4,(uint16_t)m->rsrp); uci_w16(r+6,(uint16_t)m->rsrq);
            }
        }
    }
    return (uint32_t)(p-out);
}
#endif
