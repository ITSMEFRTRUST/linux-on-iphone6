/* html.c's <link rel=icon / apple-touch-icon> pick: largest touch icon, PNG over ICO, SVG / GIF / data: skipped */
#include <stdio.h>
#include <string.h>
#include "html.h"
static int fails;
static void chk(const char *head, const char *touch, const char *icon)
{
	char src[4096];
	snprintf(src, sizeof(src), "<html><head><title>t</title>%s</head><body><p>hello</p></body></html>", head);
	html_doc_t *d = html_parse(src, strlen(src), "https://ex.com/a/page.html", "text/html");
	const char *t = d && d->touch_icon ? d->touch_icon : "-", *i = d && d->icon ? d->icon : "-";
	if (strcmp(t, touch) || strcmp(i, icon)) {
		printf("FAIL %s\n  touch %s (want %s)\n  icon %s (want %s)\n", head, t, touch, i, icon);
		fails++;
	}
	html_free(d);
}
int main(void)
{
	chk("", "-", "-");
	chk("<link rel=\"apple-touch-icon\" href=\"/t.png\">", "https://ex.com/t.png", "-");
	chk("<link rel=\"apple-touch-icon\" sizes=\"120x120\" href=\"t120.png\"><link rel=\"apple-touch-icon-precomposed\" "
	    "sizes=\"180x180\" href=\"t180.png\"><link rel=\"apple-touch-icon\" sizes=\"152x152\" href=\"t152.png\">",
	    "https://ex.com/a/t180.png", "-");
	chk("<link rel=\"shortcut icon\" href=\"/favicon.ico\"><link rel=\"icon\" type=\"image/png\" sizes=\"16x16\" href=\"/f16.png\">"
	    "<link rel=\"icon\" type=\"image/png\" sizes=\"32x32\" href=\"/f32.png\">", "-", "https://ex.com/f32.png");
	chk("<link rel=\"icon\" type=\"image/svg+xml\" href=\"/i.svg\"><link rel=\"icon\" href=\"/i.svg?v=2\">"
	    "<link rel=\"icon\" href=\"/i.gif\"><link rel=\"icon\" sizes=\"any\" href=\"/vector\">", "-", "-");
	chk("<link rel=\"icon\" href=\"data:image/png;base64,iVBORw0KGgo=\"><LINK REL=\"ICON\" HREF=\"/x.ico\">", "-", "https://ex.com/x.ico");
	chk("<link rel=\"mask-icon\" href=\"/m.png\"><link rel=\"fluid-icon\" href=\"/f.png\"><link rel=\"iconic\" href=\"/z.png\">", "-", "-");
	chk("<base href=\"https://cdn.ex.com/s/\"><link rel=\"icon\" href=\"fav.png\"><link rel=apple-touch-icon href=//o.org/t.png>",
	    "https://o.org/t.png", "https://cdn.ex.com/s/fav.png");
	if (!fails)
		printf("html_icon_test ok\n");
	return fails != 0;
}
