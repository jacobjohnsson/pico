/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define PICO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DELETE,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

struct editor_config {
  int cur_x, cur_y;             /* cursor location */
  struct termios orig_termios;  /* original terminal settings */
  int terminal_rows;            /* terminal height */
  int terminal_cols;            /* terminal width */
};

struct editor_config config;

/*** terminal ***/

void die (const char *msg)
{
  write (STDOUT_FILENO, "\x1b[2J", 4);
  write (STDOUT_FILENO, "\x1b[H", 3);
  perror (msg);
  exit(1);
}

void disable_raw_mode ()
{
  if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &config.orig_termios) == -1) {
    die ("tcsetattr");
  }
}

void enable_raw_mode ()
{
  if (tcgetattr (STDIN_FILENO, &config.orig_termios) == -1) {
    die ("tcgetattr");
  }
  atexit(disable_raw_mode);

  struct termios raw = config.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_oflag &= ~(CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die ("tcsetattr");
  }
}

int read_key ()
{
  int nread;
  char c;

  while ((nread = read (STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die ("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read (STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read (STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read (STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '2': return END_KEY;
            case '3': return DEL_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int get_cursor_position (int *rows, int *cols)
{
  char buf[32];
  unsigned int i = 0;

  if (write (STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof (buf) - 1) {
    if (read (STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int get_window_size (int *rows, int *cols) {
  struct winsize size;

  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || size.ws_col == 0) {
    if (write (STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return get_cursor_position (rows, cols);
  } else {
    *cols = size.ws_col;
    *rows = size.ws_row;
    return 0;
  }
}

/*** append buffer ***/

typedef struct _append_buffer {
  char *buf;
  int len;
} append_buffer;

#define ABUF_INIT {NULL, 0}

void ab_append (append_buffer * ab, const char *s, int len)
{
  char *new = realloc (ab->buf, ab->len + len);

  if (new == NULL) {
    return;
  }
  memcpy (&new[ab->len], s, len);
  ab->buf = new;
  ab->len += len;
}

void ab_free (append_buffer *ab)
{
  free (ab->buf);
}

/*** output ***/

void draw_rows (append_buffer * ab)
{
  int y;
  for (y = 0; y < config.terminal_rows; y++) {
    if (y == config.terminal_rows / 3) {
      char welcome[80];
      int welcome_len = snprintf (welcome, sizeof (welcome),
          "Pico editor -- version %s", PICO_VERSION);
      if (welcome_len > config.terminal_rows) {
        welcome_len = config.terminal_rows;
      }
      int padding = (config.terminal_cols - welcome_len) / 2;
      if (padding) {
        ab_append (ab, "~", 1);
        padding--;
      }
      while (padding--) {
        ab_append (ab, " ", 1);
      }
      ab_append (ab, welcome, welcome_len);
    } else {
      ab_append (ab, "~", 1);
    }

    ab_append (ab, "\x1b[K", 3);
    if (y < config.terminal_rows - 1) {
      ab_append (ab, "\r\n", 2);
    }
  }
}

void refresh_screen ()
{
  append_buffer ab = ABUF_INIT;

  ab_append (&ab, "\x1b[?25l", 6);
  ab_append (&ab, "\x1b[H", 3);

  draw_rows (&ab);

  char buf[32];
  snprintf (buf, sizeof (buf), "\x1b[%d;%dH", config.cur_y + 1,
      config.cur_x + 1);
  ab_append (&ab, buf, strlen (buf));

  ab_append (&ab, "\x1b[?25h", 6);

  write (STDOUT_FILENO, ab.buf, ab.len);
  ab_free (&ab);
}

/*** input ***/

void move_cursor (int key)
{
  switch (key) {
    case ARROW_LEFT:
      if (config.cur_x > 0) {
        config.cur_x--;
      }
      break;
    case ARROW_RIGHT:
      if (config.cur_x < config.terminal_cols - 1) {
        config.cur_x++;
      }
      break;
    case ARROW_UP:
      if (config.cur_y > 0) {
        config.cur_y--;
      }
      break;
    case ARROW_DOWN:
      if (config.cur_y < config.terminal_rows - 1) {
        config.cur_y++;
      }
      break;
  }
}

void process_key_press()
{
  int c = read_key ();

  switch (c) {
    case CTRL_KEY('q'):
      write (STDOUT_FILENO, "\x1b[2J", 4);
      write (STDOUT_FILENO, "\x1b[H", 3);
      exit (0);
      break;
    case HOME_KEY:
      config.cur_x = 0;
      break;
    case END_KEY:
      config.cur_x = config.terminal_cols - 1;
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = config.terminal_rows;
        while (times--) move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      move_cursor (c);
      break;
  }

}

/*** init ***/

void init_editor ()
{
  config.cur_x = 0;
  config.cur_y = 0;

  if (get_window_size (&config.terminal_rows, &config.terminal_cols) == -1) {
    die ("get_window_size");
  }
}

int main (int argc, char **argv)
{
  enable_raw_mode ();
  init_editor ();

  while (1) {
    refresh_screen ();
    process_key_press ();
  }

  return 0;
}
