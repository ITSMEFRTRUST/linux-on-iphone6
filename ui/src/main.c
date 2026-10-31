#include "orchard.h"

int main(int argc, char **argv)
{
	lv_init();
	theme_load();
	hal_init(argc, argv);
	input_init();
	shell_init();
	clock_init();	/* alarms + timers run with the app closed */
	music_init();
	bt_init();	/* after music: a connected headset reroutes its output */
	wifi_init();
	hal_run(argc, argv);
	return 0;
}
