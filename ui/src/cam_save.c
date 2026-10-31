/* Camera photos (cam.h): the frame turned upright at the sensor's own size (cam_conv.c at 1:1), then a JPEG through
 * libjpeg-turbo (quality 92, the library the Full Site mode already links: ORCHARD_TURBOJPEG) — or, in a build
 * without it, a plain uncompressed PNG so the shutter still works. Written to a temp name and renamed, so the
 * viewer never sees half a file. */
#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cam.h"
#include "cam_lut.h"
#ifdef ORCHARD_TURBOJPEG
#include <jpeglib.h>
#endif

const char *data_dir(void);	/* prefs.c */

#define QUALITY 92

const char *cam_photos_dir(void)
{
	static char dir[512];
	if (!dir[0]) {
		snprintf(dir, sizeof(dir), "%s/Photos", data_dir());
		mkdir(dir, 0755);
	}
	return dir;
}

static int photo_number(const char *name)
{
	int n = 0, len = 0;
	char ext[8] = "";
	if (sscanf(name, "IMG_%d.%7s%n", &n, ext, &len) != 2 || name[len] || n <= 0)
		return 0;
	return !strcmp(ext, "JPG") || !strcmp(ext, "PNG") ? n : 0;
}

static int by_number(const void *a, const void *b)
{
	const char *x = strrchr(*(char *const *)a, '/') + 1, *y = strrchr(*(char *const *)b, '/') + 1;
	return photo_number(x) - photo_number(y);
}

int cam_photos_list(char ***paths)
{
	*paths = NULL;
	DIR *d = opendir(cam_photos_dir());
	if (!d)
		return 0;
	int n = 0, cap = 0;
	char **v = NULL;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!photo_number(e->d_name))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 32;
			char **nv = realloc(v, sizeof(char *) * cap);
			if (!nv)
				break;
			v = nv;
		}
		size_t len = strlen(cam_photos_dir()) + strlen(e->d_name) + 2;
		if (!(v[n] = malloc(len)))
			break;
		snprintf(v[n++], len, "%s/%s", cam_photos_dir(), e->d_name);
	}
	closedir(d);
	if (n > 1)
		qsort(v, n, sizeof(char *), by_number);
	*paths = v;
	return n;
}

void cam_photos_free(char **paths, int n)
{
	for (int i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
}

int cam_photos_next_number(void)
{
	char **v;
	int n = cam_photos_list(&v), next = 1;
	if (n)
		next = photo_number(strrchr(v[n - 1], '/') + 1) + 1;
	cam_photos_free(v, n);
	return next;
}

#ifdef ORCHARD_TURBOJPEG
/* libjpeg's default error handler exit()s (a full disk would take Orchard down): jump back out instead */
struct jerr {
	struct jpeg_error_mgr mgr;
	jmp_buf jb;
};

static void jerr_exit(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static bool write_image(FILE *f, const uint32_t *px, int w, int h, char *why, size_t n)
{
	struct jpeg_compress_struct c;
	struct jerr err;
	c.err = jpeg_std_error(&err.mgr);
	err.mgr.error_exit = jerr_exit;
	if (setjmp(err.jb)) {
		char msg[JMSG_LENGTH_MAX];
		err.mgr.format_message((j_common_ptr)&c, msg);
		snprintf(why, n, "JPEG: %s", msg);
		jpeg_destroy_compress(&c);
		return false;
	}
	jpeg_create_compress(&c);
	jpeg_stdio_dest(&c, f);
	c.image_width = (JDIMENSION)w;
	c.image_height = (JDIMENSION)h;
	c.input_components = 4;
	c.in_color_space = JCS_EXT_BGRX;	/* XRGB8888 in memory, little-endian */
	jpeg_set_defaults(&c);
	jpeg_set_quality(&c, QUALITY, TRUE);
	jpeg_start_compress(&c, TRUE);
	while (c.next_scanline < c.image_height) {
		JSAMPROW row = (JSAMPROW)(px + (size_t)c.next_scanline * w);
		jpeg_write_scanlines(&c, &row, 1);
	}
	jpeg_finish_compress(&c);
	jpeg_destroy_compress(&c);
	return true;
}
#define EXT "JPG"
#else
static uint32_t crc_tab[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
	if (!crc_tab[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crc_tab[i] = v;
		}
	for (size_t i = 0; i < n; i++)
		c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

/* PNG: RGB, one IDAT of stored (uncompressed) deflate blocks */
static bool write_image(FILE *f, const uint32_t *px, int w, int h, char *why, size_t n)
{
	const size_t raw = (size_t)h * (1 + 3 * (size_t)w), blocks = (raw + 65534) / 65535;
	uint8_t hdr[33] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R' };
	be32(hdr + 16, (uint32_t)w);
	be32(hdr + 20, (uint32_t)h);
	hdr[24] = 8, hdr[25] = 2;
	be32(hdr + 29, crc(~0u, hdr + 12, 17) ^ ~0u);
	uint8_t idat[10];
	be32(idat, (uint32_t)(2 + raw + 5 * blocks + 4));
	memcpy(idat + 4, "IDAT\x78\x01", 6);
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(idat, 1, sizeof(idat), f);
	uint32_t c = crc(~0u, idat + 4, 6), a = 1, b = 0;
	size_t left = raw, in_block = 0;
	uint8_t *row = malloc(1 + 3 * (size_t)w);
	if (!row) {
		snprintf(why, n, "Out of memory.");
		return false;
	}
	for (int y = 0; y < h; y++) {
		row[0] = 0;
		for (int x = 0; x < w; x++) {
			uint32_t p = px[(size_t)y * w + x];
			row[1 + 3 * x] = p >> 16, row[2 + 3 * x] = p >> 8, row[3 + 3 * x] = p;
		}
		for (size_t i = 0, len = 1 + 3 * (size_t)w; i < len;) {
			if (!in_block) {	/* next stored block header */
				in_block = left < 65535 ? left : 65535;
				uint8_t bh[5] = { left == in_block, in_block & 0xff, in_block >> 8, ~in_block & 0xff, (~in_block >> 8) & 0xff };
				fwrite(bh, 1, 5, f);
				c = crc(c, bh, 5);
			}
			size_t k = len - i < in_block ? len - i : in_block;
			fwrite(row + i, 1, k, f);
			c = crc(c, row + i, k);
			for (size_t j = 0; j < k; j++)
				a = (a + row[i + j]) % 65521, b = (b + a) % 65521;
			i += k, in_block -= k, left -= k;
		}
	}
	free(row);
	uint8_t tail[20];
	be32(tail, b << 16 | a);
	c = crc(c, tail, 4);
	be32(tail + 4, c ^ ~0u);
	memcpy(tail + 8, "\0\0\0\0IEND\xae\x42\x60\x82", 12);
	fwrite(tail, 1, sizeof(tail), f);
	return true;
}
#define EXT "PNG"
#endif

bool cam_save_photo(char *path, size_t pn, int num, const uint8_t *nv12, int w, int h, int stride, cam_orient_t o,
		    char *why, size_t n)
{
	cam_conv_t c;
	int ow = o.rot % 180 ? h : w, oh = o.rot % 180 ? w : h;
	snprintf(path, pn, "%s/IMG_%04d." EXT, cam_photos_dir(), num);
	if (!cam_conv_init(&c, w, h, stride, o, ow, oh)) {
		snprintf(why, n, "Out of memory.");
		return false;
	}
	uint32_t *px = malloc(sizeof(uint32_t) * ow * oh);
	if (!px) {
		cam_conv_free(&c);
		snprintf(why, n, "Out of memory.");
		return false;
	}
	cam_convert(&c, nv12, nv12 + (size_t)stride * h, px, ow);
	cam_conv_free(&c);
	cam_lut_photo(px, (size_t)ow * oh);	/* the chosen filter (cam_lut.c; Original = nothing) */
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	bool ok = f != NULL;
	if (!f)
		snprintf(why, n, "Can't write %s: %s.", cam_photos_dir(), strerror(errno));
	else {
		ok = write_image(f, px, ow, oh, why, n);
		if (fflush(f) != 0 || ferror(f)) {
			if (ok)
				snprintf(why, n, "Couldn't save the photo: %s.", strerror(errno));
			ok = false;
		}
		fclose(f);
		if (ok && rename(tmp, path) != 0) {
			snprintf(why, n, "Couldn't save the photo: %s.", strerror(errno));
			ok = false;
		}
		if (!ok)
			remove(tmp);
	}
	free(px);
	return ok;
}

static void thumb_path(const char *photo, char *out, size_t n)
{
	const char *base = strrchr(photo, '/');
	base = base ? base + 1 : photo;
	const char *dot = strrchr(base, '.');
	int len = dot ? (int)(dot - base) : (int)strlen(base);
	snprintf(out, n, "%s/.thumbs/%.*s.bgra", cam_photos_dir(), len, base);
}

bool cam_thumb_write(const char *photo, const uint32_t *px)
{
	char p[600], tmp[620];
	uint32_t *graded = malloc(sizeof(uint32_t) * CAM_THUMB * CAM_THUMB);	/* the same filter as its photo */
	if (graded) {
		memcpy(graded, px, sizeof(uint32_t) * CAM_THUMB * CAM_THUMB);
		cam_lut_thumb(graded, CAM_THUMB * CAM_THUMB);
		px = graded;
	}
	snprintf(p, sizeof(p), "%s/.thumbs", cam_photos_dir());
	mkdir(p, 0755);
	thumb_path(photo, p, sizeof(p));
	snprintf(tmp, sizeof(tmp), "%s.part", p);
	FILE *f = fopen(tmp, "wb");
	bool ok = f && fwrite(px, sizeof(uint32_t), CAM_THUMB * CAM_THUMB, f) == CAM_THUMB * CAM_THUMB;
	ok = f && fclose(f) == 0 && ok && rename(tmp, p) == 0;
	if (!ok)
		remove(tmp);
	free(graded);
	return ok;
}

bool cam_thumb_read(const char *photo, uint32_t *px)
{
	char p[600];
	thumb_path(photo, p, sizeof(p));
	FILE *f = fopen(p, "rb");
	if (!f)
		return false;
	bool ok = fread(px, sizeof(uint32_t), CAM_THUMB * CAM_THUMB, f) == CAM_THUMB * CAM_THUMB;
	fclose(f);
	return ok;
}

bool cam_photo_delete(const char *photo)
{
	char p[600];
	thumb_path(photo, p, sizeof(p));
	remove(p);
	return remove(photo) == 0;
}
