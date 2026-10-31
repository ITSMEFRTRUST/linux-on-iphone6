/* Preferences: a key=value text file, $ORCHARD_DATA (or ~/.orchard) /prefs, rewritten on every set.
 * Lives in RAM on the phone (initramfs); `bin/orc pull` copies the data dir back to the PC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "orchard.h"

#define MAX_KEYS 64
static struct { char key[32], val[96]; } kv[MAX_KEYS];
static int nkv;
static bool loaded;

const char *data_dir(void)
{
	static char dir[256];
	if (!dir[0]) {
		const char *e = getenv("ORCHARD_DATA"), *home = getenv("HOME");
		if (e)
			snprintf(dir, sizeof(dir), "%s", e);
		else
			snprintf(dir, sizeof(dir), "%s/.orchard", home && *home ? home : "/root");
		mkdir(dir, 0755);
	}
	return dir;
}

static void load(void)
{
	char path[300], line[160];
	loaded = true;
	snprintf(path, sizeof(path), "%s/prefs", data_dir());
	FILE *f = fopen(path, "r");
	while (f && nkv < MAX_KEYS && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');
		if (!eq)
			continue;
		if (nl)
			*nl = 0;
		*eq = 0;
		snprintf(kv[nkv].key, sizeof(kv[0].key), "%s", line);
		snprintf(kv[nkv].val, sizeof(kv[0].val), "%s", eq + 1);
		nkv++;
	}
	if (f)
		fclose(f);
}

static void save(void)
{
	char path[300], tmp[310];
	snprintf(path, sizeof(path), "%s/prefs", data_dir());
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < nkv; i++)
		fprintf(f, "%s=%s\n", kv[i].key, kv[i].val);
	fclose(f);
	rename(tmp, path);
}

const char *prefs_str(const char *key, const char *def)
{
	if (!loaded)
		load();
	for (int i = 0; i < nkv; i++)
		if (!strcmp(kv[i].key, key))
			return kv[i].val;
	return def;
}

void prefs_set_str(const char *key, const char *v)
{
	int i;
	if (!loaded)
		load();
	for (i = 0; i < nkv && strcmp(kv[i].key, key); i++)
		;
	if (i == nkv) {
		if (nkv == MAX_KEYS)
			return;
		snprintf(kv[nkv++].key, sizeof(kv[0].key), "%s", key);
	}
	snprintf(kv[i].val, sizeof(kv[0].val), "%s", v);
	save();
}

int prefs_int(const char *key, int def)
{
	const char *s = prefs_str(key, NULL);
	return s ? atoi(s) : def;
}

void prefs_set_int(const char *key, int v)
{
	char b[16];
	snprintf(b, sizeof(b), "%d", v);
	prefs_set_str(key, b);
}
