/* Stream only the bytecode of a verified, immutable JaroMail generation. */
#include "../filter/generation.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct jf_generation generation;
	struct jf_error error = {0};
	unsigned char digest[32];
	char hex[65];
	if (argc != 5 && argc != 6)
		return 2;
	if (jf_generation_pin(argv[1], argv[2], &generation, &error) != 0) {
		fprintf(stderr, "Invalid Sieve generation %s; run jaro fresh-filters\n", argv[2]);
		return 1;
	}
	jf_digest(generation.sieve.data, generation.sieve.size, digest);
	jf_digest_hex(digest, hex);
	int ok = generation.bytecode.size != 0 && !strcmp(hex, argv[3]) &&
		!strcmp(generation.binding.runtime, argv[4]);
	if (ok && argc == 6) {
		FILE *map = fopen(argv[5], "wbx");
		ok = map != NULL && generation.provenance.size != 0;
		if (map != NULL) {
			ok = ok && fwrite(generation.provenance.data, 1, generation.provenance.size, map)
				== generation.provenance.size;
			if (fclose(map) != 0) ok = 0;
		}
	}
	if (!ok)
		fputs("Stale Sieve source or runtime; run jaro fresh-filters\n", stderr);
	else
		ok = fwrite(generation.bytecode.data, 1, generation.bytecode.size, stdout)
			== generation.bytecode.size && fflush(stdout) == 0;
	jf_generation_close(&generation);
	return ok ? 0 : 1;
}
