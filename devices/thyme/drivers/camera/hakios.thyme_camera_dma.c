/* SM8250 IFE-lite48x diagnostic capture. Board-local, never a default boot path.
 * A 320x240 CSID480 test pattern validates bus-v3 and the UMI IFE SIDs.
 * Sensor streaming and high-rate clocks remain outside this diagnostic.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern uint64_t loader_recording[3];
extern int hakios_camera_picture_staged;
extern lck_mtx_t hakios_smmu_lock;
extern void hakios_tl(const char *, ...) __printflike(1, 2);
int umi_camera_tpg(vm_offset_t, vm_offset_t);
void umi_camera_dma_quiesce(bool);

#define UD_IOVA 0x40000000ull
#define UD_SID 0x800u
#define UD_MASK 0x440u /* DT data writers 800/840, aliases c00/c40; no odd streams */
#define UD_FSR_FAULT 0xc00001feu
#define UD_WIDTH 320u
#define UD_HEIGHT 240u
#define UD_STRIDE 400u
#define UD_BYTES (UD_STRIDE*UD_HEIGHT)
static struct {
	vm_offset_t ife, csid, smmu, gr1, cb, memory, tables;
	vm_size_t size;
	unsigned smr, context, installed;
	unsigned slots[2];
	uint32_t old_smrs[2], old_s2crs[2];
	uint32_t old_smr, old_s2cr, old_cbar, old_cba2r, old_cb[9];
	bool mapped, running, context_changed;
	uint64_t frame_bytes;   /* one frame; the buffer holds two, the WM's ping-pong */
	unsigned bank;          /* which of them the WM was last given */
} ud;
static const unsigned ud_cbregs[9] = {0,0x10,0x20,0x24,0x28,0x2c,0x30,0x38,0x3c};
struct ud_header {
	char magic[8];
	uint32_t width,height,stride,format,bayer,frames,frame_bytes,source;
	char sensor[16];
	uint32_t last;
};
static uint32_t
dr(vm_offset_t p, unsigned o)
{
	uint32_t v=*(volatile uint32_t *)(p+o);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}
static void
dw(vm_offset_t p,unsigned o,uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(p+o)=v;
	__asm__ volatile("dsb osh" ::: "memory");
}
static void
dcache(vm_offset_t p,vm_size_t size)
{
	for(vm_offset_t end=p+size;p<end;p+=64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}
static bool
dsync(void)
{
	dw(ud.cb,0x610,ud.context); dw(ud.cb,0x7f0,1);
	for(unsigned us=0;us<1000;us++) {
		if(!(dr(ud.cb,0x7f4)&1))return true;
		delay(1);
	}
	return false;
}

static int
dmap(void)
{
	/* Only a free match and an unused, disabled, fault-free context qualify.
	 * The same mutex serializes the board's other SMMU clients.
	 */
	if(!ud.smmu)ud.smmu=ml_io_map(0x15000000,0x1000);
	if(!ud.smmu)return ENOMEM;
	uint32_t scr0=dr(ud.smmu,0),id0=dr(ud.smmu,0x20),id1=dr(ud.smmu,0x24);
	unsigned page=(id1>>31)?0x10000:0x1000, pages=2u<<((id1>>28)&7);
	unsigned matches=id0&255, contexts=id1&255;
	hakios_tl("UMI camera DMA: SMMU IDR0=%#x IDR1=%#x matches=%u contexts=%u\n",id0,id1,matches,contexts);
	if(!(id0&(1u<<27))||(scr0&(1u<<3))||!matches||matches>128||!contexts||contexts>128||contexts>pages||2ull*page*pages!=0x100000)return ENODEV;
	if(!ud.gr1)ud.gr1=ml_io_map(0x15000000+page,0x1000);
	if(!ud.gr1)return ENOMEM;
	bool used[128]={false};
	unsigned free_match=~0u, available=0;
	for(unsigned i=0;i<matches;i++) {
		uint32_t smr=dr(ud.smmu,0x800+4*i),s2cr=dr(ud.smmu,0xc00+4*i);
		if(smr&(1u<<31)) {
			if((s2cr&255)<128)used[s2cr&255]=true;
			if(!(((smr&0x7fff)^UD_SID)&~(((smr>>16)&0x7fff)|UD_MASK)))return EBUSY;
		} else if(available<2) {ud.slots[available++]=i;if(free_match==~0u)free_match=i;}
	}
	if(available<2)return EBUSY;
	unsigned cb;
	for(cb=0;cb<contexts;cb++) {
		if(used[cb])continue;
		vm_offset_t mapped=ml_io_map(0x15000000+(uint64_t)(pages+cb)*page,0x1000);
		if(!mapped)return ENOMEM;
		if(!(dr(mapped,0)&1)&&!(dr(mapped,0x58)&UD_FSR_FAULT)) {ud.cb=mapped;break;}
	}
	if(cb==contexts)return EBUSY;
	ud.context=cb;ud.smr=free_match;
	ud.old_smr=dr(ud.smmu,0x800+4*free_match);
	ud.old_s2cr=dr(ud.smmu,0xc00+4*free_match);
	for(unsigned i=0;i<2;i++) {
		ud.old_smrs[i]=dr(ud.smmu,0x800+4*ud.slots[i]);
		ud.old_s2crs[i]=dr(ud.smmu,0xc00+4*ud.slots[i]);
	}
	ud.old_cbar=dr(ud.gr1,4*cb);ud.old_cba2r=dr(ud.gr1,0x800+4*cb);
	for(unsigned i=0;i<9;i++)ud.old_cb[i]=dr(ud.cb,ud_cbregs[i]);
	hakios_tl("UMI camera DMA: selected CB%u at %#lx SMR%u old CBAR=%#x CBA2R=%#x SCTLR=%#x FSR=%#x\n",cb,(unsigned long)(0x15000000+(uint64_t)(pages+cb)*page),free_match,ud.old_cbar,ud.old_cba2r,ud.old_cb[0],dr(ud.cb,0x58));
	/* The IOVA window is 0x40000000 and may span more than one 2 MB L2
	 * entry: one L3 table per 2 MB, allocated after the L1 and L2 tables.
	 */
	unsigned npages=(unsigned)(ud.size/4096),chunks=(npages+511)/512;
	vm_size_t tables=8192+4096ull*chunks;
	if(!ud.tables&&kmem_alloc(kernel_map,&ud.tables,tables,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
	uint64_t *l1=(uint64_t *)ud.tables,*l2=(uint64_t *)(ud.tables+4096);
	l1[(UD_IOVA>>30)&511]=kvtophys((vm_offset_t)l2)|3;
	for(unsigned c=0;c<chunks;c++){
		uint64_t *l3=(uint64_t *)(ud.tables+8192+4096ull*c);
		l2[((UD_IOVA>>21)+c)&511]=kvtophys((vm_offset_t)l3)|3;
		for(unsigned i=0;i<512&&c*512+i<npages;i++)
			l3[i]=kvtophys(ud.memory+4096ull*(c*512+i))|3ull|(1ull<<6)|(2ull<<8)|(1ull<<10)|(1ull<<11);
	}
	dcache(ud.tables,tables);
	ud.context_changed=true;
	dw(ud.cb,0,0);dw(ud.gr1,0x800+4*cb,1);dw(ud.gr1,4*cb,1u<<16|3u<<8|15u<<12);
	dw(ud.cb,0x10,1u|1u<<4|7u<<15);dw(ud.cb,0x30,25u|1u<<8|1u<<10|3u<<12|1u<<23);
	uint64_t ttbr=kvtophys(ud.tables)|(uint64_t)cb<<48;
	dw(ud.cb,0x20,(uint32_t)ttbr);dw(ud.cb,0x24,(uint32_t)(ttbr>>32));
	dw(ud.cb,0x28,0);dw(ud.cb,0x2c,0);dw(ud.cb,0x38,0xf404ff44);dw(ud.cb,0x3c,0);
	/* Prove the translation configuration before enabling the context or
	 * exposing a DMA stream. A stale TTBR must never become our mapping.
	 */
	if((dr(ud.gr1,4*cb)&0x30000)!=(1u<<16)||!(dr(ud.gr1,0x800+4*cb)&1)||
	    (dr(ud.cb,0x10)&0x38017)!=(1u|1u<<4|7u<<15)||
	    dr(ud.cb,0x30)!=(25u|1u<<8|1u<<10|3u<<12|1u<<23)||
	    dr(ud.cb,0x20)!=(uint32_t)ttbr||dr(ud.cb,0x24)!=(uint32_t)(ttbr>>32)||
	    dr(ud.cb,0x28)||dr(ud.cb,0x2c)||dr(ud.cb,0x38)!=0xf404ff44||dr(ud.cb,0x3c)) {
		hakios_tl("UMI camera DMA: context translation registers did not read back; refusing DMA\n");
		return EIO;
	}
	if(!dsync())return ETIMEDOUT;
	dw(ud.cb,0,1u|1u<<1|1u<<2|1u<<5|1u<<8|1u<<12);
	hakios_tl("UMI camera DMA: CB%u enable readback SCTLR=%#x TCR=%#x TCR2=%#x TTBR=%#x:%08x FSR=%#x\n",cb,dr(ud.cb,0),dr(ud.cb,0x30),dr(ud.cb,0x10),dr(ud.cb,0x24),dr(ud.cb,0x20),dr(ud.cb,0x58));
	if(!(dr(ud.cb,0)&1))return EIO;
	/* Use only DT data writers 800/840 (and their bit10 aliases).
	 * The CPU configures this capture directly; it does not use the odd
	 * command streams. UMI accepted 800 but rejected the 801 SMR write.
	 */
	static const unsigned sids[2]={0x800,0x840};
	ud.mapped=true;
	for(unsigned i=0;i<2;i++) {
		unsigned slot=ud.slots[i];
		uint32_t wanted=1u<<31|0x400u<<16|sids[i], route=cb|(3u<<8);
		ud.installed=i+1; /* rollback includes a partially written slot */
		dw(ud.smmu,0xc00+4*slot,route); /* v500 non-shareable output */
		dw(ud.smmu,0x800+4*slot,wanted);
		uint32_t smr_back=dr(ud.smmu,0x800+4*slot),s2cr_back=dr(ud.smmu,0xc00+4*slot);
		hakios_tl("UMI camera DMA: route SMR%u requested %#x read %#x; S2CR requested %#x read %#x\n",slot,wanted,smr_back,route,s2cr_back);
		if(smr_back!=wanted||(s2cr_back&0x300ff)!=cb)return EIO;
	}
	hakios_tl("UMI camera DMA: context %u match %u, SID %#x mask %#x, %lu bytes\n",cb,free_match,UD_SID,UD_MASK,(unsigned long)ud.size);
	return 0;
}

void
umi_camera_dma_quiesce(bool panic)
{
	if(ud.running) {
		dw(ud.csid,0x600,0); /* TPG first, then the receiver and writer. */
		dw(ud.csid,0x208,2);dw(ud.csid,0x200,dr(ud.csid,0x200)&~(1u<<31));
		dw(ud.ife,0x1c00,0);dw(ud.ife,0x20,1);delay(1000);
		dw(ud.ife,0x1a18,0);dw(ud.ife,0x1a20,1u<<4);dw(ud.ife,0x1a30,1);
		dw(ud.csid,0x100,0);ud.running=false;
	}
	if(!ud.context_changed)return;
	/* Panic has stopped the other CPUs; never block on their mutex. Only
	 * this client's match/context are changed, not global SMMU controls.
	 */
	if(!panic)lck_mtx_lock(&hakios_smmu_lock);
	uint32_t fault=dr(ud.cb,0x58)&UD_FSR_FAULT;
	if(fault)hakios_tl("UMI camera DMA: context fault %#x FAR=%#x:%08x syndrome=%#x\n",fault,dr(ud.cb,0x64),dr(ud.cb,0x60),dr(ud.cb,0x68));
	if(ud.mapped) {
		for(unsigned n=ud.installed;n;n--) {
			unsigned i=n-1;
			dw(ud.smmu,0x800+4*ud.slots[i],ud.old_smrs[i]);
			dw(ud.smmu,0xc00+4*ud.slots[i],ud.old_s2crs[i]);
		}
		ud.installed=0;ud.mapped=false;
	}
	dw(ud.cb,0,0);
	if(!dsync())hakios_tl("UMI camera DMA: cleanup TLB synchronization timed out\n");
	if(fault)dw(ud.cb,0x58,fault);
	for(unsigned i=1;i<9;i++)dw(ud.cb,ud_cbregs[i],ud.old_cb[i]);
	dw(ud.gr1,4*ud.context,ud.old_cbar);dw(ud.gr1,0x800+4*ud.context,ud.old_cba2r);
	dw(ud.cb,0,ud.old_cb[0]);ud.context_changed=false;
	if(!panic)lck_mtx_unlock(&hakios_smmu_lock);
}

int
umi_camera_tpg(vm_offset_t ife,vm_offset_t csid)
{
	if(ud.memory)return EBUSY; /* one retained capture per diagnostic boot */
	ud.ife=ife;ud.csid=csid;ud.size=round_page(4096+UD_BYTES);
	if(kmem_alloc_contig(kernel_map,&ud.memory,ud.size,PAGE_MASK,0,0,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
	struct ud_header *h=(struct ud_header *)ud.memory;
	memcpy(h->magic,"RWCAM001",8);h->width=UD_WIDTH;h->height=UD_HEIGHT;h->stride=UD_STRIDE;
	h->format=1;h->bayer=0;h->frame_bytes=UD_BYTES;h->source=0;strlcpy(h->sensor,"UMI CSID TPG",sizeof(h->sensor));
	memset((void *)(ud.memory+4096),0xa5,UD_BYTES);dcache(ud.memory,ud.size);
	lck_mtx_lock(&hakios_smmu_lock);int error=dmap();lck_mtx_unlock(&hakios_smmu_lock);
	if(error){umi_camera_dma_quiesce(false);return error;}
	/* CSID480 RAW10 RDI0, internal test generator; no PHY is selected. */
	ud.running=true;
	dw(csid,0x100,3u|0x3210u<<4);dw(csid,0x104,1u<<6|1u);
	dw(csid,0x200,0x2bu<<16|15u<<12|1u<<2|3u);dw(csid,0x204,2);
	static const unsigned rdi_regs[]={0x20c,0x210,0x214,0x218,0x224,0x228,0x22c,0x230};
	for(unsigned i=0;i<8;i++)dw(csid,rdi_regs[i],i&1?1:0);
	dw(csid,0x200,1u<<31|0x2bu<<16|15u<<12|1u<<2|3u);dw(csid,0x208,1);
	/* VFE bus-v3 frame mode, RDI0 at 1c00, completion group bit4. */
	dw(ife,0x1a08,0x3ffffff);dw(ife,0x1a34,0);dw(ife,0x1adc,0);
	dw(ife,0x1c64,15);dw(ife,0x1c0c,0xffff);dw(ife,0x1c10,0);dw(ife,0x1c14,UD_STRIDE);dw(ife,0x1c18,0);
	/* Both frame-address banks: the WM alternates between them, and with the
	 * second one left at 0 the IFE wrote one frame and stopped, so every later
	 * read returned the same (often the first, often black) frame.
	 */
	dw(ife,0x1c04,(uint32_t)(UD_IOVA+4096));dw(ife,0x1c08,(uint32_t)(UD_IOVA+4096));
	dw(ife,0x1c30,0);dw(ife,0x1c34,1);dw(ife,0x1c38,0);dw(ife,0x1c3c,1);
	dw(ife,0x1a20,1u<<4);dw(ife,0x1a30,1);dw(ife,0x1a18,1u<<4);
	dw(ife,0x1c00,0x10001);dw(ife,0x20,1);
	dw(csid,0x604,1u<<10);dw(csid,0x608,0x3ffu<<12|0x740);
	dw(csid,0x60c,0x12345678);dw(csid,0x610,UD_WIDTH<<16|UD_HEIGHT);
	dw(csid,0x614,0x2b);dw(csid,0x618,2u<<16|8);dw(csid,0x640,1u<<5);dw(csid,0x648,0);
	dw(csid,0x600,4u<<20|0x80u<<8|3u<<4|7u);
	bool done=false;
	for(unsigned ms=0;ms<2000;ms+=10){
		delay_for_interval(10,NSEC_PER_MSEC);
		if(dr(ife,0x1a28)&(1u<<4)){done=true;break;}
		if(dr(ud.cb,0x58)&UD_FSR_FAULT)break;
	}
	hakios_tl("UMI camera DMA: TPG done=%u IFE IRQ=%#x/%#x bus=%#x/%#x CSID RDI=%#x status=%#x size=%#x/%#x\n",done,dr(ife,0x40),dr(ife,0x44),dr(ife,0x1a28),dr(ife,0x1a2c),dr(csid,0x30),dr(csid,0x250),dr(ife,0x1a68),dr(ife,0x1a70));
	bool faulted=(dr(ud.cb,0x58)&UD_FSR_FAULT)!=0;
	hakios_tl("UMI camera DMA: SMMU global fault=%#x stream=%#x\n",dr(ud.smmu,0x48),dr(ud.smmu,0x54));
	umi_camera_dma_quiesce(false);dcache(ud.memory,ud.size);
	const uint8_t *frame=(const uint8_t *)(ud.memory+4096);
	bool changed=false,varied=false;
	for(unsigned i=0;i<UD_BYTES;i++){changed|=frame[i]!=0xa5;varied|=frame[i]!=frame[0];}
	bool guards=!memcmp(h->magic,"RWCAM001",8)&&h->width==UD_WIDTH&&h->height==UD_HEIGHT&&h->stride==UD_STRIDE;
	for(unsigned i=4096+UD_BYTES;i<ud.size;i++)guards&=((const uint8_t *)ud.memory)[i]==0;
	h->frames=done&&changed&&varied&&guards&&!faulted?1:0;
	hakios_tl("UMI camera DMA: buffer guards=%u context fault=%u\n",guards,faulted);
	hakios_tl("UMI camera DMA: frame changed=%u varied=%u frames=%u first=%02x %02x %02x %02x\n",changed,varied,h->frames,frame[0],frame[1],frame[2],frame[3]);
	dcache(ud.memory,4096);
	loader_recording[0]=kvtophys(ud.memory);loader_recording[1]=ud.size;loader_recording[2]=ud.memory;
	return h->frames?0:EIO;
}

/* Real sensor stream: CSID RX from CSIPHY `phy' (not the generator), RAW10
 * RDI0, IFE bus-v3 writing one frame into the retained buffer. The caller
 * starts the sensor afterwards and then calls stream_wait/stop.
 */
int
umi_camera_stream_arm(vm_offset_t ife,vm_offset_t csid,unsigned width,unsigned height,unsigned stride,
    unsigned lanes,unsigned lane_cfg,unsigned phy,unsigned cphy,unsigned vc,unsigned dt,unsigned bayer,const char *sensor)
{
	if(!width||!height||!stride||lanes<1||lanes>4)return EINVAL;
	uint64_t bytes=(uint64_t)stride*height;
	if(bytes>32ull<<20)return EINVAL;
	/* Two frames: the WM takes one buffer address per frame, so the ring
	 * alternates between two so that each read can start the next frame.
	 */
	vm_size_t need=round_page(4096+(vm_size_t)bytes*2);
	if(ud.memory){
		/* The boot-time reserve is the largest mode (the front's 2592x1940);
		 * a session must not shrink it, or the next larger sensor finds the
		 * buffer "too small" although the memory is the full reserve.
		 */
		if(ud.size<need)return ENOMEM; /* a reserved buffer that is too small */
	}else if(kmem_alloc_contig(kernel_map,&ud.memory,need,PAGE_MASK,0,0,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return ENOMEM;
	else ud.size=need;
	ud.ife=ife;ud.csid=csid;
	struct ud_header *h=(struct ud_header *)ud.memory;
	memcpy(h->magic,"RWCAM001",8);h->width=width;h->height=height;h->stride=stride;
	h->format=1;h->bayer=bayer;h->frame_bytes=(uint32_t)bytes;h->source=1;
	strlcpy(h->sensor,sensor,sizeof(h->sensor));
	memset((void *)(ud.memory+4096),0xa5,(vm_size_t)bytes*2);dcache(ud.memory,ud.size);
	ud.frame_bytes=bytes;ud.bank=0;
	lck_mtx_lock(&hakios_smmu_lock);int error=dmap();lck_mtx_unlock(&hakios_smmu_lock);
	if(error){umi_camera_dma_quiesce(false);return error;}
	ud.running=true;
	/* CSID480 RX from the PHY: lane count and assignment, C-PHY, physical
	 * PHY index in bits 22:20 (three bits: CSIPHY 4 is input 4), RAW10
	 * vc/dt in the RDI0 configuration.
	 */
	dw(csid,0x100,(lanes-1)|lane_cfg<<4|(cphy?1u:0u)<<24|(phy&7u)<<20);
	dw(csid,0x104,1u<<6|1u);
	uint32_t cfg0=vc<<22|dt<<16|0xfu<<12|1u<<2|3u;
	dw(csid,0x200,cfg0);dw(csid,0x204,2);
	static const unsigned rdi_regs[]={0x20c,0x210,0x214,0x218,0x224,0x228,0x22c,0x230};
	for(unsigned i=0;i<8;i++)dw(csid,rdi_regs[i],i&1?1:0);
	dw(csid,0x200,1u<<31|cfg0);dw(csid,0x208,1);
	dw(csid,0x34,1u<<1|1u<<2|1u<<3|1u<<4|1u<<5|1u<<6);
	/* VFE bus-v3 frame mode, RDI0 at 1c00, completion group bit4. */
	dw(ife,0x1a08,0x3ffffff);dw(ife,0x1a34,0);dw(ife,0x1adc,0);
	dw(ife,0x1c64,15);dw(ife,0x1c0c,0xffff);dw(ife,0x1c10,0);dw(ife,0x1c14,stride);dw(ife,0x1c18,0);
	dw(ife,0x1c08,0);dw(ife,0x1c30,0);dw(ife,0x1c34,1);dw(ife,0x1c38,0);dw(ife,0x1c3c,1);
	dw(ife,0x1a20,1u<<4);dw(ife,0x1a30,1);dw(ife,0x1a18,1u<<4);
	dw(ife,0x1c04,(uint32_t)(UD_IOVA+4096));dw(ife,0x1c00,0x10001);dw(ife,0x20,1);
	hakios_tl("UMI camera stream: armed %ux%u stride %u, %u lane(s) cfg %#x phy %u vc %u dt %#x, %lu bytes\n",
	    width,height,stride,lanes,lane_cfg,phy,vc,dt,(unsigned long)bytes);
	return 0;
}

bool
umi_camera_stream_wait(unsigned ms)
{
	bool done=false;
	for(unsigned waited=0;waited<ms;waited+=10){
		delay_for_interval(10,NSEC_PER_MSEC);
		if(dr(ud.ife,0x1a28)&(1u<<4)){done=true;break;}
		if(dr(ud.cb,0x58)&UD_FSR_FAULT)break;
	}
	return done;
}

/* Arm the next frame: clear the frame-done and give the WM its addresses again
 * (it consumes one address per frame, as the vendor's per-buffer writes do).
 */
void
umi_camera_stream_rearm(void)
{
	if(!ud.memory||!ud.running)return;
	dw(ud.ife,0x1a30,1u<<4);
	dw(ud.ife,0x1c04,(uint32_t)(UD_IOVA+4096));
	dw(ud.ife,0x1c08,(uint32_t)(UD_IOVA+4096));
}

/* The next frame goes to the other bank; the caller reads the one that was
 * armed before, whose frame the WM has written by now.
 */
void
umi_camera_stream_advance(void)
{
	if(!ud.memory||!ud.running||!ud.frame_bytes)return;
	ud.bank^=1;
	dw(ud.ife,0x1c04,(uint32_t)(UD_IOVA+4096+ud.bank*ud.frame_bytes));
}

/* The buffer's current level (8-bit bytes, every 8th): what the sensor is
 * exposing now, for the mid-stream exposure test.
 */
uint32_t
umi_camera_stream_mean(void)
{
	if(!ud.memory)return 0;
	struct ud_header *h=(struct ud_header *)ud.memory;
	uint32_t n=h->frame_bytes;
	if(!n)return 0;
	dcache(ud.memory,ud.size);
	const uint8_t *f=(const uint8_t *)(ud.memory+4096+ud.bank*ud.frame_bytes);
	uint64_t sum=0;uint32_t count=0;
	for(uint32_t i=0;i<n;i+=8){sum+=f[i];count++;}
	return count?(uint32_t)(sum/count):0;
}

void
umi_camera_stream_report(const char *when)
{
	hakios_tl("UMI camera stream %s: IFE IRQ=%#x/%#x bus=%#x/%#x CSID rx=%#x pkts=%u ecc=%#x crc=%u RDI=%#x status=%#x bytes=%u/%u SMMU=%#x/%#x\n",
	    when,dr(ud.ife,0x40),dr(ud.ife,0x44),dr(ud.ife,0x1a28),dr(ud.ife,0x1a2c),dr(ud.csid,0x20),
	    dr(ud.csid,0x160),dr(ud.csid,0x164),dr(ud.csid,0x168),dr(ud.csid,0x30),dr(ud.csid,0x250),
	    dr(ud.csid,0x2e0),dr(ud.csid,0x2e4),dr(ud.smmu,0x48),dr(ud.smmu,0x54));
}

int
umi_camera_stream_stop(void)
{
	if(!ud.memory)return EINVAL;
	struct ud_header *h=(struct ud_header *)ud.memory;
	bool faulted=(dr(ud.cb,0x58)&UD_FSR_FAULT)!=0;
	umi_camera_dma_quiesce(false);dcache(ud.memory,ud.size);
	const uint8_t *frame=(const uint8_t *)(ud.memory+4096+ud.bank*ud.frame_bytes);
	uint32_t bytes=h->frame_bytes;
	bool changed=false,varied=false;
	for(uint32_t i=0;i<bytes;i++){changed|=frame[i]!=0xa5;varied|=frame[i]!=frame[0];}
	bool guards=!memcmp(h->magic,"RWCAM001",8)&&h->width&&h->height&&h->stride;
	for(vm_offset_t p=4096+(vm_offset_t)bytes*2;p<ud.size;p++)guards&=((const uint8_t *)ud.memory)[p]==0;
	unsigned frames=changed&&varied&&guards&&!faulted?1:0;
	h->frames=frames;
	hakios_tl("UMI camera stream: buffer guards=%u fault=%u changed=%u varied=%u frames=%u first=%02x %02x %02x %02x\n",
	    guards,faulted,changed,varied,frames,frame[0],frame[1],frame[2],frame[3]);
	dcache(ud.memory,4096);
	/* (a picture the program made for the host outlives this session's buffer) */
	if(!hakios_camera_picture_staged){loader_recording[0]=kvtophys(ud.memory);loader_recording[1]=ud.size;loader_recording[2]=ud.memory;}
	return frames?0:EIO;
}

extern uint64_t umi_camera_frame_ns(void);

/* A centre crop of the frame in the retained buffer, RAW10 packed, as the
 * session's own ring expects it; the crop offsets stay on the Bayer phase.
 */
static int
copy_bank(void *dst,unsigned width,unsigned height,unsigned bank)
{
	if(!ud.memory||!ud.running)return ENODEV;
	struct ud_header *h=(struct ud_header *)ud.memory;
	if(width<4||!height||width>h->width||height>h->height||width%4)return EINVAL;
	unsigned x0=((h->width-width)/2)&~3u,y0=((h->height-height)/2)&~1u;
	unsigned dstride=(width*10/8+15)&~15u;
	dcache(ud.memory,ud.size);
	const uint8_t *src=(const uint8_t *)(ud.memory+4096+bank*ud.frame_bytes)+(uint64_t)y0*h->stride+x0/4*5;
	uint8_t *out=(uint8_t *)dst;
	for(unsigned y=0;y<height;y++)
		memcpy(out+(uint64_t)y*dstride,src+(uint64_t)y*h->stride,width/4*5);
	return 0;
}

int
umi_camera_stream_copy(void *dst,unsigned width,unsigned height)
{
	return copy_bank(dst,width,height,ud.bank);
}

/* The frame for one read: the WM is sent to the other bank (it is writing the
 * one the last read armed), a whole frame is waited out, and that earlier bank
 * -- where the frame now is whole -- is copied out.
 *
 * The wait is the mode's own frame time, HTS x VTS / PCLK from the module file,
 * not a guessed number of milliseconds: mode 3 is 16032 x 2373 at 412.53 MHz,
 * 92 ms, ten and a bit frames a second (the module file's "30" is its label, not
 * its timing), and a 40 ms wait copied the bank while the WM was still writing
 * it -- the preview tore across, a line of the next frame's picture, whenever
 * the phone moved. A completion bit would be better still, but the bus block's
 * bit stays set and a poll for it returned at once: the session then read the
 * same bank a hundred times a second.
 */
int
umi_camera_stream_grab(void *dst,unsigned width,unsigned height)
{
	if(!ud.memory||!ud.running)return ENODEV;
	unsigned old=ud.bank;
	/*
	 * The wait covers the frame in the bank being copied, and that frame began
	 * before the exposure this read was asked for started: when the capture
	 * stack's AE makes the frame shorter (the room's light lets it), the bank
	 * still holds a frame of the longer length, and waiting only the new one
	 * copies it mid-write -- the tear across the picture, one frame's lines
	 * under the next's, that the phone showed while it was moved through
	 * changing light. The longer of this frame length and the last read's,
	 * then; a length that grows costs nothing here, and the one after a
	 * shrink runs at the new length.
	 */
	static uint64_t last_frame_ns;
	uint64_t frame_ns=umi_camera_frame_ns();
	if(frame_ns<last_frame_ns)frame_ns=last_frame_ns;
	last_frame_ns=umi_camera_frame_ns();
	unsigned wait_ms=(unsigned)(frame_ns/NSEC_PER_MSEC)+(frame_ns?4:40);

	umi_camera_stream_advance();
	delay_for_interval(wait_ms,NSEC_PER_MSEC);
	return copy_bank(dst,width,height,old);
}

/* Take the frame buffer while the system's memory is still in large pieces:
 * a cam=5 session starts after userspace is up, when a 2.6 MB contiguous
 * allocation may no longer succeed.
 */
int
umi_camera_dma_reserve(unsigned width,unsigned height)
{
	if(ud.memory)return 0;
	if(!width||!height)return EINVAL;
	unsigned stride=(width*10/8+15)&~15u;
	vm_size_t size=round_page(4096+(vm_size_t)stride*height*2);
	if(kmem_alloc_contig(kernel_map,&ud.memory,size,PAGE_MASK,0,0,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS){
		ud.memory=0;
		return ENOMEM;
	}
	ud.size=size;
	hakios_tl("UMI camera DMA: reserved %lu bytes for a %ux%u frame\n",(unsigned long)size,width,height);
	return 0;
}
