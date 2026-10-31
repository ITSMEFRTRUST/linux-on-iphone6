/* Embedded assets (paths relative to ui/, the build's working directory). */
	.section .rodata
	.macro asset name, path
	.balign 16
	.global \name, \name\()_end
\name:
	.incbin "\path"
\name\()_end:
	.endm

	asset ttf_inter, "assets/Inter-Regular.ttf"
	asset ttf_inter_medium, "assets/Inter-Medium.ttf"
	asset ttf_inter_semi, "assets/Inter-SemiBold.ttf"
	asset ttf_inter_bold, "assets/Inter-Bold.ttf"
	asset ttf_inter_display_light, "assets/InterDisplay-Light.ttf"
	asset ttf_inter_display_bold, "assets/InterDisplay-Bold.ttf"
	asset ttf_mono, "assets/JBMonoNF.ttf"
	asset words_en, "assets/words-en.txt"
	.byte 0
	asset words_shell, "assets/words-shell.txt"
	.byte 0

	/* app icons (the Makefile renders them from the SVGs in assets/icons) */
	asset icon_clock_120, "assets/icons/raw/clock-120.bgra"
	asset icon_clock_64, "assets/icons/raw/clock-64.bgra"
	asset icon_notes_120, "assets/icons/raw/notes-120.bgra"
	asset icon_notes_64, "assets/icons/raw/notes-64.bgra"
	asset icon_calculator_120, "assets/icons/raw/calculator-120.bgra"
	asset icon_calculator_64, "assets/icons/raw/calculator-64.bgra"
	asset icon_web_120, "assets/icons/raw/web-120.bgra"
	asset icon_web_64, "assets/icons/raw/web-64.bgra"
	asset icon_weather_120, "assets/icons/raw/weather-120.bgra"
	asset icon_weather_64, "assets/icons/raw/weather-64.bgra"
	asset icon_camera_120, "assets/icons/raw/camera-120.bgra"
	asset icon_camera_64, "assets/icons/raw/camera-64.bgra"
	asset icon_voicememos_120, "assets/icons/raw/voicememos-120.bgra"
	asset icon_voicememos_64, "assets/icons/raw/voicememos-64.bgra"
	asset icon_terminal_120, "assets/icons/raw/terminal-120.bgra"
	asset icon_terminal_64, "assets/icons/raw/terminal-64.bgra"
	asset icon_music_120, "assets/icons/raw/music-120.bgra"
	asset icon_music_64, "assets/icons/raw/music-64.bgra"
	asset icon_files_120, "assets/icons/raw/files-120.bgra"
	asset icon_files_64, "assets/icons/raw/files-64.bgra"
	asset icon_settings_120, "assets/icons/raw/settings-120.bgra"
	asset icon_settings_64, "assets/icons/raw/settings-64.bgra"

	.section .note.GNU-stack,"",%progbits
