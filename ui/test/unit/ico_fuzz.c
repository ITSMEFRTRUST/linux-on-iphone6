// libFuzzer harness for ico.c: no input may crash it or read outside the buffer (ASan + UBSan). From ui/test/unit:
//   clang -g -O1 -fsanitize=fuzzer,address,undefined -I../../src ico_fuzz.c ../../src/ico.c -o /tmp/ico_fuzz
//   mkdir -p /tmp/ico-corpus; for f in ../web/icons/*/favicon.ico; do cp "$f" /tmp/ico-corpus/$(basename $(dirname "$f")).ico; done
//   /tmp/ico_fuzz -max_total_time=300 -max_len=65536 /tmp/ico-corpus
// run.sh builds it with -DICO_FUZZ_MAIN instead (no libFuzzer): it replays the fixture icons, so it keeps compiling.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ico.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len)
{
	uint8_t *copy = malloc(len ? len : 1);	/* exact-size heap copy: ASan sees any read past the end */
	if (!copy)
		return 0;
	memcpy(copy, data, len);
	ico_img_t im;
	int k = ico_decode(copy, len, &im);
	if (k == ICO_RGBA) {
		volatile unsigned sum = 0;	/* touch every output byte */
		for (size_t i = 0; i < (size_t)im.w * im.h * 4; i++)
			sum += im.rgba[i];
		free(im.rgba);
	} else if (k == ICO_PNG && (im.png < copy || im.png + im.png_len > copy + len)) {
		abort();
	}
	free(copy);
	return 0;
}

#ifdef ICO_FUZZ_MAIN
#include <stdio.h>
int main(int argc, char **argv)	/* each argument: a file to run through the harness */
{
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f)
			continue;
		static uint8_t b[1 << 20];
		size_t n = fread(b, 1, sizeof(b), f);
		fclose(f);
		LLVMFuzzerTestOneInput(b, n);
	}
	printf("ico_fuzz replay ok (%d files)\n", argc - 1);
	return 0;
}
#endif
