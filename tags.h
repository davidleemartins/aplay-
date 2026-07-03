/* Metadata/tag parsers for aplay+ — ID3v2 (MP3/DSF), MP4/M4A atoms, ASF/WMA,
 * Vorbis KEY=VALUE comments, plus duration formatting.
 *	©2026 David Lee Martins (extracted from aplay+.c)
 *
 * Pure parsing: no ALSA, no theme, no globals. All readers are best-effort —
 * on any malformed structure they simply leave the Tags fields empty.
 * */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { char artist[160], album[160], title[160]; } Tags;

static void fmt_duration(char *out, size_t n, double sec)
{
	if (sec <= 0) { snprintf(out, n, "--:--"); return; }
	int s = (int)(sec + 0.5);
	int h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
	if (h) snprintf(out, n, "%d:%02d:%02d", h, m, ss);
	else   snprintf(out, n, "%d:%02d", m, ss);
}

// Fill artist/album/title from a "KEY=VALUE" comment (FLAC/Ogg Vorbis comments).
static void tag_set_kv(Tags *t, const char *kv)
{
	const char *eq = strchr(kv, '=');
	if (!eq) return;
	int klen = (int)(eq - kv);
	const char *val = eq + 1;
	if (klen == 6 && !strncasecmp(kv, "ARTIST", 6)) snprintf(t->artist, sizeof t->artist, "%s", val);
	else if (klen == 5 && !strncasecmp(kv, "ALBUM", 5)) snprintf(t->album, sizeof t->album, "%s", val);
	else if (klen == 5 && !strncasecmp(kv, "TITLE", 5)) snprintf(t->title, sizeof t->title, "%s", val);
}

// Decode an ID3v2 text frame payload into dst (handles Latin1/UTF-8 directly,
// and UTF-16 as its ASCII subset — enough for a readable now-playing line).
static void id3_text(char *dst, size_t dn, unsigned char enc, const unsigned char *s, long n)
{
	size_t o = 0;
	if (enc == 1 || enc == 2) {	// UTF-16 (with/without BOM) / UTF-16BE
		long i = 0; int be = (enc == 2);
		if (enc == 1 && n >= 2) {
			if (s[0] == 0xFF && s[1] == 0xFE) { be = 0; i = 2; }
			else if (s[0] == 0xFE && s[1] == 0xFF) { be = 1; i = 2; }
		}
		for (; i + 1 < n && o < dn - 1; i += 2) {
			unsigned char hi = be ? s[i] : s[i + 1];
			unsigned char lo = be ? s[i + 1] : s[i];
			if (hi == 0 && lo >= 0x20) dst[o++] = (char)lo;
		}
	} else {			// Latin1 (0) or UTF-8 (3)
		for (long i = 0; i < n && o < dn - 1; i++) {
			if (s[i] == 0) break;
			dst[o++] = (char)s[i];
		}
	}
	dst[o] = 0;
}

// Minimal ID3v2.3/2.4 tag reader from the current file position: pulls
// TIT2/TPE1/TALB (title/artist/album). Used for MP3 (offset 0) and DSF (the
// header points to an ID3 chunk elsewhere in the file).
static void read_id3v2_fp(FILE *fp, Tags *t)
{
	unsigned char h[10];
	if (fread(h, 1, 10, fp) != 10 || memcmp(h, "ID3", 3) != 0) return;
	int major = h[3];
	long size = ((long)(h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) |
	            ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);	// syncsafe
	if (size <= 0 || size > 10 * 1024 * 1024) return;
	unsigned char *buf = malloc(size);
	if (!buf) return;
	long got = (long)fread(buf, 1, size, fp);

	long pos = 0;
	while (pos + 10 <= got) {
		char id[5]; memcpy(id, buf + pos, 4); id[4] = 0;
		unsigned long fsize = (major == 4)
		    ? (((unsigned long)(buf[pos+4]&0x7f)<<21)|((buf[pos+5]&0x7f)<<14)|((buf[pos+6]&0x7f)<<7)|(buf[pos+7]&0x7f))
		    : (((unsigned long)buf[pos+4]<<24)|((unsigned long)buf[pos+5]<<16)|((unsigned long)buf[pos+6]<<8)|buf[pos+7]);
		pos += 10;
		if (id[0] == 0) break;	// padding
		if (fsize == 0 || pos + (long)fsize > got) break;

		char *dst = !strcmp(id, "TIT2") ? t->title
		          : !strcmp(id, "TPE1") ? t->artist
		          : !strcmp(id, "TALB") ? t->album : NULL;
		if (dst) id3_text(dst, 160, buf[pos], buf + pos + 1, (long)fsize - 1);
		pos += fsize;
	}
	free(buf);
}

// MP3: ID3v2 tag is at the start of the file.
static void read_id3v2(const char *path, Tags *t)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	read_id3v2_fp(fp, t);
	fclose(fp);
}

// DSF: the "DSD " header chunk holds a 64-bit LE pointer (at offset 20) to an
// embedded ID3v2 metadata chunk; read tags from there if present.
static void read_dsf_tags(const char *path, Tags *t)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	unsigned char hdr[28];
	if (fread(hdr, 1, 28, fp) == 28 && memcmp(hdr, "DSD ", 4) == 0) {
		unsigned long long ptr = 0;
		for (int i = 0; i < 8; i++) ptr |= (unsigned long long)hdr[20 + i] << (8 * i);
		if (ptr != 0 && fseek(fp, (long)ptr, SEEK_SET) == 0)
			read_id3v2_fp(fp, t);
	}
	fclose(fp);
}

// Find a direct child MP4/ISO atom of `type` within [pos,end). On success sets
// the child's content range [*c0,*c1) and returns 1.
static int mp4_child(FILE *fp, long pos, long end, const char *type, long *c0, long *c1)
{
	while (pos + 8 <= end) {
		unsigned char hdr[8];
		if (fseek(fp, pos, SEEK_SET) != 0 || fread(hdr, 1, 8, fp) != 8) return 0;
		unsigned long long sz = ((unsigned long)hdr[0] << 24) | ((unsigned long)hdr[1] << 16) |
		                        ((unsigned long)hdr[2] << 8) | hdr[3];
		long hdrlen = 8;
		if (sz == 1) {	// 64-bit extended size
			unsigned char ext[8];
			if (fread(ext, 1, 8, fp) != 8) return 0;
			sz = 0; for (int i = 0; i < 8; i++) sz = (sz << 8) | ext[i];
			hdrlen = 16;
		}
		if (sz < (unsigned long long)hdrlen) return 0;
		long next = pos + (long)sz;
		if (next <= pos || next > end) break;
		if (memcmp(hdr + 4, type, 4) == 0) { *c0 = pos + hdrlen; *c1 = next; return 1; }
		pos = next;
	}
	return 0;
}

// MP4/m4a tags: moov/udta/meta/ilst/{©nam,©ART,©alb}/data → UTF-8 text.
static void read_mp4_tags(const char *path, Tags *t)
{
	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	fseek(fp, 0, SEEK_END);
	long fend = ftell(fp);

	long m0, m1, u0, u1, e0, e1, i0, i1;
	if (mp4_child(fp, 0, fend, "moov", &m0, &m1) &&
	    mp4_child(fp, m0, m1, "udta", &u0, &u1) &&
	    mp4_child(fp, u0, u1, "meta", &e0, &e1) &&
	    // `meta` is a full atom (4-byte version/flags before children); fall back
	    // to no-prefix layout if the +4 form doesn't contain ilst.
	    (mp4_child(fp, e0 + 4, e1, "ilst", &i0, &i1) ||
	     mp4_child(fp, e0, e1, "ilst", &i0, &i1))) {
		struct { const char *type; char *dst; } map[] = {
			{ "\251nam", t->title }, { "\251ART", t->artist }, { "\251alb", t->album },
		};
		for (int k = 0; k < 3; k++) {
			long a0, a1, d0, d1;
			if (mp4_child(fp, i0, i1, map[k].type, &a0, &a1) &&
			    mp4_child(fp, a0, a1, "data", &d0, &d1)) {
				long vstart = d0 + 8;	// data atom: 4 version/flags + 4 reserved, then value
				long vlen = d1 - vstart;
				if (vlen > 0) {
					if (vlen > 159) vlen = 159;
					fseek(fp, vstart, SEEK_SET);
					size_t got = fread(map[k].dst, 1, (size_t)vlen, fp);
					map[k].dst[got] = 0;
				}
			}
		}
	}
	fclose(fp);
}

// WMA/ASF tags: Title+Author from the Content Description Object, Album from the
// Extended Content Description Object (WM/AlbumTitle). Strings are UTF-16LE.
static void read_asf_tags(const char *path, Tags *t)
{
	static const unsigned char G_HDR[16] = {0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
	static const unsigned char G_CDO[16] = {0x33,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
	static const unsigned char G_ECD[16] = {0x40,0xA4,0xD0,0xD2,0x07,0xE3,0xD2,0x11,0x97,0xF0,0x00,0xA0,0xC9,0x5E,0xA8,0x50};
#define LE16(p) ((unsigned)((p)[0] | ((p)[1] << 8)))

	FILE *fp = fopen(path, "rb");
	if (!fp) return;
	unsigned char head[30];
	if (fread(head, 1, 30, fp) != 30 || memcmp(head, G_HDR, 16) != 0) { fclose(fp); return; }
	unsigned long long hsize = 0;
	for (int i = 0; i < 8; i++) hsize |= (unsigned long long)head[16 + i] << (8 * i);
	if (hsize < 30 || hsize > 4 * 1024 * 1024) { fclose(fp); return; }

	unsigned char *buf = malloc(hsize);
	if (!buf) { fclose(fp); return; }
	fseek(fp, 0, SEEK_SET);
	unsigned long long got = fread(buf, 1, hsize, fp);
	fclose(fp);

	unsigned long long pos = 30;
	while (pos + 24 <= got) {
		unsigned long long osize = 0;
		for (int i = 0; i < 8; i++) osize |= (unsigned long long)buf[pos + 16 + i] << (8 * i);
		if (osize < 24 || pos + osize > got) break;

		if (memcmp(buf + pos, G_CDO, 16) == 0) {
			unsigned long long d = pos + 24;
			if (d + 10 <= got) {
				unsigned tl = LE16(buf + d), al = LE16(buf + d + 2);
				unsigned long long p = d + 10;
				if (p + tl <= got) id3_text(t->title, 160, 1, buf + p, tl);
				p += tl;
				if (p + al <= got) id3_text(t->artist, 160, 1, buf + p, al);
			}
		} else if (memcmp(buf + pos, G_ECD, 16) == 0) {
			unsigned long long d = pos + 24;
			if (d + 2 <= got) {
				unsigned count = LE16(buf + d);
				unsigned long long p = d + 2;
				for (unsigned i = 0; i < count && p + 2 <= got; i++) {
					unsigned nlen = LE16(buf + p); p += 2;
					if (p + nlen + 4 > got) break;
					char nm[64];
					id3_text(nm, sizeof nm, 1, buf + p, nlen);
					p += nlen;
					unsigned vtype = LE16(buf + p); p += 2;
					unsigned vlen = LE16(buf + p); p += 2;
					if (p + vlen > got) break;
					if (vtype == 0 && !strcmp(nm, "WM/AlbumTitle"))
						id3_text(t->album, 160, 1, buf + p, vlen);
					p += vlen;
				}
			}
		}
		pos += osize;
	}
	free(buf);
#undef LE16
}
