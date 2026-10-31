/* disp - A8 (T7000) display pipe poke tool for the iPhone 6 (n61), via /dev/mem.
 *
 * Phase 2, steps 1+2 of re/DISPLAY-PHASE2.md.  Every command first checks that the PMGR power states of
 * disp_busmux, disp0 and mipi_dsi are fully on (ACTUAL == TARGET == 0xf): an access to a power-gated block
 * hangs the bus.  The PMGR itself is always on.
 *
 *   disp pm                     PMGR check only (reads 3 PMGR registers, nothing else)
 *   disp regs [2]               tier 1 dump: registers iOS reads at runtime;  "2" adds tier 2 (config regs
 *                               iOS / mainline only write)
 *   disp sweep                  tier 3: dense 32-bit sweep of the windows (opt-in, still skips 0x10c0)
 *   disp count [MS]             ADP frame counter 0x1034 + ADBE0 line counter 0x1c, twice MS apart (def 1000)
 *   disp walk [SID [IOVA [LEN]]]  walk the dart-disp0 page tables (def: sid 0, IOVA = pipe 0 base, LEN = vram)
 *   disp fill OFF [PAT]         write a test pattern at vram+OFF (refuses frame 0, tables, unmapped pages)
 *   disp flip OFF|iIOVA [HOLD] [sync]  point pipe 0 at vram+OFF (or raw IOVA), restore after HOLD s (def 5;
 *                               0 = stay, use "disp restore").  "sync" also writes the FIFO sync word.
 *   disp nudge [HOLD] [sync]    flip to the current base + 64 bytes (image moves 16 px left): proves the
 *                               register is live without needing a second buffer
 *   disp restore [sync]         put back the base saved in /tmp/disp.save
 *
 * Build: tools/disp/build.sh (static aarch64 musl, var/sysroot). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PMGR		0x20e000000UL
#define PS_BUSMUX	0x202a8
#define PS_DISP0	0x202b0
#define PS_MIPI		0x202b8
#define ADP		0x206200000UL	/* ADT disp0 reg[0]: layers, blend, param FIFO */
#define ADBE		0x206400000UL	/* ADT disp0 reg[1]: ADBE0 timing generator */
#define DART		0x206304000UL	/* ADT dart-disp0 reg[0] */
#define VRAM		0x83e900000UL	/* ADT /vram (default; DT framebuffer node wins) */
#define VRAM_SIZE	0xbfb000UL

#define FIFO_DATA	0x10c0		/* never read */
#define P0		0x4000
#define SAVE		"/tmp/disp.save"

typedef uint32_t u32;
typedef uint64_t u64;

static int fd = -1;
static u64 fb_base = VRAM, fb_stride, fb_w, fb_h, vram_size = VRAM_SIZE;

/* one page-granular mapping cache: MMIO and table pages are read one page at a time */
static u64 cur_pg = ~0UL;
static volatile uint8_t *cur_map;
static volatile uint8_t *pg(u64 pa)
{
	u64 p = pa & ~0xfffUL;
	if (p != cur_pg) {
		if (cur_map)
			munmap((void *)cur_map, 0x1000);
		void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p);
		if (m == MAP_FAILED) {
			fprintf(stderr, "mmap %#lx: %s\n", (unsigned long)p, strerror(errno));
			exit(1);
		}
		cur_map = m;
		cur_pg = p;
	}
	return cur_map + (pa & 0xfff);
}
static u32 rd(u64 pa) { return *(volatile u32 *)pg(pa); }
static u64 rd64(u64 pa) { return *(volatile u64 *)pg(pa); }
static void wr(u64 pa, u32 v) { *(volatile u32 *)pg(pa) = v; }

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static u64 num(const char *s) { return strtoull(s, NULL, 0); }

/* ---- PMGR gate: the only thing that makes the other reads safe ---- */
static int ps_check(const char *name, u32 off, int verbose)
{
	u32 v = rd(PMGR + off);
	int target = v & 0xf, actual = (v >> 4) & 0xf;
	int ok = target == 0xf && actual == 0xf && !(v & (1u << 31)) && !(v & (1u << 10));
	if (verbose || !ok)
		printf("pmgr %-11s @%#lx = %08x  target %x actual %x%s%s  %s\n", name, PMGR + off, v, target,
		       actual, v & (1u << 31) ? " RESET" : "", v & (1u << 10) ? " DEV_DISABLE" : "",
		       ok ? "ON" : "NOT ON");
	return ok;
}
static void pm_gate(int verbose)
{
	int ok = ps_check("disp_busmux", PS_BUSMUX, verbose);
	ok &= ps_check("disp0", PS_DISP0, verbose);
	ok &= ps_check("mipi_dsi", PS_MIPI, verbose);
	if (!ok) {
		printf("ABORT: display power domains not fully on; touching disp0 now could hang the bus\n");
		exit(2);
	}
}

/* ---- /proc/device-tree/chosen/framebuffer@*: base, stride, size (filled by m1n1 from boot_args) ---- */
static u32 be32(const uint8_t *b) { return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static int dt_read(const char *dir, const char *prop, uint8_t *buf, int n)
{
	char p[512];
	snprintf(p, sizeof(p), "%s/%s", dir, prop);
	int f = open(p, O_RDONLY);
	if (f < 0)
		return -1;
	int r = read(f, buf, n);
	close(f);
	return r;
}
static void fb_info(void)
{
	DIR *d = opendir("/proc/device-tree/chosen");
	struct dirent *e;
	char dir[300] = "";
	while (d && (e = readdir(d)))
		if (!strncmp(e->d_name, "framebuffer", 11))
			snprintf(dir, sizeof(dir), "/proc/device-tree/chosen/%s", e->d_name);
	if (d)
		closedir(d);
	uint8_t b[16];
	if (dir[0] && dt_read(dir, "reg", b, 16) == 16) {
		fb_base = (u64)be32(b) << 32 | be32(b + 4);
		if (dt_read(dir, "stride", b, 4) == 4)
			fb_stride = be32(b);
		if (dt_read(dir, "width", b, 4) == 4)
			fb_w = be32(b);
		if (dt_read(dir, "height", b, 4) == 4)
			fb_h = be32(b);
	}
	if (!fb_stride) {	/* fall back to iPhone 6 defaults */
		fb_w = 750, fb_h = 1334, fb_stride = 3008;
		printf("note: no /chosen/framebuffer in DT, assuming 750x1334 stride 3008\n");
	}
	if (fb_base != VRAM)
		printf("WARN: DT framebuffer base %#lx != ADT vram %#lx\n", (unsigned long)fb_base, VRAM);
}
static u64 fb_bytes(void) { return fb_stride * fb_h; }

/* ---- register lists ---- */
struct reg { u32 off; const char *name; };
static const struct reg adp_t1[] = {
	{ 0x101c, "ADPCSTATE ([7:4] ParmSt, 2=busy)" }, { 0x1028, "IRQ enable mask" },
	{ 0x102c, "IRQ status (W1C): b0 VBI b1 under b3 pktmiss b6 PFAlign b9 PFOver b10 pkt b11 RdErr" },
	{ 0x1034, "frame/packet counter (16b)" }, { 0x1040, "FIFO level [12:0]" },
	{ 0x1064, "QoS" }, { 0x1068, "QoS" }, { 0x106c, "QoS" }, { 0x1070, "QoS" },
	{ 0x2030, "post-blend CRC" }, { 0 }
};
static const struct reg adp_t2[] = {
	{ 0x1000, "scratch (iOS: 0xdeadbeef on underrun)" }, { 0x1030, "screen size (A<<16|B)" },
	{ 0x1038, "ADP/LAYER_CTL" }, { 0x1048, "FIFO ctl" },
	{ 0x1050, "QoS" }, { 0x1054, "QoS" }, { 0x1058, "QoS" }, { 0x105c, "QoS" }, { 0x1060, "QoS" },
	{ 0x1074, "QoS" }, { 0x1078, "QoS" }, { 0x107c, "QoS" },
	{ 0x2020, "BLEND_BYPASS" }, { 0x2024, "blend bg colour 10:10:10" }, { 0x2028, "BLEND_EN1" },
	{ 0x202c, "BLEND_EN3" }, { 0x2034, "BLEND_EN4" }, { 0x2074, "BLEND_EN2" }, { 0x20ac, "tunable" },
	{ 0x2200, "MASK_BUF (mainline)" }, { 0 }
};
static const struct reg pipe_regs[] = {
	{ 0x20, "LAYER_EN1" }, { 0x24, "DMA cfg" }, { 0x28, "DMA cfg" }, { 0x2c, "PIX_FMT (XRGB 0x53e4001)" },
	{ 0x30, "BASE plane0 (IOVA)" }, { 0x34, "BASE plane1" }, { 0x38, "STRIDE plane0" }, { 0x3c, "STRIDE plane1" },
	{ 0x40, "SRC_START (x<<16|y)" }, { 0x44, "SRC_START p1" }, { 0x48, "SRC_SIZE (w<<16|h)" },
	{ 0x4c, "SRC_SIZE p1" }, { 0x50, "DST_START" }, { 0x54, "DST_SIZE" }, { 0x68, "LAYER_EN2 / region mask" },
	{ 0x6c, "region" }, { 0x70, "region" }, { 0x74, "region" }, { 0x78, "region" }, { 0x7c, "region" },
	{ 0x80, "region" }, { 0x84, "region" }, { 0x88, "region" }, { 0x94, "H scale p0" }, { 0x98, "H scale p1" },
	{ 0xa4, "V scale p0" }, { 0xa8, "V scale p1" }, { 0xac, "SCALE_CTL (bypass 0x100000)" },
	{ 0xb4, "LAYER_EN3" }, { 0xf4, "LAYER_EN4" }, { 0 }
};
static const struct reg adbe_t1[] = {
	{ 0x1c, "[31:16] line counter" }, { 0x30, "BE IRQ [2:0] status [18:16] enable" },
	{ 0x38, "b3 CRC valid" }, { 0x40, "CRC" }, { 0 }
};
static const struct reg adbe_t2[] = {
	{ 0x04, "block enables (31 AAP 30 DPB 28/29 dither)" }, { 0x08, "VFTG ctl (b15 run?)" },
	{ 0x0c, "active size (V<<16|H)" }, { 0x10, "timing pair" }, { 0x14, "timing pair" },
	{ 0x18, "timing pair" }, { 0x24, "VBI line" }, { 0x44, "border colour" }, { 0 }
};
static const struct reg dart_t1[] = {
	{ 0x00, "STREAM_COMMAND" }, { 0x0c, "TCR (8 bits/sid, b7 translate)" }, { 0x10, "ERROR" },
	{ 0x1c, "ERROR_ADDR" },
	{ 0x40, "TTBR sid0 [0]" }, { 0x44, "TTBR sid0 [1]" }, { 0x48, "TTBR sid0 [2]" }, { 0x4c, "TTBR sid0 [3]" },
	{ 0x50, "TTBR sid1 [0]" }, { 0x54, "TTBR sid1 [1]" }, { 0x58, "TTBR sid1 [2]" }, { 0x5c, "TTBR sid1 [3]" },
	{ 0x60, "TTBR sid2 [0]" }, { 0x64, "TTBR sid2 [1]" }, { 0x68, "TTBR sid2 [2]" }, { 0x6c, "TTBR sid2 [3]" },
	{ 0x70, "TTBR sid3 [0]" }, { 0x74, "TTBR sid3 [1]" }, { 0x78, "TTBR sid3 [2]" }, { 0x7c, "TTBR sid3 [3]" },
	{ 0 }
};
static const struct reg dart_t2[] = {
	{ 0x04, "?" }, { 0x08, "?" }, { 0x14, "?" }, { 0x18, "?" }, { 0x20, "?" }, { 0 }
};

static void dump(const char *blk, u64 base, const struct reg *r)
{
	for (; r->name; r++) {
		u32 v = rd(base + r->off);
		printf("%-5s +%04x = %08x  %s\n", blk, r->off, v, r->name);
		fflush(stdout);	/* if a read hangs, the log shows the last good one */
	}
}

static void cmd_regs(int tier)
{
	printf("== tier 1 (registers iOS reads at runtime) ==\n");
	dump("ADP", ADP, adp_t1);
	dump("ADBE", ADBE, adbe_t1);
	dump("DART", DART, dart_t1);
	printf("pipe0 BASE %08x  STRIDE %08x  FMT %08x  SRC_SIZE %08x\n", rd(ADP + P0 + 0x30), rd(ADP + P0 + 0x38),
	       rd(ADP + P0 + 0x2c), rd(ADP + P0 + 0x48));
	if (tier < 2)
		return;
	printf("== tier 2 (config registers iOS/mainline write) ==\n");
	dump("ADP", ADP, adp_t2);
	dump("PIPE0", ADP + 0x4000, pipe_regs);
	dump("PIPE1", ADP + 0x6000, pipe_regs);
	dump("ADBE", ADBE, adbe_t2);
	dump("DART", DART, dart_t2);
}

static void sweep(const char *blk, u64 base, u32 from, u32 to)
{
	for (u32 o = from; o < to; o += 4) {
		if (base == ADP && o == FIFO_DATA)
			continue;
		if ((o - from) % 16 == 0)
			printf("%s%-5s +%04x:", o == from ? "" : "\n", blk, o);
		printf(" %08x", rd(base + o));
		fflush(stdout);
	}
	printf("\n");
}
static void cmd_sweep(void)
{
	sweep("ADP", ADP, 0x1000, 0x1100);
	sweep("ADP", ADP, 0x2000, 0x2100);
	sweep("ADP", ADP, 0x4000, 0x4100);
	sweep("ADP", ADP, 0x6000, 0x6100);
	sweep("ADBE", ADBE, 0x0, 0x50);
	sweep("DART", DART, 0x0, 0x80);
}

static void cmd_count(int ms)
{
	u32 c0 = rd(ADP + 0x1034), l0 = rd(ADBE + 0x1c);
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	msleep(ms);
	u32 c1 = rd(ADP + 0x1034), l1 = rd(ADBE + 0x1c);
	clock_gettime(CLOCK_MONOTONIC, &b);
	double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	u32 d = (c1 - c0) & 0xffff;
	printf("ADP 0x1034: %08x -> %08x  delta %u in %.3f s = %.1f /s (60 = frame counter)\n", c0, c1, d, dt, d / dt);
	printf("ADBE 0x1c : %08x -> %08x  (line [31:16] %u -> %u)\n", l0, l1, l0 >> 16, l1 >> 16);
}

/* ---- DART walk (S5L8960X: 4 TTBRs x 1 GB per sid, 4K pages, 2 levels, PA = pte[35:12]) ---- */
#define PA_MASK 0xffffff000UL
static u64 tables[64];
static int ntables;
static void note_table(u64 pa)
{
	for (int i = 0; i < ntables; i++)
		if (tables[i] == pa)
			return;
	if (ntables < 64)
		tables[ntables++] = pa;
}
/* returns PA for iova, or ~0 if unmapped */
#define BYPASS_PA 0x800000000UL	/* TCR translate off: the pipe fetches PA = this + its 32-bit address */
static int dart_bypass(int sid) { return !((rd(DART + 0xc) >> (sid * 8)) & 0x80); }
static u64 xlate(int sid, u32 iova)
{
	if (dart_bypass(sid))	/* (iBoot leaves disp0's DART off on n61: TTBRs hold junk, never follow them) */
		return BYPASS_PA + iova;
	u32 ttbr = rd(DART + 0x40 + sid * 0x10 + (iova >> 30) * 4);
	if (!(ttbr >> 31))
		return ~0UL;
	u64 l1 = (u64)(ttbr & 0xffffff) << 12;
	note_table(l1);
	u64 e1 = rd64(l1 + ((iova >> 21) & 511) * 8);
	if (!(e1 & 1))
		return ~0UL;
	u64 l2 = e1 & PA_MASK;
	note_table(l2);
	u64 e2 = rd64(l2 + ((iova >> 12) & 511) * 8);
	if (!(e2 & 1))
		return ~0UL;
	return (e2 & PA_MASK) | (iova & 0xfff);
}
/* is [pa, pa+len) inside "System RAM" of /proc/iomem? */
static int in_sysram(u64 pa)
{
	FILE *f = fopen("/proc/iomem", "r");
	char l[256];
	int hit = 0;
	while (f && fgets(l, sizeof(l), f)) {
		unsigned long a, b;
		if (l[0] != ' ' && strstr(l, "System RAM") && sscanf(l, "%lx-%lx", &a, &b) == 2 && pa >= a && pa <= b)
			hit = 1;
	}
	if (f)
		fclose(f);
	return hit;
}
/* walk [iova, iova+len); print contiguous runs; return 1 if the whole range is mapped linearly to pa0 */
static int walk(int sid, u32 iova, u64 len, u64 pa0, int verbose)
{
	u64 run_iova = 0, run_pa = 0, run_len = 0, expect = 0;
	int linear = 1, mapped_all = 1;
	for (u64 o = 0; o < len; o += 0x1000) {
		u64 pa = xlate(sid, iova + o);
		if (pa == ~0UL)
			mapped_all = 0;
		if (pa0 != ~0UL && pa != pa0 + o)
			linear = 0;
		if (run_len && pa == expect && pa != ~0UL) {
			run_len += 0x1000, expect += 0x1000;
			continue;
		}
		if (run_len && verbose)
			printf("  iova %08lx..%08lx -> pa %09lx..%09lx  (%lu KB)\n", (unsigned long)run_iova,
			       (unsigned long)(run_iova + run_len - 1), (unsigned long)run_pa,
			       (unsigned long)(run_pa + run_len - 1), (unsigned long)run_len >> 10);
		if (pa == ~0UL) {
			if (verbose)
				printf("  iova %08lx UNMAPPED\n", (unsigned long)(iova + o));
			run_len = 0;
			/* skip the rest of an unmapped stretch quietly */
			while (o + 0x1000 < len && xlate(sid, iova + o + 0x1000) == ~0UL)
				o += 0x1000;
			continue;
		}
		run_iova = iova + o, run_pa = pa, run_len = 0x1000, expect = pa + 0x1000;
	}
	if (run_len && verbose)
		printf("  iova %08lx..%08lx -> pa %09lx..%09lx  (%lu KB)\n", (unsigned long)run_iova,
		       (unsigned long)(run_iova + run_len - 1), (unsigned long)run_pa,
		       (unsigned long)(run_pa + run_len - 1), (unsigned long)run_len >> 10);
	return mapped_all && linear;
}
static int table_overlap(u64 a, u64 len)
{
	for (int i = 0; i < ntables; i++)
		if (tables[i] + 0x1000 > a && tables[i] < a + len)
			return 1;
	return 0;
}
static void cmd_walk(int sid, u32 iova, u64 len)
{
	u32 tcr = rd(DART + 0xc);
	printf("DART TCR %08x: sid%d byte %02x (translate %s)\n", tcr, sid, (tcr >> (sid * 8)) & 0xff,
	       (tcr >> (sid * 8)) & 0x80 ? "on" : "OFF");
	if (dart_bypass(sid)) {
		printf("=> BYPASS: translation off, the pipe reads PA %#lx + address (no tables to walk)\n", BYPASS_PA);
		return;
	}
	for (int i = 0; i < 4; i++)
		printf("  TTBR sid%d[%d] = %08x%s\n", sid, i, rd(DART + 0x40 + sid * 0x10 + i * 4),
		       rd(DART + 0x40 + sid * 0x10 + i * 4) >> 31 ? "" : " (invalid)");
	printf("walk iova %08x + %#lx:\n", iova, (unsigned long)len);
	int lin = walk(sid, iova, len, fb_base, 1);
	printf("=> %s\n", lin ? "LINEAR: whole range maps 1:1 onto vram from fb base"
			       : "NOT linear onto vram (see runs above)");
	for (int i = 0; i < ntables; i++)
		printf("  table page %09lx  %s%s\n", (unsigned long)tables[i],
		       in_sysram(tables[i]) ? "in Linux System RAM (!)" : "outside Linux RAM",
		       tables[i] >= fb_base && tables[i] < fb_base + vram_size ? ", INSIDE vram (!)" : "");
}

/* ---- step 2: fill + flip ---- */
static void need_safe_buffer(u64 off, u32 iova)
{
	u64 len = fb_bytes();
	if (off + len > vram_size) {
		printf("ABORT: vram+%#lx + %#lx runs past vram (%#lx)\n", (unsigned long)off, (unsigned long)len,
		       (unsigned long)vram_size);
		exit(3);
	}
	if (!walk(0, iova, len, fb_base + off, 0)) {
		printf("ABORT: iova %08x..+%#lx is not mapped 1:1 onto vram+%#lx; run 'disp walk' and see the runbook\n",
		       iova, (unsigned long)len, (unsigned long)off);
		exit(3);
	}
	if (table_overlap(fb_base + off, len)) {
		printf("ABORT: a DART table page lives inside vram+%#lx..; writing there would break scanout\n",
		       (unsigned long)off);
		exit(3);
	}
}

static void cmd_fill(u64 off, int pat)
{
	u32 iova0 = rd(ADP + P0 + 0x30);
	if (off < fb_bytes()) {
		printf("ABORT: offset %#lx overlaps frame 0 (on screen, %#lx bytes)\n", (unsigned long)off,
		       (unsigned long)fb_bytes());
		exit(3);
	}
	if (off & 0xfff) {
		printf("ABORT: offset must be page aligned\n");
		exit(3);
	}
	need_safe_buffer(off, iova0 + off);
	/* 8 vertical colour bars, white 8 px border, black diagonal: a wrong stride shows as a skew */
	static const u32 bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
	u64 base = fb_base + off;
	void *m = mmap(NULL, fb_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (m == MAP_FAILED) {
		perror("mmap vram");
		exit(1);
	}
	for (u64 y = 0; y < fb_h; y++) {
		volatile u32 *row = (volatile u32 *)((uint8_t *)m + y * fb_stride);
		for (u64 x = 0; x < fb_w; x++) {
			u32 c = pat ? (u32)pat : bars[x * 8 / fb_w];
			if (x < 8 || y < 8 || x >= fb_w - 8 || y >= fb_h - 8)
				c = 0xffffff;
			if ((y * fb_w / fb_h) / 4 == x / 4)
				c = 0;
			row[x] = c;	/* aligned 32-bit stores only: this is a Device mapping */
		}
	}
	munmap(m, fb_bytes());
	printf("filled vram+%#lx (pa %09lx, iova %08x) %lux%lu stride %lu\n", (unsigned long)off,
	       (unsigned long)base, iova0 + (u32)off, (unsigned long)fb_w, (unsigned long)fb_h,
	       (unsigned long)fb_stride);
}

static int fifo_sync(u32 tag)
{
	u32 st = rd(ADP + 0x101c), lvl = rd(ADP + 0x1040);
	if (((st >> 4) & 0xf) == 2 || (lvl & 0x1fff)) {
		printf("FIFO busy (ADPCSTATE %08x level %08x): not writing the sync word\n", st, lvl);
		return -1;
	}
	wr(ADP + FIFO_DATA, 0xc0000000u | (tag & 0xffff));
	printf("FIFO <- %08x (sync header, zero payload)\n", 0xc0000000u | (tag & 0xffff));
	return 0;
}

static void watch(const char *what, u32 st0)
{
	for (int i = 0; i < 4; i++) {
		msleep(25);
		u32 st = rd(ADP + 0x102c);
		printf("  %s +%3dms: BASE %08x  cnt %04x  status %08x  new bits %08x\n", what, 25 * (i + 1),
		       rd(ADP + P0 + 0x30), rd(ADP + 0x1034) & 0xffff, st, st & ~st0);
	}
	u32 st = rd(ADP + 0x102c);
	if (st & ((1 << 1) | (1 << 6) | (1 << 9) | (1 << 11)))
		printf("  WARNING: error bits in status %08x (b1 underrun b6 PFAlign b9 PFOver b11 RdErr)\n", st);
}

static void set_base(u32 iova, int sync, const char *what)
{
	u32 st0 = rd(ADP + 0x102c);
	wr(ADP + 0x102c, st0);	/* W1C, exactly what iOS's handler does */
	st0 = rd(ADP + 0x102c);
	wr(ADP + P0 + 0x30, iova);
	printf("%s: pipe0 BASE <- %08x, reads back %08x\n", what, iova, rd(ADP + P0 + 0x30));
	watch("direct", st0);
	if (sync && !fifo_sync(rd(ADP + 0x1034) + 1))
		watch("synced", st0);
}

static void cmd_flip(const char *arg, int hold, int sync)
{
	u32 cur = rd(ADP + P0 + 0x30), stride = rd(ADP + P0 + 0x38);
	u32 iova;
	FILE *f = fopen(SAVE, "r");
	u32 saved;
	if (f && fscanf(f, "%x", &saved) == 1 && saved != cur) {
		printf("ABORT: %s says base %08x but pipe0 shows %08x: run 'disp restore' first\n", SAVE, saved, cur);
		exit(3);
	}
	if (f)
		fclose(f);
	if (stride != fb_stride)
		printf("note: pipe0 stride %u != DT stride %lu (using DT for buffer math)\n", stride,
		       (unsigned long)fb_stride);
	if (arg[0] == 'i') {
		iova = num(arg + 1);
		if (iova & 0x3f) {
			printf("ABORT: IOVA must be 64-byte aligned\n");
			exit(3);
		}
		/* same rule as for buffers: every byte the pipe will fetch must be mapped */
		u64 pa = xlate(0, iova & ~0xfffu);
		if (pa == ~0UL || !walk(0, iova & ~0xfffu, fb_bytes() + 0x1000, pa, 0)) {
			printf("ABORT: iova %08x + one frame is not fully mapped (contiguously)\n", iova);
			exit(3);
		}
	} else {
		u64 off = num(arg);
		if (off & 0xfff) {
			printf("ABORT: offset must be page aligned\n");
			exit(3);
		}
		iova = cur + off;
		need_safe_buffer(off, iova);
	}
	f = fopen(SAVE, "w");
	if (!f) {
		perror(SAVE);
		exit(1);
	}
	fprintf(f, "%08x\n", cur);
	fclose(f);
	printf("saved base %08x to %s\n", cur, SAVE);
	set_base(iova, sync, "flip");
	if (!hold) {
		printf("holding: run 'disp restore%s' to go back\n", sync ? " sync" : "");
		return;
	}
	printf("look at the screen now; restoring in %d s\n", hold);
	fflush(stdout);
	msleep(hold * 1000);
	set_base(cur, sync, "restore");
	unlink(SAVE);
}

static void cmd_restore(int sync)
{
	FILE *f = fopen(SAVE, "r");
	u32 saved;
	if (!f || fscanf(f, "%x", &saved) != 1) {
		printf("nothing saved in %s\n", SAVE);
		exit(3);
	}
	fclose(f);
	set_base(saved, sync, "restore");
	unlink(SAVE);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: disp pm|regs [2]|sweep|count [ms]|walk [sid [iova [len]]]|fill OFF [pat]|"
				"flip OFF|iIOVA [hold] [sync]|nudge [hold] [sync]|restore [sync]\n");
		return 1;
	}
	const char *mem = getenv("DISP_MEM");	/* test hook: a sparse file standing in for /dev/mem */
	fd = open(mem ? mem : "/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem (mknod /dev/mem c 1 1)");
		return 1;
	}
	const char *c = argv[1];
	int verbose = !strcmp(c, "pm") || !strcmp(c, "regs");
	pm_gate(verbose);
	if (!strcmp(c, "pm"))
		return 0;
	fb_info();
	printf("fb: base %#lx %lux%lu stride %lu (%#lx bytes/frame), vram %#lx\n", (unsigned long)fb_base,
	       (unsigned long)fb_w, (unsigned long)fb_h, (unsigned long)fb_stride, (unsigned long)fb_bytes(),
	       (unsigned long)vram_size);
	fflush(stdout);
	if (!strcmp(c, "regs"))
		cmd_regs(argc > 2 ? atoi(argv[2]) : 1);
	else if (!strcmp(c, "sweep"))
		cmd_sweep();
	else if (!strcmp(c, "count"))
		cmd_count(argc > 2 ? atoi(argv[2]) : 1000);
	else if (!strcmp(c, "walk"))
		cmd_walk(argc > 2 ? atoi(argv[2]) : 0, argc > 3 ? num(argv[3]) : rd(ADP + P0 + 0x30),
			 argc > 4 ? num(argv[4]) : vram_size);
	else if (!strcmp(c, "fill") && argc > 2)
		cmd_fill(num(argv[2]), argc > 3 ? num(argv[3]) : 0);
	else if (!strcmp(c, "flip") && argc > 2)
		cmd_flip(argv[2], argc > 3 ? atoi(argv[3]) : 5, argc > 4 && !strcmp(argv[4], "sync"));
	else if (!strcmp(c, "nudge")) {
		char a[32];
		snprintf(a, sizeof(a), "i%#x", rd(ADP + P0 + 0x30) + 0x40);
		cmd_flip(a, argc > 2 ? atoi(argv[2]) : 3, argc > 3 && !strcmp(argv[3], "sync"));
	} else if (!strcmp(c, "restore"))
		cmd_restore(argc > 2 && !strcmp(argv[2], "sync"));
	else {
		fprintf(stderr, "unknown command\n");
		return 1;
	}
	return 0;
}
