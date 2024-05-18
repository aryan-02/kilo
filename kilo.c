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
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdint.h>
#include <string.h>

/*** PROTOTYPES ***/
void editorRefreshScreen(void);


/*** DEFINES ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

enum editorKey
{
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
	char *chars;
}
erow;


struct editorConfig
{
	int cx, cy; // x, y position of the cursor.
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
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
void editorAppendRow(char *line, int len)
{
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int idx = E.numrows;
	E.row[idx].size = len;
	E.row[idx].chars = malloc(len + 1);
	memcpy(E.row[idx].chars, line, len);
	E.row[idx].chars[len] = '\0';
	
	E.numrows++;
}

/*** FILE I/O ***/
void editorOpen(char *filename)
{
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
		editorAppendRow(line, linelen);
	}
	free(line);
	fclose(fp);
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
void editorDrawRows(struct abuf *ab)
{
	int y;
	for(y = 0; y < E.screenrows; y++)
	{
		if(y >= E.numrows)
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
			int len = E.row[y].size;
			if(len > E.screencols)
			{
				len = E.screencols;
			}
			abAppend(ab, E.row[y].chars, len);
		}
			

		abAppend(ab, "\x1b[K", 3); // Clear line to the right of the cursor.
		
		if(y < E.screenrows - 1)
		{
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen(void)
{
	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); // Hide cursor when (re)painting
	// Removed: we will clear each line as we update it instead
	// abAppend(&ab, "\x1b[2J", 4); // Clear screen
	abAppend(&ab, "\x1b[H", 3); // Bring cursor to top left

	editorDrawRows(&ab);

	char buf[32];
	// Move cursor to position (E.cx, E.cy)
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // Show cursor

	write(STDOUT_FILENO, ab.b, ab.cap);

	abFree(&ab);
}

/*** INPUT ***/
void editorMoveCursor(int key)
{
	switch (key)
	{
		case ARROW_LEFT:
			if(E.cx != 0)
				E.cx--;
			break;
		case ARROW_RIGHT:
			if(E.cx != E.screencols - 1)
				E.cx++;
			break;
		case ARROW_UP:
			if(E.cy != 0)
				E.cy--;
			break;
		case ARROW_DOWN:
			if(E.cy != E.screenrows - 1)
				E.cy++;
			break;	
	}
}

void editorProcessKeyPress(void)
{
	int c = editorReadKey();

	switch (c)
	{
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); // Bring cursor to top left
			exit(0);
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while(times--)
				{
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;
		
		case HOME_KEY:
			E.cx = 0;
			break;
		
		case END_KEY:
			E.cx = E.screencols - 1;
			break;
			
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		default:
			break;
	}

}

/*** INIT ***/
void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.numrows = 0;
	E.row = NULL;

	if(getWindowSize(&(E.screenrows), &(E.screencols)) == -1)
	{
		die("getWindowSize");
	}
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if(argc >= 2)
	{
		editorOpen(argv[1]);
	}

	while(1)
	{
		editorRefreshScreen();
		editorProcessKeyPress();
	}
	return 0;
}