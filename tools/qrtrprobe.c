/*
 * A freestanding aarch64 probe (no libc): send the SNS "registry" SUID lookup
 * to the sensor DSP over QRTR and print the raw replies. Built for Android:
 *   clang -nostdlib -static -fno-stack-protector -target aarch64-unknown-linux-gnu \
 *         -o qrtrprobe qrtrprobe.c
 * Run as root on the phone: /data/local/tmp/qrtrprobe
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long s64;

#define AF_QIPCRTR 42

static inline long
sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static void
put(const char *s)
{
	long n = 0;

	while (s[n]) {
		n++;
	}
	sc(64, 1, (long)s, n, 0, 0, 0);
}

static void
hex(u8 b)
{
	static const char h[] = "0123456789abcdef";

	sc(64, 1, (long)&h[b >> 4], 1, 0, 0, 0);
	sc(64, 1, (long)&h[b & 15], 1, 0, 0, 0);
}

static void
dump(const u8 *b, long n)
{
	for (long i = 0; i < n; i++) {
		hex(b[i]);
		put((i & 15) == 15 || i == n - 1 ? "\n" : " ");
	}
}

static u8 *
be16(u8 *p, u16 v)
{
	*p++ = (u8)v;
	*p++ = (u8)(v >> 8);
	return p;
}

struct sockaddr_qrtr {
	u16 family;
	u16 pad;
	u32 node;
	u32 port;
};

static int
lookup(const char *name)
{
	u8 proto[96], msg[160], reply[1024];
	u8 *p = proto, *q = msg;
	long fd;
	u32 n = 0, proto_len, inner, tlv;
	struct sockaddr_qrtr addr = { 0 };

	while (name[n]) {
		n++;
	}
	/* the protobuf: field 1 the 16 0xab bytes as two fixed64s, field 2 the
	 * message id (0x200 = SUID_REQ), field 3 the primary message, field 4
	 * the lookup {name, 1, 0} nested under field 2 */
	p = be16(p, 0x120a);            /* 0a 12: field 1, 18 bytes */
	*p++ = 0x09;
	for (int i = 0; i < 8; i++) {
		*p++ = 0xab;
	}
	*p++ = 0x11;
	for (int i = 0; i < 8; i++) {
		*p++ = 0xab;
	}
	*p++ = 0x15;                    /* field 2 fixed32 = 0x200 */
	*p++ = 0x00;
	*p++ = 0x02;
	*p++ = 0x00;
	*p++ = 0x00;
	*p++ = 0x1a;                    /* field 3: 08 01 10 00 */
	*p++ = 0x04;
	*p++ = 0x08;
	*p++ = 0x01;
	*p++ = 0x10;
	*p++ = 0x00;
	*p++ = 0x22;                    /* field 4: {2: {1: name, 2: 1, 3: 0}} */
	*p++ = (u8)(n + 8);
	*p++ = 0x12;
	*p++ = (u8)(n + 6);
	*p++ = 0x0a;
	*p++ = (u8)n;
	for (u32 i = 0; i < n; i++) {
		*p++ = (u8)name[i];
	}
	*p++ = 0x10;
	*p++ = 0x01;
	*p++ = 0x18;
	*p++ = 0x00;
	proto_len = (u32)(p - proto);
	/* QMI: request, txn 7, message 0x20; TLV 1 = the array; TLV 0x10 = 1 */
	*q++ = 0;
	*q++ = 7;
	*q++ = 0;
	*q++ = 0x20;
	*q++ = 0;                       /* the instance byte, 0 as the HAL sends */
	inner = proto_len;              /* the array's inner length is the protobuf itself */
	tlv = inner + 2;
	q = be16(q, (u16)(tlv + 7));    /* everything after the header: TLV 1 (3 + tlv) + TLV 0x10 (4) */
	*q++ = 0x01;
	q = be16(q, (u16)tlv);
	q = be16(q, (u16)inner);
	for (u32 i = 0; i < proto_len; i++) {
		*q++ = proto[i];
	}
	*q++ = 0x10;
	*q++ = 0x01;
	*q++ = 0x00;
	*q++ = 0x01;
	fd = sc(198 /* socket */, AF_QIPCRTR, 2 /* SOCK_DGRAM */, 0, 0, 0, 0);
	if (fd < 0) {
		put("socket failed\n");
		return 1;
	}
	addr.family = AF_QIPCRTR;
	addr.node = 1;                   /* the local node: ask for a port on it */
	{
		long r = sc(200 /* bind */, fd, (long)&addr, sizeof(addr), 0, 0, 0);

		if (r < 0) {
			put("bind failed, sending anyway (errno ");
			hex((u8)(-r));
			put(")\n");
		} else {
			put("bound\n");
		}
	}
	addr.node = 9;
	addr.port = 11;
	if (sc(206 /* sendto */, fd, (long)msg, q - msg, 0, (long)&addr, sizeof(addr)) < 0) {
		put("sendto failed\n");
		return 1;
	}
	put("sent ");
	put(name);
	put("\n");
	for (int i = 0; i < 10; i++) {
		struct { s64 s, ns; } ts = { 0, 200000000 };
		struct sockaddr_qrtr src = { 0 };
		s64 slen = sizeof(src);
		long r;

		sc(101 /* nanosleep */, (long)&ts, 0, 0, 0, 0, 0);
		r = sc(207 /* recvfrom */, fd, (long)reply, sizeof(reply), 0x40 /* MSG_DONTWAIT */, (long)&src, (long)&slen);
		if (r > 0) {
			put("reply from node ");
			hex((u8)src.node);
			put(" port ");
			hex((u8)src.port);
			put(": ");
			dump(reply, r);
		}
	}
	sc(57 /* close */, fd, 0, 0, 0, 0, 0);
	return 0;
}

void
_start(void)
{
	lookup("registry");
	lookup("accel");
	sc(93 /* exit */, 0, 0, 0, 0, 0, 0);
	for (;;) {
	}
}
