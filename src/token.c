#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "stoma/stoma.h"

/* Fold a UTF-8 string to lowercase, preserving accents (accent-sensitive
 * search). ASCII A-Z and the Latin-1 Supplement uppercase letters
 * (U+00C0-U+00DE, encoded as 0xC3 0x80-0x9E, minus U+00D7 '×') are
 * lowercased; everything else is copied verbatim. Lowercasing never grows
 * the output, so the result fits whenever outsz > strlen(in). The fold is
 * locale-independent (no iconv, no setlocale). */
int stoma_fold(char *out, size_t outsz, const char *in)
{
	const char *in_ptr = in;
	char *out_ptr = out;
	size_t in_len = strlen(in);
	size_t out_len = outsz;

	if (outsz == 0)
		return -1;
	if (in_len >= out_len)
		return -1;

	while (in_len > 0) {
		unsigned char c = (unsigned char)*in_ptr;

		if (c >= 'A' && c <= 'Z') {
			*out_ptr++ = (char)(c + 32);
		} else if (c == 0xC3 && in_len >= 2) {
			unsigned char b = (unsigned char)in_ptr[1];

			if (b >= 0x80 && b <= 0x9E && b != 0x97) {
				*out_ptr++ = (char)0xC3;
				*out_ptr++ = (char)(b + 0x20);
				in_ptr += 2;
				in_len -= 2;
				continue;
			}
			*out_ptr++ = (char)c;
		} else {
			*out_ptr++ = (char)c;
		}
		in_ptr++;
		in_len--;
	}
	*out_ptr = '\0';
	return (int)(out_ptr - out);
}

/* Internal tokenizer: calls cb(token, len, user) for each maximal run of
 * word characters in the folded string. A word character is an ASCII
 * alphanumeric or any byte >= 0x80 (UTF-8 accented letters and other
 * non-ASCII letters stay inside their token). Everything else (space,
 * punctuation, '?') separates tokens. */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user)
{
	const char *p = folded;

	if (!folded || !cb)
		return;
	while (*p) {
		const char *start = p;
		unsigned char c = (unsigned char)*p;

		if (!isalnum(c) && c < 0x80) {
			p++;
			continue;
		}
		while (*p) {
			unsigned char w = (unsigned char)*p;

			if (!isalnum(w) && w < 0x80)
				break;
			p++;
		}
		cb(start, (size_t)(p - start), user);
	}
}

int stoma_list_contains(const char *list, const char *token)
{
	if (!list || !list[0] || !token || !token[0])
		return 0;

	size_t tlen = strlen(token);
	const char *p = list;

	while (*p) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (len == tlen && memcmp(p, token, tlen) == 0)
			return 1;

		if (!end)
			break;
		p = end + 1;
	}
	return 0;
}

int stoma_list_append(char *out, size_t out_sz, const char *token)
{
	size_t used;
	size_t len;

	if (!out || !token || !token[0])
		return 0;
	if (stoma_list_contains(out, token))
		return 0;

	used = strlen(out);
	len = strlen(token);
	if (used && used + 1 >= out_sz)
		return -1;
	if (used)
		out[used++] = '\n';
	if (used + len >= out_sz)
		return -1;
	memcpy(out + used, token, len);
	used += len;
	out[used] = '\0';
	return 0;
}

static void str_trim_internal(char *s)
{
	char *p;
	size_t len;

	if (!s || !*s)
		return;
	p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	len = strlen(s);
	while (len > 0 &&
	       (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' ||
	        s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

int stoma_list_normalize(const char *input, char *out, size_t out_sz)
{
	char copy[8192];
	char *tok;
	char *saveptr;

	if (!out || out_sz == 0)
		return 0;

	copy[0] = '\0';
	if (input && input[0])
		snprintf(copy, sizeof(copy), "%s", input);
	out[0] = '\0';
	if (!copy[0])
		return 0;

	tok = strtok_r(copy, "\r\n", &saveptr);
	while (tok) {
		char t[256];
		snprintf(t, sizeof(t), "%s", tok);
		str_trim_internal(t);
		if (t[0])
			stoma_list_append(out, out_sz, t);
		tok = strtok_r(NULL, "\r\n", &saveptr);
	}
	return 0;
}
