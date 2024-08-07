/*** INCLUDES ***/
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define _BSD_SOURCE

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/*** PROTOTYPES ***/
void editorRefreshScreen(void);
void editorSetStatusMessage(const char *fmt, ...);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** DEFINES ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3


enum editorKey
{
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	HOME_KEY,
	END_KEY,
	DEL_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/*** DATA ***/

typedef struct erow // Editor row
{
	int size;
	int rsize; // render size
	char *chars;
	char *render;
}
erow;


struct editorConfig
{
	int cx, cy; // x, y position of the cursor (wrt text in the file).
	int rx; // x position of the curor (actual - on rendered text)
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editorConfig E;

/*** TERMINAL ***/
void die(const char *message)
{
	write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
	write(STDOUT_FILENO, "\x1b[H", 3); // Bring cursor to top left
	perror(message);
	exit(1);
}

void disableRawMode(void)
{
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &(E.orig_termios)) == -1)
	{
		die("tcsetattr:");
	}
}

void enableRawMode(void)
{

	if(tcgetattr(STDIN_FILENO, &(E.orig_termios)) == -1)
	{
		die("tcgetattr:");
	}
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;

	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Disable Ctrl-M for \r, Ctrl-S and Ctrl-Q
	raw.c_oflag &= ~(OPOST); // Turn off output processing (\n to \r\n)
	raw.c_cflag |= (CS8); // Make sure it uses 8 bits per byte
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // Disable echo, canonical mode, CTRL-C and Ctrl-C

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
	{
		die("tcsetattr:");
	}
}

int editorReadKey(void)
{
	int nread;
	char c;

	while((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if(nread == -1 && errno != EAGAIN)
		{
			die("read:");
		}
	}

	if(c == '\x1b') // Special key
	{
		char seq[3];
		// If reads time out, we assume Esc key.
		if (read(STDIN_FILENO, seq, 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, seq + 1, 1) != 1)
			return '\x1b';

		if(seq[0] == '[') // Escape sequence
		{
			if(seq[1] >= '0' && seq[1] <= '9')
			{
				if(read(STDIN_FILENO, seq + 2, 1) != 1)
					return '\x1b';
				if (seq[2] == '~')
				{
					switch (seq[1])
					{
						case '1':
							return HOME_KEY;
						case '3':
							return DEL_KEY;
						case '4':
							return END_KEY;
						case '5':
							return PAGE_UP;
						case '6':
							return PAGE_DOWN;
						case '7':
							return HOME_KEY;
						case '8':
							return END_KEY;
					}
				}

			}
			else
			{
				switch (seq[1])
				{
					case 'A':
						return ARROW_UP;
						break;
					case 'B':
						return ARROW_DOWN;
						break;
					case 'C':
						return ARROW_RIGHT;
						break;
					case 'D':
						return ARROW_LEFT;
						break;
					case 'H':
						return HOME_KEY;
						break;
					case 'F':
						return END_KEY;
						break;
				}
			}
		}

		else if(seq[0] == 'O')
		{
			switch (seq[1])
			{
				case 'H':
					return HOME_KEY;
				case 'F':
					return END_KEY;
			}
		}



		return '\x1b';
	}
	else
	{
		return c;
	}
}

int getCursorPosition(int *row, int *col)
{

	if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) // Query cursor position
	{
		return -1;
	}

	char response[32];
	uint32_t i = 0;

	// Read answer from stdin
	for(i = 0; i < sizeof(response) - 1; i++)
	{
		if(read(STDIN_FILENO, &response[i], 1) != 1)
		{
			break;
		}
		if(response[i] == 'R')
		{
			break;
		}
	}

	response[i] = '\0';

	if (response[0] != '\x1b' || response[1] != '[')
	{
		return -1;
	}

	if(sscanf(response + 2, "%d;%d", row, col) != 2)
	{
		return -1;
	}

	return 0;
}


int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		// ioctl failed
		// Manually query the terminal size using escape sequences.

		// 999C: Try to move cursor forward 999
		// 999B: Try to move cursor down 999
		// C and B stop the cursor from leaving the screen.
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
		{
			return -1;
		}

		return getCursorPosition(rows, cols);

		return -1;
	}
	else
	{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** ROW OPERATIONS ***/
int editorRowCxToRx(erow *rowPtr, int cx)
{
	int rx = 0;
	int j;
	for(j = 0; j < cx; j++)
	{
		if(rowPtr -> chars[j] == '\t')
		{
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		}
		rx++;
	}
	return rx;
}

int editorRowRxToCx(erow *rowPtr, int rx)
{
    int curr_rx = 0;
    int cx;
    for(cx = 0; cx < rowPtr -> size; cx++)
    {
        if(rowPtr -> chars[cx] == '\t')
        {
            curr_rx += (KILO_TAB_STOP - 1) - (curr_rx % KILO_TAB_STOP);
        }
        curr_rx++;
        if(curr_rx > rx)
        {
            return cx;
        }
    }
    return cx;
}

void editorUpdateRow(erow *rowPtr)
{
	int tabs = 0;
	int j;

	for(j = 0; j < rowPtr -> size; j++)
	{
		if(rowPtr -> chars[j] == '\t')
		{
			tabs++;
		}
	}

	free(rowPtr -> render);
	rowPtr -> render = malloc(rowPtr -> size + tabs * (KILO_TAB_STOP - 1) + 1);


	int idx = 0;
	for(j = 0; j < rowPtr -> size; j++)
	{
		if(rowPtr -> chars[j] == '\t')
		{
			rowPtr -> render[idx++] = ' ';
			while(idx % KILO_TAB_STOP != 0)
			{
				rowPtr -> render[idx++] = ' ';
			}
		}
		else
		{
			rowPtr -> render[idx++] = rowPtr -> chars[j];
		}
	}
	rowPtr -> render[idx] = '\0';
	rowPtr -> rsize = idx;
}

void editorInsertRow(int idx, char *s, size_t len)
{
	if(idx < 0 || idx > E.numrows)
	{
		return;
	}

	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	memmove(&E.row[idx + 1], &E.row[idx], sizeof(erow) * (E.numrows - idx));

	E.row[idx].size = len;
	E.row[idx].chars = malloc(len + 1);
	memcpy(E.row[idx].chars, s, len);
	E.row[idx].chars[len] = '\0';

	E.row[idx].rsize = 0;
	E.row[idx].render = NULL;

	editorUpdateRow(&E.row[idx]);

	E.numrows++;
	E.dirty++;
}

void editorFreeRow(erow *row)
{
	free(row -> render);
	free(row -> chars);
}

void editorDelRow(int at)
{
	if(at < 0 || at >= E.numrows)
	{
		return;
	}
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
	if(at < 0 || at > row -> size)
	{
		at = row -> size;
	}
	// Make room for new character and null terminator
	row -> chars = realloc(row -> chars, row -> size + 2);
	// Move everything from index at one place to the right
	memmove(&row->chars[at + 1], &row->chars[at], row -> size - at + 1);
	(row -> size)++;
	row -> chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
	row -> chars = realloc(row -> chars, row -> size + len + 1);
	memcpy(row->chars + row->size, s, len);
	row -> size += len;
	row -> chars[row -> size] = '\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
	if(at < 0 || at >= row -> size)
	{
		return;
	}
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row -> size--;
	editorUpdateRow(row);
	E.dirty++;

}


/*** EDITOR OPERATIONS ***/
void editorInsertChar(int c)
{
	if(E.cy == E.numrows) // If cursor at EOF,
	{
		editorInsertRow(E.numrows, "", 0); // Add a new row
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewline(void)
{
	if(E.cx == 0)
	{
		editorInsertRow(E.cy, "", 0);
	}
	else
	{
		// Split into two rows
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void editorDelChar(void) // Backspace action
{
	if(E.cy == E.numrows) // If cursor at EOF,
	{
		return; // Do nothing.
	}
	if(E.cx == 0 && E.cy == 0) // If cursor at start
	{
		return; // Do nothing
	}
	erow *row = &E.row[E.cy];
	if(E.cx > 0)
	{
		editorRowDelChar(row, E.cx - 1);
		E.cx--;
	}
	else
	{
		E.cx = E.row[E.cy - 1].size; // Move cursor to start of previous line
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}

}

/*** FILE I/O ***/
char *editorRowsToString(int *buflen)
{
	int totlen = 0;
	for(int j = 0; j < E.numrows; j++)
	{
		totlen += E.row[j].size	+ 1;
	}

	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;

	for(int j = 0; j < E.numrows; j++)
	{
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size; // Get to the end of the line
		*p = '\n'; // set last character to \n
		p++;
	}

	return buf;
}

void editorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if(!fp)
	{
		die("fopen");
	}
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while((linelen = getline(&line, &linecap, fp)) != -1)
	{
		while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
		{
			linelen--;
		}
		editorInsertRow(E.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editorSave(void)
{
	if(E.filename == NULL)
	{
		E.filename = editorPrompt("Save as: %s", NULL);
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if(fd != -1)
	{
		if(ftruncate(fd, len) != -1) //  Resize file to required length
		{
			if(write(fd, buf, len) != -1)
			{
				close(fd);
				free(buf);
				E.dirty = 0;
				editorSetStatusMessage("%d bytes written to disk.", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorFindCallback(char *query, int key)
{
    static int direction = 1;
    static int last_match = -1;

    if(key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    else if(key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    }
    else if(key == ARROW_LEFT || key == ARROW_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    if(last_match == -1)
        direction = 1;

    int current = last_match;

    for(int i = 0; i < E.numrows; i++)
    {
        current += direction;
        if(current == -1)
            current = E.numrows - 1;
        else if(current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row -> render, query);
        if(match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row -> render);
            E.rowoff = E.numrows;
            break;
        }
    }
}

void editorFind(void)
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

  	char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if(query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.rowoff = saved_rowoff;
        E.coloff = saved_coloff;
    }
}


/*** APPEND BUFFER ***/
struct abuf
{
	char *b; // buffer
	int cap; // capacity
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab -> b, ab -> cap + len);
	if(new == NULL)
	{
		return;
	}

	memcpy(&new[ab -> cap], s, len);
	ab -> b = new;
	(ab -> cap) += len;
}

void abFree(struct abuf *ab)
{
	free(ab -> b);
}


/*** OUTPUT ***/

void editorScroll(void) // Computes row and column offsets
{
	E.rx = 0;

	if(E.cy < E.numrows)
	{
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

	if(E.cy < E.rowoff)
	{
		E.rowoff = E.cy;
	}
	if(E.cy >= E.rowoff + E.screenrows)
	{
		E.rowoff = E.cy - E.screenrows + 1;
	}

	if(E.rx < E.coloff)
	{
		E.coloff = E.rx;
	}
	if(E.rx >= E.coloff + E.screencols)
	{
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuf *ab)
{
	int y;
	for(y = 0; y < E.screenrows; y++)
	{
		int filerow = y + E.rowoff;
		if(filerow >= E.numrows)
		{
			if(E.numrows == 0 && y == E.screenrows / 3)
			{
				char welcome[80];
				int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo Editor -- Version %s", KILO_VERSION);
				if(welcomelen > E.screencols)
				{
					welcomelen = E.screencols;
				}
				// Center align welcome message
				int padding = (E.screencols - welcomelen) / 2;
				if(padding)
				{
					abAppend(ab, "~", 1);
					padding--;
				}
				while(padding--)
				{
					abAppend(ab, " ", 1);
				}
				abAppend(ab, welcome, welcomelen);
			}
			else
			{
				abAppend(ab, "~", 1);
			}
		}
		else
		{
			int len = E.row[filerow].rsize - E.coloff;
			if(len < 0)
			{
				len = 0;
			}
			if(len > E.screencols)
			{
				len = E.screencols;
			}
			abAppend(ab, E.row[filerow].render + E.coloff, len);
		}


		abAppend(ab, "\x1b[K", 3); // Clear line to the right of the cursor.

		abAppend(ab, "\r\n", 2); // Newline - make room for status area
	}
}

void editorDrawStatusBar(struct abuf *ab)
{
	abAppend(ab, "\x1b[7m", 4); // Inverted colors
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", (E.filename) ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
	if(len > E.screencols)
	{
		len = E.screencols;
	}
	abAppend(ab, status, len);
	while(len < E.screencols)
	{
		if(E.screencols - len == rlen)
		{
			abAppend(ab, rstatus, rlen);
			break;
		}
		else
		{
			abAppend(ab, " ", 1);
			len++;
		}

	}
	abAppend(ab, "\x1b[m", 3); // End inverted colors
	abAppend(ab, "\r\n", 2);

}

void editorDrawMessageBar(struct abuf *ab)
{
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.statusmsg);
	if(msglen > E.screencols)
	{
		msglen = E.screencols;
	}
	if(msglen && (time(NULL) - E.statusmsg_time < 5))
	{
		abAppend(ab, E.statusmsg, msglen);
	}
}

void editorRefreshScreen(void)
{
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); // Hide cursor when (re)painting
	// Removed: we will clear each line as we update it instead
	// abAppend(&ab, "\x1b[2J", 4); // Clear screen
	abAppend(&ab, "\x1b[H", 3); // Bring cursor to top left

	editorDrawRows(&ab);

	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	char buf[32];
	// Move cursor to position (E.rx, E.cy)
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // Show cursor

	write(STDOUT_FILENO, ab.b, ab.cap);

	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*** INPUT ***/
char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);

	size_t buflen = 0;
	buf[0] = '\0';

	while (1)
	{
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();
		if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
		    if(buflen != 0)
			{
			    buf[--buflen] = '\0';
			}
		}
		else if(c == '\x1b') // Escape key
		{
			editorSetStatusMessage("");
			if(callback)
			    callback(buf, c);
			free(buf);
			return NULL;
		}
		else if(c == '\r')
		{
			if(buflen != 0)
			{
				editorSetStatusMessage("");
				if(callback)
				    callback(buf, c);
				return buf;
			}
		}
		else if(!iscntrl(c) && c < 128)
		{
			if (buflen == bufsize - 1)
			{
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';

		}

		if(callback)
		    callback(buf, c);
	}

}

void editorMoveCursor(int key)
{
	// Cursor is allowed to be one past the last character on the line
	// Check if cursor is on an actual line. If it's one past, row is NULL.
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	switch (key)
	{
		case ARROW_LEFT:
			if(E.cx != 0)
				E.cx--;
			else if(E.cy > 0) // Left at the end of line goes up and to the end
			{
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if(row && E.cx < row -> size)
				E.cx++;
			else if(row && E.cx == row -> size) // right at EOL takes you down and at the start of next line
			{
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if(E.cy != 0)
				E.cy--;
			break;
		case ARROW_DOWN:
			if(E.cy < E.numrows)
				E.cy++;
			break;
	}

	// Snap to the end of line when we scroll to a shorter line from the end
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row -> size : 0;
	if(E.cx > rowlen)
	{
		E.cx = rowlen;
	}
}

void editorProcessKeyPress(void)
{
	static int quit_times = KILO_QUIT_TIMES;
	int c = editorReadKey();

	switch (c)
	{
		case '\r':
			editorInsertNewline();
			break;
		case CTRL_KEY('q'): // Quit
			if (E.dirty && quit_times > 0)
			{
				editorSetStatusMessage("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); // Bring cursor to top left
			exit(0);
			break;

		case CTRL_KEY('s'): // Save
			editorSave();
			break;

		case BACKSPACE:
		case DEL_KEY:
		case CTRL_KEY('h'):
			if(c == DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if(c == PAGE_UP)
				{
					E.cy = E.rowoff;
				}
				else if(c == PAGE_DOWN)
				{
					E.cy = E.rowoff + E.screenrows - 1;
					if(E.cy > E.numrows)
					{
						E.cy = E.numrows;
					}
				}
			}
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if(E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case CTRL_KEY('f'):
		    editorFind();
    		break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editorInsertChar(c);
			break;

	}

	quit_times = KILO_QUIT_TIMES;

}

/*** INIT ***/
void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.dirty = 0;

	if(getWindowSize(&(E.screenrows), &(E.screencols)) == -1)
	{
		die("getWindowSize");
	}

	E.screenrows -= 2; // 2 less to make room for status bar and message bar
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if(argc >= 2)
	{
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

	while(1)
	{
		editorRefreshScreen();
		editorProcessKeyPress();
	}
	return 0;
}
