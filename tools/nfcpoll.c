/* nfcpoll - bring an NFC adapter up and poll for tags over the kernel nfc generic-netlink API (no libnl/neard).
 * usage: nfcpoll [up|down|poll [SECS [MASK]]]   MASK = hex NFC_PROTO_*_MASK set (default: A+B+F+V)   default: up + poll 60 s, prints every target found, re-polls after each.
 * build: clang --target=aarch64-linux-musl --sysroot=var/sysroot -static -fuse-ld=lld -rtlib=compiler-rt -O2 */
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nfc.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fd, fam, grp, seq;
static unsigned dev;  /* nfc0 */
static char buf[8192];

#define NA(a) ((struct nlattr *)(a))
#define NLA_OK(a, l) ((l) >= (int)sizeof(struct nlattr) && NA(a)->nla_len >= sizeof(struct nlattr) && NA(a)->nla_len <= (l))
#define NLA_NEXT(a, l) ((l) -= NLA_ALIGN(NA(a)->nla_len), (struct nlattr *)((char *)(a) + NLA_ALIGN(NA(a)->nla_len)))
#define NLA_DATA(a) ((char *)(a) + NLA_HDRLEN)

static void put(char **p, int type, const void *d, int len)
{
	struct nlattr *a = (struct nlattr *)*p;
	a->nla_type = type; a->nla_len = NLA_HDRLEN + len;
	memcpy(NLA_DATA(a), d, len);
	*p += NLA_ALIGN(a->nla_len);
}

static int send_cmd(int family, int cmd, int flags, const void *attrs, int alen)
{
	char m[512]; struct nlmsghdr *h = (void *)m; struct genlmsghdr *g = NLMSG_DATA(h);
	h->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN) + alen; h->nlmsg_type = family;
	h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | flags; h->nlmsg_seq = ++seq; h->nlmsg_pid = 0;
	g->cmd = cmd; g->version = 1; g->reserved = 0;
	memcpy((char *)g + GENL_HDRLEN, attrs, alen);
	return send(fd, m, h->nlmsg_len, 0);
}

/* read replies until the ACK / DONE for seq; cb gets each data message. returns 0 or -errno */
static int wait_ack(void (*cb)(struct nlmsghdr *))
{
	for (;;) {
		int n = recv(fd, buf, sizeof buf, 0);
		if (n < 0) return -errno;
		for (struct nlmsghdr *h = (void *)buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(h);
				if (h->nlmsg_seq == (unsigned)seq) return e->error;
			} else if (h->nlmsg_type == NLMSG_DONE) {
				if (h->nlmsg_seq == (unsigned)seq) return 0;
			} else if (cb && h->nlmsg_seq == (unsigned)seq) cb(h);
		}
	}
}

static void family_cb(struct nlmsghdr *h)
{
	int l = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	for (struct nlattr *a = (void *)((char *)NLMSG_DATA(h) + GENL_HDRLEN); NLA_OK(a, l); a = NLA_NEXT(a, l)) {
		if (a->nla_type == CTRL_ATTR_FAMILY_ID) fam = *(unsigned short *)NLA_DATA(a);
		if (a->nla_type != CTRL_ATTR_MCAST_GROUPS) continue;
		int gl = a->nla_len - NLA_HDRLEN;
		for (struct nlattr *g = (void *)NLA_DATA(a); NLA_OK(g, gl); g = NLA_NEXT(g, gl)) {
			int il = g->nla_len - NLA_HDRLEN, id = 0; const char *name = "";
			for (struct nlattr *i = (void *)NLA_DATA(g); NLA_OK(i, il); i = NLA_NEXT(i, il)) {
				if (i->nla_type == CTRL_ATTR_MCAST_GRP_ID) id = *(int *)NLA_DATA(i);
				if (i->nla_type == CTRL_ATTR_MCAST_GRP_NAME) name = NLA_DATA(i);
			}
			if (!strcmp(name, NFC_GENL_MCAST_EVENT_NAME)) grp = id;
		}
	}
}

static void hex(const char *tag, const unsigned char *d, int n)
{
	printf(" %s=", tag);
	for (int i = 0; i < n; i++) printf("%02x", d[i]);
}

static const char *proto(unsigned m)
{
	static char s[96]; s[0] = 0;
	static const char *n[] = { "", "jewel", "mifare", "felica", "iso14443-4a", "nfc-dep", "iso14443-4b", "iso15693" };
	for (int i = 1; i < 8; i++) if (m & (1u << i)) { strcat(s, s[0] ? "," : ""); strcat(s, n[i]); }
	return s;
}

static void target_cb(struct nlmsghdr *h)
{
	int l = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	printf("TARGET");
	for (struct nlattr *a = (void *)((char *)NLMSG_DATA(h) + GENL_HDRLEN); NLA_OK(a, l); a = NLA_NEXT(a, l)) {
		unsigned char *d = (void *)NLA_DATA(a); int n = a->nla_len - NLA_HDRLEN;
		switch (a->nla_type) {
		case NFC_ATTR_TARGET_INDEX: printf(" idx=%u", *(unsigned *)d); break;
		case NFC_ATTR_PROTOCOLS: printf(" proto=%s", proto(*(unsigned *)d)); break;
		case NFC_ATTR_TARGET_SENS_RES: printf(" sens_res=%04x", *(unsigned short *)d); break;
		case NFC_ATTR_TARGET_SEL_RES: printf(" sel_res=%02x", d[0]); break;
		case NFC_ATTR_TARGET_NFCID1: hex("uid", d, n); break;
		case NFC_ATTR_TARGET_SENSB_RES: hex("sensb", d, n); break;
		case NFC_ATTR_TARGET_SENSF_RES: hex("sensf", d, n); break;
		case NFC_ATTR_TARGET_ISO15693_UID: hex("uid15693", d, n); break;
		}
	}
	printf("\n"); fflush(stdout);
}

static int dev_cmd(int cmd, unsigned protocols)
{
	char a[64], *p = a;
	put(&p, NFC_ATTR_DEVICE_INDEX, &dev, 4);
	if (protocols) put(&p, NFC_ATTR_PROTOCOLS, &protocols, 4);
	send_cmd(fam, cmd, 0, a, p - a);
	return wait_ack(NULL);
}

int main(int argc, char **argv)
{
	const char *op = argc > 1 ? argv[1] : "all";
	int secs = argc > 2 ? atoi(argv[2]) : 60, r;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0 || bind(fd, (void *)&sa, sizeof sa)) { perror("netlink"); return 1; }
	char a[64], *p = a;
	put(&p, CTRL_ATTR_FAMILY_NAME, NFC_GENL_NAME, sizeof NFC_GENL_NAME);
	send_cmd(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, a, p - a);
	if ((r = wait_ack(family_cb)) || !fam) { fprintf(stderr, "no nfc family (%s) - modules loaded?\n", strerror(-r)); return 1; }

	if (!strcmp(op, "down")) { r = dev_cmd(NFC_CMD_DEV_DOWN, 0); printf("down: %s\n", strerror(-r)); return !!r; }
	if (strcmp(op, "poll")) {
		r = dev_cmd(NFC_CMD_DEV_UP, 0);
		printf("up: %s\n", r ? strerror(-r) : "ok");
		if (r && r != -EALREADY) return 1;
		if (!strcmp(op, "up")) return 0;
	}
	if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &grp, sizeof grp)) { perror("mcast"); return 1; }

	unsigned all = NFC_PROTO_JEWEL_MASK | NFC_PROTO_MIFARE_MASK | NFC_PROTO_FELICA_MASK | NFC_PROTO_ISO14443_MASK |
		       NFC_PROTO_ISO14443_B_MASK | NFC_PROTO_ISO15693_MASK;
	if (argc > 3) all = strtoul(argv[3], NULL, 16);
	time_t end = time(NULL) + secs;
	while (time(NULL) < end) {
		r = dev_cmd(NFC_CMD_START_POLL, all);
		if (r && r != -EBUSY) { printf("start_poll: %s\n", strerror(-r)); return 1; }
		printf("polling...\n"); fflush(stdout);
		int found = 0;
		while (!found && time(NULL) < end) {
			struct pollfd pf = { fd, POLLIN, 0 };
			if (poll(&pf, 1, 1000) <= 0) continue;
			int n = recv(fd, buf, sizeof buf, 0);
			for (struct nlmsghdr *h = (void *)buf; n > 0 && NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
				struct genlmsghdr *g = NLMSG_DATA(h);
				if (h->nlmsg_type != fam) continue;
				if (g->cmd == NFC_EVENT_TARGETS_FOUND) found = 1;
				else if (g->cmd == NFC_EVENT_TARGET_LOST) printf("target lost\n");
			}
		}
		if (!found) break;
		p = a; put(&p, NFC_ATTR_DEVICE_INDEX, &dev, 4);
		send_cmd(fam, NFC_CMD_GET_TARGET, NLM_F_DUMP, a, p - a);
		wait_ack(target_cb);
		sleep(1);
		dev_cmd(NFC_CMD_STOP_POLL, 0);  /* deactivate, then poll again */
	}
	dev_cmd(NFC_CMD_STOP_POLL, 0);
	printf("done\n");
	return 0;
}
