/* net.c on-link classification: fake interfaces through an interposed getifaddrs; a LAN with global IPv6 /
 * public IPv4 is PRIVATE, own addresses BLOCKED, a /0 tun ignored */
#include "net.c"
#include <stdio.h>
static struct ifaddrs fk[8];
static struct sockaddr_storage ad[8], nm[8];
static int nf;
static void add(const char *name, const char *a, int plen, unsigned flags) {
	struct ifaddrs *i = &fk[nf];
	memset(i, 0, sizeof *i);
	i->ifa_name = (char *)name; i->ifa_flags = flags;
	if (strchr(a, ':')) {
		struct sockaddr_in6 *x = (void *)&ad[nf], *m = (void *)&nm[nf];
		x->sin6_family = m->sin6_family = AF_INET6; inet_pton(AF_INET6, a, &x->sin6_addr);
		for (int b = 0; b < plen; b++) m->sin6_addr.s6_addr[b / 8] |= 0x80 >> (b % 8);
	} else {
		struct sockaddr_in *x = (void *)&ad[nf], *m = (void *)&nm[nf];
		x->sin_family = m->sin_family = AF_INET; inet_pton(AF_INET, a, &x->sin_addr);
		m->sin_addr.s_addr = plen ? htonl(0xffffffffu << (32 - plen)) : 0;
	}
	i->ifa_addr = (void *)&ad[nf]; i->ifa_netmask = (void *)&nm[nf];
	if (nf) fk[nf - 1].ifa_next = i;
	nf++;
}
int getifaddrs(struct ifaddrs **o) { *o = fk; return 0; }
void freeifaddrs(struct ifaddrs *i) {}
static int fails;
static void chk(const char *ip, int want) {
	int c = ip_text_class(ip);
	static const char *nm_[] = { [AC_PUBLIC] = "PUBLIC", [AC_PRIVATE] = "PRIVATE", [AC_BLOCKED] = "BLOCKED" };
	printf("%-28s %-8s%s\n", ip, nm_[c], c == want ? "" : "   <-- FAIL");
	fails += c != want;
}
int main(void) {
	add("lo", "127.0.0.1", 8, IFF_LOOPBACK | IFF_UP);
	add("lo", "::1", 128, IFF_LOOPBACK | IFF_UP);
	add("wlan0", "203.0.113.5", 24, IFF_UP);			/* a LAN on public IPv4 */
	add("wlan0", "2001:db8:1:2::5", 64, IFF_UP);		/* SLAAC global IPv6 */
	add("tailscale0", "100.101.1.2", 32, IFF_UP);
	add("tailscale0", "fd7a:115c:a1e0::1", 128, IFF_UP);
	add("tun0", "198.51.100.1", 0, IFF_UP);			/* a /0 must not make the internet private */
	chk("203.0.113.9", AC_PRIVATE);		/* LAN neighbour, public v4 */
	chk("203.0.113.5", AC_BLOCKED);		/* own */
	chk("203.0.114.9", AC_PUBLIC);
	chk("2001:db8:1:2::abcd", AC_PRIVATE);	/* LAN neighbour, global v6 */
	chk("2001:db8:1:2::5", AC_BLOCKED);	/* own */
	chk("2001:db8:1:3::1", AC_PUBLIC);
	chk("2606:4700::1111", AC_PUBLIC);
	chk("1.1.1.1", AC_PUBLIC);
	chk("198.51.100.7", AC_PUBLIC);		/* tun0 /0 ignored */
	chk("198.51.100.1", AC_BLOCKED);	/* but its own address is still own */
	chk("100.101.1.2", AC_BLOCKED);
	chk("100.101.1.3", AC_PRIVATE);
	chk("192.168.1.10", AC_PRIVATE);
	chk("127.0.0.1", loopback_ok() ? AC_PRIVATE : AC_BLOCKED);
	chk("172.16.43.2", AC_BLOCKED);
	printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
