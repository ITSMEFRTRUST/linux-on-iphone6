/* Reader-mode HTML → blocks for the Web app; see html.h.
 * A forgiving tokenizer (comments, raw-text elements, attributes with any quoting, entities) feeds an
 * element stack that knows which elements are blocks, which imply closing (p, li, td, …) and which are
 * skipped (scripts, hidden or boilerplate classes). Navigation (<nav>, role=navigation, <menu>) isn't shown
 * in the text: its links are collected into doc->menu. Forms become field blocks (inputs, textareas, selects,
 * buttons; checkboxes / radios are inline runs) with their hidden fields kept for submitting. Text is collapsed
 * into runs with bold / code / link flags. When the page marks its content (<main>, <article>, role=main) and
 * that holds enough text, everything outside it is dropped (site search forms excepted) — the "reader" part. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "html.h"
#include "net.h"

#define MAX_BLOCKS 1500
#define MAX_TEXT (1 << 20)	/* total text kept */
#define MAX_LINKS 5000
#define MAX_IMGS 60
#define MAX_DEPTH 256
#define MIN_IMG 40		/* declared smaller than this = icon / tracking pixel */
#define MAIN_MIN 400		/* chars inside <main>/<article> before the rest is dropped */
#define MAX_FIELDS 400
#define MAX_FORMS 60
#define MAX_MENU 80
#define MAX_OPTS 400

/* ---------- growable strings ---------- */

typedef struct sb { char *p; int len, cap; } sb_t;

static bool sb_room(sb_t *b, int n)
{
	if (b->len + n + 1 <= b->cap)
		return true;
	int nc = b->cap ? b->cap * 2 : 256;
	while (nc < b->len + n + 1)
		nc *= 2;
	char *np = realloc(b->p, nc);
	if (!np)
		return false;
	b->p = np;
	b->cap = nc;
	return true;
}

static void sb_put(sb_t *b, const char *s, int n)
{
	if (n <= 0 || !sb_room(b, n))
		return;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void sb_c(sb_t *b, char c) { sb_put(b, &c, 1); }

static int utf8_put(char *o, unsigned cp)
{
	if (cp == 0 || (cp >= 0xd800 && cp < 0xe000) || cp > 0x10ffff)
		cp = 0xfffd;
	if (cp < 0x80) {
		o[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = 0xc0 | cp >> 6;
		o[1] = 0x80 | (cp & 63);
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = 0xe0 | cp >> 12;
		o[1] = 0x80 | (cp >> 6 & 63);
		o[2] = 0x80 | (cp & 63);
		return 3;
	}
	o[0] = 0xf0 | cp >> 18;
	o[1] = 0x80 | (cp >> 12 & 63);
	o[2] = 0x80 | (cp >> 6 & 63);
	o[3] = 0x80 | (cp & 63);
	return 4;
}

static void sb_cp(sb_t *b, unsigned cp)
{
	char t[4];
	sb_put(b, t, utf8_put(t, cp));
}

static char *dupn(const char *s, int n)
{
	char *r = malloc(n + 1);
	if (r) {
		memcpy(r, s, n);
		r[n] = 0;
	}
	return r;
}

/* ---------- charset ---------- */

static const uint16_t cp1252[32] = { 0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030,
				     0x0160, 0x2039, 0x0152, 0x8d, 0x017d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c,
				     0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d,
				     0x017e, 0x0178 };

static unsigned latin(unsigned char c) { return c >= 0x80 && c < 0xa0 ? cp1252[c - 0x80] : c; }

/* length of a valid UTF-8 sequence at s (n bytes left), 0 = invalid */
static int utf8_ok(const unsigned char *s, size_t n)
{
	unsigned c = s[0];
	int k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
	if (!k || (size_t)k > n || (k == 2 && c < 0xc2) || (k == 4 && c > 0xf4))
		return 0;
	for (int i = 1; i < k; i++)
		if ((s[i] & 0xc0) != 0x80)
			return 0;
	if (k == 3 && c == 0xe0 && s[1] < 0xa0)
		return 0;
	if (k == 4 && c == 0xf0 && s[1] < 0x90)
		return 0;
	return k;
}

static bool is_latin_name(const char *v, int n)
{
	static const char *const names[] = { "iso-8859-1", "iso8859-1", "latin1", "latin-1", "windows-1252",
					     "cp1252", "iso-8859-15", "us-ascii", "ascii" };
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if ((int)strlen(names[i]) == n && !strncasecmp(v, names[i], n))
			return true;
	return false;
}

static bool charset_latin(const char *src, size_t len, const char *ctype)
{
	const char *c = ctype ? strcasestr(ctype, "charset=") : NULL;
	if (c) {
		c += 8;
		c += *c == '"';
		return is_latin_name(c, strcspn(c, "\"; "));
	}
	size_t n = len < 4096 ? len : 4096;
	for (size_t i = 0; i + 8 < n; i++) {
		if (strncasecmp(src + i, "charset=", 8))
			continue;
		const char *v = src + i + 8;
		while (*v == '"' || *v == '\'' || *v == ' ')
			v++;
		int k = 0;
		while (v + k < src + n && (isalnum((unsigned char)v[k]) || v[k] == '-' || v[k] == '_'))
			k++;
		return is_latin_name(v, k);
	}
	return false;
}

/* whole document as UTF-8 (latin-1 / cp1252 converted, invalid UTF-8 bytes read as cp1252, NULs dropped) */
static char *to_utf8(const char *src, size_t len, bool lat, size_t *olen)
{
	sb_t b = { 0 };
	if (!sb_room(&b, len + len / 8 + 16))
		return NULL;
	b.p[0] = 0;	/* an empty / all-NUL body is still a C string */
	const unsigned char *s = (const unsigned char *)src;
	for (size_t i = 0; i < len;) {
		if (!s[i]) {
			i++;
			continue;
		}
		int k = lat ? (s[i] < 0x80) : utf8_ok(s + i, len - i);
		if (k) {
			sb_put(&b, (const char *)s + i, k);
			i += k;
		} else {
			sb_cp(&b, latin(s[i]));
			i++;
		}
	}
	*olen = b.len;
	if (!b.p)
		b.p = calloc(1, 1);
	return b.p;
}

/* ---------- entities ---------- */

static const struct { const char *n; unsigned cp; } ents[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 },
	{ "ndash", 0x2013 }, { "mdash", 0x2014 }, { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
	{ "ldquo", 0x201c }, { "rdquo", 0x201d }, { "sbquo", 0x201a }, { "bdquo", 0x201e }, { "laquo", 0xab },
	{ "raquo", 0xbb }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203a }, { "middot", 0xb7 }, { "bull", 0x2022 },
	{ "copy", 0xa9 }, { "reg", 0xae }, { "trade", 0x2122 }, { "deg", 0xb0 }, { "times", 0xd7 },
	{ "divide", 0xf7 }, { "plusmn", 0xb1 }, { "euro", 0x20ac }, { "pound", 0xa3 }, { "yen", 0xa5 },
	{ "cent", 0xa2 }, { "sect", 0xa7 }, { "para", 0xb6 }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
	{ "prime", 0x2032 }, { "Prime", 0x2033 }, { "minus", 0x2212 }, { "le", 0x2264 }, { "ge", 0x2265 },
	{ "ne", 0x2260 }, { "asymp", 0x2248 }, { "infin", 0x221e }, { "larr", 0x2190 }, { "rarr", 0x2192 },
	{ "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 }, { "thinsp", 0x2009 }, { "ensp", 0x2002 },
	{ "emsp", 0x2003 }, { "zwj", 0x200d }, { "zwnj", 0x200c }, { "shy", 0xad }, { "iexcl", 0xa1 },
	{ "iquest", 0xbf }, { "frac12", 0xbd }, { "frac14", 0xbc }, { "frac34", 0xbe }, { "sup2", 0xb2 },
	{ "sup3", 0xb3 }, { "micro", 0xb5 }, { "ordm", 0xba }, { "ordf", 0xaa }, { "szlig", 0xdf },
	{ "agrave", 0xe0 }, { "aacute", 0xe1 }, { "acirc", 0xe2 }, { "atilde", 0xe3 }, { "auml", 0xe4 },
	{ "aring", 0xe5 }, { "aelig", 0xe6 }, { "ccedil", 0xe7 }, { "egrave", 0xe8 }, { "eacute", 0xe9 },
	{ "ecirc", 0xea }, { "euml", 0xeb }, { "igrave", 0xec }, { "iacute", 0xed }, { "icirc", 0xee },
	{ "iuml", 0xef }, { "ntilde", 0xf1 }, { "ograve", 0xf2 }, { "oacute", 0xf3 }, { "ocirc", 0xf4 },
	{ "otilde", 0xf5 }, { "ouml", 0xf6 }, { "oslash", 0xf8 }, { "ugrave", 0xf9 }, { "uacute", 0xfa },
	{ "ucirc", 0xfb }, { "uuml", 0xfc }, { "yacute", 0xfd }, { "yuml", 0xff }, { "Agrave", 0xc0 },
	{ "Aacute", 0xc1 }, { "Acirc", 0xc2 }, { "Auml", 0xc4 }, { "Aring", 0xc5 }, { "Ccedil", 0xc7 },
	{ "Eacute", 0xc9 }, { "Egrave", 0xc8 }, { "Ntilde", 0xd1 }, { "Oacute", 0xd3 }, { "Ouml", 0xd6 },
	{ "Uacute", 0xda }, { "Uuml", 0xdc }, { "alpha", 0x3b1 }, { "beta", 0x3b2 }, { "gamma", 0x3b3 },
	{ "delta", 0x3b4 }, { "pi", 0x3c0 }, { "mu", 0x3bc }, { "sigma", 0x3c3 }, { "omega", 0x3c9 },
	{ "lambda", 0x3bb }, { "theta", 0x3b8 }, { "Delta", 0x394 }, { "Omega", 0x3a9 }, { "Sigma", 0x3a3 },
	{ "hearts", 0x2665 }, { "check", 0x2713 }, { "star", 0x2606 }, { "starf", 0x2605 },
};

/* s points just after '&'; returns the characters consumed (0 = not an entity) */
static int ent_decode(const char *s, int n, unsigned *cp)
{
	if (n > 0 && s[0] == '#') {
		int i = 1;
		bool hex = i < n && (s[i] == 'x' || s[i] == 'X');
		i += hex;
		unsigned v = 0;
		int d0 = i;
		while (i < n && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i])) && i - d0 < 8) {
			int c = s[i];
			v = v * (hex ? 16 : 10) + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
			i++;
		}
		if (i == d0)
			return 0;
		if (i < n && s[i] == ';')
			i++;
		*cp = v >= 0x80 && v < 0xa0 ? cp1252[v - 0x80] : v;
		return i;
	}
	int k = 0;
	while (k < n && k < 10 && isalnum((unsigned char)s[k]))
		k++;
	if (!k)
		return 0;
	for (unsigned e = 0; e < sizeof(ents) / sizeof(ents[0]); e++)
		if ((int)strlen(ents[e].n) == k && !strncmp(s, ents[e].n, k)) {
			*cp = ents[e].cp;
			return k + (k < n && s[k] == ';');
		}
	return 0;
}

/* decoded entities appended to b; ws = collapse whitespace runs to one space */
static void put_decoded(sb_t *b, const char *s, int n, bool ws)
{
	bool sp = false;
	for (int i = 0; i < n;) {
		unsigned char c = s[i];
		if (ws && (isspace(c))) {
			sp = true;
			i++;
			continue;
		}
		if (sp && b->len) {
			sb_c(b, ' ');
		}
		sp = false;
		unsigned cp;
		int k;
		if (c == '&' && (k = ent_decode(s + i + 1, n - i - 1, &cp)) > 0) {
			if (ws && (cp == 0xa0 || cp == ' '))
				sb_c(b, ' ');
			else
				sb_cp(b, cp);
			i += 1 + k;
		} else {
			sb_c(b, c);
			i++;
		}
	}
	if (sp && b->len && ws)
		sb_c(b, ' ');
}

/* ---------- tags ---------- */

static bool in_list(const char *t, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(t, *l))
			return true;
	return false;
}

static const char *const VOID_TAGS[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
					 "param", "source", "track", "wbr", "keygen", NULL };
static const char *const BLOCK_TAGS[] = { "address", "article", "aside", "blockquote", "body", "center", "dd",
					  "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption",
					  "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header",
					  "hgroup", "html", "li", "main", "menu", "nav", "ol", "p", "pre", "section",
					  "summary", "table", "tbody", "thead", "tfoot", "tr", "ul", "caption",
					  "legend", "noscript", NULL };	/* td / th: cells join their row */
/* button / select / option / textarea text is the field's own (collected separately, see put_text) */
static const char *const SKIP_TAGS[] = { "script", "style", "template", "svg", "math", "footer", "aside",
					 "button", "select", "option", "textarea", "input", "iframe", "object",
					 "embed", "video", "audio", "canvas", "dialog", "noembed", "map", "head",
					 "datalist", NULL };
static const char *const RAW_TAGS[] = { "script", "style", "textarea", "title", "iframe", "noembed", "xmp", NULL };
/* class tokens that mark boilerplate / hidden things */
static const char *const SKIP_CLASSES[] = { "mw-editsection", "navbox", "vertical-navbox", "navbox-styles",
					    "mw-jump-link", "noprint", "sr-only", "screen-reader-text",
					    "visually-hidden", "visuallyhidden", "reference", "mw-cite-backlink",
					    "metadata", "sidebar", "catlinks", "printfooter", "mw-indicators",
					    "mw-empty-elt", "hidden", "d-none", "is-hidden", "skip-link", "skiplink",
					    "cookie-banner", "cookie-notice", "toc", "mw-hidden-catlinks", "share",
					    "social-share", "newsletter", "advert", "advertisement", "ad-container",
					    "related-links", "breadcrumb", "breadcrumbs",
					    "mw-references-wrap", "minerva__tab-container", "page-actions-menu",
					    "vector-page-toolbar", "mw-portlet", "vector-menu-tabs", "vector-dropdown",
					    "mw-footer", "post-navigation", "comment-respond"
					    /* references stay off: hundreds of rows of citations */,
					    "reflist", NULL };

/* class tokens of secondary text (bylines, dates, Hacker News' "subtext"): shown smaller */
static const char *const META_CLASSES[] = { "subtext", "subline", "byline", "dateline", "meta", "post-meta", "entry-meta",
					    "article-meta", "timestamp", "credit", "credits", "comhead", "text-muted",
					    "muted", "small", NULL };

typedef struct attr { const char *n, *v; int nl, vl; } attr_t;

static const char *attr_get(const attr_t *a, int na, const char *name, int *len)
{
	int l = strlen(name);
	for (int i = 0; i < na; i++)
		if (a[i].nl == l && !strncasecmp(a[i].n, name, l)) {
			*len = a[i].vl;
			return a[i].v;
		}
	return NULL;
}

/* attribute value with entities decoded (malloc'd) */
static char *attr_dup(const attr_t *a, int na, const char *name)
{
	int l;
	const char *v = attr_get(a, na, name, &l);
	if (!v)
		return NULL;
	sb_t b = { 0 };
	put_decoded(&b, v, l, false);
	if (!b.p)
		b.p = calloc(1, 1);
	return b.p;
}

static bool has_token(const char *v, int vl, const char *const *list)
{
	for (int i = 0; i < vl;) {
		while (i < vl && isspace((unsigned char)v[i]))
			i++;
		int s = i;
		while (i < vl && !isspace((unsigned char)v[i]))
			i++;
		if (i > s) {
			for (const char *const *l = list; *l; l++)
				if ((int)strlen(*l) == i - s && !strncasecmp(v + s, *l, i - s))
					return true;
		}
	}
	return false;
}

/* ---------- parser ---------- */

struct ent {
	char tag[16];
	bool skip, block, main;
	bool menu;		/* a navigation element (its links go to doc->menu) */
	bool menu_a;		/* the <a> whose text is being collected for doc->menu */
	bool searchrole;	/* role=search */
	int field;		/* <label> (and what is inside it) toggles this checkbox / radio, -1 none */
	bool cell;		/* inside a table cell: blocks in it join the row */
	int flags, link;	/* cumulative inline flags + link */
	enum html_kind kind;	/* block kind for text inside */
	int level;
	int ol_count;		/* ol: last number given */
	bool ol;
	int li_num;		/* li: its number (0 bullet), -1 after its first block */
};

#define MAX_TSTACK 16
#define MAX_TCOLS 16		/* columns looked at while parsing (a data table shows at most HT_MAXCOLS) */
#define MAX_TROWS 200		/* longer tables are flattened (objects per row) */
#define MAX_CELL 240		/* a cell with more text = a layout table */
struct ptable { int first, nrows; bool bad; };	/* an open <table>: its first block, rows so far */

typedef struct parser {
	html_doc_t *d;
	struct ent st[MAX_DEPTH];
	int depth, overflow, nskip;
	/* current block */
	sb_t t;
	html_run_t *runs;
	int nruns, rcap;
	bool space, sep;	/* pending space / table-cell separator */
	enum html_kind kind;
	int level, num;
	bool started, main;
	char *anchor;
	bool *inmain;		/* per block */
	int bcap, total, lcap, icap;
	bool stop;
	/* forms */
	int form;		/* open <form>, -1 none */
	int fcap, formcap, mcap;
	int sel;		/* open <select> field, -1 none */
	bool opt_on;		/* collecting an <option>'s text */
	int btn;		/* open <button> field (its text = label), -1 none */
	sb_t fbuf;		/* option / button text */
	char **form_attr;	/* per field: its form="id" attribute (resolved at the end), or NULL */
	int nsearch;		/* role=search elements open */
	int ntemplate;
	/* navigation menus */
	int nmenu;		/* menu elements open (each also counts in nskip) */
	int menu_cur;		/* doc->menu entry collecting its text, -1 none */
	sb_t mbuf;
	/* tables: open ones (innermost last) and the row being collected */
	struct ptable pt[MAX_TSTACK];
	int npt;
	int rtable;		/* pt index of the row's table, -1 = no row */
	html_cell_t *rc;	/* the row's cells */
	int nrc, rccap, rcol;	/* rcol = next column */
	int cell_wait;		/* rc index of the cell waiting for its first text, -1 none */
	int cell_txt;		/* rc index of the cell taking text (len still open), -1 none */
	bool rhead, rallhead;	/* row in <thead> / only <th> cells so far */
	bool row_end;		/* flush() for the end of a <tr> */
} parser_t;

static struct ent *top(parser_t *p) { return p->depth ? &p->st[p->depth - 1] : NULL; }

static void add_block(parser_t *p, html_block_t *blk, bool main)
{
	html_doc_t *d = p->d;
	if (d->nb >= MAX_BLOCKS || p->total > MAX_TEXT) {
		d->cut = p->stop = true;
		free(blk->text);
		free(blk->runs);
		free(blk->anchor);
		free(blk->cells);
		return;
	}
	if (d->nb == p->bcap) {
		int nc = p->bcap ? p->bcap * 2 : 64;
		html_block_t *nb = realloc(d->b, nc * sizeof(*nb));
		bool *nm = realloc(p->inmain, nc * sizeof(*nm));
		if (nm)
			p->inmain = nm;
		if (!nb || !nm) {
			if (nb)
				d->b = nb;
			p->stop = true;
			free(blk->text);
			free(blk->runs);
			free(blk->anchor);
			free(blk->cells);
			return;
		}
		d->b = nb;
		p->bcap = nc;
	}
	p->inmain[d->nb] = main;
	d->b[d->nb++] = *blk;
	p->total += blk->len;
}

static void flush(parser_t *p)
{
	p->space = p->sep = false;
	if (!p->started)
		return;
	/* trim trailing whitespace */
	while (p->t.len && (p->t.p[p->t.len - 1] == ' ' || p->t.p[p->t.len - 1] == '\n'))
		p->t.len--;
	while (p->nruns && p->runs[p->nruns - 1].off >= p->t.len)
		p->nruns--;
	if (p->nruns && p->runs[p->nruns - 1].off + p->runs[p->nruns - 1].len > p->t.len)
		p->runs[p->nruns - 1].len = p->t.len - p->runs[p->nruns - 1].off;
	bool row = p->rtable >= 0 && p->nrc;
	if (row && !p->row_end)	/* a picture / form field broke the row up: not a data table */
		p->pt[p->rtable].bad = true;
	if (p->t.len) {
		html_block_t b = { .kind = p->kind, .level = p->level, .num = p->num, .img = -1 };
		b.text = dupn(p->t.p, p->t.len);
		b.len = p->t.len;
		b.runs = malloc(p->nruns * sizeof(html_run_t) + 1);
		if (row && p->row_end) {	/* a table row: its cells (table_end decides whether they are shown as such) */
			if (p->cell_txt >= 0)
				p->rc[p->cell_txt].len = p->t.len - p->rc[p->cell_txt].off;
			b.cells = malloc(p->nrc * sizeof(html_cell_t));
			if (b.cells) {
				bool any = false;
				for (int k = 0; k < p->nrc; k++) {
					html_cell_t c = p->rc[k];
					if (c.off < 0 || c.off >= p->t.len)
						c.off = c.len = 0;
					else if (c.off + c.len > p->t.len)
						c.len = p->t.len - c.off;
					any = any || c.len;
					b.cells[b.ncells++] = c;
				}
				b.head = p->rhead || (p->rallhead && any);
				p->pt[p->rtable].nrows++;
			}
		}
		if (b.text && b.runs) {
			memcpy(b.runs, p->runs, p->nruns * sizeof(html_run_t));
			b.nruns = p->nruns;
			b.anchor = p->anchor;
			p->anchor = NULL;
			add_block(p, &b, p->main);
		} else {
			free(b.text);
			free(b.runs);
			free(b.cells);
		}
	}
	if (row) {
		p->nrc = 0;
		p->cell_wait = p->cell_txt = -1;
	}
	p->t.len = 0;
	p->nruns = 0;
	p->started = false;
}

static void begin_block(parser_t *p)
{
	struct ent *e = top(p);
	p->kind = e ? e->kind : HB_P;
	p->level = e ? e->level : 0;
	p->num = 0;
	p->main = e && e->main;
	if (p->kind == HB_LI) {	/* the li's first block gets its bullet / number */
		for (int i = p->depth - 1; i >= 0; i--)
			if (!strcmp(p->st[i].tag, "li")) {
				if (p->st[i].li_num < 0)
					p->kind = HB_P;	/* a later paragraph in the same item: indented, no bullet */
				else
					p->num = p->st[i].li_num;
				p->st[i].li_num = -1;
				break;
			}
	}
	p->started = true;
	p->t.len = 0;
	p->nruns = 0;
}

static void put_run_text(parser_t *p, const char *s, int n, int flags, int link)
{
	if (n <= 0)
		return;
	html_run_t *r = p->nruns ? &p->runs[p->nruns - 1] : NULL;
	if (!r || r->flags != flags || r->link != link || r->off + r->len != p->t.len) {
		if (p->nruns == p->rcap) {
			int nc = p->rcap ? p->rcap * 2 : 16;
			html_run_t *nr = realloc(p->runs, nc * sizeof(*nr));
			if (!nr)
				return;
			p->runs = nr;
			p->rcap = nc;
		}
		r = &p->runs[p->nruns++];
		r->off = p->t.len;
		r->len = 0;
		r->flags = flags;
		r->link = link;
	}
	sb_put(&p->t, s, n);
	r->len += n;
}

/* text is about to go into the current block: a table cell waiting for its first text starts here */
static void cell_mark(parser_t *p)
{
	if (p->cell_wait >= 0 && p->rtable >= 0) {
		p->rc[p->cell_wait].off = p->t.len;
		p->cell_txt = p->cell_wait;
		p->cell_wait = -1;
	}
}

/* what joins two table cells in the text: " · ", or a space after a rank cell ("12.") */
static const char *cell_sep(parser_t *p)
{
	for (int k = p->nrc - 1; k >= 0 && p->rtable >= 0; k--) {
		const html_cell_t *c = &p->rc[k];
		if (c->off < 0 || !c->len)
			continue;
		int n = c->len;
		const char *t = p->t.p + c->off;
		if (n > 1 && t[n - 1] == '.')
			n--;
		if (n > 4)
			break;
		for (int i = 0; i < n; i++)
			if (!isdigit((unsigned char)t[i]))
				return " \xc2\xb7 ";
		return " ";
	}
	return " \xc2\xb7 ";
}

static void cell_start(parser_t *p, const attr_t *a, int na, bool th);
static void row_start(parser_t *p);
static void table_end(parser_t *p);

/* decoded text into the current block */
static void put_text(parser_t *p, const char *s, int n, bool pre)
{
	if (p->stop)
		return;
	if (p->nskip) {	/* not shown, but it may be an option's / button's / menu link's text */
		if ((p->sel >= 0 && p->opt_on) || p->btn >= 0) {
			if (p->fbuf.len < 400)
				put_decoded(&p->fbuf, s, n, true);
		} else if (p->menu_cur >= 0 && p->nskip == p->nmenu && p->mbuf.len < 400) {
			put_decoded(&p->mbuf, s, n, true);
		}
		return;
	}
	struct ent *e = top(p);
	int flags = e ? e->flags : 0, link = e ? e->link : -1;
	if (e && e->field >= 0) {	/* label text: toggles its checkbox */
		flags = (flags & ~HR_LINK) | HR_FIELD;
		link = e->field;
	}
	for (int i = 0; i < n;) {
		unsigned char c = s[i];
		if (!pre && (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f')) {
			p->space = true;
			i++;
			continue;
		}
		int k = 1;
		char tmp[4];
		const char *out = s + i;
		unsigned cp = 0;
		if (c == '&' && (k = ent_decode(s + i + 1, n - i - 1, &cp)) > 0) {
			k++;
			if (cp == 0xad || cp == 0x200b) {	/* soft hyphen / zero-width space */
				i += k;
				continue;
			}
			if (cp == 0xa0)
				cp = ' ';	/* no-break space: a space that isn't collapsed */
			out = tmp;
			int ol = utf8_put(tmp, cp);
			if (!p->started)
				begin_block(p);
			if (p->sep && p->t.len) {
				const char *sp = cell_sep(p);
				put_run_text(p, sp, strlen(sp), flags & ~HR_LINK, -1);
				p->space = false;
			} else if (p->space && p->t.len && p->t.p[p->t.len - 1] != '\n') {
				put_run_text(p, " ", 1, flags, link);
			}
			p->space = p->sep = false;
			cell_mark(p);
			put_run_text(p, out, ol, flags, link);
			i += k;
			continue;
		}
		k = 1;
		if (c == '\r' && pre) {
			i++;
			continue;
		}
		/* a run of plain bytes */
		int j = i + 1;
		while (j < n && s[j] != '&' && (pre || !(s[j] == ' ' || s[j] == '\n' || s[j] == '\t' || s[j] == '\r' ||
						      s[j] == '\f')) && !(pre && s[j] == '\r'))
			j++;
		if (!p->started)
			begin_block(p);
		if (p->sep && p->t.len) {
			const char *sp = cell_sep(p);
			put_run_text(p, sp, strlen(sp), flags & ~HR_LINK, -1);
		} else if (p->space && p->t.len && p->t.p[p->t.len - 1] != '\n') {
			put_run_text(p, " ", 1, flags, link);	/* leads the next run: LVGL wraps a span whose trailing space overflows as a whole */
		}
		p->space = p->sep = false;
		cell_mark(p);
		put_run_text(p, s + i, j - i, flags, link);
		i = j;
	}
}

static int add_link(parser_t *p, const char *href)
{
	html_doc_t *d = p->d;
	char abs[2048];
	if (!href || !net_resolve(d->base, href, abs, sizeof(abs)) || d->nlinks >= MAX_LINKS)
		return -1;
	if (d->nlinks == p->lcap) {
		int nc = p->lcap ? p->lcap * 2 : 64;
		char **nl = realloc(d->links, nc * sizeof(*nl));
		if (!nl)
			return -1;
		d->links = nl;
		p->lcap = nc;
	}
	char *s = strdup(abs);
	if (!s)
		return -1;
	d->links[d->nlinks] = s;
	return d->nlinks++;
}

static bool skip_ext(const char *url)
{
	int n = strcspn(url, "?#");
	static const char *const ext[] = { ".svg", ".gif", ".webp", ".avif", ".ico", ".bmp", ".tif", ".tiff", NULL };
	for (const char *const *e = ext; *e; e++) {
		int el = strlen(*e);
		if (n >= el && !strncasecmp(url + n - el, *e, el))
			return true;
	}
	return !strncasecmp(url, "data:", 5);
}

/* srcset: the smallest candidate at least 600 px wide, else the widest; density lists: 1x (or the first) */
static char *pick_srcset(const char *v)
{
	char *best = NULL;
	int bestw = -1;
	bool best_ok = false;
	const char *s = v;
	while (*s) {
		while (*s == ',' || isspace((unsigned char)*s))
			s++;
		if (!*s)
			break;
		const char *u = s;
		while (*s && !isspace((unsigned char)*s) && !(*s == ',' && (isspace((unsigned char)s[1]) || !s[1])))
			s++;
		int ul = s - u;
		while (isspace((unsigned char)*s))
			s++;
		int w = 0;
		if (isdigit((unsigned char)*s)) {
			char *e;
			double v = strtod(s, &e);
			if (!(v > 0))	/* also NaN */
				v = 0;
			else if (v > 1e6)	/* "1e99w": (int) of that is undefined */
				v = 1e6;
			w = (*e == 'x' || *e == 'X') ? (v <= 1.0 ? 600 : 0) : (int)v;	/* density lists: 1x preferred */
			s = e;
		}
		while (*s && *s != ',')
			s++;
		char *cand = dupn(u, ul);
		if (!cand)
			break;
		if (skip_ext(cand)) {
			free(cand);
			continue;
		}
		bool ok = w >= 600;
		bool better = !best || (ok && (!best_ok || w < bestw)) || (!ok && !best_ok && w > bestw);
		if (better) {
			free(best);
			best = cand;
			bestw = w;
			best_ok = ok;
		} else {
			free(cand);
		}
	}
	return best;
}

static void add_img(parser_t *p, const attr_t *a, int na)
{
	html_doc_t *d = p->d;
	if (p->nskip || p->stop || d->nimgs >= MAX_IMGS)
		return;
	int l;
	const char *v;
	int w = (v = attr_get(a, na, "width", &l)) && !memchr(v, '%', l) ? atoi(v) : 0;
	int h = (v = attr_get(a, na, "height", &l)) && !memchr(v, '%', l) ? atoi(v) : 0;
	if ((w && w < MIN_IMG) || (h && h < MIN_IMG))
		return;
	if (p->started && p->t.len && w && w <= 120)
		return;	/* small inline picture inside a sentence */
	char *src = NULL;
	static const char *const srcs[] = { "data-src", "data-lazy-src", "data-original", "src", NULL };
	char *set = attr_dup(a, na, "data-srcset");
	if (!set)
		set = attr_dup(a, na, "srcset");
	if (set) {
		src = pick_srcset(set);
		free(set);
	}
	for (int i = 0; !src && srcs[i]; i++) {
		char *s = attr_dup(a, na, srcs[i]);
		if (s && *s && !skip_ext(s))
			src = s;
		else
			free(s);
	}
	char abs[2048];
	if (!src || !net_resolve(d->base, src, abs, sizeof(abs))) {
		free(src);
		return;
	}
	free(src);
	if (d->nimgs == p->icap) {
		int nc = p->icap ? p->icap * 2 : 8;
		html_img_t *ni = realloc(d->imgs, nc * sizeof(*ni));
		if (!ni)
			return;
		d->imgs = ni;
		p->icap = nc;
	}
	html_img_t *im = &d->imgs[d->nimgs];
	im->url = strdup(abs);
	im->w = w;
	im->h = h;
	im->alt = attr_dup(a, na, "alt");
	if (!im->url) {
		free(im->alt);
		return;
	}
	struct ent *e = top(p);
	/* the image breaks the paragraph: text before and after become their own blocks */
	bool resume = p->started;
	enum html_kind k = p->kind;
	int lvl = p->level;
	flush(p);
	html_block_t b = { .kind = HB_IMG, .img = d->nimgs++, .level = e ? e->level : 0 };
	b.text = calloc(1, 1);
	b.runs = malloc(sizeof(html_run_t));
	add_block(p, &b, e && e->main);
	if (resume) {
		begin_block(p);
		p->kind = k == HB_LI ? HB_P : k;
		p->level = lvl;
	}
}

static char *sb_trimmed(sb_t *b)	/* the buffer's text without outer spaces (malloc'd), and the buffer emptied */
{
	int s = 0, e = b->len;
	while (s < e && b->p[s] == ' ')
		s++;
	while (e > s && b->p[e - 1] == ' ')
		e--;
	char *r = dupn(b->p ? b->p + s : "", e - s);
	b->len = 0;
	if (b->p)
		b->p[0] = 0;
	return r;
}

static void option_end(parser_t *p)
{
	if (!p->opt_on)
		return;
	p->opt_on = false;
	html_field_t *f = &p->d->fields[p->sel];
	char *t = sb_trimmed(&p->fbuf);
	if (f->nopt > 0 && t) {
		free(f->opt[f->nopt - 1]);
		f->opt[f->nopt - 1] = t;
	} else {
		free(t);
	}
}

static void menu_end(parser_t *p)
{
	html_doc_t *d = p->d;
	if (p->menu_cur < 0)
		return;
	html_menu_t *m = &d->menu[p->menu_cur];
	char *t = sb_trimmed(&p->mbuf);
	if (t && *t) {
		free(m->text);
		m->text = t;
	} else {
		free(t);
	}
	bool drop = !m->text || !*m->text;	/* an icon without words */
	for (int i = 0; !drop && i < d->nmenu - 1; i++)	/* the same link twice (logo + "Home"): keep the first */
		drop = !strcmp(d->links[d->menu[i].link], d->links[m->link]);
	if (drop) {
		free(m->text);
		d->nmenu--;
	}
	p->menu_cur = -1;
}

static void pop_to(parser_t *p, int idx)	/* pop entries idx.. */
{
	while (p->depth > idx) {
		struct ent *e = &p->st[p->depth - 1];
		bool tr = !strcmp(e->tag, "tr");
		p->row_end = tr;
		if (e->block)
			flush(p);
		p->row_end = false;
		if (tr) {
			p->rtable = -1;
			p->nrc = 0;
		}
		if (!strcmp(e->tag, "table"))
			table_end(p);
		if (e->menu_a)
			menu_end(p);
		if (e->menu)
			p->nmenu--;
		if (e->searchrole)
			p->nsearch--;
		if (!strcmp(e->tag, "template"))
			p->ntemplate--;
		if (!strcmp(e->tag, "option") && p->sel >= 0)
			option_end(p);
		if (!strcmp(e->tag, "select") && p->sel >= 0) {
			option_end(p);
			p->sel = -1;
		}
		if (!strcmp(e->tag, "button") && p->btn >= 0) {
			html_field_t *f = &p->d->fields[p->btn];
			char *t = sb_trimmed(&p->fbuf);
			if (t && *t) {
				free(f->label);
				f->label = t;
			} else {
				free(t);
			}
			if (!f->label)
				f->label = strdup(f->value && *f->value ? f->value : "Submit");
			p->btn = -1;
		}
		if (!strcmp(e->tag, "form"))
			p->form = -1;
		if (e->skip)
			p->nskip--;
		p->depth--;
	}
}

/* ---------- forms ---------- */

static int add_form(parser_t *p, const attr_t *a, int na)
{
	html_doc_t *d = p->d;
	if (d->nforms >= MAX_FORMS)
		return -1;
	if (d->nforms == p->formcap) {
		int nc = p->formcap ? p->formcap * 2 : 8;
		html_form_t *nf = realloc(d->forms, nc * sizeof(*nf));
		if (!nf)
			return -1;
		d->forms = nf;
		p->formcap = nc;
	}
	html_form_t *f = &d->forms[d->nforms];
	memset(f, 0, sizeof(*f));
	char *act = attr_dup(a, na, "action"), abs[2048];
	if (act && *act) {
		if (!net_resolve(d->base, act, abs, sizeof(abs))) {	/* javascript:, mailto: … */
			free(act);
			return -1;
		}
		f->action = strdup(abs);
	} else {
		f->action = strdup(d->url ? d->url : d->base);
	}
	free(act);
	if (!f->action)
		return -1;
	int l;
	const char *v = attr_get(a, na, "method", &l);
	f->post = v && l == 4 && !strncasecmp(v, "post", 4);
	f->id = attr_dup(a, na, "id");
	f->search = p->nsearch > 0 || ((v = attr_get(a, na, "role", &l)) && l == 6 && !strncasecmp(v, "search", 6));
	return d->nforms++;
}

static int add_field(parser_t *p, enum html_ftype type, const attr_t *a, int na)
{
	html_doc_t *d = p->d;
	if (d->nfields >= MAX_FIELDS)
		return -1;
	char *fa = attr_dup(a, na, "form");
	if (p->form < 0 && !fa)
		return -1;	/* not in a form: nothing it could submit (a JavaScript widget) */
	if (d->nfields == p->fcap) {
		int nc = p->fcap ? p->fcap * 2 : 16;
		html_field_t *nf = realloc(d->fields, nc * sizeof(*nf));
		if (nf)
			d->fields = nf;
		char **nfa = realloc(p->form_attr, nc * sizeof(*nfa));
		if (nfa)
			p->form_attr = nfa;
		if (!nf || !nfa) {
			free(fa);
			return -1;
		}
		p->fcap = nc;
	}
	html_field_t *f = &d->fields[d->nfields];
	memset(f, 0, sizeof(*f));
	f->form = p->form;
	f->type = type;
	f->sel = -1;
	f->fmethod = -1;
	f->name = attr_dup(a, na, "name");
	if (f->name && !*f->name) {
		free(f->name);
		f->name = NULL;
	}
	f->value = attr_dup(a, na, "value");
	f->id = attr_dup(a, na, "id");
	int l;
	f->disabled = attr_get(a, na, "disabled", &l) != NULL;
	f->checked = attr_get(a, na, "checked", &l) != NULL;
	const char *v = attr_get(a, na, "maxlength", &l);
	f->maxlen = v ? atoi(v) : 0;
	if (f->maxlen < 0)
		f->maxlen = 0;
	static const char *const lab[] = { "placeholder", "aria-label", "title", "alt", NULL };
	for (int i = 0; lab[i] && !f->label; i++) {
		f->label = attr_dup(a, na, lab[i]);
		if (f->label && !*f->label) {
			free(f->label);
			f->label = NULL;
		}
	}
	char *fact = attr_dup(a, na, "formaction"), abs[2048];
	if (fact && *fact && net_resolve(d->base, fact, abs, sizeof(abs)))
		f->faction = strdup(abs);
	free(fact);
	if ((v = attr_get(a, na, "formmethod", &l)))
		f->fmethod = l == 4 && !strncasecmp(v, "post", 4);
	p->form_attr[d->nfields] = fa;
	return d->nfields++;
}

/* a field shown as its own block */
static void field_block(parser_t *p, int fi)
{
	flush(p);
	struct ent *e = top(p);
	html_block_t b = { .kind = HB_FIELD, .img = -1, .field = fi, .level = e ? e->level : 0 };
	b.text = calloc(1, 1);
	b.runs = malloc(sizeof(html_run_t));
	add_block(p, &b, e && e->main);
}

/* visible here? (only navigation menus hide it: its text isn't shown, its fields are) */
static bool field_shown(parser_t *p) { return p->nskip == p->nmenu && !p->ntemplate; }

static void start_input(parser_t *p, const attr_t *a, int na)
{
	int l;
	const char *v = attr_get(a, na, "type", &l);
	char t[16] = "text";
	if (v && l < (int)sizeof(t)) {
		for (int i = 0; i < l; i++)
			t[i] = tolower((unsigned char)v[i]);
		t[l] = 0;
	} else if (v) {
		t[0] = 0;
	}
	static const struct { const char *n; enum html_ftype t; } types[] = {
		{ "text", HF_TEXT }, { "search", HF_SEARCH }, { "password", HF_PASSWORD }, { "email", HF_EMAIL },
		{ "url", HF_URL }, { "number", HF_NUMBER }, { "tel", HF_TEL }, { "checkbox", HF_CHECKBOX },
		{ "radio", HF_RADIO }, { "submit", HF_SUBMIT }, { "image", HF_IMAGE }, { "hidden", HF_HIDDEN },
		{ "date", HF_TEXT }, { "time", HF_TEXT }, { "datetime-local", HF_TEXT }, { "month", HF_TEXT },
		{ "week", HF_TEXT }, { "color", HF_TEXT }, { "range", HF_NUMBER },
	};
	int type = -1;
	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (!strcmp(t, types[i].n))
			type = types[i].t;
	if (type < 0)	/* file, reset, button (JavaScript), unknown */
		return;
	if (type == HF_HIDDEN ? p->ntemplate > 0 : !field_shown(p))
		return;
	int fi = add_field(p, type, a, na);
	if (fi < 0)
		return;
	html_field_t *f = &p->d->fields[fi];
	if (type == HF_HIDDEN)
		return;
	if (type == HF_SEARCH && f->form >= 0)
		p->d->forms[f->form].search = true;
	if (type == HF_CHECKBOX || type == HF_RADIO) {	/* inline: a box run in the text, its label after it */
		if (!f->value)
			f->value = strdup("on");
		struct ent *e = top(p);
		int flags = e ? e->flags & ~HR_LINK : 0;
		if (!p->started)
			begin_block(p);
		if ((p->space || p->sep) && p->t.len && p->t.p[p->t.len - 1] != '\n')
			put_run_text(p, " ", 1, flags, -1);
		p->space = p->sep = false;
		cell_mark(p);
		put_run_text(p, "\xe2\x98\x90", 3, flags | HR_FIELD | HR_BOX, fi);
		p->space = true;
		for (int i = p->depth - 1; i >= 0; i--)	/* a <label> around it: its text toggles it too */
			if (!strcmp(p->st[i].tag, "label")) {
				if (p->st[i].field < 0)
					p->st[i].field = fi;
				break;
			}
		return;
	}
	if ((type == HF_SUBMIT || type == HF_IMAGE) && !f->label)
		f->label = strdup(f->value && *f->value ? f->value : "Submit");
	field_block(p, fi);
}

static void start_select(parser_t *p, const attr_t *a, int na)
{
	if (!field_shown(p) || p->sel >= 0)
		return;
	int fi = add_field(p, HF_SELECT, a, na);
	if (fi < 0)
		return;
	p->sel = fi;
	field_block(p, fi);
}

static void start_option(parser_t *p, const attr_t *a, int na)
{
	html_field_t *f = &p->d->fields[p->sel];
	option_end(p);
	if (f->nopt >= MAX_OPTS)
		return;
	char **no = realloc(f->opt, (f->nopt + 1) * sizeof(*no));
	if (no)
		f->opt = no;
	char **nv = realloc(f->optval, (f->nopt + 1) * sizeof(*nv));
	if (nv)
		f->optval = nv;
	if (!no || !nv)
		return;
	int l;
	f->opt[f->nopt] = attr_dup(a, na, "label");
	f->optval[f->nopt] = attr_dup(a, na, "value");
	if (attr_get(a, na, "selected", &l) || f->sel < 0)
		f->sel = f->nopt;
	f->nopt++;
	p->opt_on = true;
	p->fbuf.len = 0;
}

static void start_button(parser_t *p, const attr_t *a, int na)
{
	int l;
	const char *v = attr_get(a, na, "type", &l);
	if (v && !(l == 6 && !strncasecmp(v, "submit", 6)))
		return;	/* reset / button: JavaScript's */
	if (!field_shown(p) || p->btn >= 0)
		return;
	int fi = add_field(p, HF_SUBMIT, a, na);
	if (fi < 0)
		return;
	p->btn = fi;
	p->fbuf.len = 0;
	field_block(p, fi);
}

static void start_textarea(parser_t *p, const attr_t *a, int na, const char *raw, int rl)
{
	if (!field_shown(p))
		return;
	int fi = add_field(p, HF_TEXTAREA, a, na);
	if (fi < 0)
		return;
	html_field_t *f = &p->d->fields[fi];
	if (rl > 0 && raw[0] == '\n')	/* the newline right after <textarea> isn't part of it */
		raw++, rl--;
	sb_t b = { 0 };
	put_decoded(&b, raw, rl, false);
	free(f->value);
	f->value = b.p ? b.p : calloc(1, 1);
	field_block(p, fi);
}

/* a link inside a navigation menu: an entry in doc->menu (text collected until the </a>) */
static void menu_link(parser_t *p, struct ent *e, const attr_t *a, int na)
{
	html_doc_t *d = p->d;
	if (d->nmenu >= MAX_MENU || p->menu_cur >= 0)
		return;
	char *h = attr_dup(a, na, "href");
	int li = h && *h && h[0] != '#' ? add_link(p, h) : -1;
	free(h);
	if (li < 0)
		return;
	if (d->nmenu == p->mcap) {
		int nc = p->mcap ? p->mcap * 2 : 16;
		html_menu_t *nm = realloc(d->menu, nc * sizeof(*nm));
		if (!nm)
			return;
		d->menu = nm;
		p->mcap = nc;
	}
	html_menu_t *m = &d->menu[d->nmenu++];
	m->link = li;
	m->text = attr_dup(a, na, "aria-label");	/* until its own text comes */
	if (!m->text)
		m->text = attr_dup(a, na, "title");
	p->menu_cur = d->nmenu - 1;
	p->mbuf.len = 0;
	e->menu_a = true;
}

static int find_open(parser_t *p, const char *tag, const char *const *stop)
{
	for (int i = p->depth - 1; i >= 0; i--) {
		if (!strcmp(p->st[i].tag, tag))
			return i;
		if (stop && in_list(p->st[i].tag, stop))
			return -1;
	}
	return -1;
}

/* is word (whole, any case) one of the space-separated tokens in list? */
static bool rel_has(const char *list, const char *word)
{
	size_t wl = strlen(word);
	for (const char *q = list; *q;) {
		while (*q && isspace((unsigned char)*q))
			q++;
		size_t n = 0;
		while (q[n] && !isspace((unsigned char)q[n]))
			n++;
		if (n == wl && !strncasecmp(q, word, n))
			return true;
		q += n;
	}
	return false;
}

/* <link rel=icon / apple-touch-icon>: keep the best of each kind (sizes is malloc'd or NULL, freed here) */
static void link_icon(html_doc_t *d, const char *rel, const char *ty, const char *href, char *sizes)
{
	bool touch = rel_has(rel, "apple-touch-icon") || rel_has(rel, "apple-touch-icon-precomposed");
	bool icon = rel_has(rel, "icon");
	char abs[2048];
	if ((!touch && !icon) || !net_resolve(d->base, href, abs, sizeof(abs))) {
		free(sizes);
		return;
	}
	size_t pl = strcspn(abs, "?#");
	bool ext_svg = pl > 4 && !strncasecmp(abs + pl - 4, ".svg", 4), ext_png = pl > 4 && !strncasecmp(abs + pl - 4, ".png", 4);
	bool other = (pl > 4 && (!strncasecmp(abs + pl - 4, ".gif", 4) || !strncasecmp(abs + pl - 5, ".webp", 5))) ||
		     (ty && (strcasestr(ty, "svg") || strcasestr(ty, "gif") || strcasestr(ty, "webp")));
	int px = 0;	/* largest "NxM" in sizes ("any" = a vector icon) */
	for (const char *q = sizes; q && *q;) {
		char *e;
		long v = strtol(q, &e, 10);
		if (e != q && (*e == 'x' || *e == 'X') && v > px && v <= 4096)
			px = (int)v;
		q = e != q ? e : q + 1;
	}
	bool any = sizes && strcasestr(sizes, "any");
	free(sizes);
	if (ext_svg || other || (any && !px))
		return;
	if (touch) {
		int score = px ? px : 180;	/* apple-touch-icon without sizes: 180 × 180 by convention */
		if (!d->touch_icon || score > d->touch_score) {
			free(d->touch_icon);
			d->touch_icon = strdup(abs);
			d->touch_score = score;
		}
		return;
	}
	bool png = ext_png || (ty && strcasestr(ty, "png"));
	int score = (png ? 10000 : 0) + (px ? px : 16) + 1;	/* > 0: taken */
	if (score > d->icon_score) {
		free(d->icon);
		d->icon = strdup(abs);
		d->icon_score = score;
	}
}

static void start_tag(parser_t *p, const char *tag, const attr_t *a, int na, bool selfclose)
{
	html_doc_t *d = p->d;
	int l;
	const char *v;
	if (!strcmp(tag, "base")) {
		char *h = attr_dup(a, na, "href");
		char abs[2048];
		if (h && net_resolve(d->base, h, abs, sizeof(abs))) {
			free(d->base);
			d->base = strdup(abs);
		}
		free(h);
		return;
	}
	if (!strcmp(tag, "br")) {
		if (!p->nskip && p->started) {
			struct ent *e = top(p);
			put_run_text(p, "\n", 1, e ? e->flags : 0, -1);
			p->space = false;
		}
		return;
	}
	if (!strcmp(tag, "hr")) {
		if (p->nskip)
			return;
		flush(p);
		html_block_t b = { .kind = HB_HR, .img = -1 };
		b.text = calloc(1, 1);
		b.runs = malloc(sizeof(html_run_t));
		struct ent *e = top(p);
		add_block(p, &b, e && e->main);
		return;
	}
	if (!strcmp(tag, "img")) {
		add_img(p, a, na);
		return;
	}
	if (!strcmp(tag, "input")) {
		start_input(p, a, na);
		return;
	}
	if (!strcmp(tag, "link")) {	/* OpenSearch: the site's own search, for pages whose search box needs JavaScript */
		char *rel = attr_dup(a, na, "rel"), *ty = attr_dup(a, na, "type"), *href = attr_dup(a, na, "href"), abs[2048];
		if (!d->opensearch && rel && ty && href && strcasestr(rel, "search") &&
		    !strcasecmp(ty, "application/opensearchdescription+xml") && net_resolve(d->base, href, abs, sizeof(abs)))
			d->opensearch = strdup(abs);
		if (rel && href)
			link_icon(d, rel, ty, href, attr_dup(a, na, "sizes"));
		free(rel);
		free(ty);
		free(href);
		return;
	}
	if (!strcmp(tag, "meta")) {	/* <meta http-equiv=refresh content="5; url=…"> */
		if ((v = attr_get(a, na, "http-equiv", &l)) && l == 7 && !strncasecmp(v, "refresh", 7) && !d->refresh) {
			char *c = attr_dup(a, na, "content"), abs[2048];
			const char *u = c ? strcasestr(c, "url=") : NULL;
			if (u) {
				u += 4;
				while (*u == '\'' || *u == '"' || *u == ' ')
					u++;
				char tmp[2048];
				snprintf(tmp, sizeof(tmp), "%s", u);
				tmp[strcspn(tmp, "'\"")] = 0;
				if (*tmp && net_resolve(d->base, tmp, abs, sizeof(abs))) {
					d->refresh = strdup(abs);
					d->refresh_s = atoi(c);
				}
			}
			free(c);
		}
		return;
	}
	if (in_list(tag, VOID_TAGS))
		return;

	bool block = in_list(tag, BLOCK_TAGS);
	struct ent *pt = top(p);
	bool incell = pt && pt->cell && strcmp(tag, "table") && strcmp(tag, "tr") && strcmp(tag, "tbody") &&
		      strcmp(tag, "thead") && strcmp(tag, "tfoot");
	if (incell && block) {	/* a block inside a table cell: just a separator in the row */
		block = false;
		if (p->started && p->t.len)
			p->sep = true;
	}
	/* implied end tags */
	if (block || !strcmp(tag, "li")) {
		for (int i = p->depth - 1; i >= 0; i--) {
			if (!strcmp(p->st[i].tag, "p")) {
				pop_to(p, i);
				break;
			}
			if (p->st[i].block)
				break;
		}
	}
	static const char *const list_scope[] = { "ul", "ol", "menu", "dir", NULL };
	static const char *const dl_scope[] = { "dl", NULL };
	static const char *const tr_scope[] = { "table", NULL };
	static const char *const td_scope[] = { "tr", "table", NULL };
	int i = -1;
	if (!strcmp(tag, "li"))
		i = find_open(p, "li", list_scope);
	else if (!strcmp(tag, "dt") || !strcmp(tag, "dd")) {
		i = find_open(p, "dd", dl_scope);
		if (i < 0)
			i = find_open(p, "dt", dl_scope);
	} else if (!strcmp(tag, "tr"))
		i = find_open(p, "tr", tr_scope);
	else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
		i = find_open(p, "td", td_scope);
		if (i < 0)
			i = find_open(p, "th", td_scope);
	} else if (!strcmp(tag, "option") || !strcmp(tag, "optgroup"))
		i = find_open(p, "option", NULL);
	else if (!strcmp(tag, "a"))
		i = find_open(p, "a", NULL);	/* <a> doesn't nest */
	if (i >= 0)
		pop_to(p, i);

	if (selfclose && !block)
		return;
	if (p->depth >= MAX_DEPTH) {
		p->overflow++;
		return;
	}
	struct ent *parent = top(p);
	struct ent *e = &p->st[p->depth++];
	memset(e, 0, sizeof(*e));
	snprintf(e->tag, sizeof(e->tag), "%s", tag);
	e->block = block;
	e->flags = parent ? parent->flags : 0;
	e->link = parent ? parent->link : -1;
	e->kind = parent ? parent->kind : HB_P;
	e->level = parent ? parent->level : 0;
	e->main = parent && parent->main;
	e->cell = parent && parent->cell && strcmp(tag, "table");
	e->field = parent ? parent->field : -1;

	bool skip_tag = in_list(tag, SKIP_TAGS), menu = !strcmp(tag, "nav") || !strcmp(tag, "menu");
	bool fieldtag = !strcmp(tag, "select") || !strcmp(tag, "button");	/* hidden, but its field may show */
	bool skip = skip_tag && !fieldtag;
	if (!skip && (attr_get(a, na, "hidden", &l) || ((v = attr_get(a, na, "aria-hidden", &l)) && l == 4 &&
							   !strncasecmp(v, "true", 4))))
		skip = true;
	if (!skip && (v = attr_get(a, na, "style", &l))) {
		char st[160];
		int k = 0;
		for (int j = 0; j < l && k < (int)sizeof(st) - 1; j++)
			if (!isspace((unsigned char)v[j]))
				st[k++] = tolower((unsigned char)v[j]);
		st[k] = 0;
		skip = strstr(st, "display:none") || strstr(st, "visibility:hidden");
	}
	bool small = !strcmp(tag, "small");
	if (!skip && (v = attr_get(a, na, "class", &l))) {
		if (has_token(v, l, SKIP_CLASSES))
			skip = true;
		else if (has_token(v, l, META_CLASSES))
			small = true;
	}
	if (!skip && (v = attr_get(a, na, "role", &l))) {
		static const char *const roles[] = { "complementary", "dialog", "contentinfo", "alert", NULL };
		static const char *const mroles[] = { "navigation", "menu", "menubar", NULL };
		skip = has_token(v, l, roles);
		menu = menu || has_token(v, l, mroles);
		if (l == 4 && !strncasecmp(v, "main", 4))
			e->main = true;
		if (l == 6 && !strncasecmp(v, "search", 6)) {
			e->searchrole = true;
			p->nsearch++;
		}
	}
	if (!strcmp(tag, "main") || !strcmp(tag, "article"))
		e->main = true;
	if (!strcmp(tag, "noscript"))
		skip = false;	/* no JS here: its content is what we'd see */
	bool skip_attr = skip;	/* hidden by an attribute / class / role */
	skip = skip || skip_tag;
	if (!strcmp(tag, "template"))
		p->ntemplate++;
	/* before this element's own skip counts: <select> / <button> are hidden, their field isn't */
	if (!strcmp(tag, "select") && !skip_attr)
		start_select(p, a, na);
	else if (!strcmp(tag, "option") && p->sel >= 0)
		start_option(p, a, na);
	else if (!strcmp(tag, "button") && !skip_attr)
		start_button(p, a, na);
	if (skip) {
		e->skip = true;
		p->nskip++;
	} else if (menu && !p->nskip) {	/* navigation: not in the text, its links go to doc->menu */
		e->skip = e->menu = true;
		p->nskip++;
		p->nmenu++;
	} else if (menu) {	/* inside another menu (or something hidden): just hidden */
		e->skip = true;
		p->nskip++;
		if (p->nskip - 1 == p->nmenu) {	/* nested in a menu only: still part of it */
			e->menu = true;
			p->nmenu++;
		}
	}
	if (!strcmp(tag, "form") && p->form < 0)
		p->form = add_form(p, a, na);
	else if (!strcmp(tag, "label") && (v = attr_get(a, na, "for", &l)) && l) {
		for (int k = d->nfields - 1; k >= 0; k--)	/* <input type=checkbox id=x><label for=x> */
			if (d->fields[k].id && (int)strlen(d->fields[k].id) == l && !strncmp(d->fields[k].id, v, l)) {
				if (d->fields[k].type == HF_CHECKBOX || d->fields[k].type == HF_RADIO)
					e->field = k;
				break;
			}
	}
	if (block)
		flush(p);

	if (!strcmp(tag, "table")) {
		if (p->npt)	/* a table in a table: layout */
			p->pt[p->npt - 1].bad = true;
		if (p->npt < MAX_TSTACK)
			p->pt[p->npt++] = (struct ptable){ .first = d->nb };
		else
			p->pt[p->npt - 1].bad = true;
	} else if (!strcmp(tag, "tr")) {
		row_start(p);
	}
	if (small)
		e->flags |= HR_SMALL;
	if (!strcmp(tag, "b") || !strcmp(tag, "strong") || !strcmp(tag, "dt"))
		e->flags |= HR_BOLD;
	else if (!strcmp(tag, "i") || !strcmp(tag, "em") || !strcmp(tag, "cite") || !strcmp(tag, "var"))
		e->flags |= HR_ITAL;
	else if (!strcmp(tag, "code") || !strcmp(tag, "kbd") || !strcmp(tag, "samp") || !strcmp(tag, "tt"))
		e->flags |= HR_CODE;
	else if (!strcmp(tag, "a")) {
		char *h = attr_dup(a, na, "href");
		if (h && p->nskip && p->nskip == p->nmenu)
			menu_link(p, e, a, na);
		if (h && !p->nskip) {
			int li = add_link(p, h);
			if (li >= 0) {
				e->link = li;
				e->flags |= HR_LINK;
			}
		}
		free(h);
	} else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
		e->kind = tag[1] == '1' ? HB_H1 : tag[1] == '2' ? HB_H2 : tag[1] == '3' ? HB_H3 : HB_H4;
		e->flags |= HR_BOLD;
	} else if (!strcmp(tag, "ul") || !strcmp(tag, "ol") || !strcmp(tag, "menu") || !strcmp(tag, "dir")) {
		e->level++;
		e->ol = !strcmp(tag, "ol");
		if (e->ol && (v = attr_get(a, na, "start", &l)))
			e->ol_count = atoi(v) - 1;
	} else if (!strcmp(tag, "li") && !e->cell) {
		e->kind = HB_LI;
		e->li_num = 0;
		for (int j = p->depth - 2; j >= 0; j--)
			if (!strcmp(p->st[j].tag, "ol") || !strcmp(p->st[j].tag, "ul") || !strcmp(p->st[j].tag, "menu")) {
				if (p->st[j].ol)
					e->li_num = ++p->st[j].ol_count;
				break;
			}
		if (!e->level)
			e->level = 1;
	} else if (!strcmp(tag, "blockquote")) {
		e->kind = HB_QUOTE;
		e->level++;
	} else if (!strcmp(tag, "pre")) {
		e->kind = HB_PRE;
		e->flags |= HR_CODE;
	} else if (!strcmp(tag, "figcaption") || !strcmp(tag, "caption")) {
		e->kind = HB_CAPTION;
	} else if (!strcmp(tag, "dd")) {
		e->level++;
	} else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
		p->sep = p->started && p->t.len;
		e->cell = true;
		if (tag[1] == 'h')
			e->flags |= HR_BOLD;
		cell_start(p, a, na, tag[1] == 'h');
	}
	/* anchors for #fragment links */
	if (!p->nskip && !p->anchor) {
		char *id = attr_dup(a, na, "id");
		if (!id && !strcmp(tag, "a"))
			id = attr_dup(a, na, "name");
		if (id && *id && (block || !p->started || !p->t.len))
			p->anchor = id;
		else
			free(id);
	}
}

/* <tr> of the innermost open table: a new row */
static void row_start(parser_t *p)
{
	p->nrc = p->rcol = 0;
	p->cell_wait = p->cell_txt = -1;
	p->rtable = p->npt ? p->npt - 1 : -1;
	p->rhead = false;
	p->rallhead = true;
	for (int i = p->depth - 1; i >= 0 && p->rtable >= 0 && strcmp(p->st[i].tag, "table"); i--)
		if (!strcmp(p->st[i].tag, "thead"))
			p->rhead = true;
}

/* <td> / <th> in that row */
static void cell_start(parser_t *p, const attr_t *a, int na, bool th)
{
	if (p->rtable < 0 || p->rtable != p->npt - 1)
		return;
	if (p->cell_txt >= 0)	/* the previous cell ends here */
		p->rc[p->cell_txt].len = p->t.len - p->rc[p->cell_txt].off;
	p->cell_txt = p->cell_wait = -1;
	int l, span = 1;
	const char *v = attr_get(a, na, "colspan", &l);
	if (v && l < 4)
		span = atoi(v);
	if (span < 1)
		span = 1;
	if (p->rcol + span > MAX_TCOLS) {
		p->pt[p->rtable].bad = true;
		return;
	}
	if (p->nrc == p->rccap) {
		int nc = p->rccap ? p->rccap * 2 : 8;
		html_cell_t *n = realloc(p->rc, nc * sizeof(*n));
		if (!n) {
			p->pt[p->rtable].bad = true;
			return;
		}
		p->rc = n;
		p->rccap = nc;
	}
	p->rc[p->nrc] = (html_cell_t){ .off = -1, .col = p->rcol, .span = span, .head = th };
	p->cell_wait = p->nrc++;
	p->rcol += span;
	if (!th)
		p->rallhead = false;
}

/* </table>: rows of a small grid of short cells (2..HT_MAXCOLS columns that have text) stay a table, anything
 * else (layout tables, long text, pictures or fields in cells, nesting) keeps the flattened rows */
static void table_end(parser_t *p)
{
	if (!p->npt)
		return;
	struct ptable *t = &p->pt[--p->npt];
	if (p->rtable >= p->npt) {
		p->rtable = -1;
		p->nrc = 0;
	}
	html_doc_t *d = p->d;
	int first = t->first < d->nb ? t->first : d->nb;
	bool data = !t->bad && t->nrows >= 2 && t->nrows <= MAX_TROWS;
	int wlen[MAX_TCOLS] = { 0 }, map[MAX_TCOLS];
	bool used[MAX_TCOLS] = { false };
	for (int k = first; data && k < d->nb; k++) {
		const html_block_t *b = &d->b[k];
		if (!b->ncells) {
			data = b->kind == HB_CAPTION;	/* <caption> may sit among the rows */
			continue;
		}
		for (int c = 0; c < b->ncells && data; c++) {
			const html_cell_t *cl = &b->cells[c];
			if (cl->len > MAX_CELL)
				data = false;
			else if (cl->len && cl->span == 1) {
				used[cl->col] = true;
				int code = 0;	/* monospace text is wider: it counts a third more */
				for (int r = 0; r < b->nruns; r++) {
					const html_run_t *rn = &b->runs[r];
					int ro = rn->off > cl->off ? rn->off : cl->off;
					int re = rn->off + rn->len < cl->off + cl->len ? rn->off + rn->len : cl->off + cl->len;
					if (re > ro && (rn->flags & HR_CODE))
						code += re - ro;
				}
				int n = cl->len + code / 3;
				n = n < 48 ? n : 48;
				if (n > wlen[cl->col])
					wlen[cl->col] = n;
			}
		}
	}
	for (int k = first; data && k < d->nb; k++)	/* text only in a spanning cell: its first column counts */
		for (int c = 0; c < d->b[k].ncells; c++) {
			const html_cell_t *cl = &d->b[k].cells[c];
			bool any = false;
			for (int j = cl->col; j < cl->col + cl->span; j++)
				any = any || used[j];
			if (cl->len && !any)
				used[cl->col] = true;
		}
	int ncols = 0;
	for (int j = 0; j < MAX_TCOLS; j++)
		map[j] = used[j] ? ncols++ : -1;
	data = data && ncols >= 2 && ncols <= HT_MAXCOLS;
	html_table_t *nt = NULL;
	if (data) {
		nt = realloc(d->tables, (d->ntables + 1) * sizeof(*nt));
		if (nt)
			d->tables = nt;
		data = nt != NULL;
	}
	if (!data) {	/* flattened: the rows keep their " · " text */
		for (int k = first; k < d->nb; k++)
			if (d->b[k].kind != HB_ROW) {
				free(d->b[k].cells);
				d->b[k].cells = NULL;
				d->b[k].ncells = 0;
			}
		return;
	}
	html_table_t *tb = &d->tables[d->ntables];
	memset(tb, 0, sizeof(*tb));
	tb->ncols = ncols;
	int sum = 0, wt[HT_MAXCOLS], mn[HT_MAXCOLS], mnsum = 0;
	for (int j = 0; j < MAX_TCOLS; j++)
		if (map[j] >= 0) {
			int c = map[j], n = wlen[j] ? wlen[j] : 8;
			wt[c] = n + 2;
			mn[c] = 5 + (n < 8 ? n : 8) * 5 / 2;	/* a short column still fits its text (~2.5 % a character) */
			sum += wt[c];
			mnsum += mn[c];
		}
	if (mnsum > 100) {	/* five wide columns: the minimums shrink to fit */
		int t = 0;
		for (int c = 0; c < ncols; c++)
			t += mn[c] = mn[c] * 100 / mnsum;
		mnsum = t;
	}
	int left = 100;
	for (int c = 0; c < ncols; c++) {	/* by text length, but no column under its minimum */
		mnsum -= mn[c];
		int pc = wt[c] * 100 / sum;
		if (pc < mn[c])
			pc = mn[c];
		if (pc > left - mnsum)
			pc = left - mnsum;
		if (c == ncols - 1)
			pc = left;
		tb->pct[c] = pc;
		left -= pc;
	}
	for (int k = first; k < d->nb; k++) {
		html_block_t *b = &d->b[k];
		if (!b->ncells)
			continue;
		int n = 0;
		for (int c = 0; c < b->ncells; c++) {
			html_cell_t cl = b->cells[c];
			int col = -1, span = 0;
			for (int j = cl.col; j < cl.col + cl.span; j++)
				if (map[j] >= 0) {
					if (col < 0)
						col = map[j];
					span++;
				}
			if (col < 0)
				continue;
			cl.col = col;
			cl.span = span;
			b->cells[n++] = cl;
		}
		b->ncells = n;
		b->kind = HB_ROW;
		b->table = d->ntables;
		b->row = tb->nrows++;
	}
	d->ntables++;
}

static void end_tag(parser_t *p, const char *tag)
{
	if (!strcmp(tag, "br")) {
		start_tag(p, "br", NULL, 0, true);
		return;
	}
	if (p->overflow && p->depth == MAX_DEPTH) {
		/* unbalanced deep nesting: drop the end tag if it isn't open */
		if (find_open(p, tag, NULL) < 0) {
			p->overflow--;
			return;
		}
	}
	int i = find_open(p, tag, NULL);
	if (i < 0)
		return;
	pop_to(p, i);
}

static void parse_tag_name(const char *s, int n, char *tag, int tn, int *used)
{
	int k = 0, i = 0;
	while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == ':' || s[i] == '_')) {
		if (k < tn - 1)
			tag[k++] = tolower((unsigned char)s[i]);
		i++;
	}
	tag[k] = 0;
	*used = i;
}

static html_doc_t *doc_new(const char *url)
{
	html_doc_t *d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->base = strdup(url && *url ? url : "about:blank");
	return d;
}

static void set_title(html_doc_t *d, const char *s, int n)
{
	sb_t b = { 0 };
	put_decoded(&b, s, n, true);
	while (b.len && b.p[b.len - 1] == ' ')
		b.p[--b.len] = 0;
	char *t = b.p;
	while (t && *t == ' ')
		t++;
	if (t && *t) {
		free(d->title);
		d->title = strdup(t);
	}
	free(b.p);
}

html_doc_t *html_parse(const char *src0, size_t len0, const char *url, const char *ctype)
{
	size_t len;
	char *src = to_utf8(src0, len0, charset_latin(src0, len0, ctype), &len);
	html_doc_t *d = doc_new(url);
	parser_t *p = calloc(1, sizeof(*p));
	if (!src || !d || !p) {
		free(src);
		free(p);
		html_free(d);
		return NULL;
	}
	p->d = d;
	p->form = p->sel = p->btn = p->menu_cur = -1;
	p->rtable = p->cell_wait = p->cell_txt = -1;
	d->url = strdup(d->base);
	attr_t attrs[48];
	size_t i = 0;
	while (i < len && !p->stop) {
		if (src[i] != '<') {
			size_t j = i;
			while (j < len && src[j] != '<')
				j++;
			int pre = 0;
			for (int k = p->depth - 1; k >= 0 && !pre; k--)
				pre = !strcmp(p->st[k].tag, "pre");
			put_text(p, src + i, j - i, pre);
			i = j;
			continue;
		}
		/* comments, doctype, processing instructions, CDATA */
		if (!strncmp(src + i, "<!--", 4)) {
			const char *e = strstr(src + i + 4, "-->");
			i = e ? (size_t)(e - src) + 3 : len;
			continue;
		}
		if (src[i + 1] == '!' || src[i + 1] == '?') {
			const char *e = memchr(src + i, '>', len - i);
			i = e ? (size_t)(e - src) + 1 : len;
			continue;
		}
		bool end = src[i + 1] == '/';
		size_t s = i + 1 + end;
		if (s >= len || !isalpha((unsigned char)src[s])) {	/* a lone '<' is text */
			put_text(p, "<", 1, false);
			i++;
			continue;
		}
		char tag[16];
		int used;
		parse_tag_name(src + s, len - s, tag, sizeof(tag), &used);
		s += used;
		/* attributes */
		int na = 0;
		bool selfclose = false;
		while (s < len && src[s] != '>') {
			if (isspace((unsigned char)src[s])) {
				s++;
				continue;
			}
			if (src[s] == '/') {
				selfclose = s + 1 < len && src[s + 1] == '>';
				s++;
				continue;
			}
			size_t ns = s;
			while (s < len && !isspace((unsigned char)src[s]) && src[s] != '=' && src[s] != '>' &&
			       !(src[s] == '/' && s + 1 < len && src[s + 1] == '>'))
				s++;
			attr_t at = { src + ns, "", (int)(s - ns), 0 };
			while (s < len && isspace((unsigned char)src[s]))
				s++;
			if (s < len && src[s] == '=') {
				s++;
				while (s < len && isspace((unsigned char)src[s]))
					s++;
				if (s < len && (src[s] == '"' || src[s] == '\'')) {
					char q = src[s++];
					size_t vs = s;
					while (s < len && src[s] != q)
						s++;
					at.v = src + vs;
					at.vl = s - vs;
					if (s < len)
						s++;
				} else {
					size_t vs = s;
					while (s < len && !isspace((unsigned char)src[s]) && src[s] != '>')
						s++;
					at.v = src + vs;
					at.vl = s - vs;
				}
			}
			if (na < (int)(sizeof(attrs) / sizeof(attrs[0])) && at.nl)
				attrs[na++] = at;
		}
		i = s < len ? s + 1 : len;
		if (end) {
			end_tag(p, tag);
			continue;
		}
		if (in_list(tag, RAW_TAGS)) {	/* raw text up to </tag> */
			char close[24];
			int cl = snprintf(close, sizeof(close), "</%s", tag);
			const char *e = NULL;
			for (size_t k = i; k + cl <= len; k++)
				if (src[k] == '<' && !strncasecmp(src + k, close, cl)) {
					e = src + k;
					break;
				}
			size_t stop = e ? (size_t)(e - src) : len;
			if (!strcmp(tag, "title") && !d->title)
				set_title(d, src + i, stop - i);
			else if (!strcmp(tag, "textarea"))
				start_textarea(p, attrs, na, src + i, stop - i);
			else if (!strcmp(tag, "script"))
				d->scripts++;
			if (e) {
				const char *gt = memchr(e, '>', len - (e - src));
				i = gt ? (size_t)(gt - src) + 1 : len;
			} else {
				i = len;
			}
			continue;
		}
		start_tag(p, tag, attrs, na, selfclose);
	}
	pop_to(p, 0);
	flush(p);
	free(p->anchor);

	/* fields with form="id" */
	for (int k = 0; k < d->nfields; k++) {
		char *fa = p->form_attr[k];
		if (fa) {
			for (int f = 0; f < d->nforms; f++)
				if (d->forms[f].id && !strcmp(d->forms[f].id, fa))
					d->fields[k].form = f;
			free(fa);
		}
	}
	free(p->form_attr);
	/* a site search: role=search, a type=search field, …/search actions, a lone q / search / query box */
	for (int k = 0; k < d->nfields; k++) {
		html_field_t *f = &d->fields[k];
		if (f->form < 0 || !f->name)
			continue;
		static const char *const qn[] = { "q", "search", "query", "s", "k", "p", "keywords", "search_query", NULL };
		if (in_list(f->name, qn) && (f->type == HF_TEXT || f->type == HF_SEARCH) &&
		    !d->forms[f->form].post && strcasestr(d->forms[f->form].action, "search"))
			d->forms[f->form].search = true;
		if (f->type == HF_PASSWORD)	/* a login form, whatever it is called */
			d->forms[f->form].search = false;
	}
	int kept = 0;	/* a field whose form="id" names no form: nothing it could submit */
	for (int k = 0; k < d->nb; k++) {
		if (d->b[k].kind == HB_FIELD && d->fields[d->b[k].field].form < 0) {
			free(d->b[k].text);
			free(d->b[k].runs);
			free(d->b[k].anchor);
			free(d->b[k].cells);
			continue;
		}
		p->inmain[kept] = p->inmain[k];
		d->b[kept++] = d->b[k];
	}
	d->nb = kept;

	/* reader: keep the marked content when it has enough text */
	int mainchars = 0;
	for (int k = 0; k < d->nb; k++)
		if (p->inmain[k])
			mainchars += d->b[k].len;
	if (mainchars >= MAIN_MIN) {
		int w = 0;
		for (int k = 0; k < d->nb; k++) {
			html_block_t *bk = &d->b[k];
			bool search = bk->kind == HB_FIELD && d->forms[d->fields[bk->field].form].search;
			if (p->inmain[k] || search) {
				d->b[w++] = d->b[k];
			} else {
				free(d->b[k].text);
				free(d->b[k].runs);
				free(d->b[k].anchor);
				free(d->b[k].cells);
			}
		}
		d->nb = w;
	}
	if (!d->title) {
		for (int k = 0; k < d->nb; k++)
			if (d->b[k].kind == HB_H1) {
				d->title = strdup(d->b[k].text);
				break;
			}
	}
	for (int k = 0; k < d->nb; k++)
		d->text_len += d->b[k].len;
	free(p->t.p);
	free(p->runs);
	free(p->rc);
	free(p->inmain);
	free(p->fbuf.p);
	free(p->mbuf.p);
	free(p);
	free(src);
	return d;
}

html_doc_t *html_plain(const char *src0, size_t len0, const char *url, const char *ctype)
{
	size_t len;
	char *src = to_utf8(src0, len0, charset_latin(src0, 0, ctype), &len);
	html_doc_t *d = doc_new(url);
	if (!src || !d) {
		free(src);
		html_free(d);
		return NULL;
	}
	/* paragraphs = blank-line separated, lines kept */
	size_t i = 0;
	int cap = 0;
	while (i < len && d->nb < MAX_BLOCKS) {
		while (i < len && (src[i] == '\n' || src[i] == '\r'))
			i++;
		size_t s = i;
		while (i < len && !(src[i] == '\n' && i + 1 < len && (src[i + 1] == '\n' || src[i + 1] == '\r')))
			i++;
		size_t e = i;
		while (e > s && isspace((unsigned char)src[e - 1]))
			e--;
		if (e <= s)
			continue;
		if (d->nb == cap) {
			cap = cap ? cap * 2 : 64;
			html_block_t *nb = realloc(d->b, cap * sizeof(*nb));
			if (!nb)
				break;
			d->b = nb;
		}
		html_block_t *b = &d->b[d->nb];
		memset(b, 0, sizeof(*b));
		b->kind = HB_PRE;
		b->img = -1;
		b->text = dupn(src + s, e - s);
		b->len = e - s;
		b->runs = malloc(sizeof(html_run_t));
		if (!b->text || !b->runs) {
			free(b->text);
			free(b->runs);
			break;
		}
		b->runs[0] = (html_run_t){ 0, b->len, HR_CODE, -1 };
		b->nruns = 1;
		d->nb++;
	}
	d->cut = i < len;
	free(src);
	return d;
}

/* ---------- DuckDuckGo html results ---------- */

/* text between s and e with tags dropped, entities decoded, whitespace collapsed */
static char *inner_text(const char *s, const char *e)
{
	sb_t b = { 0 };
	while (s < e) {
		if (*s == '<') {
			const char *g = memchr(s, '>', e - s);
			s = g ? g + 1 : e;
			continue;
		}
		const char *t = s;
		while (t < e && *t != '<')
			t++;
		put_decoded(&b, s, t - s, true);
		s = t;
	}
	char *r = b.p ? b.p : calloc(1, 1);
	if (!r)
		return NULL;
	char *st = r;
	while (*st == ' ')
		st++;
	memmove(r, st, strlen(st) + 1);
	int n = strlen(r);
	while (n && r[n - 1] == ' ')
		r[--n] = 0;
	return r;
}

static char *attr_in(const char *tag_s, const char *tag_e, const char *name)
{
	char pat[32];
	int pl = snprintf(pat, sizeof(pat), "%s=\"", name);
	for (const char *s = tag_s; s + pl < tag_e; s++)
		if (!strncasecmp(s, pat, pl)) {
			const char *v = s + pl, *q = memchr(v, '"', tag_e - v);
			if (!q)
				return NULL;
			sb_t b = { 0 };
			put_decoded(&b, v, q - v, false);
			return b.p ? b.p : calloc(1, 1);
		}
	return NULL;
}

/* the element whose start tag contains `cls` at or after s, before lim: its inner text */
static char *class_text(const char *s, const char *lim, const char *cls, const char *close)
{
	const char *c = strstr(s, cls);
	if (!c || (lim && c > lim))
		return NULL;
	const char *g = strchr(c, '>');
	if (!g)
		return NULL;
	const char *e = strcasestr(g, close);
	if (!e || (lim && e > lim))
		e = lim ? lim : g + strlen(g);
	return inner_text(g + 1, e);
}

html_doc_t *html_ddg(const char *src0, size_t len0, const char *url)
{
	size_t len;
	char *src = to_utf8(src0, len0, false, &len);
	html_doc_t *d = doc_new(url);
	if (!src || !d) {
		free(src);
		html_free(d);
		return NULL;
	}
	parser_t pp = { .d = d };
	parser_t *p = &pp;
	const char *t = strcasestr(src, "<title>");
	if (t) {
		const char *te = strcasestr(t, "</title>");
		if (te)
			set_title(d, t + 7, te - t - 7);
	}
	static const char A[] = "class=\"result__a\"";
	const char *s = src;
	while ((s = strstr(s, A)) && !p->stop) {
		const char *ts = s;
		while (ts > src && *ts != '<')
			ts--;
		const char *te = strchr(s, '>');
		if (!te)
			break;
		const char *ae = strcasestr(te, "</a>");
		if (!ae)
			break;
		const char *next = strstr(ae, A);
		char *href = attr_in(ts, te, "href");
		char *title = inner_text(te + 1, ae);
		char *disp = class_text(ae, next, "class=\"result__url\"", "</a>");
		char *snip = class_text(ae, next, "class=\"result__snippet\"", "</a>");
		s = ae;
		if (href && strstr(href, "/y.js"))	/* ad */
			goto skip;
		if (href) {
			char *u = strstr(href, "uddg=");
			if (u) {
				u += 5;
				u[strcspn(u, "&")] = 0;
				net_urldecode(u);
				memmove(href, u, strlen(u) + 1);
			}
		}
		int li = href && title && *title ? add_link(p, href) : -1;
		if (li < 0)
			goto skip;
		html_block_t b = { .kind = HB_RESULT, .img = -1 };
		b.text = title;
		b.len = strlen(title);
		b.runs = malloc(sizeof(html_run_t));
		if (!b.runs)
			goto skip;
		b.runs[0] = (html_run_t){ 0, b.len, HR_LINK, li };
		b.nruns = 1;
		b.sub = disp;
		b.snip = snip;
		title = disp = snip = NULL;
		add_block(p, &b, false);
	skip:
		free(href);
		free(title);
		free(disp);
		free(snip);
	}
	/* next page: the "Next" form's hidden fields, as a GET */
	const char *nl = strstr(src, "class=\"nav-link\"");
	const char *last = NULL;
	for (const char *q = nl; q; q = strstr(q + 1, "class=\"nav-link\""))
		last = q;
	if (last && strstr(last, "value=\"Next\"")) {
		const char *fe = strstr(last, "</form>");
		char *q = NULL, *sv = NULL, *dc = NULL;
		for (const char *in = strstr(last, "<input"); in && (!fe || in < fe); in = strstr(in + 1, "<input")) {
			const char *ie = strchr(in, '>');
			if (!ie)
				break;
			char *nm = attr_in(in, ie, "name"), *val = attr_in(in, ie, "value");
			if (nm && val && !strcmp(nm, "q") && !q)
				q = val, val = NULL;
			else if (nm && val && !strcmp(nm, "s") && !sv)
				sv = val, val = NULL;
			else if (nm && val && !strcmp(nm, "dc") && !dc)
				dc = val, val = NULL;
			free(nm);
			free(val);
		}
		if (q && sv) {
			char enc[1024], m[1400];
			net_urlencode(q, enc, sizeof(enc));
			snprintf(m, sizeof(m), "https://html.duckduckgo.com/html/?q=%s&s=%d&dc=%d", enc, atoi(sv),
				 dc ? atoi(dc) : atoi(sv) + 1);
			d->more = strdup(m);
		}
		free(q);
		free(sv);
		free(dc);
	}
	if (!d->nb) {
		html_block_t b = { .kind = HB_NOTE, .img = -1 };
		bool none = strstr(src, "No results") != NULL;
		b.text = strdup(none ? "No results found." : "DuckDuckGo didn\xe2\x80\x99t return any results.");
		b.len = b.text ? strlen(b.text) : 0;
		b.runs = malloc(sizeof(html_run_t));
		if (b.text && b.runs) {
			b.runs[0] = (html_run_t){ 0, b.len, 0, -1 };
			b.nruns = 1;
			add_block(p, &b, false);
		} else {
			free(b.text);
			free(b.runs);
		}
	}
	free(p->inmain);
	free(src);
	return d;
}

void html_free(html_doc_t *d)
{
	if (!d)
		return;
	for (int i = 0; i < d->nb; i++) {
		free(d->b[i].text);
		free(d->b[i].runs);
		free(d->b[i].anchor);
		free(d->b[i].sub);
		free(d->b[i].snip);
		free(d->b[i].cells);
	}
	free(d->b);
	free(d->tables);
	for (int i = 0; i < d->nlinks; i++)
		free(d->links[i]);
	free(d->links);
	for (int i = 0; i < d->nimgs; i++) {
		free(d->imgs[i].url);
		free(d->imgs[i].alt);
	}
	free(d->imgs);
	for (int i = 0; i < d->nfields; i++) {
		html_field_t *f = &d->fields[i];
		free(f->name);
		free(f->value);
		free(f->label);
		free(f->id);
		free(f->faction);
		for (int k = 0; k < f->nopt; k++) {
			free(f->opt[k]);
			free(f->optval[k]);
		}
		free(f->opt);
		free(f->optval);
	}
	free(d->fields);
	for (int i = 0; i < d->nforms; i++) {
		free(d->forms[i].action);
		free(d->forms[i].id);
	}
	free(d->forms);
	for (int i = 0; i < d->nmenu; i++)
		free(d->menu[i].text);
	free(d->menu);
	free(d->refresh);
	free(d->opensearch);
	free(d->touch_icon);
	free(d->icon);
	free(d->url);
	free(d->title);
	free(d->base);
	free(d->more);
	free(d);
}

/* ---------- submitting a form ---------- */

static void enc_pair(sb_t *b, const char *name, const char *value)
{
	int nl = strlen(name), vl = strlen(value);
	char *en = malloc(nl * 3 + 4), *ev = malloc(vl * 3 + 4);
	if (en && ev) {
		net_urlencode(name, en, nl * 3 + 4);
		net_urlencode(value, ev, vl * 3 + 4);
		if (b->len)
			sb_c(b, '&');
		sb_put(b, en, strlen(en));
		sb_c(b, '=');
		sb_put(b, ev, strlen(ev));
	}
	free(en);
	free(ev);
}

/* CRLF line breaks in a textarea value (what browsers send) */
static void enc_text(sb_t *b, const char *name, const char *v)
{
	sb_t t = { 0 };
	for (; *v; v++) {
		if (*v == '\n' && (t.len == 0 || t.p[t.len - 1] != '\r'))
			sb_c(&t, '\r');
		sb_c(&t, *v);
	}
	enc_pair(b, name, t.p ? t.p : "");
	free(t.p);
}

char *html_form_url(const html_doc_t *d, int form, int submitter, char **post)
{
	*post = NULL;
	if (!d || form < 0 || form >= d->nforms)
		return NULL;
	const html_form_t *fm = &d->forms[form];
	const html_field_t *sub = submitter >= 0 && submitter < d->nfields ? &d->fields[submitter] : NULL;
	const char *action = sub && sub->faction ? sub->faction : fm->action;
	bool is_post = sub && sub->fmethod >= 0 ? sub->fmethod == 1 : fm->post;
	sb_t b = { 0 };
	for (int i = 0; i < d->nfields; i++) {
		const html_field_t *f = &d->fields[i];
		if (f->form != form || f->disabled)
			continue;
		switch (f->type) {
		case HF_SUBMIT:
			if (f == sub && f->name)
				enc_pair(&b, f->name, f->value ? f->value : "");
			break;
		case HF_IMAGE:
			if (f == sub) {
				char n[300];
				snprintf(n, sizeof(n), "%s%s", f->name ? f->name : "", f->name ? ".x" : "x");
				enc_pair(&b, n, "0");
				snprintf(n, sizeof(n), "%s%s", f->name ? f->name : "", f->name ? ".y" : "y");
				enc_pair(&b, n, "0");
			}
			break;
		case HF_CHECKBOX:
		case HF_RADIO:
			if (f->checked && f->name)
				enc_pair(&b, f->name, f->value ? f->value : "on");
			break;
		case HF_SELECT:
			if (f->name && f->sel >= 0 && f->sel < f->nopt)
				enc_pair(&b, f->name, f->optval[f->sel] ? f->optval[f->sel] : f->opt[f->sel] ? f->opt[f->sel] : "");
			break;
		case HF_TEXTAREA:
			if (f->name)
				enc_text(&b, f->name, f->value ? f->value : "");
			break;
		default:
			if (f->name)
				enc_pair(&b, f->name, f->value ? f->value : "");
			break;
		}
	}
	char *body = b.p ? b.p : calloc(1, 1);
	if (!body)
		return NULL;
	if (is_post) {
		*post = body;
		return strdup(action);
	}
	/* GET: the fields replace the action's query */
	int al = strcspn(action, "?#");
	char *u = malloc(al + strlen(body) + 2);
	if (u)
		sprintf(u, "%.*s?%s", al, action, body);
	free(body);
	return u;
}
