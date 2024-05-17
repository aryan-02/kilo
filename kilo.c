/*** INCLUDES ***/
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <string.h>

/*** PROTOTYPES ***/
void editorRefreshScreen(void);


/*** DEFINES ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"


/*** DATA ***/
struct editorConfig
{
	int cx, cy; // x, y position of the cursor.
	int screenrows;
	int screencols;
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

char editorReadKey(void)
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
	return c;
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
	while(i < sizeof(response) - 1)
	{
		if(read(STDIN_FILENO, &response[i], 1) != 1)
		{
			break;
		}
		if(response[i] == 'R')
		{
			break;
		}
		i++;
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
		if(y == E.screenrows / 3)
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
void editorMoveCursor(char key)
{
	switch (key)
	{
		case 'a':
			E.cx--;
			break;
		case 'd':
			E.cx++;
			break;
		case 'w':
			E.cy--;
			break;
		case 's':
			E.cy++;
			break;	
	}
}

void editorProcessKeyPress(void)
{
	char c = editorReadKey();

	switch (c)
	{
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
			write(STDOUT_FILENO, "\x1b[H", 3); // Bring cursor to top left
			exit(0);
			break;
		case 'w':
		case 'a':
		case 's':
		case 'd':
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
	if(getWindowSize(&(E.screenrows), &(E.screencols)) == -1)
	{
		die("getWindowSize");
	}
}

int main(void)
{
	enableRawMode();
	initEditor();

	while(1)
	{
		editorRefreshScreen();
		editorProcessKeyPress();
	}
	return 0;
}