/* Touch router between the backends and LVGL. Every finger is tracked on its own:
 * fingers that land on the keyboard go straight to it (so fast two-thumb typing never loses
 * a key), edge swipes become system gestures (pull down from the top zone = Control Center,
 * following the finger; left edge = back; home is the Home button), and the first remaining
 * finger drives LVGL's pointer through a queue, so short taps and press/release pairs inside
 * one LVGL read period are never dropped. Gestures hand over the finger's speed on release. */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include "orchard.h"

#define EDGE_TOP 120	/* status bar + nav bar: a pull down that starts here opens Control Center */
#define EDGE_LEFT 64	/* calibrated touch can't land left of ~40 px (the panel stops sensing there) */
#define SLOP 16		/* px before a touch in the top zone counts as a pull (or else as a tap/drag) */
#define MAXC 10

/* O_TOP: landed in the top zone, undecided — LVGL gets nothing until it is a tap or a drag */
enum owner { O_EAT, O_PTR, O_KBD, O_CC, O_BACK, O_TOP, O_LEFT };
static struct contact {
	int id, x, y, x0, y0;
	enum owner owner;
	uint32_t t;	/* last move */
	int vx, vy;	/* px/s, smoothed */
} cs[MAXC];
static int nc, ptr_id = -1;

/* ---------- LVGL pointer queue ---------- */

enum { Q_PRESS, Q_MOVE, Q_RELEASE };
#define QN 64
static struct { int16_t x, y; uint8_t type; } q[QN];
static int qh, qt;	/* head = next to read, tail = next to write */
static int lx, ly;
static bool ldown;

static lv_indev_t *indev;

static void qpush(int x, int y, int type)
{
	/* LVGL reads (and draws) on 16 ms timers: have them run on the next pass instead of up to a
	 * period later — a tap shows its result one or two frames sooner. The read timer sleeps while no
	 * finger is down (read_cb): wake it. */
	lv_timer_resume(lv_indev_get_read_timer(indev));
	lv_timer_ready(lv_indev_get_read_timer(indev));
	if (type != Q_MOVE)
		lv_timer_ready(lv_display_get_refr_timer(NULL));
	int last = (qt + QN - 1) % QN;
	if (type == Q_MOVE && qh != qt && q[last].type == Q_MOVE) {	/* coalesce moves */
		q[last].x = x;
		q[last].y = y;
		return;
	}
	if ((qt + 1) % QN == qh)	/* full: drop the oldest */
		qh = (qh + 1) % QN;
	q[qt].x = x;
	q[qt].y = y;
	q[qt].type = type;
	qt = (qt + 1) % QN;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	if (qh != qt) {
		lx = q[qh].x;
		ly = q[qh].y;
		ldown = q[qh].type != Q_RELEASE;
		qh = (qh + 1) % QN;
	}
	data->point.x = lx;
	data->point.y = ly;
	data->state = ldown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	data->continue_reading = qh != qt;
	/* released and nothing queued: this read delivers the release, then no more reads (60 a second, the
	 * biggest idle cost) until qpush. Long press / gestures only need reads while pressed; a scroll's
	 * fling and its snap-back are animations. */
	if (!ldown && qh == qt)
		lv_timer_pause(lv_indev_get_read_timer(indev));
}

void input_init(void)
{
	lv_indev_t *ind = indev = lv_indev_create();
	lv_indev_set_type(ind, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(ind, read_cb);
	/* touch reports (60 Hz) and LVGL reads (16 ms) beat against each other, so some reads see
	 * no motion; LVGL's swipe detector would reset on each of those. Accumulate the whole
	 * drag instead: a swipe = 70 px of travel in one direction. */
	lv_indev_set_gesture_min_velocity(ind, 0);
	lv_indev_set_gesture_min_distance(ind, 70);
}

/* ---------- contacts ---------- */

static enum owner classify(struct contact *c)
{
	int x = c->x, y = c->y;
	if (shell_screen_off())
		return O_EAT;
	if (kbd_hit(x, y))
		return O_KBD;
	if (ptr_id >= 0)
		return O_EAT;	/* LVGL has one pointer: extra fingers are ignored */
	if (shell_switcher_open() || shell_cc_open())
		return O_PTR;
	if (y < EDGE_TOP)
		return O_TOP;	/* home, apps and the lock screen alike */
	if (x < EDGE_LEFT && !shell_locked() && shell_app_open() && nav_depth() > 0 && !nav_locked())
		return O_LEFT;	/* undecided like O_TOP: a swipe right = back, anything else reaches the app */
	return O_PTR;
}

static void down(struct contact *c)
{
	c->owner = classify(c);
	c->t = lv_tick_get();
	c->vx = c->vy = 0;
	switch (c->owner) {
	case O_TOP:
	case O_LEFT:
		ptr_id = c->id;	/* keeps LVGL's pointer for it in case it turns out to be a tap */
		break;
	case O_PTR:
		ptr_id = c->id;
		qpush(c->x, c->y, Q_PRESS);
		break;
	case O_KBD:
		kbd_touch(c->id, c->x, c->y, 0);
		break;
	default:
		break;
	}
}

static void move(struct contact *c, int px, int py)
{
	/* finger speed: smoothed over the last few reports */
	uint32_t now = lv_tick_get(), dt = now - c->t;
	if (dt > 0) {
		int vx = (c->x - px) * 1000 / (int)dt, vy = (c->y - py) * 1000 / (int)dt;
		c->vx = dt > 100 ? vx : (c->vx + 2 * vx) / 3;
		c->vy = dt > 100 ? vy : (c->vy + 2 * vy) / 3;
		c->t = now;
	}
	if (c->owner == O_TOP) {
		int dx = c->x - c->x0, dy = c->y - c->y0, adx = dx < 0 ? -dx : dx;
		if (dy > SLOP && 2 * dy > adx) {	/* down, up to ~60 degrees off vertical */
			c->owner = O_CC;	/* a pull: the panel comes down under the finger from here */
			ptr_id = -1;
			cc_drag(0, 0, 0);
		} else if (adx > 2 * SLOP || dy < -SLOP) {
			c->owner = O_PTR;	/* an ordinary drag that happened to start up there */
			qpush(c->x0, c->y0, Q_PRESS);
		}
	}
	if (c->owner == O_LEFT) {
		int dx = c->x - c->x0, dy = c->y - c->y0, ady = dy < 0 ? -dy : dy;
		if (dx > SLOP && dx > 2 * ady) {	/* rightwards, within ~25 degrees: back swipe */
			c->owner = O_BACK;
			ptr_id = -1;
			nav_drag(0, 0, 0);
		} else if (ady > SLOP || dx < -SLOP) {
			c->owner = O_PTR;	/* a scroll or drag that started near the edge */
			qpush(c->x0, c->y0, Q_PRESS);
		}
	}
	switch (c->owner) {
	case O_PTR:
		qpush(c->x, c->y, Q_MOVE);
		break;
	case O_KBD:
		kbd_touch(c->id, c->x, c->y, 1);
		break;
	case O_CC:
		cc_drag(1, c->y - c->y0, c->vy);
		break;
	case O_BACK:
		nav_drag(1, c->x - c->x0, c->vx);
		break;
	default:
		break;
	}
}

/* a finger that stopped before lifting has no speed left */
static int speed(const struct contact *c, int v) { return lv_tick_elaps(c->t) > 80 ? 0 : v; }

static void up(struct contact *c)
{
	switch (c->owner) {
	case O_TOP:	/* never moved far: a tap */
	case O_LEFT:
		qpush(c->x0, c->y0, Q_PRESS);
		qpush(c->x0, c->y0, Q_RELEASE);
		ptr_id = -1;
		break;
	case O_PTR:
		qpush(c->x, c->y, Q_RELEASE);
		ptr_id = -1;
		break;
	case O_KBD:
		kbd_touch(c->id, c->x, c->y, 2);
		break;
	case O_CC:
		cc_drag(2, c->y - c->y0, speed(c, c->vy));
		break;
	case O_BACK:
		nav_drag(2, c->x - c->x0, speed(c, c->vx));
		break;
	default:
		break;
	}
}

void input_touches(const struct touch *t, int n)
{
	bool undimmed = false;	/* the screen was dimmed for auto-lock: this touch only brightens it (iOS) */
	if (n > 0) {
		lv_display_trigger_activity(NULL);
		undimmed = power_input(0);
	}
	/* lifted fingers first, so a release and a new press in one frame keep their order */
	for (int i = 0; i < nc;) {
		bool still = false;
		for (int k = 0; k < n; k++)
			still |= t[k].id == cs[i].id;
		if (still) {
			i++;
			continue;
		}
		up(&cs[i]);
		cs[i] = cs[--nc];
	}
	for (int k = 0; k < n; k++) {
		struct contact *c = NULL;
		for (int i = 0; i < nc; i++)
			if (cs[i].id == t[k].id)
				c = &cs[i];
		if (c) {
			if (c->x != t[k].x || c->y != t[k].y) {
				int px = c->x, py = c->y;
				c->x = t[k].x;
				c->y = t[k].y;
				move(c, px, py);
			}
		} else if (nc < MAXC) {
			c = &cs[nc++];
			*c = (struct contact){ .id = t[k].id, .x = t[k].x, .y = t[k].y, .x0 = t[k].x, .y0 = t[k].y };
			if (undimmed)
				c->owner = O_EAT;	/* (held until it lifts) */
			else
				down(c);
		}
	}
}

void input_feed(int x, int y, bool pressed)
{
	struct touch t = { 0, x, y };
	input_touches(&t, pressed ? 1 : 0);
}

void input_key(int code, bool pressed)
{
	lv_timer_ready(lv_display_get_refr_timer(NULL));	/* show what the key did on the next pass */
	if (pressed)
		power_input(code);
	switch (code) {
	case KEY_HOMEPAGE:
		shell_home_key(pressed);
		break;
	case KEY_POWER: {
		/* a release whose press never arrived (evdev overflow: SYN_DROPPED) would leave a dark phone that
		 * ignores its Power key: with the screen off, take it as the press */
		static bool down;
		if (pressed) {
			shell_power_key();
		} else if (!down && shell_screen_off()) {
			fprintf(stderr, "orchard: Power released without a press: taken as one\n");
			shell_power_key();
		}
		down = pressed;
		break;
	}
	case KEY_VOLUMEDOWN:
	case KEY_VOLUMEUP:
		/* Termux-style: in the terminal, hold Vol− as Ctrl */
		if (code == KEY_VOLUMEDOWN && shell_current_app() == &app_terminal && !shell_locked()) {
			kbd_ctrl_held(pressed);
			break;
		}
		if (pressed && !shell_screen_off())
			shell_volume_step(code == KEY_VOLUMEUP ? 1 : -1);
		else if (pressed && music_playing())	/* in the pocket: louder / softer without waking the screen */
			sys_volume_set(sys_volume_get() + (code == KEY_VOLUMEUP ? 7 : -7));
		break;
	}
}
