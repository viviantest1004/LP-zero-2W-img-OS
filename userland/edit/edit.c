/* edit - a full-screen text editor.
 *
 *   edit <file>        open it for editing, creating it if it is not there
 *
 * Keys:
 *   arrows          move           Home/End       start/end of line
 *   PgUp/PgDn       one screen     Ctrl-Home/End  start/end of file
 *   Backspace/Del   erase          Enter          split the line
 *   Ctrl-S          save           Ctrl-Q/Ctrl-C  quit
 *   Ctrl-K          delete line    Ctrl-G         help
 *
 * Why this exists:
 *   There was no editor, so changing a file meant retyping the whole
 *   thing through 'cat > file'. You could not even fix one line of
 *   configuration over SSH.
 *
 * Drawing:
 *   The serial console runs at 115200 baud. Redrawing all of 80x24 is
 *   1920 bytes, about 170ms, and typing visibly lags. So we remember
 *   what changed and draw only that much:
 *     DIRTY_NONE  just move the cursor (arrow keys)
 *     DIRTY_LINE  redraw that one line (typing)
 *     DIRTY_ALL   everything (scrolling, adding or removing lines)
 *   It also goes out in one write. Writing in pieces makes it flicker.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

#define MAX_LINES     20000
#define TAB_WIDTH     4
#define OUT_CAP       65536

typedef struct {
    char  *text;      /* NUL terminated */
    size_t len;
    size_t cap;
} line_t;

static line_t lines[MAX_LINES];
static int    nlines   = 0;

static int  cy = 0, cx = 0;        /* cursor: line, and byte within it */
static int  top = 0, left = 0;     /* what the top-left of the screen shows */
static int  rows = 24, cols = 80;  /* terminal size; the text gets rows-2 */
static bool modified = false;
static bool running  = true;

static const char *filename = NULL;
static char        status[256];

typedef enum { DIRTY_NONE = 0, DIRTY_LINE, DIRTY_ALL } dirty_t;
static dirty_t dirty = DIRTY_ALL;

static lp_termios_t saved_term;

/* ── Output buffer ───────────────────────────────────────────────
 * Everything is gathered here so it leaves in a single write. */
static char   outbuf[OUT_CAP];
static size_t outlen = 0;

static void out(const char *s, size_t n)
{
    if (outlen + n > sizeof(outbuf))
        n = sizeof(outbuf) - outlen;
    memcpy(outbuf + outlen, s, n);
    outlen += n;
}
static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, int a, int b)
{
    char buf[64];
    int  n = snprintf(buf, sizeof(buf), fmt, a, b);
    if (n > 0)
        out(buf, (size_t)n);
}

static void flush_out(void)
{
    size_t off = 0;
    while (off < outlen) {
        long w = lp_write(STDOUT_FILENO, outbuf + off, outlen - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    outlen = 0;
}

/* ── Managing lines ────────────────────────────────────────────── */

static bool line_reserve(line_t *l, size_t need)
{
    if (l->cap >= need + 1)
        return true;
    size_t cap = l->cap ? l->cap : 64;
    while (cap < need + 1)
        cap *= 2;
    char *p = realloc(l->text, cap);
    if (!p)
        return false;
    l->text = p;
    l->cap  = cap;
    return true;
}

/* Insert an empty line at idx. true on success. */
static bool insert_line(int idx)
{
    if (nlines >= MAX_LINES)
        return false;
    for (int i = nlines; i > idx; i--)
        lines[i] = lines[i - 1];
    lines[idx].text = NULL;
    lines[idx].len  = 0;
    lines[idx].cap  = 0;
    if (!line_reserve(&lines[idx], 0))
        return false;
    lines[idx].text[0] = '\0';
    nlines++;
    return true;
}

static void delete_line(int idx)
{
    free(lines[idx].text);
    for (int i = idx; i < nlines - 1; i++)
        lines[i] = lines[i + 1];
    nlines--;
    if (nlines == 0) {
        insert_line(0);
    }
}

/* ── Files ─────────────────────────────────────────────────────── */

static void set_status(const char *s) { strlcpy(status, s, sizeof(status)); }

static bool load_file(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0) {
        /* Missing? Start empty - we are creating it. */
        insert_line(0);
        set_status("new file");
        return true;
    }

    if (!insert_line(0)) { lp_close((int)fd); return false; }

    char buf[8192];
    for (;;) {
        long n = lp_read((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (!insert_line(nlines)) { lp_close((int)fd); return false; }
                continue;
            }
            if (ch == '\r')
                continue;                   /* read CRLF files too */
            line_t *l = &lines[nlines - 1];
            if (!line_reserve(l, l->len + 1)) { lp_close((int)fd); return false; }
            l->text[l->len++] = ch;
            l->text[l->len]   = '\0';
        }
    }
    lp_close((int)fd);

    /* A file ending in a newline leaves one empty line here. That is right. */
    char msg[128];
    snprintf(msg, sizeof(msg), "%d lines", nlines);
    set_status(msg);
    return true;
}

static bool save_file(void)
{
    /* Write to a temporary file and rename it over. A failure part way
     * through then leaves the original intact. */
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", filename) >= (int)sizeof(tmp)) {
        set_status("path too long");
        return false;
    }

    long fd = lp_open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "save failed (%d)", (int)-fd);
        set_status(msg);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < nlines && ok; i++) {
        size_t off = 0;
        while (off < lines[i].len) {
            long w = lp_write((int)fd, lines[i].text + off, lines[i].len - off);
            if (w <= 0) { ok = false; break; }
            off += (size_t)w;
        }
        /* No newline after the last line, so the line count is the same
         * when we read it back. */
        if (ok && i < nlines - 1) {
            if (lp_write((int)fd, "\n", 1) != 1)
                ok = false;
        }
    }
    lp_close((int)fd);

    if (!ok) {
        lp_unlink(tmp);
        set_status("write error - the original is untouched");
        return false;
    }

    long r = lp_rename(tmp, filename);
    if (r < 0) {
        lp_unlink(tmp);
        char msg[160];
        snprintf(msg, sizeof(msg), "rename failed (%d)", (int)-r);
        set_status(msg);
        return false;
    }

    lp_sync();          /* the power can go at any moment on an SD card */
    modified = false;
    char msg[160];
    snprintf(msg, sizeof(msg), "saved %s (%d lines)", filename, nlines);
    set_status(msg);
    return true;
}

/* ── The screen ────────────────────────────────────────────────── */

/* The screen column the cursor sits at.
 *
 * cx is a byte offset; this is a screen column. They differ: a tab is
 * several columns, and one Hangul character is 3 bytes and 2 columns. */
static int screen_col(void)
{
    const line_t *l = &lines[cy];
    int col = 0;
    for (int i = 0; i < cx && i < (int)l->len; ) {
        if (l->text[i] == '\t') {
            col = (col / TAB_WIDTH + 1) * TAB_WIDTH;
            i++;
            continue;
        }
        int used;
        u32 cp = utf8_decode(l->text + i, l->len - (size_t)i, &used);
        if (used == 0) break;
        col += utf8_width(cp);
        i   += used;
    }
    return col;
}

/* Draw one line of text, expanding tabs and honouring horizontal scroll. */
static void draw_line(int idx, int screen_row)
{
    outf("\x1b[%d;%dH", screen_row + 1, 1);
    outs("\x1b[K");                        /* clear to end of line */

    if (idx >= nlines)
        return;

    /* Draw only what falls in screen columns [left, left+cols).
     *
     * Cutting by bytes would be wrong: Hangul is 3 bytes and 2 columns, so
     * a byte cut lands mid-character, prints a broken glyph and throws the
     * line length off. Count columns and copy whole characters. */
    const line_t *l = &lines[idx];
    char   render[2048];
    size_t r    = 0;
    int    col  = 0;

    for (size_t i = 0; i < l->len && r < sizeof(render) - 8; ) {
        if (l->text[i] == '\t') {
            int stop = (col / TAB_WIDTH + 1) * TAB_WIDTH;
            while (col < stop) {
                if (col >= left && col < left + cols)
                    render[r++] = ' ';
                col++;
            }
            i++;
            continue;
        }

        int used;
        u32 cp = utf8_decode(l->text + i, l->len - (size_t)i, &used);
        if (used == 0)
            break;

        int w = utf8_width(cp);

        if ((unsigned char)l->text[i] < 32) {
            /* control characters just take up their space */
            if (col >= left && col < left + cols)
                render[r++] = '?';
            col++;
            i += used;
            continue;
        }

        /* Stop if the character would cross the right edge: half of one is garbage. */
        if (col + w > left + cols)
            break;

        if (col >= left) {
            memcpy(render + r, l->text + i, (size_t)used);
            r += (size_t)used;
        } else if (col + w > left) {
            /* A wide character straddling the left edge. We cannot draw half
             * of it, so a space holds the column. */
            render[r++] = ' ';
        }

        col += w;
        i   += used;
    }

    out(render, r);
}

static void draw_status(void)
{
    outf("\x1b[%d;%dH", rows - 1, 1);
    outs("\x1b[K\x1b[7m");                 /* reverse video */

    char bar[256];
    int n = snprintf(bar, sizeof(bar), " %s%s  line %d/%d  col %d ",
                     filename, modified ? " *" : "",
                     cy + 1, nlines, screen_col() + 1);
    if (n > cols) n = cols;
    out(bar, (size_t)n);
    for (int i = n; i < cols; i++) outs(" ");
    outs("\x1b[0m");

    outf("\x1b[%d;%dH", rows, 1);
    outs("\x1b[K");
    if (status[0]) {
        outs(status);
    } else {
        outs("Ctrl-S save   Ctrl-Q quit   Ctrl-G help");
    }
}

static void place_cursor(void)
{
    outf("\x1b[%d;%dH", cy - top + 1, screen_col() - left + 1);
}

/* Scroll the view to follow the cursor if it left the screen.
 * Returns true if it moved, which means a full redraw. */
static bool scroll_to_cursor(void)
{
    int body = rows - 2;
    int before_top = top, before_left = left;

    if (cy < top)            top = cy;
    if (cy >= top + body)    top = cy - body + 1;

    int sc = screen_col();
    if (sc < left)           left = sc;
    if (sc >= left + cols)   left = sc - cols + 1;

    return top != before_top || left != before_left;
}

static void refresh(void)
{
    if (scroll_to_cursor())
        dirty = DIRTY_ALL;

    outs("\x1b[?25l");                     /* hide the cursor while drawing */

    if (dirty == DIRTY_ALL) {
        int body = rows - 2;
        for (int i = 0; i < body; i++)
            draw_line(top + i, i);
        draw_status();
    } else if (dirty == DIRTY_LINE) {
        draw_line(cy, cy - top);
        draw_status();
    } else {
        draw_status();                     /* the cursor position changed */
    }

    place_cursor();
    outs("\x1b[?25h");
    flush_out();
    dirty = DIRTY_NONE;
}

/* ── Key input ───────────────────────────────────────────────────
 * Arrow keys arrive as several bytes, like ESC [ A. We read the whole
 * sequence and turn it into one number of our own. */
enum {
    KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL,
    KEY_FILE_TOP, KEY_FILE_END
};

static int read_key(void)
{
    char c;
    long n;
    do {
        n = lp_read(STDIN_FILENO, &c, 1);
    } while (n == 0);
    if (n < 0)
        return -1;

    if (c != 0x1b)
        return (unsigned char)c;

    /* A sequence starting with ESC. With nothing after it, it is the ESC key. */
    char seq[8];
    if (lp_read(STDIN_FILENO, &seq[0], 1) != 1) return 0x1b;
    if (lp_read(STDIN_FILENO, &seq[1], 1) != 1) return 0x1b;

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            /* ESC [ n ~   or   ESC [ 1 ; 5 H (Ctrl-Home) */
            if (lp_read(STDIN_FILENO, &seq[2], 1) != 1) return 0x1b;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1': case '7': return KEY_HOME;
                case '3':           return KEY_DEL;
                case '4': case '8': return KEY_END;
                case '5':           return KEY_PGUP;
                case '6':           return KEY_PGDN;
                }
                return 0x1b;
            }
            if (seq[2] == ';') {
                char mod, fin;
                if (lp_read(STDIN_FILENO, &mod, 1) != 1) return 0x1b;
                if (lp_read(STDIN_FILENO, &fin, 1) != 1) return 0x1b;
                if (mod == '5') {          /* Ctrl */
                    if (fin == 'H') return KEY_FILE_TOP;
                    if (fin == 'F') return KEY_FILE_END;
                }
                return 0x1b;
            }
            return 0x1b;
        }
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
        return 0x1b;
    }
    if (seq[0] == 'O') {                   /* vt100 application mode */
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
    }
    return 0x1b;
}

/* ── Editing ───────────────────────────────────────────────────── */

/* Clamp cx into the line, and onto a character boundary. Moving up or
 * down keeps the byte offset, which can land mid-character. */
static void clamp_cx(void)
{
    const line_t *l = &lines[cy];
    if (cx > (int)l->len) cx = (int)l->len;
    if (cx < 0)           cx = 0;

    /* On a continuation byte? Step back to the start of the character. */
    if (cx > 0 && cx < (int)l->len &&
        ((unsigned char)l->text[cx] & 0xC0) == 0x80)
        cx = (int)utf8_prev(l->text, (size_t)cx);
}

static void insert_char(char ch)
{
    line_t *l = &lines[cy];
    if (!line_reserve(l, l->len + 1)) {
        set_status("out of memory");
        return;
    }
    memmove(l->text + cx + 1, l->text + cx, l->len - (size_t)cx + 1);
    l->text[cx] = ch;
    l->len++;
    cx++;
    modified = true;
    dirty = DIRTY_LINE;
}

static void split_line(void)
{
    if (!insert_line(cy + 1)) {
        set_status("too many lines");
        return;
    }
    line_t *cur  = &lines[cy];
    line_t *next = &lines[cy + 1];
    size_t  tail = cur->len - (size_t)cx;

    if (!line_reserve(next, tail)) {
        set_status("out of memory");
        return;
    }
    memcpy(next->text, cur->text + cx, tail);
    next->text[tail] = '\0';
    next->len = tail;

    cur->len = (size_t)cx;
    cur->text[cx] = '\0';

    cy++;
    cx = 0;
    modified = true;
    dirty = DIRTY_ALL;
}

/* Erase the character before the cursor. At the start of a line, join
 * it onto the line above. */
static void backspace(void)
{
    if (cx > 0) {
        line_t *l = &lines[cy];
        /* Erase a character, not a byte. Erasing one byte of Hangul
         * would leave the other two behind as a broken character. */
        int start = (int)utf8_prev(l->text, (size_t)cx);
        int n     = cx - start;
        memmove(l->text + start, l->text + cx, l->len - (size_t)cx + 1);
        l->len -= (size_t)n;
        cx = start;
        modified = true;
        dirty = DIRTY_LINE;
        return;
    }
    if (cy == 0)
        return;

    line_t *prev = &lines[cy - 1];
    line_t *cur  = &lines[cy];
    size_t  at   = prev->len;

    if (!line_reserve(prev, prev->len + cur->len)) {
        set_status("out of memory");
        return;
    }
    memcpy(prev->text + prev->len, cur->text, cur->len + 1);
    prev->len += cur->len;

    delete_line(cy);
    cy--;
    cx = (int)at;
    modified = true;
    dirty = DIRTY_ALL;
}

static void delete_forward(void)
{
    line_t *l = &lines[cy];
    if (cx < (int)l->len) {
        int next = (int)utf8_next(l->text, l->len, (size_t)cx);
        int n    = next - cx;
        memmove(l->text + cx, l->text + next, l->len - (size_t)next + 1);
        l->len -= (size_t)n;
        modified = true;
        dirty = DIRTY_LINE;
        return;
    }
    if (cy + 1 >= nlines)
        return;
    /* Delete at end of line pulls the next line up. Move the cursor
     * and let backspace do the same work. */
    cy++;
    cx = 0;
    backspace();
}

static void show_help(void)
{
    outs("\x1b[2J\x1b[H");
    outs("edit - help\r\n\r\n");
    outs("  arrow keys       move around\r\n");
    outs("  Home / End       start / end of line\r\n");
    outs("  PgUp / PgDn      one screen at a time\r\n");
    outs("  Ctrl-Home/End    start / end of file\r\n");
    outs("  Backspace / Del  erase\r\n");
    outs("  Enter            split the line\r\n");
    outs("  Ctrl-K           delete this line\r\n");
    outs("  Ctrl-S           save\r\n");
    outs("  Ctrl-Q, Ctrl-C   quit (press twice if there are changes)\r\n\r\n");
    outs("  Saving writes a temporary file and renames it over.\r\n");
    outs("  If the write fails, the original is left untouched.\r\n\r\n");
    outs("Press any key...");
    flush_out();
    read_key();
    dirty = DIRTY_ALL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: edit <file>\n");
        return 2;
    }
    filename = argv[1];

    if (lp_is_dir(filename)) {
        dprintf(STDERR_FILENO, "edit: %s is a directory\n", filename);
        return 1;
    }

    if (!load_file(filename)) {
        dprintf(STDERR_FILENO, "edit: cannot read %s\n", filename);
        return 1;
    }

    if (lp_term_raw(STDIN_FILENO, &saved_term) < 0) {
        dprintf(STDERR_FILENO,
                "edit: cannot take control of the terminal.\n"
                "      a full-screen editor only works on a terminal.\n");
        return 1;
    }
    lp_term_size(STDOUT_FILENO, &rows, &cols);
    if (rows < 4)  rows = 4;
    if (cols < 20) cols = 20;

    outs("\x1b[2J");                       /* clear the screen */
    refresh();

    int quit_confirm = 0;

    while (running) {
        int k = read_key();
        if (k < 0)
            break;

        /* A status message clears on the next keypress. */
        if (status[0] && k != 0) {
            status[0] = '\0';
            if (dirty == DIRTY_NONE) dirty = DIRTY_NONE;   /* the status bar is always drawn */
        }

        bool was_quit_key = false;

        switch (k) {
        case KEY_UP:
            if (cy > 0) { cy--; clamp_cx(); }
            break;
        case KEY_DOWN:
            if (cy < nlines - 1) { cy++; clamp_cx(); }
            break;
        case KEY_LEFT:
            if (cx > 0)
                cx = (int)utf8_prev(lines[cy].text, (size_t)cx);
            else if (cy > 0) { cy--; cx = (int)lines[cy].len; }
            break;
        case KEY_RIGHT:
            if (cx < (int)lines[cy].len)
                cx = (int)utf8_next(lines[cy].text, lines[cy].len, (size_t)cx);
            else if (cy < nlines - 1) { cy++; cx = 0; }
            break;
        case KEY_HOME: cx = 0; break;
        case KEY_END:  cx = (int)lines[cy].len; break;
        case KEY_PGUP:
            cy -= rows - 2;
            if (cy < 0) cy = 0;
            clamp_cx();
            break;
        case KEY_PGDN:
            cy += rows - 2;
            if (cy > nlines - 1) cy = nlines - 1;
            clamp_cx();
            break;
        case KEY_FILE_TOP: cy = 0; cx = 0; break;
        case KEY_FILE_END: cy = nlines - 1; cx = (int)lines[cy].len; break;
        case KEY_DEL:      delete_forward(); break;

        case 127:                          /* Backspace */
        case 8:
            backspace();
            break;

        case '\r':                         /* Enter; ICRNL is off, so it is CR */
        case '\n':
            split_line();
            break;

        case 11:                           /* Ctrl-K: delete the line */
            delete_line(cy);
            if (cy >= nlines) cy = nlines - 1;
            clamp_cx();
            modified = true;
            dirty = DIRTY_ALL;
            break;

        case 19:                           /* Ctrl-S */
            save_file();
            break;

        case 7:                            /* Ctrl-G */
            show_help();
            break;

        case 17:                           /* Ctrl-Q */
        case 3:                            /* Ctrl-C - the habit */
            was_quit_key = true;
            if (modified && quit_confirm == 0) {
                set_status("unsaved changes. Press Ctrl-Q again to quit anyway.");
                quit_confirm = 1;
            } else {
                running = false;
            }
            break;

        case 12:                           /* Ctrl-L: redraw the screen */
            lp_term_size(STDOUT_FILENO, &rows, &cols);
            outs("\x1b[2J");
            dirty = DIRTY_ALL;
            break;

        default:
            if (k == '\t' || (k >= 32 && k < 127)) {
                insert_char((char)k);
            } else if (k >= 0xC0 && k < 0x100) {
                /* A UTF-8 lead byte. Read the rest right away and insert
                 * the character as a whole. Inserting byte by byte and
                 * redrawing between would flash a half-formed glyph. */
                int need = utf8_seq_len((unsigned char)k) - 1;
                insert_char((char)k);
                for (int i = 0; i < need; i++) {
                    char b;
                    if (lp_read(STDIN_FILENO, &b, 1) != 1)
                        break;
                    insert_char(b);
                }
            } else if (k >= 0x80 && k < 0xC0) {
                /* An orphaned continuation byte. Inserting it only makes garbage. */
            }
            break;
        }

        if (!was_quit_key)
            quit_confirm = 0;

        if (running)
            refresh();
    }

    /* Always put the terminal back, or the shell will look hung. */
    lp_term_restore(STDIN_FILENO, &saved_term);
    outs("\x1b[2J\x1b[H");
    flush_out();
    return 0;
}
