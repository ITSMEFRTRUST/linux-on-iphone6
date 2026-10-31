/* hdqtest - talk HDQ to the bq27540 gas gauge on the iPhone 6 (n61) through UART5, like iOS's
 * AppleHDQGasGauge does: 57600 8N2, one UART frame per HDQ bit (0xFE = '1', 0xC0 = '0', LSB first),
 * the line is shared so TX echoes into RX; a received frame > 0xF8 is a '1' from the gauge.
 * Break = one 0x00 frame at 9600 baud (~940 us low).
 * usage: hdqtest [-v] [on] [d] [r REG]... [w REG VAL]... [R REG16]... [off]
 *   on/off = power UART5 via PMGR (ps_uart5), d = dump UART regs, r = read 8-bit, R = read 16-bit LE (lo,hi). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

#define PMGR_PS  0x20e0201c8UL   /* ps_uart5 */
#define UART5    0x20a0d4000UL
enum { ULCON = 0x00, UCON = 0x04, UFCON = 0x08, UTRSTAT = 0x10, UERSTAT = 0x14, UFSTAT = 0x18,
       UTXH = 0x20, URXH = 0x24, UBRDIV = 0x28 };

static volatile uint32_t *ps, *u;
static int verbose, loop;

static void msleep(int us) { struct timespec t = { 0, us * 1000L }; nanosleep(&t, 0); }
static uint32_t rd(int o) { return u[o / 4]; }
static void wr(int o, uint32_t v) { u[o / 4] = v; }

static volatile uint32_t *map(int fd, unsigned long pa)
{
	unsigned long pg = pa & ~0x3fffUL;
	void *p = mmap(0, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pg);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return (volatile uint32_t *)((char *)p + (pa - pg));
}

static int power(int on)
{
	uint32_t v = *ps;
	*ps = (v & ~0xfU) | (on ? 0xf : 0);
	for (int i = 0; i < 100; i++) {
		v = *ps;
		if (((v >> 4) & 0xf) == (on ? 0xf : 0)) { printf("ps_uart5 %s (%#x)\n", on ? "on" : "off", v); return 0; }
		msleep(1000);
	}
	printf("ps_uart5 did not reach state (%#x)\n", v);
	return -1;
}

static int powered(void) { return ((*ps >> 4) & 0xf) == 0xf; }

static void setbaud(int baud) { wr(UBRDIV, 24000000 / (16 * baud) - 1); }

static int rxcount(void) { uint32_t f = rd(UFSTAT); return (f & 0x100) ? 16 : (f & 0xf); }

static void drain(void) { int n = 0; while (rxcount() && n++ < 64) (void)rd(URXH); (void)rd(UERSTAT); }

static void txwait(void)
{
	for (int i = 0; i < 50 && !(rd(UTRSTAT) & 4); i++) msleep(200);
}

static void setup(void)
{
	wr(UCON, 0);
	wr(UFCON, 0x7);              /* FIFO on, reset both */
	msleep(100);
	wr(UFCON, 0x1);
	wr(ULCON, 0x3 | 0x4);        /* 8 bits, 2 stop, no parity */
	setbaud(57600);
	wr(UCON, 0x5 | (loop ? 0x20 : 0)); /* polled RX/TX (+loopback) */
	msleep(500);
	drain();
}

static void hdq_break(void)
{
	txwait();
	setbaud(9600);
	wr(UTXH, 0x00);
	txwait();
	msleep(200);
	setbaud(57600);
	msleep(100);
	if (verbose) printf("break: rx=%d err=%#x\n", rxcount(), rd(UERSTAT));
	drain();
}

/* send one HDQ byte (8 frames), check echo */
static int sendbyte(uint8_t b)
{
	uint8_t e[8];
	for (int i = 0; i < 8; i++) wr(UTXH, (b >> i) & 1 ? 0xfe : 0xc0);
	for (int t = 0; t < 40 && rxcount() < 8; t++) msleep(250);
	int n = rxcount();
	for (int i = 0; i < n && i < 8; i++) e[i] = rd(URXH);
	if (verbose) { printf("tx %02x echo(%d):", b, n); for (int i = 0; i < n && i < 8; i++) printf(" %02x", e[i]); printf("\n"); }
	return n >= 8 ? 0 : -1;
}

static int recvbyte(void)
{
	uint8_t e[16];
	for (int t = 0; t < 40 && rxcount() < 8; t++) msleep(250);
	int n = rxcount(), v = 0;
	for (int i = 0; i < n; i++) e[i] = rd(URXH);
	if (verbose) { printf("rx(%d):", n); for (int i = 0; i < n; i++) printf(" %02x", e[i]); printf(" err=%#x\n", rd(UERSTAT)); }
	if (n < 8) return -1;
	for (int i = 0; i < 8; i++) if (e[i] > 0xf8) v |= 1 << i;
	return v;
}

static int hdq_read(uint8_t reg)
{
	hdq_break();
	if (sendbyte(reg & 0x7f) < 0) return -2;
	return recvbyte();
}

static int hdq_write(uint8_t reg, uint8_t val)
{
	hdq_break();
	if (sendbyte(0x80 | reg) < 0) return -2;
	if (sendbyte(val) < 0) return -2;
	return 0;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	ps = map(fd, PMGR_PS);
	u = map(fd, UART5);
	int ready = 0;
	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp(a, "-v")) { verbose = 1; continue; }
		if (!strcmp(a, "-L")) { loop = 1; continue; }
		if (!strcmp(a, "on")) { if (power(1)) return 1; continue; }
		if (!strcmp(a, "off")) { if (powered()) wr(UCON, 0); power(0); ready = 0; continue; }
		if (!powered()) { printf("ps_uart5 is off (%#x); refusing to touch UART5\n", *ps); return 1; }
		if (!strcmp(a, "d")) {
			for (int o = 0; o <= 0x2c; o += 4) printf("%02x: %08x\n", o, rd(o));
			continue;
		}
		if (!ready) { setup(); ready = 1; }
		if (!strcmp(a, "r") && i + 1 < argc) {
			int reg = strtol(argv[++i], 0, 16);
			int v = hdq_read(reg);
			printf("r %02x = %d (%#x)\n", reg, v, v);
		} else if (!strcmp(a, "R") && i + 1 < argc) {
			int reg = strtol(argv[++i], 0, 16);
			int lo = hdq_read(reg), hi = hdq_read(reg + 1);
			printf("R %02x = %d (lo %#x hi %#x) s16=%d\n", reg, lo | hi << 8, lo, hi, (int16_t)(lo | hi << 8));
		} else if (!strcmp(a, "w") && i + 2 < argc) {
			int reg = strtol(argv[i + 1], 0, 16), val = strtol(argv[i + 2], 0, 16);
			i += 2;
			printf("w %02x %02x -> %d\n", reg, val, hdq_write(reg, val));
		}
	}
	return 0;
}
