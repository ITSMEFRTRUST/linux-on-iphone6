/* html.c — a small reader-mode HTML parser for the Web app (no JS, no CSS): a page becomes a flat list
 * of blocks (headings, paragraphs, list items, quotes, code, images, rules, form fields) with styled runs and
 * links, plus the page's forms (html_form_url builds what a submit sends) and its navigation links (menu).
 * Pure C, no LVGL: runs on the net_bg worker thread. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum html_kind { HB_P, HB_H1, HB_H2, HB_H3, HB_H4, HB_LI, HB_QUOTE, HB_PRE, HB_CAPTION, HB_IMG, HB_HR, HB_RESULT, HB_NOTE,
		 HB_FIELD, HB_ROW };	/* HB_FIELD: a text input / textarea / select / submit button (field = doc->fields index);
				 * HB_ROW: a row of a data table (cells; other tables are flattened into paragraphs, cells
				 * joined by " · ") */
/* HR_FIELD: a checkbox / radio (the run is its box, one placeholder character) or text of the <label> that toggles
 * it; link = its doc->fields index instead of a link */
enum { HR_BOLD = 1, HR_ITAL = 2, HR_CODE = 4, HR_LINK = 8, HR_FIELD = 16, HR_BOX = 32,
       HR_SMALL = 64 };	/* <small>, bylines / dates / "subtext" (class names): smaller, secondary */

typedef struct html_run { int off, len, flags, link; } html_run_t;	/* link = index in doc->links, -1 none */

#define HT_MAXCOLS 5
/* a data table's cell: text[off, off+len) of its row (len 0 = empty), in column col (of the table's ncols,
 * empty columns dropped) spanning span columns */
typedef struct html_cell { int off, len, col, span; bool head; } html_cell_t;
typedef struct html_table { int ncols, nrows; int pct[HT_MAXCOLS]; } html_table_t;	/* pct = column widths (%) */

typedef struct html_block {
	enum html_kind kind;
	int level;		/* list / quote nesting (1 = top) */
	int num;		/* ordered list item number, 0 = bullet */
	char *text;		/* UTF-8; runs cover it in order */
	int len;
	html_run_t *runs;
	int nruns;
	int img;		/* HB_IMG: index in doc->imgs */
	char *anchor;		/* id / name of the element it starts (for #fragment links), or NULL */
	char *sub, *snip;	/* HB_RESULT: display URL + snippet (text = title, link = runs[0].link) */
	int field;		/* HB_FIELD: index in doc->fields */
	html_cell_t *cells;	/* HB_ROW: its cells, in column order */
	int ncells;
	int table, row;		/* HB_ROW: doc->tables index, row number in it (0 = first) */
	bool head;		/* HB_ROW: a header row (<th> cells / <thead>) */
} html_block_t;

enum html_ftype { HF_TEXT, HF_SEARCH, HF_PASSWORD, HF_EMAIL, HF_URL, HF_NUMBER, HF_TEL, HF_TEXTAREA, HF_SELECT,
		  HF_CHECKBOX, HF_RADIO, HF_SUBMIT, HF_IMAGE, HF_HIDDEN };
typedef struct html_field {
	int form;		/* doc->forms index (fields that belong to no form are dropped) */
	enum html_ftype type;
	char *name;		/* NULL = not submitted (a submit button without one still submits its form) */
	char *value;		/* text fields: what is typed (the app replaces it, malloc'd); boxes: value sent
				 * when checked ("on"); submit: its value */
	char *label;		/* placeholder / aria-label (text fields), button text */
	char *id;
	bool checked;		/* checkbox / radio */
	bool disabled;
	int maxlen;		/* 0 = none */
	char **opt, **optval;	/* select: labels, values (NULL = the label) */
	int nopt, sel;		/* sel = chosen option, -1 none */
	char *faction;		/* submit: formaction (absolute) / formmethod (-1 = the form's, 0 GET, 1 POST) */
	int fmethod;
} html_field_t;

typedef struct html_form {
	char *action;		/* absolute URL */
	bool post;
	bool search;		/* a site search (role=search, a type=search field, …): kept in reader mode */
	char *id;
} html_form_t;

typedef struct html_menu { char *text; int link; } html_menu_t;	/* a <nav> / menu link */

typedef struct html_img { char *url; int w, h; char *alt; } html_img_t;

typedef struct html_doc {
	char *title;
	char *base;		/* URL that relative links resolve against */
	html_block_t *b;
	int nb;
	char **links;		/* absolute URLs */
	int nlinks;
	html_img_t *imgs;
	int nimgs;
	char *more;		/* search results: URL of the next page, or NULL */
	bool cut;		/* stopped at the block / size cap */
	char *url;		/* the page's own URL (an empty form action submits here) */
	html_field_t *fields;
	int nfields;
	html_form_t *forms;
	int nforms;
	html_menu_t *menu;	/* site navigation, kept out of the text */
	int nmenu;
	char *refresh;		/* <meta http-equiv=refresh> target (absolute), NULL = none */
	int refresh_s;
	int scripts;		/* <script> elements: a page with many and little text needs JavaScript */
	int text_len;		/* characters of text shown */
	char *opensearch;	/* <link rel=search type=application/opensearchdescription+xml> (absolute), NULL = none */
	/* the site's icon (absolute URLs, NULL = none): the largest <link rel=apple-touch-icon(-precomposed)>, and the
	 * best <link rel=icon> that isn't SVG / GIF / WebP (PNG first, then the largest); *_score ranks them while parsing */
	char *touch_icon, *icon;
	int touch_score, icon_score;
	html_table_t *tables;	/* data tables (HB_ROW blocks) */
	int ntables;
} html_doc_t;

/* url = the page's final URL; ctype = Content-Type header (charset), may be "" */
html_doc_t *html_parse(const char *src, size_t len, const char *url, const char *ctype);
html_doc_t *html_plain(const char *src, size_t len, const char *url, const char *ctype);	/* text/plain */
html_doc_t *html_ddg(const char *src, size_t len, const char *url);	/* DuckDuckGo html results */
void html_free(html_doc_t *d);
/* what submitting form (with button field `submitter`, -1 = none/implicit) sends: the URL to load (malloc'd), and
 * *post = the x-www-form-urlencoded body for a POST (malloc'd) or NULL for a GET. NULL = no such form. */
char *html_form_url(const html_doc_t *d, int form, int submitter, char **post);
