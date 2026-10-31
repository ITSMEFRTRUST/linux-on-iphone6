/* JSON → tree; see json.h. Recursive descent with a depth limit; \u escapes (with surrogate pairs) → UTF-8. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

#define MAX_DEPTH 64

static const char *ws(const char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	return s;
}

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		int c = s[i];
		int d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (d < 0)
			return -1;
		v = v * 16 + d;
	}
	return v;
}

static int put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xc0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
	if (cp < 0x10000) { o[0] = 0xe0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
	o[0] = 0xf0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
	return 4;
}

/* s at the opening quote; returns past the closing one, *out malloc'd */
static const char *str(const char *s, char **out)
{
	const char *e = ++s;
	size_t n = 0;
	while (*e && *e != '"') {	/* upper bound of the decoded size */
		if (*e == '\\' && e[1])
			e++;
		e++;
		n++;
	}
	if (*e != '"')
		return NULL;
	char *o = malloc(n * 4 + 1), *w = o;
	if (!o)
		return NULL;
	while (s < e) {
		if (*s != '\\') {
			*w++ = *s++;
			continue;
		}
		s++;
		switch (*s) {
		case 'n': *w++ = '\n'; s++; break;
		case 't': *w++ = '\t'; s++; break;
		case 'r': *w++ = '\r'; s++; break;
		case 'b': *w++ = '\b'; s++; break;
		case 'f': *w++ = '\f'; s++; break;
		case 'u': {
			int cp = e - s >= 5 ? hex4(s + 1) : -1;
			if (cp < 0) {
				*w++ = '?';
				s++;
				break;
			}
			s += 5;
			if (cp >= 0xd800 && cp < 0xdc00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
				int lo = hex4(s + 2);
				if (lo >= 0xdc00 && lo < 0xe000) {
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					s += 6;
				}
			}
			if (cp == 0 || (cp >= 0xd800 && cp < 0xe000))
				cp = 0xfffd;
			w += put_utf8(w, cp);
			break;
		}
		default: *w++ = *s++; break;
		}
	}
	*w = 0;
	*out = o;
	return e + 1;
}

static const char *value(const char *s, json_t **out, int depth);

static bool add(json_t *p, json_t *k)
{
	if (p->n == 0 || (p->n >= 4 && (p->n & (p->n - 1)) == 0)) {	/* capacity 4, 8, 16, … */
		json_t **nk = realloc(p->kids, (p->n ? p->n * 2 : 4) * sizeof(*nk));
		if (!nk)
			return false;
		p->kids = nk;
	}
	p->kids[p->n++] = k;
	return true;
}

static const char *value(const char *s, json_t **out, int depth)
{
	*out = NULL;
	s = ws(s);
	if (depth > MAX_DEPTH)
		return NULL;
	json_t *j = calloc(1, sizeof(*j));
	if (!j)
		return NULL;
	*out = j;
	if (*s == '{' || *s == '[') {
		bool obj = *s == '{';
		j->type = obj ? J_OBJ : J_ARR;
		s = ws(s + 1);
		if (*s == (obj ? '}' : ']'))
			return s + 1;
		for (;;) {
			char *key = NULL;
			if (obj) {
				if (*s != '"' || !(s = str(s, &key)))
					return NULL;
				s = ws(s);
				if (*s != ':') {
					free(key);
					return NULL;
				}
				s++;
			}
			json_t *k;
			s = value(s, &k, depth + 1);
			if (k)
				k->key = key;
			else
				free(key);
			if (!s || !k || !add(j, k)) {
				json_free(k);
				return NULL;
			}
			s = ws(s);
			if (*s == ',') {
				s = ws(s + 1);
				continue;
			}
			if (*s == (obj ? '}' : ']'))
				return s + 1;
			return NULL;
		}
	}
	if (*s == '"') {
		j->type = J_STR;
		return str(s, &j->str);
	}
	if (!strncmp(s, "true", 4) || !strncmp(s, "false", 5)) {
		j->type = J_BOOL;
		j->num = *s == 't';
		return s + (*s == 't' ? 4 : 5);
	}
	if (!strncmp(s, "null", 4)) {
		j->type = J_NULL;
		return s + 4;
	}
	char *e;
	j->num = strtod(s, &e);
	if (e == s)
		return NULL;
	j->type = J_NUM;
	return e;
}

json_t *json_parse(const char *s)
{
	json_t *j;
	if (!s)
		return NULL;
	const char *e = value(s, &j, 0);
	if (!e) {
		json_free(j);
		return NULL;
	}
	return j;
}

void json_free(json_t *j)
{
	if (!j)
		return;
	for (int i = 0; i < j->n; i++)
		json_free(j->kids[i]);
	free(j->kids);
	free(j->str);
	free(j->key);
	free(j);
}

json_t *json_get(const json_t *o, const char *key)
{
	if (!o || o->type != J_OBJ)
		return NULL;
	for (int i = 0; i < o->n; i++)
		if (o->kids[i]->key && !strcmp(o->kids[i]->key, key))
			return o->kids[i];
	return NULL;
}

json_t *json_at(const json_t *a, int i) { return a && a->type == J_ARR && i >= 0 && i < a->n ? a->kids[i] : NULL; }
int json_len(const json_t *a) { return a && a->type == J_ARR ? a->n : 0; }
double json_num(const json_t *j, double def) { return j && (j->type == J_NUM || j->type == J_BOOL) ? j->num : def; }
const char *json_str(const json_t *j, const char *def) { return j && j->type == J_STR ? j->str : def; }
