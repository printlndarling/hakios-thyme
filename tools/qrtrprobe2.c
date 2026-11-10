/*
 * Freestanding aarch64 probe, second form: registry writes/reads on demand.
 *   qrtrprobe2 [port] [node] [w|g|r] [group]
 *     w group : REGISTRY_WRITE_REQ (0x201) with three int items (heap_size...)
 *     g group : REGISTRY_WRITE_REQ (0x201) with one group item ("mag")
 *     r group : REGISTRY_READ_REQ  (0x200) for the group
 * Built: clang -nostdlib -static -fno-stack-protector -target aarch64-unknown-linux-gnu
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
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

static u32 dst_node = 9, dst_port = 11;

static const u8 registry_suid[16] = {
	0x75, 0x22, 0x1e, 0x70, 0xb4, 0x41, 0x25, 0x5e,
	0x59, 0x27, 0x7f, 0x00, 0xa7, 0x54, 0x27, 0xe1
};

static u32
num(const char *s)
{
	u32 v = 0;

	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (u32)(*s - '0');
		s++;
	}
	return v;
}

static u32
strlen_(const char *s)
{
	u32 n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

static int
same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* item {1: name, 2: fixed32 0, 13: sfixed64 value} */
static u32
item_int(u8 *out, const char *name, u64 value)
{
	u8 *p = out;
	u32 n = strlen_(name), i;

	*p++ = 0x0a;
	*p++ = (u8)n;
	for (i = 0; i < n; i++) {
		*p++ = (u8)name[i];
	}
	*p++ = 0x15;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0x69;
	for (i = 0; i < 8; i++) {
		*p++ = (u8)(value >> (8 * i));
	}
	return (u32)(p - out);
}

/* item {1: name, 2: fixed32 0, 10: {} } */
static u32
item_grp(u8 *out, const char *name)
{
	u8 *p = out;
	u32 n = strlen_(name), i;

	*p++ = 0x0a;
	*p++ = (u8)n;
	for (i = 0; i < n; i++) {
		*p++ = (u8)name[i];
	}
	*p++ = 0x15;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;
	*p++ = 0x52;
	*p++ = 0x00;
	return (u32)(p - out);
}

/* One QMI SNS_CLIENT_REQ with the protobuf in TLV 1 and TLV 0x10 = 1. */
static u32
build_qmi(u8 *msg, u32 msgid, const u8 *payload, u32 pl)
{
	u8 proto[1024];
	u8 *p = proto, *q = msg;
	u32 proto_len;

	p = be16(p, 0x120a);
	*p++ = 0x09;
	for (int i = 0; i < 8; i++) {
		*p++ = registry_suid[i];
	}
	*p++ = 0x11;
	for (int i = 0; i < 8; i++) {
		*p++ = registry_suid[8 + i];
	}
	*p++ = 0x15;
	*p++ = (u8)msgid;
	*p++ = (u8)(msgid >> 8);
	*p++ = (u8)(msgid >> 16);
	*p++ = (u8)(msgid >> 24);
	*p++ = 0x1a;
	*p++ = 0x04;
	*p++ = 0x08;
	*p++ = 0x01;
	*p++ = 0x10;
	*p++ = 0x00;
	*p++ = 0x22;
	if (pl < 0x80) {
		*p++ = (u8)pl;
	} else {
		*p++ = (u8)((pl & 0x7f) | 0x80);
		*p++ = (u8)(pl >> 7);
	}
	for (u32 i = 0; i < pl; i++) {
		*p++ = payload[i];
	}
	proto_len = (u32)(p - proto);
	*q++ = 0;
	*q++ = 8;
	*q++ = 0;
	*q++ = 0x20;
	*q++ = 0;
	q = be16(q, (u16)(proto_len + 2 + 7));
	*q++ = 0x01;
	q = be16(q, (u16)(proto_len + 2));
	q = be16(q, (u16)proto_len);
	for (u32 i = 0; i < proto_len; i++) {
		*q++ = proto[i];
	}
	*q++ = 0x10;
	*q++ = 0x01;
	*q++ = 0x00;
	*q++ = 0x01;
	return (u32)(q - msg);
}

static void
exchange(const u8 *msg, u32 len, const char *what)
{
	u8 reply[1024];
	long fd;
	struct sockaddr_qrtr addr = { 0 };

	fd = sc(198, AF_QIPCRTR, 2, 0, 0, 0, 0);
	if (fd < 0) {
		put("socket failed\n");
		return;
	}
	addr.family = AF_QIPCRTR;
	addr.node = 1;
	sc(200, fd, (long)&addr, sizeof(addr), 0, 0, 0);
	addr.node = dst_node;
	addr.port = dst_port;
	if (sc(206, fd, (long)msg, len, 0, (long)&addr, sizeof(addr)) < 0) {
		put("sendto failed\n");
		return;
	}
	put("sent ");
	put(what);
	put("\n");
	for (int i = 0; i < 10; i++) {
		struct { s64 s, ns; } ts = { 0, 200000000 };
		struct sockaddr_qrtr src = { 0 };
		s64 slen = sizeof(src);
		long r;

		sc(101, (long)&ts, 0, 0, 0, 0, 0);
		r = sc(207, fd, (long)reply, sizeof(reply), 0x40, (long)&src, (long)&slen);
		if (r > 0) {
			put("reply: ");
			dump(reply, r);
		}
	}
	sc(57, fd, 0, 0, 0, 0, 0);
}

static void
write_group(const char *group, int grp)
{
	u8 items[256], payload[352], msg[512];
	u8 *d = items, *s = payload;
	u32 n = strlen_(group), dl, pl;

	if (grp) {
		u32 l = item_grp(items, "mag");

		*d++ = 0x1a;
		*d++ = (u8)l;
		for (u32 i = 0; i < l; i++) {
			*d++ = items[i];
		}
	} else {
		static const struct { const char *name; u64 value; } ints[] = {
			{ "heap_size", 1572864 }, { "max_batch_disabled", 0 }, { "min_batch_period_thrshld_ms", 10 },
		};
		for (int k = 0; k < 3; k++) {
			u32 l = item_int(items, ints[k].name, ints[k].value);

			*d++ = 0x1a;
			*d++ = (u8)l;
			for (u32 i = 0; i < l; i++) {
				*d++ = items[i];
			}
		}
	}
	dl = (u32)(d - items);
	*s++ = 0x0a;
	*s++ = (u8)n;
	for (u32 i = 0; i < n; i++) {
		*s++ = (u8)group[i];
	}
	*s++ = 0x12;
	*s++ = (u8)dl;
	for (u32 i = 0; i < dl; i++) {
		*s++ = items[i];
	}
	pl = (u32)(s - payload);
	{
		u32 len = build_qmi(msg, 0x201, payload, pl);

		exchange(msg, len, group);
	}
}

static void
read_group(const char *group)
{
	u8 payload[96], msg[256];
	u8 *s = payload;
	u32 n = strlen_(group), pl, len;

	*s++ = 0x0a;
	*s++ = (u8)n;
	for (u32 i = 0; i < n; i++) {
		*s++ = (u8)group[i];
	}
	pl = (u32)(s - payload);
	len = build_qmi(msg, 0x200, payload, pl);
	exchange(msg, len, group);
}

void start_c(unsigned long *sp) __attribute__((used));

__attribute__((naked)) void
_start(void)
{
	__asm__ volatile ("mov x0, sp\n\tb start_c\n");
}

void
start_c(unsigned long *sp)
{
	long argc = (long)sp[0];
	char **argv = (char **)&sp[1];
	const char *mode, *group;

	if (argc > 1 && num(argv[1]) != 0) {
		dst_port = num(argv[1]);
	}
	if (argc > 2 && num(argv[2]) != 0) {
		dst_node = num(argv[2]);
	}
	mode = argc > 3 ? argv[3] : "r";
	group = argc > 4 ? argv[4] : "sns_cm";
	if (same(mode, "w")) {
		write_group(group, 0);
	} else if (same(mode, "g")) {
		write_group(group, 1);
	} else {
		read_group(group);
	}
	sc(93, 0, 0, 0, 0, 0, 0);
	for (;;) {
	}
}
