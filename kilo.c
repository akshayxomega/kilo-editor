/*-----DEFINES------------------------------------------------*/

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1F)

/*-----INCLUDES-----------------------------------------------*/

#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*-----DATA---------------------------------------------------*/

enum editor_keys {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

typedef struct erow {
  int size;
  char *chars;
} erow;

struct editor_config {
  int cx;
  int cy;
  int row_offset;
  int col_offset;
  int screen_rows;
  int screen_cols;
  int num_rows;
  erow *row;
  struct termios og_termios;
};

struct editor_config CONF;

/*-----APPEND BUFFER------------------------------------------*/

#define ABUF_INIT {NULL, 0}
struct abuf {
  char *b;
  int len;
};

void abuf_append(struct abuf* ab, const char* s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
 
void abuf_free(struct abuf* ab){
  free(ab->b);
}

/*-----TERMINAL-----------------------------------------------*/

void die(const char* s){
  write(STDOUT_FILENO, "\x1B[2J", 4);
  write(STDOUT_FILENO, "\x1B[H", 3);
  perror(s);
  exit(1);
}

void disable_raw_mode(){
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &CONF.og_termios) == -1)
    die("tcsetattr - disable_raw_mode");
}

void enable_raw_mode(){
  if (tcgetattr(STDIN_FILENO, &CONF.og_termios) == -1)
    die("tcgetattr - enable_raw_mode");

  atexit(disable_raw_mode);

  struct termios raw = CONF.og_termios;
  raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr - enable_raw_mode");
}

int editor_read_key(){
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if (nread == -1 && errno != EAGAIN){
      die("read");
    }
  }

  if (c == '\x1B') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1B';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1B';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1B';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      }
      else {
        switch(seq[1]){
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    }
    else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1B';
  }
  else
    return c;
}

int get_cursor_position(int* rows, int* cols) {

  char buf[32];
  unsigned int i = 0;

  if(write(STDOUT_FILENO, "\x1B[6n", 4) != 4) return -1;

  while(i < sizeof(buf)-1) {
    if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if(buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1B' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int get_window_size(int* rows, int* cols){
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1B[999C\x1B[999B", 12) != 12)
      { return -1;}
    return get_cursor_position(rows, cols);
  }
  else {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
  }
}

/*-----ROW OPERATIONS-----------------------------------------*/

void editor_append_row(char *s, size_t len) {

  CONF.row = realloc(CONF.row, sizeof(erow)*(CONF.num_rows+1));

  int at = CONF.num_rows;
  CONF.row[at].size = len;
  CONF.row[at].chars = malloc(len+1);
  memcpy(CONF.row[at].chars, s, len);
  CONF.row[at].chars[len] = '\0';
  CONF.num_rows++;
}

/*-----FILE I/O-----------------------------------------------*/

void editor_open(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) 
    die("fopen");

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;

  while((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 && 
           (line[line_len-1] == '\n' || line[line_len-1] == '\r'))
      line_len--;
    editor_append_row(line, line_len);
  }
  free(line);
  fclose(fp);
}

/*-----INPUT--------------------------------------------------*/

void editor_move_cursor(int key){
  switch(key) {
    case ARROW_LEFT:
      if (CONF.cx != 0)
        CONF.cx--;
      break;
    case ARROW_RIGHT:
        CONF.cx++;
      break;
    case ARROW_UP:
      if (CONF.cy != 0)
        CONF.cy--;
      break;
    case ARROW_DOWN:
      if (CONF.cy < CONF.num_rows)
        CONF.cy++;
      break;
  }
}

void editor_process_keypress(){
  int c = editor_read_key();
  switch(c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1B[2J", 4);
      write(STDOUT_FILENO, "\x1B[H", 3);
      exit(0);
      break;

    case HOME_KEY:
      CONF.cx = 0;
      break;

    case END_KEY:
      CONF.cx = CONF.screen_cols-1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = CONF.screen_rows;
        while(times--)
          editor_move_cursor(c == PAGE_UP?ARROW_UP:ARROW_DOWN);  
      }
      break;

    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;  
  }
}

/*-----OUTPUT-------------------------------------------------*/

void editor_scroll() {
  if (CONF.cy < CONF.row_offset) {
    CONF.row_offset = CONF.cy;
  }
  if (CONF.cy >= CONF.row_offset+CONF.screen_rows){
    CONF.row_offset = CONF.cy-CONF.screen_rows+1;
  }
  if (CONF.cx < CONF.col_offset) {
    CONF.col_offset = CONF.cx;
  }
  if (CONF.cx >= CONF.col_offset+CONF.screen_cols){
    CONF.col_offset = CONF.cx-CONF.screen_cols+1;
  }
}

void editor_draw_rows(struct abuf* ab){
  for(int y=0; y<CONF.screen_rows; y++){
    int filerow = y + CONF.row_offset;
    if (filerow >= CONF.num_rows) {
      if (CONF.num_rows == 0 && y == CONF.screen_rows/2) {

        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        if (welcomelen > CONF.screen_cols) 
          welcomelen = CONF.screen_cols;
        int padding = (CONF.screen_cols - welcomelen)/2;
        if (padding) {
          abuf_append(ab, "~", 1);
          padding--;
        }
        while (padding--) 
          abuf_append(ab, " ", 1);
        abuf_append(ab, welcome, welcomelen);
      }

      else
        abuf_append(ab, "~", 1);
    }
    else {
      int len = CONF.row[filerow].size-CONF.col_offset;
      if (len < 0) len = 0;
      if (len > CONF.screen_cols) len = CONF.screen_cols;
      abuf_append(ab, &CONF.row[filerow].chars[CONF.col_offset], len);
    }

    abuf_append(ab, "\x1B[K", 3);
    if (y<CONF.screen_rows-1){
      abuf_append(ab, "\r\n", 2);
    }
  }
}

void editor_refresh_screen(){

  editor_scroll();

  struct abuf ab = ABUF_INIT;
 
  abuf_append(&ab, "\x1B[?25l", 6); 
  abuf_append(&ab, "\x1B[H", 3);

  editor_draw_rows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1B[%d;%dH", 
           CONF.cy-CONF.row_offset+1, CONF.cx-CONF.col_offset+1);
  abuf_append(&ab, buf, strlen(buf));

  abuf_append(&ab, "\x1B[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abuf_free(&ab);
}

/*-----INIT---------------------------------------------------*/

void init_editor(){
  CONF.cx = 0;
  CONF.cy = 0;
  CONF.row_offset = 0;
  CONF.col_offset = 0;
  CONF.num_rows = 0;
  CONF.row = NULL;
  if (get_window_size(&CONF.screen_rows,&CONF.screen_cols) == -1)
    die("get_window_size");
}

int main(int argc, char *argv[]) {

  enable_raw_mode();
  init_editor();
  if (argc >= 2) {
    editor_open(argv[1]);
  }
  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }    

  return 0;
}
