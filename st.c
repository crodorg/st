/* See LICENSE for license details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "st.h"
#include "win.h"
#include "graphics.h"

extern char *argv0;

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define CAR_PER_ARG   4
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ

/* PUA character used as an image placeholder */
#define IMAGE_PLACEHOLDER_CHAR 0x10EEEE
#define IMAGE_PLACEHOLDER_CHAR_OLD 0xEEEE

/* macros */
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(u && wcschr(worddelimiters, u))

#define TSCREEN term.screen[IS_SET(MODE_ALTSCREEN)]
#define TLINEOFFSET(y) (((y) + TSCREEN.cur - TSCREEN.off + TSCREEN.size) % TSCREEN.size)
#define TLINE(y) (TSCREEN.buffer[TLINEOFFSET(y)])

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,  /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
	ESC_UTF8       = 64,
};

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	int alt;
} Selection;

/* Screen lines */
typedef struct {
	Line* buffer;  /* ring buffer */
	int size;      /* size of buffer */
	int cur;       /* start of active screen */
	int off;       /* scrollback line offset */
	TCursor sc;    /* saved cursor */
} LineBuffer;

/* Internal representation of the screen */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	int pixw;     /* width of the text area in pixels */
	int pixh;     /* height of the text area in pixels */
	LineBuffer screen[2]; /* screen and alternate screen */
	int linelen;  /* allocated line length */
	int *dirty;   /* dirtyness of lines */
	TCursor c;    /* cursor */
	int ocx;      /* old cursor col */
	int ocy;      /* old cursor row */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int *tabs;
	Rune lastc;   /* last printed char outside of sequence, 0 if control */
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	size_t len;            /* raw string length */
	char priv;
	char prefix;           /* '>' '<' or '=' private-marker prefix */
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
	int carg[ESC_ARG_SIZ][CAR_PER_ARG]; /* colon args */
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char *buf;             /* allocated raw string */
	size_t siz;            /* allocation size */
	size_t len;            /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

static void execsh(char *, char **);
static int chdir_by_pid(pid_t pid);
static void stty(char **);
static void sigchld(int);
static void ttywriteraw(const char *, size_t);

static void csidump(void);
static void csihandle(void);
static void readcolonargs(char **, int, int[][CAR_PER_ARG]);
static void csiparse(void);
static void csireset(void);
static void osc_color_response(int, int, int);
static int eschandle(uchar);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tprinter(char *, size_t);
static void tdumpsel(void);
static void tdumpline(int);
static void tdump(void);
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static int tlinelen(int);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(const int *, int);
static void tsetchar(Rune, const Glyph *, int, int);
static void tsetdirt(int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetmode(int, int, const int *, int);
static int twrite(const char *, int, int);
static void tfulldirt(void);
static void tcontrolcode(uchar );
static void tdectest(char );
static void tdefutf8(char);
static int32_t tdefcolor(const int *, int *, int);
static void tdeftran(char);
static void tstrsequence(uchar);

static void drawregion(int, int, int, int);
static void clearline(Line, Glyph, int, int);
static Line ensureline(Line);

static void selnormalize(void);
static void selscroll(int, int);
static void selsnap(int *, int *, int);

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static char base64dec_getc(const char **);

static ssize_t xwrite(int, const char *, size_t);

/* Globals */
static Term term;
static Selection sel;
static CSIEscape csiescseq;
static STREscape strescseq;
static int kbdcsiu; /* kitty keyboard disambiguate enabled (CSI u handshake) */
static int iofd = 1;
static int cmdfd;
static pid_t pid;

static const uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static const uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static const Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

/* Converts a diacritic to a row/column/etc number. The result is 1-base, 0
 * means "couldn't convert". Defined in rowcolumn_diacritics_helpers.c */
uint16_t diacritic_to_num(uint32_t code);

static int su = 0;
struct timespec sutv;

static void
tsync_begin()
{
	clock_gettime(CLOCK_MONOTONIC, &sutv);
	su = 1;
}

static void
tsync_end()
{
	su = 0;
}

int
tinsync(uint timeout)
{
	struct timespec now;
	if (su && !clock_gettime(CLOCK_MONOTONIC, &now)
	       && TIMEDIFF(now, sutv) >= timeout)
		su = 0;
	return su;
}

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return p;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

char
base64dec_getc(const char **src)
{
	while (**src && !isprint((unsigned char)**src))
		(*src)++;
	return **src ? *((*src)++) : '=';  /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;
	static const char base64_digits[256] = {
		[43] = 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		0, 0, 0, -1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0,
		0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
		40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
	};

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
	while (*src) {
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		/* invalid input. 'a' can be -1, e.g. if src is "\n" (c-str) */
		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

void
selinit(void)
{
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

int
tlinelen(int y)
{
	int i = term.col;
	Line line = TLINE(y);

	if (line[i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && line[i - 1].u == ' ')
		--i;

	return i;
}

void
selstart(int col, int row, int snap)
{
	selclear();
	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.snap = snap;
	sel.oe.x = sel.ob.x = col;
	sel.oe.y = sel.ob.y = row;
	selnormalize();

	if (sel.snap != 0)
		sel.mode = SEL_READY;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
selextend(int col, int row, int type, int done)
{
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (sel.mode == SEL_IDLE)
		return;
	if (done && sel.mode == SEL_EMPTY) {
		selclear();
		return;
	}

	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	oldtype = sel.type;

	sel.oe.x = col;
	sel.oe.y = row;
	selnormalize();
	sel.type = type;

	if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type || sel.mode == SEL_EMPTY)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

	sel.mode = done ? SEL_IDLE : SEL_READY;
}

void
selnormalize(void)
{
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	/* expand selection over line breaks */
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

int
selected(int x, int y)
{
	if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
			sel.alt != IS_SET(MODE_ALTSCREEN))
		return 0;

	if (sel.type == SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void
selsnap(int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	const Glyph *gp, *prevgp;

	switch (sel.snap) {
	case SNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		prevgp = &TLINE(*y)[*x];
		prevdelim = ISDELIM(prevgp->u);
		for (;;) {
			newx = *x + direction;
			newy = *y;
			if (!BETWEEN(newx, 0, term.col - 1)) {
				newy += direction;
				newx = (newx + term.col) % term.col;
				if (!BETWEEN(newy, 0, term.row - 1))
					break;

				if (direction > 0)
					yt = *y, xt = *x;
				else
					yt = newy, xt = newx;
				if (!(TLINE(yt)[xt].mode & ATTR_WRAP))
					break;
			}

			if (newx >= tlinelen(newy))
				break;

			gp = &TLINE(newy)[newx];
			delim = ISDELIM(gp->u);
			if (!(gp->mode & ATTR_WDUMMY) && (delim != prevdelim
					|| (delim && gp->u != prevgp->u)))
				break;

			*x = newx;
			*y = newy;
			prevgp = gp;
			prevdelim = delim;
		}
		break;
	case SNAP_LINE:
		/*
		 * Snap to paragraph: extend selection forward/backward
		 * until a blank row (tlinelen == 0) is reached, or buffer end.
		 */
		*x = (direction < 0) ? 0 : term.col - 1;
		if (tlinelen(*y) == 0)
			break;
		if (direction < 0) {
			for (; *y > 0; *y += direction) {
				if (tlinelen(*y - 1) == 0)
					break;
			}
		} else if (direction > 0) {
			for (; *y < term.row - 1; *y += direction) {
				if (tlinelen(*y + 1) == 0)
					break;
			}
		}
		break;
	}
}

char *
getsel(void)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	const Glyph *gp, *last;

	if (sel.ob.x == -1)
		return NULL;

	bufsize = (term.col+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	ptr = str = xmalloc(bufsize);

	/* append every set & selected glyph to the selection */
	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		if ((linelen = tlinelen(y)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR) {
			gp = &TLINE(y)[sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &TLINE(y)[sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		last = &TLINE(y)[MIN(lastx, linelen-1)];
		while (last >= gp && last->u == ' ')
			--last;

		for ( ; gp <= last; ++gp) {
			if (gp->mode & ATTR_WDUMMY)
				continue;

			if (gp->mode & ATTR_IMAGE) {
				// TODO: Copy diacritics as well
				ptr += utf8encode(IMAGE_PLACEHOLDER_CHAR, ptr);
				continue;
			}

			ptr += utf8encode(gp->u, ptr);
		}

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if (y < sel.ne.y || lastx >= linelen) {
			if ((last->mode & ATTR_WRAP) && sel.type != SEL_RECTANGULAR) {
				/* terminal-wrapped row: no separator */
			} else if (sel.snap == SNAP_LINE && sel.type != SEL_RECTANGULAR
			           && y + 1 <= sel.ne.y && tlinelen(y + 1) > 0) {
				*ptr++ = ' ';
			} else {
				*ptr++ = '\n';
			}
		}
	}
	*ptr = 0;
	return str;
}

char *
strstrany(char* s, char** strs) {
	char *match;
	for (int i = 0; strs[i]; i++) {
		if ((match = strstr(s, strs[i]))) {
			return match;
		}
	}
	return NULL;
}

void
highlighturlsline(int row)
{
	char *linestr = calloc(sizeof(char), term.col+1); /* assume ascii */
	char *match;
	for (int j = 0; j < term.col; j++) {
		if (TLINE(row)[j].u < 127) {
			linestr[j] = TLINE(row)[j].u;
		}
		linestr[term.col] = '\0';
	}
	int url_start = -1;
	while ((match = strstrany(linestr + url_start + 1, urlprefixes))) {
		url_start = match - linestr;
		for (int c = url_start; c < term.col && strchr(urlchars, linestr[c]); c++) {
			TLINE(row)[c].mode |= ATTR_URL;
			tsetdirt(row, c);
		}
	}
	free(linestr);
}

void
unhighlighturlsline(int row)
{
	for (int j = 0; j < term.col; j++) {
		Glyph* g = &TLINE(row)[j];
		if (g->mode & ATTR_URL) {
			g->mode &= ~ATTR_URL;
			tsetdirt(row, j);
		}
	}
	return;
}

/*
 * OSC 8 hyperlinks. URIs are interned into a session-global table; cells store
 * a 1-based id (0 = no link). Deduplicated so repeated links (e.g. one per
 * `ls --hyperlink` entry) don't grow the table without bound.
 */
static char **hlinks;
static size_t hlinks_len, hlinks_cap;

static uint
hlinkintern(const char *uri)
{
	size_t i;

	if (!uri || !*uri)
		return 0;
	for (i = 0; i < hlinks_len; i++)
		if (!strcmp(hlinks[i], uri))
			return i + 1;
	if (hlinks_len == hlinks_cap) {
		hlinks_cap = hlinks_cap ? hlinks_cap * 2 : 32;
		hlinks = xrealloc(hlinks, hlinks_cap * sizeof(*hlinks));
	}
	hlinks[hlinks_len++] = xstrdup(uri);
	return hlinks_len; /* 1-based */
}

char *
gethlink(uint id)
{
	return (id && id <= hlinks_len) ? hlinks[id - 1] : NULL;
}

int
followhlink(int col, int row)
{
	char *url;
	pid_t chpid;

	if (col < 0 || col >= term.col || row < 0 || row >= term.row)
		return 0;
	if (!(url = gethlink(TLINE(row)[col].hlink)))
		return 0;

	if ((chpid = fork()) == 0) {
		if (fork() == 0)
			execlp(urlhandler, urlhandler, url, NULL);
		exit(1);
	}
	if (chpid > 0)
		waitpid(chpid, NULL, 0);
	return 1;
}

int
followurl(int col, int row) {
	char *linestr = calloc(sizeof(char), term.col+1); /* assume ascii */
	char *match;
	for (int i = 0; i < term.col; i++) {
		if (TLINE(row)[i].u < 127) {
			linestr[i] = TLINE(row)[i].u;
		}
		linestr[term.col] = '\0';
	}
	int url_start = -1, found_url = 0;
	while ((match = strstrany(linestr + url_start + 1, urlprefixes))) {
		url_start = match - linestr;
		int url_end = url_start;
		for (int c = url_start; c < term.col && strchr(urlchars, linestr[c]); c++) {
			url_end++;
		}
		if (url_start <= col && col < url_end) {
			found_url = 1;
			linestr[url_end] = '\0';
			break;
		}
	}
	if (!found_url) {
		free(linestr);
		return 0;
	}

	pid_t chpid;
	if ((chpid = fork()) == 0) {
		if (fork() == 0)
			execlp(urlhandler, urlhandler, linestr + url_start, NULL);
		exit(1);
	}
	if (chpid > 0)
		waitpid(chpid, NULL, 0);
	free(linestr);
    return 1;
}

void
selclear(void)
{
	if (sel.ob.x == -1)
		return;
	sel.mode = SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void
execsh(char *cmd, char **args)
{
	char *sh, *prog, *arg;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args) {
		prog = args[0];
		arg = NULL;
	} else if (scroll) {
		prog = scroll;
		arg = utmp ? utmp : sh;
	} else if (utmp) {
		prog = utmp;
		arg = NULL;
	} else {
		prog = sh;
		arg = NULL;
	}
	DEFAULT(args, ((char *[]) {prog, arg, NULL}));

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void
sigchld(int a)
{
	int stat;
	pid_t p;

	if ((p = waitpid(-1, &stat, WNOHANG)) < 0)
		die("waiting for pid %hd failed: %s\n", pid, strerror(errno));

	if (pid != p) {
		/* reinstall sigchld handler */
		signal(SIGCHLD, sigchld);
		return;
	}

	if (WIFEXITED(stat) && WEXITSTATUS(stat))
		die("child exited with status %d\n", WEXITSTATUS(stat));
	else if (WIFSIGNALED(stat))
		die("child terminated due to signal %d\n", WTERMSIG(stat));
	_exit(0);
}

void
stty(char **args)
{
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd)-1)
		die("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = args; p && (s = *p); ++p) {
		if ((n = strlen(s)) > siz-1)
			die("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
		perror("Couldn't call stty");
}

int
ttynew(const char *line, char *cmd, const char *out, char **args)
{
	int m, s;

	if (out) {
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ?
			  1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				out, strerror(errno));
		}
	}

	if (line) {
		if ((cmdfd = open(line, O_RDWR)) < 0)
			die("open line '%s' failed: %s\n",
			    line, strerror(errno));
		dup2(cmdfd, 0);
		stty(args);
		return cmdfd;
	}

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, NULL) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (pid = fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		close(iofd);
		close(m);
		setsid(); /* create a new process group */
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		if (s > 2)
			close(s);
#ifdef __OpenBSD__
		if (pledge("stdio getpw proc exec", NULL) == -1)
			die("pledge\n");
#endif
		execsh(cmd, args);
		break;
	default:
#ifdef __OpenBSD__
		if (pledge("stdio rpath tty proc exec", NULL) == -1)
			die("pledge\n");
#endif
		fcntl(m, F_SETFD, FD_CLOEXEC);
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		break;
	}
	return cmdfd;
}

static int twrite_aborted = 0;
int ttyread_pending() { return twrite_aborted; }

size_t
ttyread(void)
{
	static char buf[BUFSIZ];
	static int buflen = 0;
	static int already_processing = 0;
	int ret, written = 0;

	if (buflen >= LEN(buf))
		return 0;

	/* append read bytes to unprocessed bytes */
	ret = twrite_aborted ? 1 : read(cmdfd, buf+buflen, LEN(buf)-buflen);

	switch (ret) {
	case 0:
		exit(0);
	case -1:
		die("couldn't read from shell: %s\n", strerror(errno));
	default:
		buflen += twrite_aborted ? 0 : ret;
		if (already_processing) {
			/* Avoid recursive call to twrite() */
			return ret;
		}
		already_processing = 1;
		while (1) {
			int buflen_before_processing = buflen;
			written += twrite(buf + written, buflen - written, 0);
			// If buflen changed during the call to twrite, there is
			// new data, and we need to keep processing, otherwise
			// we can exit. This will not loop forever because the
			// buffer is limited, and we don't clean it in this
			// loop, so at some point ttywrite will have to drop
			// some data.
			if (buflen_before_processing == buflen)
				break;
		}
		already_processing = 0;
		buflen -= written;
		/* keep any incomplete UTF-8 byte sequence for the next call */
		if (buflen > 0)
			memmove(buf, buf + written, buflen);
		return ret;
	}
}

void
ttywrite(const char *s, size_t n, int may_echo)
{
	const char *next;

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(s, n, 1);

	if (!IS_SET(MODE_CRLF)) {
		ttywriteraw(s, n);
		return;
	}

	/* This is similar to how the kernel handles ONLCR for ttys */
	while (n > 0) {
		if (*s == '\r') {
			next = s + 1;
			ttywriteraw("\r\n", 2);
		} else {
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void
ttywriteraw(const char *s, size_t n)
{
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;
	int retries_left = 100;

	/*
	 * Remember that we are using a pty, which might be a modem line.
	 * Writing too much will clog the line. That's why we are doing this
	 * dance.
	 * FIXME: Migrate the world to Plan 9.
	 */
	while (n > 0) {
		if (retries_left-- <= 0)
			goto too_many_retries;

		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &wfd);
		FD_SET(cmdfd, &rfd);

		/* Check if we can write. */
		if (pselect(cmdfd+1, &rfd, &wfd, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(cmdfd, &wfd)) {
			/*
			 * Only write the bytes written by ttywrite() or the
			 * default of 256. This seems to be a reasonable value
			 * for a serial line. Bigger values might clog the I/O.
			 */
			if ((r = write(cmdfd, s, (n < lim)? n : lim)) < 0)
				goto write_error;
			if (r < n) {
				/*
				 * We weren't able to write out everything.
				 * This means the buffer is getting full
				 * again. Empty it.
				 */
				if (n < lim)
					lim = ttyread();
				n -= r;
				s += r;
			} else {
				/* All bytes have been written. */
				break;
			}
		}
		if (FD_ISSET(cmdfd, &rfd))
			lim = ttyread();
	}
	return;

write_error:
	die("write error on tty: %s\n", strerror(errno));
too_many_retries:
	fprintf(stderr, "Could not write %zu bytes to tty\n", n);
}

void
ttyresize(int tw, int th)
{
	term.pixw = tw;
	term.pixh = th;

	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
ttyhangup(void)
{
	/* Send SIGHUP to shell */
	kill(pid, SIGHUP);
}

int
tattrset(int attr)
{
	int i, j;
	int y = TLINEOFFSET(0);

	for (i = 0; i < term.row-1; i++) {
		Line line = TSCREEN.buffer[y];
		for (j = 0; j < term.col-1; j++) {
			if (line[j].mode & attr)
				return 1;
		}
		y = (y+1) % TSCREEN.size;
	}

	return 0;
}

void
tsetdirt(int top, int bot)
{
	int i;

	LIMIT(top, 0, term.row-1);
	LIMIT(bot, 0, term.row-1);

	for (i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
tsetdirtattr(int attr)
{
	int i, j;
	int y = TLINEOFFSET(0);

	for (i = 0; i < term.row-1; i++) {
		Line line = TSCREEN.buffer[y];
		for (j = 0; j < term.col-1; j++) {
			if (line[j].mode & attr) {
				tsetdirt(i, i);
				break;
			}
		}
		y = (y+1) % TSCREEN.size;
	}
}

void
tfulldirt(void)
{
	tsync_end();
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode)
{
	if (mode == CURSOR_SAVE) {
		TSCREEN.sc = term.c;
	} else if (mode == CURSOR_LOAD) {
		term.c = TSCREEN.sc;
		tmoveto(term.c.x, term.c.y);
	}
}

void
treset(void)
{
	int i, j;
	Glyph g = (Glyph){.mode = ATTR_NULL,
			  .fg = defaultfg,
			  .bg = defaultbg,
			  .decor = DECOR_DEFAULT_COLOR};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = tabspaces; i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP|MODE_UTF8;
	memset(term.trantbl, CS_USA, sizeof(term.trantbl));
	term.charset = 0;

	for (i = 0; i < 2; i++) {
		term.screen[i].sc = (TCursor){{
			.fg = defaultfg,
			.bg = defaultbg,
			.decor = DECOR_DEFAULT_COLOR
		}};
		term.screen[i].cur = 0;
		term.screen[i].off = 0;
		for (j = 0; j < term.row; ++j) {
			if (term.col != term.linelen)
				term.screen[i].buffer[j] = xrealloc(term.screen[i].buffer[j], term.col * sizeof(Glyph));
			clearline(term.screen[i].buffer[j], g, 0, term.col);
		}
		for (j = term.row; j < term.screen[i].size; ++j) {
			free(term.screen[i].buffer[j]);
			term.screen[i].buffer[j] = NULL;
		}
	}
	tcursor(CURSOR_LOAD);
	term.linelen = term.col;
	tfulldirt();
}

void
tnew(int col, int row)
{
	int i;
	term = (Term){};
	term.screen[0].buffer = xmalloc(HISTSIZE * sizeof(Line));
	term.screen[0].size = HISTSIZE;
	term.screen[1].buffer = NULL;
	for (i = 0; i < HISTSIZE; ++i) term.screen[0].buffer[i] = NULL;

	tresize(col, row);
	treset();
}

int tisaltscr(void)
{
	return IS_SET(MODE_ALTSCREEN);
}

void
tswapscreen(void)
{
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

void
kscrollup(const Arg *a)
{
	int n = a->i;

	if (IS_SET(MODE_ALTSCREEN))
		return;

	if (n < 0) n = (-n) * term.row;
	if (n > TSCREEN.size - term.row - TSCREEN.off) n = TSCREEN.size - term.row - TSCREEN.off;
	while (!TLINE(-n)) --n;
	TSCREEN.off += n;
	selscroll(0, n);
	tfulldirt();
}

void
kscrolldown(const Arg *a)
{

	int n = a->i;

	if (IS_SET(MODE_ALTSCREEN))
		return;

	if (n < 0) n = (-n) * term.row;
	if (n > TSCREEN.off) n = TSCREEN.off;
	TSCREEN.off -= n;
	selscroll(0, -n);
	tfulldirt();
}

/*
 * OSC 7: working directory reported by the shell as file://host/path.
 * Preferred by newterm over the /proc fallback. Stays NULL (and thus inert)
 * unless the shell emits OSC 7 each prompt.
 */
static char *osc7cwd;

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void
osc7setcwd(const char *uri)
{
	const char *p;
	char *path, *w;
	int hi, lo;

	if (!strncmp(uri, "file://", 7)) {
		if (!(p = strchr(uri + 7, '/'))) /* skip host, keep leading / */
			return;
	} else {
		p = uri; /* tolerate a bare path */
	}

	w = path = xmalloc(strlen(p) + 1);
	for (; *p; p++) {
		if (*p == '%' && (hi = hexval(p[1])) >= 0 && (lo = hexval(p[2])) >= 0) {
			*w++ = (hi << 4) | lo;
			p += 2;
		} else {
			*w++ = *p;
		}
	}
	*w = '\0';

	free(osc7cwd);
	osc7cwd = path;
}

void
newterm(const Arg* a)
{
	switch (fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		switch (fork()) {
		case -1:
			fprintf(stderr, "fork failed: %s\n", strerror(errno));
			_exit(1);
			break;
		case 0:
			chdir_by_pid(pid);
			execl("/proc/self/exe", argv0, NULL);
			_exit(1);
			break;
		default:
			_exit(0);
		}
	default:
		wait(NULL);
	}
}

static int
chdir_by_pid(pid_t pid)
{
	char buf[32];
	if (osc7cwd && chdir(osc7cwd) == 0)
		return 0;
	snprintf(buf, sizeof buf, "/proc/%ld/cwd", (long)pid);
	return chdir(buf);
}

void
tscrolldown(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);

	/* Ensure that lines are allocated */
	for (i = -n; i < 0; i++) {
		TLINE(i) = ensureline(TLINE(i));
	}

	/* Shift non-scrolling areas in ring buffer */
	for (i = term.bot+1; i < term.row; i++) {
		temp = TLINE(i);
		TLINE(i) = TLINE(i-n);
		TLINE(i-n) = temp;
	}
	for (i = 0; i < orig; i++) {
		temp = TLINE(i);
		TLINE(i) = TLINE(i-n);
		TLINE(i-n) = temp;
	}

	/* Scroll buffer */
	TSCREEN.cur = (TSCREEN.cur + TSCREEN.size - n) % TSCREEN.size;
	/* Clear lines that have entered the view */
	tclearregion(0, orig, term.linelen-1, orig+n-1);
	/* Redraw portion of the screen that has scrolled */
	tsetdirt(orig+n-1, term.bot);
	selscroll(orig, n);
}

void
tscrollup(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);

	/* Ensure that lines are allocated */
	for (i = term.row; i < term.row + n; i++) {
		TLINE(i) = ensureline(TLINE(i));
	}

	/* Shift non-scrolling areas in ring buffer */
	for (i = orig-1; i >= 0; i--) {
		temp = TLINE(i);
		TLINE(i) = TLINE(i+n);
		TLINE(i+n) = temp;
	}
	for (i = term.row-1; i >term.bot; i--) {
		temp = TLINE(i);
		TLINE(i) = TLINE(i+n);
		TLINE(i+n) = temp;
	}

	/* Scroll buffer */
	TSCREEN.cur = (TSCREEN.cur + n) % TSCREEN.size;
	/* Clear lines that have entered the view */
	tclearregion(0, term.bot-n+1, term.linelen-1, term.bot);
	/* Redraw portion of the screen that has scrolled */
	tsetdirt(orig, term.bot-n+1);
	selscroll(orig, -n);
}

void
selscroll(int orig, int n)
{
	if (sel.ob.x == -1 || sel.alt != IS_SET(MODE_ALTSCREEN))
		return;

	if (BETWEEN(sel.nb.y, orig, term.bot) != BETWEEN(sel.ne.y, orig, term.bot)) {
		selclear();
	} else if (BETWEEN(sel.nb.y, orig, term.bot)) {
		sel.ob.y += n;
		sel.oe.y += n;
		if (sel.ob.y < term.top || sel.ob.y > term.bot ||
		    sel.oe.y < term.top || sel.oe.y > term.bot) {
			selclear();
		} else {
			selnormalize();
		}
	}
}

void
tnewline(int first_col)
{
	int y = term.c.y;

	if (y == term.bot) {
		tscrollup(term.top, 1);
	} else {
		y++;
	}
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
readcolonargs(char **p, int cursor, int params[][CAR_PER_ARG])
{
	int i = 0;
	for (; i < CAR_PER_ARG; i++)
		params[cursor][i] = -1;

	if (**p != ':')
		return;

	char *np = NULL;
	i = 0;

	while (**p == ':' && i < CAR_PER_ARG) {
		while (**p == ':')
			(*p)++;
		params[cursor][i] = strtol(*p, &np, 10);
		*p = np;
		i++;
	}
}

void
csiparse(void)
{
	char *p = csiescseq.buf, *np;
	long int v;
	int sep = ';'; /* colon or semi-colon, but not both */

	csiescseq.narg = 0;
	if (*p == '?') {
		csiescseq.priv = 1;
		p++;
	} else if (*p == '>' || *p == '<' || *p == '=') {
		csiescseq.prefix = *p;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		readcolonargs(&p, csiescseq.narg-1, csiescseq.carg);
		if (sep == ';' && *p == ':')
			sep = ':'; /* allow override to colon once */
		if (*p != sep || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < csiescseq.buf+csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void
tmoveato(int x, int y)
{
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top: 0));
}

void
tmoveto(int x, int y)
{
	int miny, maxy;

	if (term.c.state & CURSOR_ORIGIN) {
		miny = term.top;
		maxy = term.bot;
	} else {
		miny = 0;
		maxy = term.row - 1;
	}
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = LIMIT(x, 0, term.col-1);
	term.c.y = LIMIT(y, miny, maxy);
}

void
tsetchar(Rune u, const Glyph *attr, int x, int y)
{
	static const char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};
	Line line = TLINE(y);

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (term.trantbl[term.charset] == CS_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (line[x].mode & ATTR_WIDE) {
		if (x+1 < term.col) {
			line[x+1].u = ' ';
			line[x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if (line[x].mode & ATTR_WDUMMY) {
		line[x-1].u = ' ';
		line[x-1].mode &= ~ATTR_WIDE;
	}

	if (u == ' ' && line[x].mode & ATTR_IMAGE &&
	    tgetisclassicplaceholder(&line[x])) {
		// This is a workaround: don't overwrite classic placement
		// placeholders with space symbols (unlike Unicode placeholders
		// which must be overwritten by anything).
		line[x].bg = attr->bg;
		term.dirty[y] = 1;
		return;
	}

	term.dirty[y] = 1;
	line[x] = *attr;
	line[x].u = u;

	if (u == IMAGE_PLACEHOLDER_CHAR || u == IMAGE_PLACEHOLDER_CHAR_OLD) {
		line[x].u = 0;
		line[x].mode |= ATTR_IMAGE;
	}

	if (isboxdraw(u))
		line[x].mode |= ATTR_BOXDRAW;
}

void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, L, S, temp;
	Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.linelen-1);
	LIMIT(x2, 0, term.linelen-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	L = TLINEOFFSET(y1);
	for (y = y1; y <= y2; y++) {
		term.dirty[y] = 1;
		for (x = x1; x <= x2; x++) {
			gp = &TSCREEN.buffer[L][x];
			if (selected(x, y))
				selclear();
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->decor = term.c.attr.decor;
			gp->mode = 0;
			gp->u = ' ';
			gp->hlink = 0;
		}
		L = (L + 1) % TSCREEN.size;
	}
}

/// Fills a rectangle area with an image placeholder. The starting point is the
/// cursor. Adds empty lines if needed. The placeholder will be marked as
/// classic.
void tcreateimgplaceholder(uint32_t image_id, uint32_t placement_id, int cols,
			   int rows, char do_not_move_cursor,
			   Glyph *text_underneath) {
	for (int row = 0; row < rows; ++row) {
		int y = term.c.y;
		term.dirty[y] = 1;
		for (int col = 0; col < cols; ++col) {
			int x = term.c.x + col;
			if (x >= term.col)
				break;
			Glyph *gp = &TLINE(y)[x];
			if (selected(x, y))
				selclear();
			if (text_underneath) {
				Glyph *to_save = gp;
				// If there is already a classic placeholder,
				// use the text underneath it. This will leave
				// holes in images, but at least we are
				// guaranteed to restore the original text.
				if (gp->mode & ATTR_IMAGE &&
				    tgetisclassicplaceholder(gp)) {
					Glyph *under =
						gr_get_glyph_underneath_image(
							tgetimgid(gp),
							tgetimgplacementid(gp),
							tgetimgcol(gp),
							tgetimgrow(gp));
					if (under)
						to_save = under;
				}
				text_underneath[cols * row + col] = *to_save;
			}
			gp->mode = ATTR_IMAGE;
			gp->u = 0;
			gp->hlink = 0;
			tsetimgrow(gp, row + 1);
			tsetimgcol(gp, col + 1);
			tsetimgid(gp, image_id);
			tsetimgplacementid(gp, placement_id);
			tsetimgdiacriticcount(gp, 3);
			tsetisclassicplaceholder(gp, 1);
		}
		// If moving the cursor is not allowed and this is the last line
		// of the terminal, we are done.
		if (do_not_move_cursor && y == term.row - 1)
			break;
		// Move the cursor down, maybe creating a new line. The x is
		// preserved (we never change term.c.x in the loop above).
		if (row != rows - 1)
			tnewline(/*first_col=*/0);
	}
	if (do_not_move_cursor) {
		// Return the cursor to the original position.
		tmoveto(term.c.x, term.c.y - rows + 1);
	} else {
		// Move the cursor beyond the last column, as required by the
		// protocol. If the cursor goes beyond the screen edge, insert a
		// newline to match the behavior of kitty.
		if (term.c.x + cols >= term.col)
			tnewline(/*first_col=*/1);
		else
			tmoveto(term.c.x + cols, term.c.y);
	}
}

void gr_for_each_image_cell(int (*callback)(void *data, Glyph *gp),
			    void *data) {
	for (int row = 0; row < term.row; ++row) {
		for (int col = 0; col < term.col; ++col) {
			Glyph *gp = &TLINE(row)[col];
			if (gp->mode & ATTR_IMAGE) {
				if (callback(data, gp))
					term.dirty[row] = 1;
			}
		}
	}
}

void gr_schedule_image_redraw_by_id(uint32_t image_id) {
	for (int row = 0; row < term.row; ++row) {
		if (term.dirty[row])
			continue;
		for (int col = 0; col < term.col; ++col) {
			Glyph *gp = &TLINE(row)[col];
			if (gp->mode & ATTR_IMAGE) {
				uint32_t cell_image_id = tgetimgid(gp);
				if (cell_image_id == image_id) {
					term.dirty[row] = 1;
					break;
				}
			}
		}
	}
}

void
tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = TLINE(term.c.y);

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = TLINE(term.c.y);

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void
tinsertblankline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n);
}

void
tdeleteline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n);
}

int32_t
tdefcolor(const int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			term.c.attr.decor = DECOR_DEFAULT_COLOR;
			term.c.attr.ustyle = -1;
			term.c.attr.ucolor[0] = -1;
			term.c.attr.ucolor[1] = -1;
			term.c.attr.ucolor[2] = -1;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term.c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.ustyle = csiescseq.carg[i][0];

			if (term.c.attr.ustyle != 0)
				term.c.attr.mode |= ATTR_UNDERLINE;
			else
				term.c.attr.mode &= ~ATTR_UNDERLINE;

			term.c.attr.mode ^= ATTR_DIRTYUNDERLINE;
			break;
		case 5: /* slow blink */
			/* FALLTHROUGH */
		case 6: /* rapid blink */
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term.c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term.c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			tsetdecorstyle(&term.c.attr, 0);
			break;
		case 25:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term.c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term.c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39: /* set foreground color to default */
			term.c.attr.fg = defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49: /* set background color to default */
			term.c.attr.bg = defaultbg;
			break;
		case 58:
			term.c.attr.ucolor[0] = csiescseq.carg[i][1];
			term.c.attr.ucolor[1] = csiescseq.carg[i][2];
			term.c.attr.ucolor[2] = csiescseq.carg[i][3];
			term.c.attr.mode ^= ATTR_DIRTYUNDERLINE;
			break;
		case 59:
			term.c.attr.ucolor[0] = -1;
			term.c.attr.ucolor[1] = -1;
			term.c.attr.ucolor[2] = -1;
			term.c.attr.mode ^= ATTR_DIRTYUNDERLINE;
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term.c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term.c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term.c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term.c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]);
				csidump();
			}
			break;
		}
	}
}

void
tsetscroll(int t, int b)
{
	int temp;

	LIMIT(t, 0, term.row-1);
	LIMIT(b, 0, term.row-1);
	if (t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term.top = t;
	term.bot = b;
}

void
tsetmode(int priv, int set, const int *args, int narg)
{
	int alt; const int *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1: /* DECCKM -- Cursor key */
				xsetmode(set, MODE_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				xsetmode(set, MODE_REVERSE);
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term.c.state, set, CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				xsetmode(!set, MODE_HIDE);
				break;
			case 9:    /* X10 mouse compatibility mode */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEX10);
				break;
			case 1000: /* 1000: report button press */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEBTN);
				break;
			case 1002: /* 1002: report motion on button press */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMOTION);
				break;
			case 1003: /* 1003: enable all mouse motions */
				xsetpointermotion(set);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMANY);
				break;
			case 1004: /* 1004: send focus events to tty */
				xsetmode(set, MODE_FOCUS);
				break;
			case 1006: /* 1006: extended reporting mode */
				xsetmode(set, MODE_MOUSESGR);
				break;
			case 1034: /* 1034: enable 8-bit mode for keyboard input */
				xsetmode(set, MODE_8BIT);
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!allowaltscreen)
					break;
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen buffer */
			case 1047: /* swap screen buffer */
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt) {
					tclearregion(0, 0, term.col-1,
							term.row-1);
				}
				if (set ^ alt) /* set is always 1 or 0 */
					tswapscreen();
				if (*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048: /* save/restore cursor (like DECSC/DECRC) */
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				xsetmode(set, MODE_BRCKTPASTE);
				break;
			case 2026: /* Synchronized Update */
				if (set)
					tsync_begin();
				else
					tsync_end();
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:
				xsetmode(set, MODE_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term.mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term.mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

void
csihandle(void)
{
	char buf[40];
	int len;

	/*
	 * CSI sequences with a '>', '<' or '=' prefix. Only a couple are
	 * implemented; the rest are ignored (as before). Handling them here
	 * keeps prefixed input (e.g. secondary DA "CSI > c") from leaking into
	 * the unprefixed cases below.
	 */
	if (csiescseq.prefix) {
		switch (csiescseq.mode[0]) {
		case 'u': /* kitty keyboard protocol, level 1 (disambiguate) */
			kbdcsiu = (csiescseq.prefix == '>') ? 1 :
			          (csiescseq.prefix == '<') ? 0 :
			          (csiescseq.arg[0] & 1); /* '=' : set from flags */
			xsetmode(kbdcsiu, MODE_KBD_CSIU);
			break;
		case 'q': /* CSI > q -- XTVERSION */
			if (csiescseq.prefix == '>') {
				len = snprintf(buf, sizeof(buf),
				    "\033P>|st-graphics(%s)\033\\", VERSION);
				ttywrite(buf, len, 0);
			}
			break;
		}
		return;
	}

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+csiescseq.arg[0]);
		break;
	case 'i': /* MC -- Media Copy */
		switch (csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(term.c.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			term.mode &= ~MODE_PRINT;
			break;
		case 5:
			term.mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if (csiescseq.arg[0] == 0)
			ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'b': /* REP -- if last char is printable print it <n> more times */
		LIMIT(csiescseq.arg[0], 1, 65535);
		if (term.lastc)
			while (csiescseq.arg[0]-- > 0)
				tputc(term.lastc);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x+csiescseq.arg[0], term.c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x-csiescseq.arg[0], term.c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y+csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y-csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch (csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term.tabs[term.c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term.tabs, 0, term.col * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0]-1, term.c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		switch (csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if (term.c.y < term.row-1) {
				tclearregion(0, term.c.y+1, term.col-1,
						term.row-1);
			}
			break;
		case 1: /* above */
			if (term.c.y > 0)
				tclearregion(0, 0, term.col-1, term.c.y-1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, 0, term.col-1, term.row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch (csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term.c.x, term.c.y, term.col-1,
					term.c.y);
			break;
		case 1: /* left */
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, term.c.y, term.col-1, term.c.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		if (csiescseq.priv) break;
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0]);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y,
				term.c.x + csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n': /* DSR -- Device Status Report */
		switch (csiescseq.arg[0]) {
		case 5: /* Status Report "OK" `0n` */
			ttywrite("\033[0n", sizeof("\033[0n") - 1, 0);
			break;
		case 6: /* Report Cursor Position (CPR) "<row>;<column>R" */
			len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
			               term.c.y+1, term.c.x+1);
			ttywrite(buf, len, 0);
			break;
		default:
			goto unknown;
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if (csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor; CSI ? u -- query kbd flags */
		if (csiescseq.priv) {
			len = snprintf(buf, sizeof(buf), "\033[?%du", kbdcsiu);
			ttywrite(buf, len, 0);
		} else {
			tcursor(CURSOR_LOAD);
		}
		break;
	case ' ':
		switch (csiescseq.mode[1]) {
		case 'q': /* DECSCUSR -- Set Cursor Style */
			if (xsetcursor(csiescseq.arg[0]))
				goto unknown;
			break;
		default:
			goto unknown;
		}
		break;
	case 't': /* XTWINOPS / title stack operations */
		switch (csiescseq.arg[0]) {
		case 14: /* Report text area size in pixels. */
			len = snprintf(buf, sizeof(buf), "\033[4;%i;%it",
					term.pixh, term.pixw);
			ttywrite(buf, len, 0);
			break;
		case 16: /* Report character cell size in pixels. */
			len = snprintf(buf, sizeof(buf), "\033[6;%i;%it",
					term.pixh / term.row,
					term.pixw / term.col);
			ttywrite(buf, len, 0);
			break;
		case 18: /* Report the size of the text area in characters. */
			len = snprintf(buf, sizeof(buf), "\033[8;%i;%it",
					term.row, term.col);
			ttywrite(buf, len, 0);
			break;
		case 22: /* push current title on stack */
			switch (csiescseq.arg[1]) {
			case 0:
			case 1:
			case 2:
				xpushtitle();
				break;
			default:
				goto unknown;
			}
			break;
		case 23: /* pop last title from stack */
			switch (csiescseq.arg[1]) {
			case 0:
			case 1:
			case 2:
				xsettitle(NULL, 1);
				break;
			default:
				goto unknown;
			}
			break;
		default:
			goto unknown;
		}
		break;
	}
}

void
csidump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
csireset(void)
{
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
osc_color_response(int num, int index, int is_osc4)
{
	int n;
	char buf[32];
	unsigned char r, g, b;

	if (xgetcolor(is_osc4 ? num : index, &r, &g, &b)) {
		fprintf(stderr, "erresc: failed to fetch %s color %d\n",
		        is_osc4 ? "osc4" : "osc",
		        is_osc4 ? num : index);
		return;
	}

	n = snprintf(buf, sizeof buf, "\033]%s%d;rgb:%02x%02x/%02x%02x/%02x%02x\007",
	             is_osc4 ? "4;" : "", num, r, r, g, g, b, b);
	if (n < 0 || n >= sizeof(buf)) {
		fprintf(stderr, "error: %s while printing %s response\n",
		        n < 0 ? "snprintf failed" : "truncation occurred",
		        is_osc4 ? "osc4" : "osc");
	} else {
		ttywrite(buf, n, 1);
	}
}

void
strhandle(void)
{
	char *p = NULL, *dec;
	int j, narg, par;
	const struct { int idx; char *str; } osc_table[] = {
		{ defaultfg, "foreground" },
		{ defaultbg, "background" },
		{ defaultcs, "cursor" }
	};

	term.esc &= ~(ESC_STR_END|ESC_STR);
	strparse();
	par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch (par) {
		case 0:
			if (narg > 1) {
				xsettitle(strescseq.args[1], 0);
				xseticontitle(strescseq.args[1]);
			}
			return;
		case 1:
			if (narg > 1)
				xseticontitle(strescseq.args[1]);
			return;
		case 2:
			if (narg > 1)
				xsettitle(strescseq.args[1], 0);
			return;
		case 9: /* iTerm2-style desktop notification: OSC 9 ; body BEL */
			if (narg > 1)
				xnotify(NULL, strescseq.args[1]);
			return;
		case 777: /* urxvt-style: OSC 777 ; notify ; title ; body BEL */
			if (narg > 1 && !strcmp(strescseq.args[1], "notify")) {
				xnotify(narg > 2 ? strescseq.args[2] : NULL,
				        narg > 3 ? strescseq.args[3] : NULL);
			}
			return;
		case 7: /* OSC 7 ; file://host/path -- report working directory */
			if (narg > 1)
				osc7setcwd(strescseq.args[1]);
			return;
		case 8: /* OSC 8 ; params ; URI -- hyperlink (empty URI closes) */
			/* NOTE: strparse split on ';', so a URI containing a
			 * literal ';' would be truncated. OSC 8 producers don't
			 * emit those, so take args[2] as the URI. */
			term.c.attr.hlink = (narg > 2) ? hlinkintern(strescseq.args[2]) : 0;
			return;
		case 52: /* manipulate selection data */
			if (narg > 2 && allowwindowops) {
				dec = base64dec(strescseq.args[2]);
				if (dec) {
					xsetsel(dec);
					xclipcopy();
				} else {
					fprintf(stderr, "erresc: invalid base64\n");
				}
			}
			return;
		case 10: /* set dynamic VT100 text foreground color */
		case 11: /* set dynamic VT100 text background color */
		case 12: /* set dynamic text cursor color */
			if (narg < 2)
				break;
			p = strescseq.args[1];
			if ((j = par - 10) < 0 || j >= LEN(osc_table))
				break; /* shouldn't be possible */

			if (!strcmp(p, "?")) {
				osc_color_response(par, osc_table[j].idx, 0);
			} else if (xsetcolorname(osc_table[j].idx, p)) {
				fprintf(stderr, "erresc: invalid %s color: %s\n",
				        osc_table[j].str, p);
			} else {
				tfulldirt();
			}
			return;
		case 4: /* color set */
			if (narg < 3)
				break;
			p = strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;

			if (p && !strcmp(p, "?")) {
				osc_color_response(j, 0, 1);
			} else if (xsetcolorname(j, p)) {
				if (par == 104 && narg <= 1) {
					xloadcols();
					return; /* color reset without parameter */
				}
				fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
				        j, p ? p : "(null)");
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				tfulldirt();
			}
			return;
		case 110: /* reset dynamic VT100 text foreground color */
		case 111: /* reset dynamic VT100 text background color */
		case 112: /* reset dynamic text cursor color */
			if (narg != 1)
				break;
			if ((j = par - 110) < 0 || j >= LEN(osc_table))
				break; /* shouldn't be possible */
			if (xsetcolorname(osc_table[j].idx, NULL)) {
				fprintf(stderr, "erresc: %s color not found\n", osc_table[j].str);
			} else {
				tfulldirt();
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0], 0);
		return;
	case 'P': /* DCS -- Device Control String */
		/* https://gitlab.com/gnachman/iterm2/-/wikis/synchronized-updates-spec */
		if (strstr(strescseq.buf, "=1s") == strescseq.buf)
			tsync_begin();  /* BSU */
		else if (strstr(strescseq.buf, "=2s") == strescseq.buf)
			tsync_end();  /* ESU */
		return;
	case '_': /* APC -- Application Program Command */
		if (gr_parse_command(strescseq.buf, strescseq.len)) {
			GraphicsCommandResult *res = &graphics_command_result;
			if (res->create_placeholder) {
				tcreateimgplaceholder(
					res->placeholder.image_id,
					res->placeholder.placement_id,
					res->placeholder.columns,
					res->placeholder.rows,
					res->placeholder.do_not_move_cursor,
					res->placeholder.text_underneath);
			}
			if (res->response[0])
				ttywrite(res->response, strlen(res->response),
					 0);
			if (res->redraw)
				tfulldirt();
			return;
		}
		return;
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < STR_ARG_SIZ) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
externalpipe(const Arg *arg)
{
	int to[2];
	char buf[UTF_SIZ];
	void (*oldsigpipe)(int);
	Glyph *bp, *end;
	int lastpos, n, newline;

	if (pipe(to) == -1)
		return;

	switch (fork()) {
	case -1:
		close(to[0]);
		close(to[1]);
		return;
	case 0:
		dup2(to[0], STDIN_FILENO);
		close(to[0]);
		close(to[1]);
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "st: execvp %s\n", ((char **)arg->v)[0]);
		perror("failed");
		exit(0);
	}

	close(to[0]);
	/* ignore sigpipe for now, in case child exists early */
	oldsigpipe = signal(SIGPIPE, SIG_IGN);
	newline = 0;
	for (n = 0; n < term.row; n++) {
		bp = TLINE(n);
		lastpos = MIN(tlinelen(n) + 1, term.col) - 1;
		if (lastpos < 0)
			break;
		end = &bp[lastpos + 1];
		for (; bp < end; ++bp)
			if (xwrite(to[1], buf, utf8encode(bp->u, buf)) < 0)
				break;
		if ((newline = TLINE(n)[lastpos].mode & ATTR_WRAP))
			continue;
		if (xwrite(to[1], "\n", 1) < 0)
			break;
		newline = 0;
	}
	if (newline)
		(void)xwrite(to[1], "\n", 1);
	close(to[1]);
	/* restore */
	signal(SIGPIPE, oldsigpipe);
}

void
strdump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", strescseq.type);
	for (i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if (c == '\0') {
			putc('\n', stderr);
			return;
		} else if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void
strreset(void)
{
	strescseq = (STREscape){
		.buf = xrealloc(strescseq.buf, STR_BUF_SIZ),
		.siz = STR_BUF_SIZ,
	};
}

void
sendbreak(const Arg *arg)
{
	if (tcsendbreak(cmdfd, 0))
		perror("Error sending break");
}

void
tprinter(char *s, size_t len)
{
	if (iofd != -1 && xwrite(iofd, s, len) < 0) {
		perror("Error writing to output file");
		close(iofd);
		iofd = -1;
	}
}

void
toggleprinter(const Arg *arg)
{
	term.mode ^= MODE_PRINT;
}

void
printscreen(const Arg *arg)
{
	tdump();
}

void
printsel(const Arg *arg)
{
	tdumpsel();
}

void
tdumpsel(void)
{
	char *ptr;

	if ((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(int n)
{
	char buf[UTF_SIZ];
	const Glyph *bp, *end;

	bp = &TLINE(n)[0];
	end = &bp[MIN(tlinelen(n), term.col) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ; bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void
tdump(void)
{
	int i;

	for (i = 0; i < term.row; ++i)
		tdumpline(i);
}

void
tputtab(int n)
{
	uint x = term.c.x;

	if (n > 0) {
		while (x < term.col && n--)
			for (++x; x < term.col && !term.tabs[x]; ++x)
				/* nothing */ ;
	} else if (n < 0) {
		while (x > 0 && n++)
			for (--x; x > 0 && !term.tabs[x]; --x)
				/* nothing */ ;
	}
	term.c.x = LIMIT(x, 0, term.col-1);
}

void
tdefutf8(char ascii)
{
	if (ascii == 'G')
		term.mode |= MODE_UTF8;
	else if (ascii == '@')
		term.mode &= ~MODE_UTF8;
}

void
tdeftran(char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

void
tdectest(char c)
{
	int x, y;

	if (c == '8') { /* DEC screen alignment test. */
		for (x = 0; x < term.col; ++x) {
			for (y = 0; y < term.row; ++y)
				tsetchar('E', &term.c.attr, x, y);
		}
	}
}

void
tstrsequence(uchar c)
{
	switch (c) {
	case 0x90:   /* DCS -- Device Control String */
		c = 'P';
		break;
	case 0x9f:   /* APC -- Application Program Command */
		c = '_';
		break;
	case 0x9e:   /* PM -- Privacy Message */
		c = '^';
		break;
	case 0x9d:   /* OSC -- Operating System Command */
		c = ']';
		break;
	}
	strreset();
	strescseq.type = c;
	term.esc |= ESC_STR;
}

void
tcontrolcode(uchar ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(1);
		return;
	case '\b':   /* BS */
		tmoveto(term.c.x-1, term.c.y);
		return;
	case '\r':   /* CR */
		tmoveto(0, term.c.y);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a':   /* BEL */
		if (term.esc & ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle();
		} else {
			xbell();
		}
		break;
	case '\033': /* ESC */
		csireset();
		term.esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		term.charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar('?', &term.c.attr, term.c.x, term.c.y);
		/* FALLTHROUGH */
	case '\030': /* CAN */
		csireset();
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x80:   /* TODO: PAD */
	case 0x81:   /* TODO: HOP */
	case 0x82:   /* TODO: BPH */
	case 0x83:   /* TODO: NBH */
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 0x86:   /* TODO: SSA */
	case 0x87:   /* TODO: ESA */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 0x89:   /* TODO: HTJ */
	case 0x8a:   /* TODO: VTS */
	case 0x8b:   /* TODO: PLD */
	case 0x8c:   /* TODO: PLU */
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x91:   /* TODO: PU1 */
	case 0x92:   /* TODO: PU2 */
	case 0x93:   /* TODO: STS */
	case 0x94:   /* TODO: CCH */
	case 0x95:   /* TODO: MW */
	case 0x96:   /* TODO: SPA */
	case 0x97:   /* TODO: EPA */
	case 0x98:   /* TODO: SOS */
	case 0x99:   /* TODO: SGCI */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9d:   /* OSC -- Operating System Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9f:   /* APC -- Application Program Command */
		tstrsequence(ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	term.esc &= ~(ESC_STR_END|ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(uchar ascii)
{
	switch (ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case '%':
		term.esc |= ESC_UTF8;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		term.charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed */
		if (term.c.y == term.bot) {
			tscrollup(term.top, 1);
		} else {
			tmoveto(term.c.x, term.c.y+1);
		}
		break;
	case 'E': /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		if (term.c.y == term.top) {
			tscrolldown(term.top, 1);
		} else {
			tmoveto(term.c.x, term.c.y-1);
		}
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'c': /* RIS -- Reset to initial state */
		treset();
		xfreetitlestack();
		resettitle();
		xloadcols();
		xsetmode(0, MODE_HIDE);
		break;
	case '=': /* DECPAM -- Application keypad */
		xsetmode(1, MODE_APPKEYPAD);
		break;
	case '>': /* DECPNM -- Normal keypad */
		xsetmode(0, MODE_APPKEYPAD);
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if (term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(MODE_UTF8)) {
		c[0] = u;
		width = len = 1;
	} else {
		len = utf8encode(u, c);
		if (!control && (width = wcwidth(u)) == -1)
			width = 1;
	}

	if (IS_SET(MODE_PRINT))
		tprinter(c, len);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if (term.esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			term.esc &= ~(ESC_START|ESC_STR);
			term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (strescseq.len+len >= strescseq.siz) {
			/*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
			/*
			 * term.esc = 0;
			 * strhandle();
			 */
			if (strescseq.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			strescseq.siz *= 2;
			strescseq.buf = xrealloc(strescseq.buf, strescseq.siz);
		}

		memmove(&strescseq.buf[strescseq.len], c, len);
		strescseq.len += len;
		return;
	}

check_control_code:
	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		/* in UTF-8 mode ignore handling C1 control characters */
		if (IS_SET(MODE_UTF8) && ISCONTROLC1(u))
			return;
		tcontrolcode(u);
		/*
		 * control codes are not shown ever
		 */
		if (!term.esc)
			term.lastc = 0;
		return;
	} else if (term.esc & ESC_START) {
		if (term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (term.esc & ESC_UTF8) {
			tdefutf8(u);
		} else if (term.esc & ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (term.esc & ESC_TEST) {
			tdectest(u);
		} else {
			if (!eschandle(u))
				return;
			/* sequence already finished */
		}
		term.esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	if (selected(term.c.x, term.c.y))
		selclear();

	// wcwidth is broken on some systems, set the width to 0 if it's a known
	// diacritic used for images.
	uint16_t num = diacritic_to_num(u);
	if (num != 0)
		width = 0;
	// Set the width to 1 if it's an image placeholder character.
	if (u == IMAGE_PLACEHOLDER_CHAR || u == IMAGE_PLACEHOLDER_CHAR_OLD)
		width = 1;

	if (width == 0) {
		// It's probably a combining char. Combining characters are not
		// supported, so we just ignore them, unless it denotes the row and
		// column of an image character.
		if (term.c.y <= 0 && term.c.x <= 0)
			return;
		else if (term.c.x == 0)
			gp = &TLINE(term.c.y-1)[term.col-1];
		else if (term.c.state & CURSOR_WRAPNEXT)
			gp = &TLINE(term.c.y)[term.c.x];
		else
			gp = &TLINE(term.c.y)[term.c.x-1];
		if (num && (gp->mode & ATTR_IMAGE)) {
			unsigned diaccount = tgetimgdiacriticcount(gp);
			if (diaccount == 0)
				tsetimgrow(gp, num);
			else if (diaccount == 1)
				tsetimgcol(gp, num);
			else if (diaccount == 2)
				tsetimg4thbyteplus1(gp, num);
			tsetimgdiacriticcount(gp, diaccount + 1);
		}
		term.lastc = u;
		return;
	}

	gp = &TLINE(term.c.y)[term.c.x];
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &TLINE(term.c.y)[term.c.x];
	}

	if (IS_SET(MODE_INSERT) && term.c.x+width < term.col) {
		memmove(gp+width, gp, (term.col - term.c.x - width) * sizeof(Glyph));
		gp->mode &= ~ATTR_WIDE;
	}

	if (term.c.x+width > term.col) {
		if (IS_SET(MODE_WRAP))
			tnewline(1);
		else
			tmoveto(term.col - width, term.c.y);
		gp = &TLINE(term.c.y)[term.c.x];
	}

	tsetchar(u, &term.c.attr, term.c.x, term.c.y);
	term.lastc = u;

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		if (term.c.x+1 < term.col) {
			if (gp[1].mode == ATTR_WIDE && term.c.x+2 < term.col) {
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term.c.x+width < term.col) {
		tmoveto(term.c.x+width, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

int
twrite(const char *buf, int buflen, int show_ctrl)
{
	int charsize;
	Rune u;
	int n;

	int su0 = su;
	twrite_aborted = 0;

	/* Local "no auto-snap" fix: writes target live area, view stays
	 * anchored. While off>0, every TLINE access subtracts off, so writing
	 * through the cursor would clobber visible scrollback. Zero off for
	 * the duration of this call, then re-anchor by the cur-delta produced
	 * by any tscrollup() inside. */
	int saved_off = TSCREEN.off;
	int saved_cur = TSCREEN.cur;
	if (saved_off > 0)
		TSCREEN.off = 0;

	for (n = 0; n < buflen; n += charsize) {
		if (IS_SET(MODE_UTF8)) {
			/* process a complete utf8 char */
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		} else {
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		if (su0 && !su) {
			twrite_aborted = 1;
			break;  // ESU - allow rendering before a new BSU
		}
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) {
				u &= 0x7f;
				tputc('^');
				tputc('[');
			} else if (u != '\n' && u != '\r' && u != '\t') {
				u ^= 0x40;
				tputc('^');
			}
		}
		tputc(u);
	}

	if (saved_off > 0) {
		int cur_delta = (TSCREEN.cur - saved_cur + TSCREEN.size) % TSCREEN.size;
		TSCREEN.off = saved_off + cur_delta;
		if (TSCREEN.off > TSCREEN.size - term.row)
			TSCREEN.off = TSCREEN.size - term.row;
		tfulldirt();
	}
	return n;
}

void
clearline(Line line, Glyph g, int x, int xend)
{
	int i;
	g.mode = 0;
	g.u = ' ';
	for (i = x; i < xend; ++i) {
		line[i] = g;
	}
}

Line
ensureline(Line line)
{
	if (!line) {
		line = xmalloc(term.linelen * sizeof(Glyph));
	}
	return line;
}

/*
 * Reflow scratch buffers (reused across resizes; single-threaded).
 * rfnb collects the rewrapped line pointers; lbuf accumulates one logical
 * line (a run of physical lines joined by ATTR_WRAP) before re-emission.
 */
static Line *rfnb;
static int rfnl, rfnlcap;
static Glyph *lbuf;
static int lbufcap;

/*
 * Re-emit one logical line (lb[0..ll)) into rfnb as physical lines of width
 * `col`, inserting a soft-wrap (ATTR_WRAP on the last cell) wherever content
 * spills past `col`. Wide glyphs keep their dummy; an over-wide glyph in a
 * 1-column terminal is degraded to narrow so we always make progress. If the
 * cursor's logical offset (cur_logical, or ll for end-of-line) is emitted,
 * its new (rfnb index, column) is reported via *cabs/*cnx.
 */
static void
reflow_emit(const Glyph *lb, int ll, int col, int linelen, Glyph blank,
            int cur_logical, int *cabs, int *cnx)
{
	int idx = 0;

	do {
		Line line = xmalloc(linelen * sizeof(Glyph));
		int ecol = 0;

		while (idx < ll && ecol < col) {
			Glyph g = lb[idx];
			int w;

			if (g.mode & ATTR_WDUMMY) { idx++; continue; }
			w = (g.mode & ATTR_WIDE) ? 2 : 1;
			if (ecol + w > col) {
				if (ecol == 0) {
					/* glyph wider than the whole line */
					g.mode &= ~ATTR_WIDE;
					if (idx == cur_logical) { *cabs = rfnl; *cnx = 0; }
					line[0] = g;
					ecol = 1;
					idx++;
				}
				break;
			}
			if (idx == cur_logical) { *cabs = rfnl; *cnx = ecol; }
			line[ecol] = g;
			if (w == 2) {
				line[ecol + 1] = g;
				line[ecol + 1].u = '\0';
				line[ecol + 1].mode = ATTR_WDUMMY;
			}
			ecol += w;
			idx++;
		}
		if (cur_logical == ll && idx >= ll) { *cabs = rfnl; *cnx = ecol; }

		clearline(line, blank, ecol, linelen);
		if (idx < ll && ecol > 0)
			line[ecol - 1].mode |= ATTR_WRAP;

		if (rfnl >= rfnlcap) {
			rfnlcap = rfnlcap ? rfnlcap * 2 : 256;
			rfnb = xrealloc(rfnb, rfnlcap * sizeof(Line));
		}
		rfnb[rfnl++] = line;
	} while (idx < ll);
}

void
tresize(int col, int row)
{
	int i;
	int minrow = MIN(row, term.row);
	int linelen = MAX(col, term.linelen);
	int *bp;
	LineBuffer *sb = &term.screen[0];
	int alt = IS_SET(MODE_ALTSCREEN);
	int reflow_cursor = !alt;
	Glyph blank;

	if (col < 1 || row < 1 || row > HISTSIZE) {
		fprintf(stderr,
		        "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	/* Reflow renumbers rows, so any selection is invalidated. Skip on the
	 * first resize from tnew, when term.dirty/term.row aren't set up yet. */
	if (term.col > 0)
		selclear();

	blank = term.c.attr;
	blank.mode = 0;
	blank.u = ' ';
	blank.hlink = 0;

	/* ---- Reflow the main screen (history + active) into a fresh ring ---- */
#define S0(yy) (sb->buffer[(((yy) + sb->cur) % sb->size + sb->size) % sb->size])
	rfnl = 0;
	{
		int cabs = -1, cnx = 0;

		if (term.col > 0) {  /* skip on the first tresize from tnew */
			int histlen = 0, bottomrow, ll = 0, y;

			/* count history lines above the live screen */
			while (histlen < sb->size - term.row && S0(-histlen - 1))
				histlen++;

			/* lowest live row that carries content (or the cursor) */
			bottomrow = reflow_cursor ? term.c.y : 0;
			for (y = term.row - 1; y >= 0; y--) {
				Line ln = S0(y);
				int wrapped = (ln[term.col-1].mode & ATTR_WRAP) ||
				    ((ln[term.col-1].mode & ATTR_WDUMMY) && term.col >= 2 &&
				     (ln[term.col-2].mode & ATTR_WRAP));
				int len = term.col;
				if (!wrapped)
					while (len > 0 && ln[len-1].u == ' ' &&
					       !(ln[len-1].mode & (ATTR_WIDE|ATTR_IMAGE)))
						len--;
				if (len > 0) { bottomrow = MAX(bottomrow, y); break; }
			}

			/* walk source rows oldest..bottom, build and emit logical lines */
			for (y = -histlen; y <= bottomrow; y++) {
				Line ln = S0(y);
				int wrapped = (ln[term.col-1].mode & ATTR_WRAP) ||
				    ((ln[term.col-1].mode & ATTR_WDUMMY) && term.col >= 2 &&
				     (ln[term.col-2].mode & ATTR_WRAP));
				int len = term.col;

				if (!wrapped)
					while (len > 0 && ln[len-1].u == ' ' &&
					       !(ln[len-1].mode & (ATTR_WIDE|ATTR_IMAGE)))
						len--;

				if (reflow_cursor && y == term.c.y) {
					int cx = MIN(term.c.x, term.col);
					if (cx > len)   /* keep cells up to the cursor */
						len = cx;
					cabs = -2;      /* stash logical offset in cnx */
					cnx = ll + cx;
				}

				if (ll + len > lbufcap) {
					lbufcap = MAX(ll + len, lbufcap ? lbufcap * 2 : 256);
					lbuf = xrealloc(lbuf, lbufcap * sizeof(Glyph));
				}
				for (i = 0; i < len; i++) {
					Glyph g = ln[i];
					if (g.mode & ATTR_IMAGE)   /* drop image placement */
						g = blank;
					g.mode &= ~ATTR_WRAP;      /* wrap is recomputed on emit */
					lbuf[ll++] = g;
				}

				if (wrapped)
					continue;

				/* logical line complete: emit it */
				{
					int cur_logical = (cabs == -2) ? cnx : -1;
					int rcabs = -1, rcnx = 0;
					reflow_emit(lbuf, ll, col, linelen, blank,
					            cur_logical, &rcabs, &rcnx);
					if (cabs == -2) { cabs = rcabs; cnx = rcnx; }
				}
				ll = 0;
			}
			/* trailing unterminated wrap (rare) */
			if (ll > 0) {
				int cur_logical = (cabs == -2) ? cnx : -1;
				int rcabs = -1, rcnx = 0;
				reflow_emit(lbuf, ll, col, linelen, blank,
				            cur_logical, &rcabs, &rcnx);
				if (cabs == -2) { cabs = rcabs; cnx = rcnx; }
			}
		}

		/* free old main-screen lines now that content is copied out */
		for (i = 0; i < sb->size; i++) {
			free(sb->buffer[i]);
			sb->buffer[i] = NULL;
		}

		/* pad the bottom so the live screen is full */
		while (rfnl < row) {
			Line line = xmalloc(linelen * sizeof(Glyph));
			clearline(line, blank, 0, linelen);
			if (rfnl >= rfnlcap) {
				rfnlcap = rfnlcap ? rfnlcap * 2 : 256;
				rfnb = xrealloc(rfnb, rfnlcap * sizeof(Line));
			}
			rfnb[rfnl++] = line;
		}

		/* keep only the newest `size` lines */
		if (rfnl > sb->size) {
			int drop = rfnl - sb->size;
			for (i = 0; i < drop; i++)
				free(rfnb[i]);
			memmove(rfnb, rfnb + drop, (rfnl - drop) * sizeof(Line));
			rfnl -= drop;
			if (cabs >= 0) cabs -= drop;
		}

		/* install into the ring; active screen = bottom `row` lines */
		for (i = 0; i < rfnl; i++)
			sb->buffer[i] = rfnb[i];
		sb->cur = rfnl - row;
		sb->off = 0;
		rfnl = 0;

		if (reflow_cursor) {
			if (cabs < 0) {
				term.c.x = 0;
				term.c.y = row - 1;
			} else {
				term.c.y = cabs - sb->cur;
				term.c.x = cnx;
			}
			term.c.state &= ~CURSOR_WRAPNEXT;
		}
		sb->sc.x = MIN(sb->sc.x, col - 1);
		sb->sc.y = MIN(sb->sc.y, row - 1);
	}
#undef S0

	/* ---- Alt screen: clip/pad, never reflow ---- */
	if (linelen > term.linelen) {
		for (i = 0; i < minrow; i++) {
			term.screen[1].buffer[i] = xrealloc(term.screen[1].buffer[i], linelen * sizeof(Glyph));
			clearline(term.screen[1].buffer[i], blank, term.linelen, linelen);
		}
	}
	for (i = row; i < term.row; i++)
		free(term.screen[1].buffer[i]);
	term.screen[1].buffer = xrealloc(term.screen[1].buffer, row * sizeof(Line));
	term.screen[1].cur = 0;
	term.screen[1].off = 0;
	term.screen[1].size = row;
	for (i = term.row; i < row; i++) {
		term.screen[1].buffer[i] = xmalloc(linelen * sizeof(Glyph));
		clearline(term.screen[1].buffer[i], blank, 0, linelen);
	}

	/* resize to new height */
	term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

	/* fix tabstops */
	if (col > term.col) {
		bp = term.tabs + term.col;

		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while (--bp > term.tabs && !*bp)
			/* nothing */ ;
		for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}

	/* update terminal size */
	term.col = col;
	term.row = row;
	term.linelen = linelen;
	/* reset scrolling region */
	tsetscroll(0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term.c.x, term.c.y);
	tfulldirt();
}

void
resettitle(void)
{
	xsettitle(NULL, 0);
}

void
drawregion(int x1, int y1, int x2, int y2)
{
	int y, L;

	xstartimagedraw(term.dirty, term.row);

	L = TLINEOFFSET(y1);
	for (y = y1; y < y2; y++) {
		if (term.dirty[y]) {
			term.dirty[y] = 0;
			unhighlighturlsline(y);
			highlighturlsline(y);
			xdrawline(TSCREEN.buffer[L], x1, y, x2);
		}
		L = (L + 1) % TSCREEN.size;
	}

	xfinishimagedraw();
}

void
draw(void)
{
	int cx = term.c.x, ocx = term.ocx, ocy = term.ocy;

	if (!xstartdraw())
		return;

	/* adjust cursor position */
	LIMIT(term.ocx, 0, term.col-1);
	LIMIT(term.ocy, 0, term.row-1);
	if (TLINE(term.ocy)[term.ocx].mode & ATTR_WDUMMY)
		term.ocx--;
	if (TLINE(term.c.y)[cx].mode & ATTR_WDUMMY)
		cx--;

	drawregion(0, 0, term.col, term.row);
	if (TSCREEN.off == 0)
		xdrawcursor(cx, term.c.y, TLINE(term.c.y)[cx],
				term.ocx, term.ocy, TLINE(term.ocy)[term.ocx],
				TLINE(term.ocy), term.col);
	term.ocx = cx;
	term.ocy = term.c.y;
	xfinishdraw();
	if (ocx != term.ocx || ocy != term.ocy)
		xximspot(term.ocx, term.ocy);
}

void
redraw(void)
{
	tfulldirt();
	draw();
}

Glyph
getglyphat(int col, int row)
{
	return TLINE(row)[col];
}
