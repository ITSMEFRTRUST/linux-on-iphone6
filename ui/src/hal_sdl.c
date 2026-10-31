/* Desktop preview: SDL window at 60% scale. Mouse = finger, F1 = Home, F2 = Power,
 * physical keyboard types into the Terminal app. */
#include <linux/input-event-codes.h>
#include <time.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include "orchard.h"

#define ZOOM 0.6f

static bool mouse_down;

static void hw_key(int code)
{
	input_key(code, true);
	input_key(code, false);
}

static int watch(void *u, SDL_Event *e)
{
	(void)u;
	bool typing = kbd_visible();
	switch (e->type) {
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		if (e->button.button != SDL_BUTTON_LEFT)
			break;
		mouse_down = e->type == SDL_MOUSEBUTTONDOWN;
		input_feed(e->button.x / ZOOM, e->button.y / ZOOM, mouse_down);
		break;
	case SDL_MOUSEMOTION:
		if (mouse_down)
			input_feed(e->motion.x / ZOOM, e->motion.y / ZOOM, true);
		break;
	case SDL_TEXTINPUT:
		if (typing && !(SDL_GetModState() & KMOD_CTRL))
			kbd_type(e->text.text);
		break;
	case SDL_KEYDOWN: {
		SDL_Keycode k = e->key.keysym.sym;
		if (k == SDLK_F1)
			hw_key(KEY_HOMEPAGE);
		else if (k == SDLK_F2)
			hw_key(KEY_POWER);
		if (!typing)
			break;
		if ((e->key.keysym.mod & KMOD_CTRL) && k >= SDLK_a && k <= SDLK_z) {
			char s[2] = { (char)k, 0 };
			kbd_ctrl_held(true);
			kbd_type(s);
			kbd_ctrl_held(false);
		}
		static const struct { SDL_Keycode sdl; int tk; } map[] = {
			{ SDLK_RETURN, TK_ENTER }, { SDLK_BACKSPACE, TK_BKSP }, { SDLK_TAB, TK_TAB },
			{ SDLK_ESCAPE, TK_ESC }, { SDLK_UP, TK_UP }, { SDLK_DOWN, TK_DOWN },
			{ SDLK_LEFT, TK_LEFT }, { SDLK_RIGHT, TK_RIGHT },
		};
		for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
			if (map[i].sdl == k)
				kbd_press(map[i].tk);
		break;
	}
	}
	return 0;
}

static uint32_t tick(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

lv_display_t *hal_init(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	lv_tick_set_cb(tick);
	lv_display_t *d = lv_sdl_window_create(SCR_W, SCR_H);
	lv_sdl_window_set_zoom(d, ZOOM);
	lv_sdl_window_set_title(d, "Orchard (F1 = Home, F2 = Power)");
	SDL_AddEventWatch(watch, NULL);
	return d;
}

void hal_run(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (;;) {
		uint32_t idle = lv_timer_handler();
		usleep((idle > 10 ? 10 : idle) * 1000);
	}
}

void hal_haptic(int ms) { (void)ms; }
void hal_damage(const lv_area_t *a) { (void)a; }	/* the window is updated whole */
void hal_screen(bool on) { (void)on; }
bool hal_display(bool on) { (void)on; return true; }
void hal_wake_set(void (*cb)(void)) { (void)cb; }
void hal_wake(void) {}
