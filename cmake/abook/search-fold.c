#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "search-fold.h"

struct fold_range {
	uint32_t first, last;
	const char *replacement; /* NULL means preserve the original character */
};

#include "search-fold-data.h"

/* Return 0 for a malformed UTF-8 sequence and let the caller skip one byte. */
static size_t
decode(const unsigned char *s, size_t remaining, uint32_t *point)
{
	size_t count, i;
	uint32_t value;

	if(s[0] < 0x80) {
		*point = s[0];
		return 1;
	}
	if(s[0] >= 0xc2 && s[0] <= 0xdf) {
		count = 2;
		value = s[0] & 0x1f;
	} else if(s[0] >= 0xe0 && s[0] <= 0xef) {
		count = 3;
		value = s[0] & 0x0f;
	} else if(s[0] >= 0xf0 && s[0] <= 0xf4) {
		count = 4;
		value = s[0] & 0x07;
	} else {
		return 0;
	}
	if(remaining < count)
		return 0;
	for(i = 1; i < count; i++) {
		if((s[i] & 0xc0) != 0x80)
			return 0;
		value = (value << 6) | (s[i] & 0x3f);
	}
	if((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
			(value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
		return 0;
	*point = value;
	return count;
}

static const struct fold_range *
find_range(uint32_t point)
{
	size_t lo = 0, hi = sizeof(fold_ranges) / sizeof(fold_ranges[0]);

	while(lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const struct fold_range *range = &fold_ranges[mid];
		if(point < range->first)
			hi = mid;
		else if(point > range->last)
			lo = mid + 1;
		else
			return range;
	}
	return NULL;
}

static char *
write_jamo(char *out, uint32_t point)
{
	*out++ = (char)(0xe0 | (point >> 12));
	*out++ = (char)(0x80 | ((point >> 6) & 0x3f));
	*out++ = (char)(0x80 | (point & 0x3f));
	return out;
}

char *
abook_search_key_alloc(const char *text, size_t length, void *(*allocate)(size_t))
{
	const unsigned char *src = (const unsigned char *)text;
	size_t remaining = length;
	char *key, *out;

	/* A character expands by at most 3x in UTF-8, including Hangul. */
	if(length > (SIZE_MAX - 1) / 3)
		return NULL;
	key = out = allocate(3 * length + 1);
	if(!key)
		return NULL;
	while(remaining) {
		uint32_t point;
		size_t count = decode(src, remaining, &point);
		const struct fold_range *range;

		if(!count) {
			src++;
			remaining--;
			continue;
		}
		if(point >= 0xac00 && point <= 0xd7a3) {
			uint32_t index = point - 0xac00;
			out = write_jamo(out, 0x1100 + index / (21 * 28));
			out = write_jamo(out, 0x1161 + (index / 28) % 21);
			if(index % 28)
				out = write_jamo(out, 0x11a7 + index % 28);
		} else if((range = find_range(point)) != NULL) {
			if(range->replacement) {
				size_t size = strlen(range->replacement);
				memcpy(out, range->replacement, size);
				out += size;
			} else {
				memcpy(out, src, count);
				out += count;
			}
		}
		src += count;
		remaining -= count;
	}
	*out = '\0';
	return key;
}

char *
abook_search_key_bytes(const char *text, size_t length)
{
	return abook_search_key_alloc(text, length, malloc);
}
