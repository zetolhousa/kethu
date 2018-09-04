//feature test macros: explicitly requesting additional features by defining one or more of the feature macros
#define _DEFAULT_SOURCE ////If you define this macro, most features are included apart from X/Open, LFS and GNU extensions: the effect is to enable features from the 2008 edition of POSIX, as well as certain BSD and SVID features without a separate feature test macro to control them.
#define _BSD_SOURCE
#define _GNU_SOURCE //If you define this macro, everything is included: ISO C89, ISO C99, POSIX.1, POSIX.2, BSD, SVID, X/Open, LFS, and GNU extensions. In the cases where POSIX.1 conflicts with BSD, the POSIX definitions take precedence.

/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>     //atexit()
#include <string.h>     //memcpy()
#include <sys/ioctl.h>  //to get size of terminal with TIOCGWINSZ
#include <sys/types.h>
#include <termios.h>    //terminal settings
#include <time.h>
#include <unistd.h>


/*** defines ***/

#define KETHU_VERSION "0.0.1"
#define KETHU_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)    //bitwise ANDs char with 0x1f(00011111)
                                    //upper 3bits of character made 0, mirroring what ctrl key does in terminal, it strips bit 5 and 6 from whatever key pressed in combo with ctrl and sends that.
                                    //notice: first 4bits of both upper and lower case are same.
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,    //1000
  ARROW_RIGHT,          //1001
  ARROW_UP,             //1002
  ARROW_DOWN,           //1003
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig { //to store editor state
  int cx, cy;         //store position of cursor
  int rx;             //position of cursor on the render field
  int rowoff;         //keep track of row offset in file the user in currently at
  int coloff;         //keep track of col offset in file
  int screenrows;     //number of rows visible in terminal
  int screencols;     //number of cols visible in terminal
  int numrows;        //number of rows in file opened
  erow *row;          //struct to store each row in file
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};
struct editorConfig E;


/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);   //clear screen
  write(STDOUT_FILENO, "\x1b[H", 3);    //reposition cursor

  perror(s);    //C lib funcs on failing set global errno var. perror() looks at the value and prints apt msg
  exit(1);      //nonzero means unsuccesful exectution
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); //misc flags included
  raw.c_oflag &= ~(OPOST);      //turn off all output processing
  raw.c_cflag |= (CS8);         //misc mask with multiple bits
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;   //defines min bytes to input before read() returns
  raw.c_cc[VTIME] = 1;  //defines max amount of time to wait before read() returns. Here 1/10s or 100ms. After timeout returns 0
                        //Here in WSL read() still gets blocked and gives no shit about VTIME, but it won't matter later

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr"); //call die() on error
}

int editorReadKey() {  //job is to wait for ONE keypress and return it
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {    //as long as error keep trying
    if (nread == -1 && errno != EAGAIN) die("read");    //but if <cond> then exit.
  }

  if (c == '\x1b') {                                        //if ESC character is read from above
    char seq[3];    //Here only 2 used by 3B just for future purposes
    // !=1 is used because read() returns 0 if when starting at the current file offset, the file offset if detected to be at or past the EOF 0B are read.
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b'; //then read() another byte and put into seq[0]  : for 2nd Byte of ESC seq '['   but IF read()!=1, unsuccesful read(because offset beyond EOF) return only ESC char
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b'; //and read() another byte and put into seq[1]   : for the last Byte of ESC seq  but IF read()!=1, unsuccesful read(because offset beyond EOF) return only ESC char
    
    if (seq[0] == '[') {    //if we get this far it means 3B have been read succesfully. 1st Byte c='\x1b' | 2nd Byte seq[0] | 3rd Byte seq[1]
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; //read() 3rd Byte(does not include ESC char), if a Byte is not read succesfully then return ESC char
        if (seq[2] == '~') {                                    //condition for pageUp and pageDown, which take the following format: '\x1b[5~' and '\x1b[6~'
          switch (seq[1]) {
            case '1': return HOME_KEY;  //<esc>[1~, <esc>[7~, <esc>[H, or <esc>OH
            case '3': return DEL_KEY;
            case '4': return END_KEY;   //<esc>[4~, <esc>[8~, <esc>[F, or <esc>OF
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
          case 'H': return HOME_KEY;  //<esc>[H
          case 'F': return END_KEY;   //<esc>[F
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;  //<esc>OH
        case 'F': return END_KEY;   //<esc>OF
      }
    }

    return '\x1b';
  } else {  //else if normal character return it
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;   //write to stdout
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;         //read output result from stdin and store into buf one at a time
    if (buf[i] == 'R') break;                               //break out when we read 'R' (part of the command)
    i++;
  }
  buf[i] = '\0';    //'R' replaced with null character

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;         //make sure response is in ESC seq form, else on error return -1
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1; //next we start from 3rd Byte, which comes after '\x1b['. So string format is in the from '24;130' and sscanf() reads that appropriately
  return 0;                                                 //and pass it into rows and cols variables.
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {  //removed prior 1 || conditions
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}


/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {  //converts chars index into render index
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KETHU_TAB_STOP - 1) - (rx % KETHU_TAB_STOP);
    rx++;
  }
  return rx;
}

void editorUpdateRow(erow *row) { //cleanup the tabs
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*(KETHU_TAB_STOP-1) + 1); //*7 because tab is counted only as one entity(of width=8 but row->size already has +1 for each tab so *7 and not *8)
                                                //but we need to break thar up into spaces the width of a tab so we allocate that much space
                                                //and end with +1 to accomodate null char '\0'
  int idx = 0;
  for (j = 0; j < row->size; j++) { //for each char in row (tab is also a single char)
    if (row->chars[j] == '\t') {    //if tab found in chars
      row->render[idx++] = ' ';     //add ' ' in render
      while (idx % KETHU_TAB_STOP != 0) row->render[idx++] = ' ';  //and modulate for possible remaining whitepaces to reach width of 8. Eg: "Hello\tWorld" would be "Hello---World". Since at 'o' idx=5 and 5/8 till 8/8 will be filled with ' '
    } else {  //else normal copy
      row->render[idx++] = row->chars[j];
    }
  } //idx now is the size of characters in render. Suppose idx=11, we allocated +1 space so total length = 12
  row->render[idx] = '\0';  //make 11 index as null character to signal end
  row->rsize = idx; //does not include '\0'??
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);
  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) { //free up mem for a row
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) { //replace freed up row with rows below it
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1)); //move all rows that comes after deleted row to 'at'
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);  // +1 for new char +1 for nullchar
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1); //(dest, src, size) works like strcpy but for overlapping locations
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {  //append for backspacing a row to the upper row
  row->chars = realloc(row->chars, row->size + len + 1);  //space for the row + nullchar
  memcpy(&row->chars[row->size], s, len); //append to that row
  row->size += len; //new size
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {  //means that cursor is on the last line, the ~ line
    editorInsertRow(E.numrows, "", 0); //insert an empty row
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx); //move all chars from current cursor position to the next row
    row = &E.row[E.cy]; //row now points to remaining lines before enter pressed  (reassigned again because realloc() in editorInsertRow() changes mem locations)
    row->size = E.cx;   //and we give it appropriate size
    row->chars[row->size] = '\0'; //end with nullchar
    editorUpdateRow(row);
  }
  E.cy++; //then go to the next line
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;  //last col of prev line
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;  //lengths of all rows summed up in totlen and +1 for newline character
  *buflen = totlen; //tell caller how long
  char *buf = malloc(totlen); //allocate space for all characters
  char *p = buf;  //to keep *buf pointing to start of mem and let *p do the moving
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size); //copy one row onto *buf
    p += E.row[j].size; //move ahead required Bytes for the row
    *p = '\n';  //add newline to that row
    p++;  //moving onto the next row
  }
  return buf; //return mem location
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);  //makes copy of given string, also allocates required mem but has to be free() after use

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {  //-1 when end of file
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) //removing all the newlines?
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct abuf { //struct to buffer all output and write it out only later
  char *b;                      //pointer to buffer in memory
  int len;                      //length
};
#define ABUF_INIT {NULL, 0}     //acts as constructor for abuf type

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);    //realloc memory block where size is old len + new len
  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);    //(dest, src, size)     Here we copy s after the end of old data
  ab->b = new;                      //updating pointer
  ab->len += len;                   //and new length
}

void abFree(struct abuf *ab) {      //Destructor
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff; //offset. y ranges from top to bottom of visible screen eg: in a 50*200 terminal y->{0...49}
                                //On update of rowoff=1(from above function) filerow will now be =0+1=1. Therefore dispay on terminal will start from 2nd line of file. Basically scrolled one row down.
    if (filerow >= E.numrows) { //when current row greater than or equal to number of row in text file we start inserting '~' for the rest of the empty lines.
      if (E.numrows == 0 && y == E.screenrows / 3) {  //only when there is no file opened (and we are 1/3 of the way down) we display welcome text
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome), "Kethu editor -- version %s", KETHU_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1); //insert empty spaces in line before the welcome text follows
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else { //or else display the row of text in file
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4); //graphicRendition(m) command switches to inverted colors. 1-bold, 4-underscore, 5-blink, 7-invert
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);  //current line no out of total lines
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1); //add spaces until we just have enough length left for rstatus
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);  //switches back to normal. Using no args clears all set attributes
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)  //show statusmsg while diff between time when msg was set and current time(updated every refresh) is within 5secs
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;
  abAppend(&ab, "\x1b[?25l", 6);    //resetMode(l) command to turn off features/modes. '?25l' cursor hiding
  //abAppend(&ab, "\x1b[2J", 4);    REMOVED and instead clear as we redraw on line at a time
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy-E.rowoff) + 1, (E.rx-E.coloff) + 1);    //Display cursor position. Updated!!!
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);    //setMode(h) command to turn on. '?25h' cursor show. If hide/show feature not supported, ESC seq just ignored. No big deal
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap); //writes to E.statusmsg still the given sizeof(). The content is given by format(fmt) with argument pointer(ap)
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) { //backspace and delete while entering filename
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {  //ESC key pressed
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {  //when user presses enter we get into exit protocol
      if (buflen != 0) {
        editorSetStatusMessage(""); //clear/reset status message before returning value
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {  //is char is not a control char and it is printable char
      if (buflen == bufsize - 1) {  //is user input length reaches alloted size we double allotment
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen]   = '\0';
    }
  }
}

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows)? NULL : &E.row[E.cy];  //pointer to row at current line

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {                  //only if cursor is not at leftmost of screen
        E.cx--;
      }
      else if(E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {    //if row exists and cursor does not cross right limit
        E.cx++;
      }
      else if(row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {                  //only if cursor is not at top of screen
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {           //if cursor pos is smaller than the number of rows in text
        E.cy++;
      }
      break;
  }
  //snapping to the rightmost of current line
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];  //if cursor on valid row | another row assignment because cursor E.cy just changed line from above
  int rowlen = row ? row->size : 0;                 //if row valid then assing size of that row
  if (E.cx > rowlen) {                              //if cursor past the rows length
    E.cx = rowlen;                                  //assign length of current row to cursor value
  }                                                 //else cursor position from previos row remains
}

void editorProcessKeypress() {  //get keypress from editorReadKey() and handles it as needed
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();
  switch (c) {
    case '\r':
      editorInsertNewline();
      break;
    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);   //clear screen
      write(STDOUT_FILENO, "\x1b[H", 3);    //reposition cursor
      exit(0);  //0 is success in terms of program execution but 0 is FALSE in terms of boolean
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;  //end of current row and not rightmost of screen
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      { //this block of braces is used because declaring vars directly is not allowed
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--) //for now pageUp is aliased as multiple Downs arrow till we reach bottom vice versa for pageUp
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
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
  quit_times = KILO_QUIT_TIMES; //in the midst of quiting if user press another key the count is reset
}

/*** init ***/

void initEditor() { //func to init fields in struct E
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;   //scroll to top by default initially
  E.coloff = 0;   //beginning of line
  E.numrows = 0;  //temporary
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -=2;
}


int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);  //open the file if arg provided else a blank file
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();      //renders screen
    editorProcessKeypress();    //takes in keypress and process it
  }

  return 0;
}
