/* the LVGL (and Orchard) calls net.c makes, for PC tests that link net.c alone */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void *lv_timer_create(void *a, unsigned b, void *c) { return (void*)1; }
void lv_timer_resume(void *t) {}
void lv_timer_pause(void *t) {}
void power_keep(void *t, unsigned dark_ms, const char *name) {}
/* Wi-Fi Sleep (wifi.c / power.c): weak, a test may define its own */
__attribute__((weak)) bool wifi_sleep_wait(void) { return false; }
__attribute__((weak)) void power_net_for(const char *token, uint32_t ms) { (void)token; (void)ms; }
