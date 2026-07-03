/* Unit tests for tags.h: ID3v2 (MP3 + DSF-embedded), MP4/M4A atoms, ASF/WMA,
 * tag_set_kv, fmt_duration. Fixture files come from make_fixtures.sh; pass the
 * fixture directory as argv[1].
 * ©2026 David Lee Martins
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tags.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void test_fmt_duration(void)
{
	char b[16];
	fmt_duration(b, sizeof b, 0);      CHECK(!strcmp(b, "--:--"), "0s -> %s", b);
	fmt_duration(b, sizeof b, -3);     CHECK(!strcmp(b, "--:--"), "neg -> %s", b);
	fmt_duration(b, sizeof b, 61);     CHECK(!strcmp(b, "1:01"), "61s -> %s", b);
	fmt_duration(b, sizeof b, 59.6);   CHECK(!strcmp(b, "1:00"), "59.6s rounds -> %s", b);
	fmt_duration(b, sizeof b, 3661);   CHECK(!strcmp(b, "1:01:01"), "3661s -> %s", b);
}

static void test_tag_set_kv(void)
{
	Tags t = {{0}};
	tag_set_kv(&t, "ARTIST=Someone");
	tag_set_kv(&t, "album=Somewhere");	// case-insensitive keys
	tag_set_kv(&t, "TITLE=Something");
	tag_set_kv(&t, "COMMENT=ignored");
	tag_set_kv(&t, "no-equals-sign");
	CHECK(!strcmp(t.artist, "Someone"), "artist: %s", t.artist);
	CHECK(!strcmp(t.album, "Somewhere"), "album: %s", t.album);
	CHECK(!strcmp(t.title, "Something"), "title: %s", t.title);
}

static void test_id3_text_utf16(void)
{
	char out[32];
	// UTF-16LE with BOM: "Hi"
	const unsigned char u16le[] = { 0xFF, 0xFE, 'H', 0, 'i', 0 };
	id3_text(out, sizeof out, 1, u16le, sizeof u16le);
	CHECK(!strcmp(out, "Hi"), "utf16le: %s", out);
	// Latin-1
	const unsigned char lat[] = { 'O', 'k', 0 };
	id3_text(out, sizeof out, 0, lat, 3);
	CHECK(!strcmp(out, "Ok"), "latin1: %s", out);
}

static void expect_tags(const char *what, const Tags *t,
                        const char *ti, const char *ar, const char *al)
{
	CHECK(!strcmp(t->title, ti), "%s title: want '%s' got '%s'", what, ti, t->title);
	CHECK(!strcmp(t->artist, ar), "%s artist: want '%s' got '%s'", what, ar, t->artist);
	CHECK(!strcmp(t->album, al), "%s album: want '%s' got '%s'", what, al, t->album);
}

int main(int argc, char **argv)
{
	test_fmt_duration();
	test_tag_set_kv();
	test_id3_text_utf16();

	if (argc > 1) {
		char p[4096];
		Tags t;

		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/t.mp3", argv[1]);
		read_id3v2(p, &t);
		expect_tags("mp3", &t, "Test Title", "Test Artist", "Test Album");

		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/t.m4a", argv[1]);
		read_mp4_tags(p, &t);
		expect_tags("m4a", &t, "Test Title", "Test Artist", "Test Album");

		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/t.wma", argv[1]);
		read_asf_tags(p, &t);
		expect_tags("wma", &t, "Test Title", "Test Artist", "Test Album");

		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/t.dsf", argv[1]);
		read_dsf_tags(p, &t);
		expect_tags("dsf", &t, "DSF Title", "DSF Artist", "DSF Album");

		// Robustness: parsers must not crash / fabricate on the wrong format.
		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/t16.flac", argv[1]);
		read_id3v2(p, &t);
		read_mp4_tags(p, &t);
		read_asf_tags(p, &t);
		read_dsf_tags(p, &t);
		CHECK(t.title[0] == 0 && t.artist[0] == 0 && t.album[0] == 0,
		      "wrong-format parses must stay empty");

		memset(&t, 0, sizeof t);
		snprintf(p, sizeof p, "%s/empty.flac", argv[1]);
		read_id3v2(p, &t);
		read_mp4_tags(p, &t);
		read_asf_tags(p, &t);
		read_dsf_tags(p, &t);
		CHECK(t.title[0] == 0, "empty file must parse to nothing");
	} else {
		fprintf(stderr, "note: no fixture dir given — file-based tests skipped\n");
	}

	printf("test_tags: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
