/* web_icon.c — site icons (favicons) for the Web app's start page, favorites and history rows.
 * A page that loads in the reader gets its icon looked up once a week at most (apple-touch-icon, else a
 * non-SVG rel=icon, else /apple-touch-icon.png, else /favicon.ico), fetched by net.c with that page's own
 * LAN / tailnet pass and no cookies, decoded on the net_bg worker and kept in data_dir()/web/icons/<host>.bgra
 * (raw pixels, ≤ 108 px). Tiles are read from there off the LVGL thread — the start page never touches the
 * network. Everything here runs on the LVGL thread. */
#pragma once
#include <stdbool.h>
#include "html.h"
#include "lvgl.h"

#define WICON_BIG 108	/* Favorites grid tile */
#define WICON_SMALL 60	/* list row tile */

/* a page finished loading at url (its final URL): look its site's icon up when the cache has none from this
 * week. priv = the page's own LAN / tailnet pass (net_res_t.priv), desktop = its desktop User-Agent pref */
void wicon_page(const char *url, const html_doc_t *d, bool priv, bool desktop);
/* the site's tile (WICON_BIG or WICON_SMALL px, rounded, see-through corners) or NULL = none (yet): a miss
 * starts reading the cache file, and the listener runs once tiles arrived. The image stays valid while the
 * app is open. */
const lv_image_dsc_t *wicon_tile(const char *url, int size);
void wicon_listen(void (*cb)(void));
/* forget icons (files + memory) of every site but the ones of keep_urls (n of them; 0 = all) */
void wicon_clear(const char *const *keep_urls, int n);
void wicon_trim(void);	/* app open (no image shows a tile): memory back under its cap */
