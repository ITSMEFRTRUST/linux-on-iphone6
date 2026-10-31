/* iOS flash usage: the APFS container on the ASP flash (/dev/asp0n1p1), opened READ-ONLY.
 * Follows Apple's "Apple File System Reference": the latest valid checkpoint (the NXSB with the highest
 * xid in the checkpoint descriptor area; block 0 may be stale), the container object map B-tree
 * ({oid, xid} → paddr) to each volume superblock (APSB: name, role, blocks allocated), and the space
 * manager (ephemeral, found through that checkpoint's map) for the free block count.
 * Blocking (a few hundred block reads): call it off the LVGL thread. No LVGL use here. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "orchard.h"

#define NX_MAGIC 0x4253584e	/* "NXSB" */
#define APSB_MAGIC 0x42535041	/* "APSB" */
#define T_NX 0x01
#define T_BTREE 0x02
#define T_BTREE_NODE 0x03
#define T_SPACEMAN 0x05
#define T_OMAP 0x0b
#define T_CPMAP 0x0c
#define T_FS 0x0d
#define BTNODE_ROOT 1
#define BTNODE_FIXED 4
#define OMAP_VAL_DELETED 1

struct ctx {
	int fd;
	uint32_t bs;
	uint64_t nblocks;
	uint8_t *b;
};

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t otype(const uint8_t *b) { return le32(b + 24) & 0xffff; }

/* Fletcher-64 over the block after the checksum field */
static int cksum_ok(const uint8_t *b, uint32_t bs)
{
	uint64_t s1 = 0, s2 = 0, m = 0xffffffffULL;
	for (uint32_t i = 8; i < bs; i += 4) {
		s1 = (s1 + le32(b + i)) % m;
		s2 = (s2 + s1) % m;
	}
	uint64_t c1 = m - (s1 + s2) % m, c2 = m - (s1 + c1) % m;
	return le64(b) == (c2 << 32 | c1);
}

static int rd(struct ctx *c, uint64_t paddr, uint8_t *buf)
{
	if (paddr == 0 || paddr >= c->nblocks)
		return 0;
	return pread(c->fd, buf, c->bs, (off_t)(paddr * c->bs)) == (ssize_t)c->bs;
}

/* container omap: newest mapping of oid with xid <= xid → physical address (0 = none) */
static uint64_t omap_lookup(struct ctx *c, uint64_t node, uint64_t oid, uint64_t xid)
{
	uint8_t *b = c->b;
	for (int depth = 0; depth < 8; depth++) {
		if (!rd(c, node, b) || !cksum_ok(b, c->bs) || (otype(b) != T_BTREE && otype(b) != T_BTREE_NODE))
			return 0;
		uint16_t flags = le16(b + 32), level = le16(b + 34), toff = le16(b + 40), tlen = le16(b + 42);
		uint32_t nkeys = le32(b + 36), kstart = 56 + toff + tlen, vend = c->bs - (flags & BTNODE_ROOT ? 40 : 0);
		int fixed = flags & BTNODE_FIXED, best = -1;
		uint32_t bv = 0;
		for (uint32_t i = 0; i < nkeys; i++) {
			uint32_t e = 56 + toff + i * (fixed ? 4 : 8);
			if (e + 8 > c->bs)
				return 0;
			uint32_t k = kstart + le16(b + e), v = le16(b + e + (fixed ? 2 : 4));
			if (k + 16 > c->bs || v > vend)
				return 0;
			uint64_t ko = le64(b + k), kx = le64(b + k + 8);
			if (ko < oid || (ko == oid && kx <= xid)) {
				best = i;
				bv = v;
			} else {
				break;	/* keys are sorted by (oid, xid) */
			}
		}
		if (best < 0 || vend - bv + 16 > c->bs)
			return 0;
		const uint8_t *val = b + vend - bv;
		if (level == 0) {	/* leaf: omap_val_t {flags, size, paddr} */
			uint32_t e = 56 + toff + best * (fixed ? 4 : 8);
			if (le64(b + kstart + le16(b + e)) != oid || le32(val) & OMAP_VAL_DELETED)
				return 0;
			return le64(val + 8);
		}
		node = le64(val);	/* index node: child's physical address */
	}
	return 0;
}

int apfs_read(const char *dev, apfs_info_t *out)
{
	struct ctx c = { .fd = -1 };
	uint8_t *nx = NULL, *blk = NULL;
	memset(out, 0, sizeof(*out));
	c.fd = open(dev, O_RDONLY | O_CLOEXEC);	/* never anything but read-only */
	if (c.fd < 0) {
		snprintf(out->err, sizeof(out->err), "no %s", dev);
		return out->ok = 0;
	}
	out->ok = -1;
	uint8_t head[4096];
	if (pread(c.fd, head, sizeof(head), 0) != (ssize_t)sizeof(head) || le32(head + 32) != NX_MAGIC) {
		snprintf(out->err, sizeof(out->err), "not an APFS container");
		goto done;
	}
	c.bs = le32(head + 36);
	c.nblocks = le64(head + 40);
	if (c.bs < 4096 || c.bs > 65536 || (c.bs & (c.bs - 1)) || !c.nblocks) {
		snprintf(out->err, sizeof(out->err), "odd block size %u", c.bs);
		goto done;
	}
	c.b = malloc(c.bs);
	nx = malloc(c.bs);
	blk = malloc(c.bs);
	if (!c.b || !nx || !blk)
		goto done;

	/* the newest valid superblock in the checkpoint descriptor area */
	uint64_t xid = 0, base = le64(head + 112);
	uint32_t ndesc = le32(head + 104);
	if (!(ndesc & 0x80000000u) && ndesc < 65536)
		for (uint32_t i = 0; i < ndesc; i++)
			if (rd(&c, base + i, blk) && otype(blk) == T_NX && le32(blk + 32) == NX_MAGIC &&
			    le64(blk + 16) > xid && cksum_ok(blk, c.bs)) {
				xid = le64(blk + 16);
				memcpy(nx, blk, c.bs);
			}
	if (!xid) {	/* no valid checkpoint found: the copy in block 0 */
		if (pread(c.fd, nx, c.bs, 0) != (ssize_t)c.bs || !cksum_ok(nx, c.bs)) {
			snprintf(out->err, sizeof(out->err), "no valid checkpoint");
			goto done;
		}
		xid = le64(nx + 16);
	}
	out->block_size = c.bs;
	out->total = c.nblocks * c.bs;
	out->xid = xid;

	/* volumes through the container object map */
	uint64_t tree = 0;
	if (rd(&c, le64(nx + 160), blk) && otype(blk) == T_OMAP && cksum_ok(blk, c.bs))
		tree = le64(blk + 48);
	uint32_t maxfs = le32(nx + 180);
	uint64_t used = 0;
	for (uint32_t i = 0; tree && i < maxfs && i < 100 && out->nvol < APFS_MAXVOL; i++) {
		uint64_t oid = le64(nx + 184 + 8 * i);
		if (!oid)
			continue;
		uint64_t pa = omap_lookup(&c, tree, oid, xid);
		if (!pa || !rd(&c, pa, blk) || le32(blk + 32) != APSB_MAGIC || !cksum_ok(blk, c.bs))
			continue;
		struct apfs_vol *v = &out->vol[out->nvol++];
		snprintf(v->name, sizeof(v->name), "%.*s", 255, (const char *)blk + 704);
		v->role = le16(blk + 964);
		v->used = le64(blk + 88) * c.bs;	/* apfs_fs_alloc_count */
		v->encrypted = !(le64(blk + 264) & 1);	/* !APFS_FS_UNENCRYPTED */
		used += v->used;
	}
	if (!out->nvol) {
		snprintf(out->err, sizeof(out->err), "no volumes found");
		goto done;
	}

	/* free blocks: the space manager, found in this checkpoint's map */
	uint64_t sm = le64(nx + 152);
	for (uint32_t i = 0; i < ndesc && i < 65536 && !out->free_exact; i++) {
		if (!rd(&c, base + i, blk) || otype(blk) != T_CPMAP || le64(blk + 16) != xid || !cksum_ok(blk, c.bs))
			continue;
		uint32_t n = le32(blk + 36);
		for (uint32_t j = 0; j < n && 40 + 40 * (j + 1) <= c.bs; j++) {
			const uint8_t *m = blk + 40 + 40 * j;
			if (le64(m + 24) != sm)
				continue;
			if (rd(&c, le64(m + 32), c.b) && otype(c.b) == T_SPACEMAN && le64(c.b + 48) == c.nblocks) {
				out->free = le64(c.b + 72) * c.bs;	/* sm_dev[SD_MAIN].sm_free_count */
				out->free_exact = 1;
			}
			break;
		}
	}
	if (!out->free_exact)
		out->free = out->total > used ? out->total - used : 0;
	out->ok = 1;
done:
	free(blk);
	free(c.b);
	free(nx);
	close(c.fd);
	return out->ok;
}
