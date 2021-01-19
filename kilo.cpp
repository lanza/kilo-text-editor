#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <iostream>

#include <Person.hpp>
#include <Utility.hpp>

#include <dbg.h>

#include <llvm/Support/CommandLine.h>

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

struct EditorSyntax {
  char const *filetype;
  char const **filematch;
  char const **keywords;
  char const *singleline_comment_start;
  char const *multiline_comment_start;
  char const *multiline_comment_end;
  int flags;
};

int const HLDB_ENTRIES = 1;
char const *C_HL_extensions[] = {".c", ".h", ".cpp", nullptr};
char const *C_HL_keywords[] = {
    "switch", "if",    "while",     "for",     "break",   "continue",
    "return", "else",  "struct",    "union",   "typedef", "static",
    "enum",   "class", "case",      "int|",    "long|",   "double|",
    "float|", "char|", "unsigned|", "signed|", "void|",   nullptr};

struct EditorSyntax HLDB[] = {
    {"c", C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

// https://vt100.net/docs/vt100-ug/chapter3.html
const char *ClearScreen = "\x1b[2J";
const char *MoveCursorHome = "\x1b[H";
char const *MakeCursorInvisible = "\x1b[?25l";
char const *MakeCursorVisible = "\x1b[?25h";
char const *PleaseReportActivePosition = "\x1b[6n";
char const *ClearRow = "\x1b[K";
std::string moveCursorUp(int n) { return "\x1b[" + std::to_string(n) + "A"; }
std::string moveCursorDown(int n) { return "\x1b[" + std::to_string(n) + "B"; }
std::string moveCursorRight(int n) { return "\x1b[" + std::to_string(n) + "C"; }
std::string moveCursorLeft(int n) { return "\x1b[" + std::to_string(n) + "D"; }
char const *DefaultForegroundColor = "\x1b[39m";
std::string setColor_m(int n) { return "\x1b[" + std::to_string(n) + "m"; }
char const *ResetColor = "\x1b[m";
char const *ColorFormatString = "\x1b[%dm";
char const *ReverseVideo = "\x1b[7m";
char const *EraseLine = "\x1b[K";
std::string setCursorPosition(int x, int y) {
  return "\x1b[" + std::to_string(x) + ';' + std::to_string(y) + 'H';
}

static llvm::cl::OptionCategory MuffinCategory("muffin");
llvm::cl::opt<bool> MuffinIsCool("muffin-is-cool",
                                 llvm::cl::desc("Muffin is a cool dog."),
                                 llvm::cl::init(true));

llvm::cl::opt<bool> DoLoop("loop", llvm::cl::desc("Do the loop."),
                           llvm::cl::init(false));

inline constexpr char addCtrl(char c) { return c & 0x1f; }

static llvm::cl::opt<std::string> InputFilename(llvm::cl::Positional,
                                                llvm::cl::desc("<filename>"),
                                                llvm::cl::init(""));

struct Row {
  int idx;
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
  int hl_open_comment;
};

struct EditorConfig {
  int cursorX;
  int renderX;
  int cursorY;
  int screenRows;
  int screenCols;
  int numRows;
  int rowOffset;
  int colOffset;
  Row *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct EditorSyntax *syntax;
  struct termios originalTermios;
};

EditorConfig E;

void die(const char *s) {
  write(STDOUT_FILENO, ClearScreen, 4);
  write(STDOUT_FILENO, MoveCursorHome, 3);

  perror(s);
  exit(1);
}

enum Key {
  BackSpace = 127,
  ArrowLeft = 1000,
  ArrowRight,
  ArrowUp,
  ArrowDown,
  Delete,
  PageUp,
  PageDown,
  Home,
  End,
};

enum Highlight {
  Normal = 0,
  Comment,
  MultiLineComment,
  String,
  Number,
  Keyword1,
  Keyword2,
  Match,
};

int editorReadKey() {
  int numberRead;
  char c;
  while ((numberRead = read(STDIN_FILENO, &c, 1)) != 1) {
    if (numberRead == -1) {
      if (errno != EAGAIN && errno != EINTR)
        die("read");
    }
  }

  if (c == '\x1b') {
    char sequence[3];

    if (read(STDIN_FILENO, &sequence[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &sequence[1], 1) != 1)
      return '\x1b';

    if (sequence[0] == '[') {
      if (sequence[1] >= '0' && sequence[1] <= '9') {
        if (read(STDIN_FILENO, &sequence[2], 1) != 1)
          return '\x1b';
        if (sequence[2] == '~') {
          switch (sequence[1]) {
          case '1':
            return Key::Home;
          case '3':
            return Key::Delete;
          case '4':
            return Key::End;
          case '5':
            return Key::PageUp;
          case '6':
            return Key::PageDown;
          case '7':
            return Key::Home;
          case '8':
            return Key::End;
          }
        }
      } else {
        switch (sequence[1]) {
        case 'A':
          return Key::ArrowUp;
        case 'B':
          return Key::ArrowDown;
        case 'C':
          return Key::ArrowRight;
        case 'D':
          return Key::ArrowLeft;
        case 'H':
          return Key::Home;
        case 'F':
          return Key::End;
        }
      }
    } else if (sequence[0] == '0') {
      switch (sequence[1]) {
      case 'H':
        return Key::Home;
      case 'F':
        return Key::End;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buffer[32];

  if (write(STDOUT_FILENO, PleaseReportActivePosition, 4) != 4)
    return -1;

  // We're expecting back \033[ Pn ; Pn R where the `Pn` are the vt100 manual's
  // way of specifying a numerical parameter. This gives back the cursor
  // position as a response to the above lines asking for
  // `PleaseReportActivePosition`.
  unsigned int i = 0;
  while (i < sizeof(buffer) - 1) {
    if (read(STDIN_FILENO, &buffer[i], 1) != 1)
      break;
    if (buffer[i] == 'R')
      break;
    ++i;
  }
  buffer[i] = '\0';

  if (buffer[0] != '\x1b' || buffer[1] != '[')
    return -1;
  if (sscanf(&buffer[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    auto move = moveCursorLeft(999) + moveCursorDown(999);
    if (write(STDOUT_FILENO, move.c_str(), 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != nullptr;
}

void editorUpdateSyntax(Row *row) {
  row->hl = static_cast<unsigned char *>(realloc(row->hl, row->rsize));
  memset(row->hl, Highlight::Normal, row->rsize);

  if (E.syntax == nullptr)
    return;

  char const **keywords = E.syntax->keywords;

  char const *scs = E.syntax->singleline_comment_start;
  char const *mcs = E.syntax->multiline_comment_start;
  char const *mce = E.syntax->multiline_comment_end;

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : Highlight::Normal;

    if (scs_len && !in_string && !in_comment) {
      if (!strncmp(&row->render[i], scs, scs_len)) {
        memset(&row->hl[i], Highlight::Comment, row->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        row->hl[i] = Highlight::MultiLineComment;
        if (!strncmp(&row->render[i], mce, mce_len)) {
          memset(&row->hl[i], Highlight::MultiLineComment, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          ++i;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
        memset(&row->hl[i], Highlight::MultiLineComment, mcs_len);
        i += mcs_len;
        in_comment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        row->hl[i] = Highlight::String;
        if (c == '\\' && i + 1 < row->rsize) {
          row->hl[i + 1] = Highlight::String;
          i += 2;
          continue;
        }
        if (c == in_string)
          in_string = 0;
        ++i;
        prev_sep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          in_string = c;
          row->hl[i] = Highlight::String;
          ++i;
          continue;
        }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == Highlight::Number)) ||
          (c == '.' && prev_hl == Highlight::Number)) {
        row->hl[i] = Highlight::Number;
        ++i;
        prev_sep = 0;
        continue;
      }
    }

    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; ++j) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen - 1] == '|';
        if (kw2)
          --klen;

        if (!strncmp(&row->render[i], keywords[j], klen) &&
            is_separator(row->render[i + klen])) {
          std::memset(&row->hl[i],
                      kw2 ? Highlight::Keyword1 : Highlight::Keyword2, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != nullptr) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    ++i;
  }

  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if (changed && row->idx + 1 < E.numRows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

void editorSelectSyntaxHighlight() {
  E.syntax = nullptr;
  if (E.filename == nullptr)
    return;

  char *ext = strchr(E.filename, '.');

  for (unsigned int j = 0; j < HLDB_ENTRIES; ++j) {
    struct EditorSyntax *s = &HLDB[j];
    unsigned int i = 0;

    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
          (!is_ext && strstr(E.filename, s->filematch[i]))) {
        E.syntax = s;

        for (int filerow = 0; filerow < E.numRows; ++filerow) {
          editorUpdateSyntax(&E.row[filerow]);
        }
        return;
      }
      ++i;
    }
  }
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
  case Highlight::Comment:
    [[fallthrough]];
  case Highlight::MultiLineComment:
    return 36;
  case Highlight::Number:
    return 31;
  case Highlight::String:
    return 35;
  case Highlight::Match:
    return 34;
  case Highlight::Keyword1:
    return 33;
  case Highlight::Keyword2:
    return 32;
  default:
    return 37;
  }
}

template <typename T> void free(T *t) { free(reinterpret_cast<void *>(t)); }

template <typename T> void free(T const *t) {
  free(reinterpret_cast<void *>(const_cast<char *>(t)));
}

const uint TabSize = 4;

void editorUpdateRow(Row *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; ++j)
    if (row->chars[j] == '\t')
      ++tabs;

  free(row->render);
  row->render =
      static_cast<char *>(malloc(row->size + tabs * (TabSize - 1) + 1));

  int index = 0;
  for (int j = 0; j < row->size; ++j) {
    if (row->chars[j] == '\t') {
      row->render[index++] = ' ';
      while (index % TabSize != 0)
        row->render[index++] = ' ';
    } else {
      row->render[index++] = row->chars[j];
    }
  }

  row->render[index] = '\0';
  row->rsize = index;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numRows)
    return;

  E.row = static_cast<Row *>(realloc(E.row, sizeof(Row) * (E.numRows + 1)));
  memmove(&E.row[at + 1], &E.row[at], sizeof(Row) * (E.numRows - at));
  for (int j = at + 1; j <= E.numRows; ++j)
    E.row[j].idx++;

  E.row[at].idx = at;

  E.row[at].size = len;
  E.row[at].chars = static_cast<char *>(malloc(len + 1));
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = nullptr;
  E.row[at].hl = nullptr;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  ++E.numRows;
  ++E.dirty;
}

void editorRowInsertChar(Row *row, int at, int c) {
  if (at < 0 || at > row->size)
    at = row->size;

  row->chars = static_cast<char *>(realloc(row->chars, row->size + 2));
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  ++E.dirty;
}

void editorInsertChar(int c) {
  if (E.cursorY == E.numRows)
    editorInsertRow(E.numRows, const_cast<char *>(""), 0);

  editorRowInsertChar(&E.row[E.cursorY], E.cursorX, c);
  E.cursorX++;
}

void editorInsertNewLine() {
  if (E.cursorX == 0)
    editorInsertRow(E.cursorY, const_cast<char *>(""), 0);
  else {
    Row *row = &E.row[E.cursorY];
    editorInsertRow(E.cursorY + 1, &row->chars[E.cursorX],
                    row->size - E.cursorX);
    row = &E.row[E.cursorY];
    row->size = E.cursorX;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cursorY++;
  E.cursorX = 0;
}

void editorRowDelChar(Row *row, int at) {
  if (at < 0 || at >= row->size)
    return;

  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

void editorFreeRow(Row *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorRowAppendString(Row *row, char *s, size_t len) {
  row->chars = static_cast<char *>(realloc(row->chars, row->size + len + 1));
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numRows)
    return;

  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(Row) * (E.numRows - at - 1));
  for (int j = at; j < E.numRows - 1; ++j)
    E.row[j].idx--;

  E.numRows--;
  E.dirty++;
}

void editorDelChar() {
  if (E.cursorY == E.numRows)
    return;
  if (E.cursorX == 0 && E.cursorY == 0)
    return;

  Row *row = &E.row[E.cursorY];
  if (E.cursorX > 0) {
    editorRowDelChar(row, E.cursorX - 1);
    E.cursorX--;
  } else {
    E.cursorX = E.row[E.cursorY - 1].size;
    editorRowAppendString(&E.row[E.cursorY - 1], row->chars, row->size);
    editorDelRow(E.cursorY);
    E.cursorY--;
  }
}

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int j = 0; j < E.numRows; ++j)
    totlen += E.row[j].size + 1;
  *buflen = totlen;

  char *buf = static_cast<char *>(malloc(totlen));
  char *p = buf;
  for (int j = 0; j < E.numRows; ++j) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    ++p;
  }

  return buf;
}

void editorOpen(char const *filename) {
  E.filename = const_cast<char *>(filename);
  FILE *fp = fopen(filename, "r");

  editorSelectSyntaxHighlight();

  if (!fp)
    die("fopen");

  char *line = nullptr;
  size_t lineCap = 0;
  ssize_t lineLen = 0;
  while ((lineLen = getline(&line, &lineCap, fp)) != -1) {
    while (lineLen > 0 &&
           (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r'))
      --lineLen;
    editorInsertRow(E.numRows, line, lineLen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorRefreshScreen();

void editorSetStatusMessage(char const *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(nullptr);
}

char *editorPrompt(char *prompt, void (*callback)(char *, int) = nullptr) {
  size_t bufsize = 128;
  char *buf = static_cast<char *>(malloc(bufsize));

  size_t buflen = 0;
  buf[0] = '\0';

  while (true) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == Key::Delete || c == addCtrl('h') || c == Key::BackSpace) {
      if (buflen != 0)
        buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return nullptr;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = static_cast<char *>(realloc(buf, bufsize));
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback)
      callback(buf, c);
  }
}

int editorRowRxToCx(Row *row, int renderX);

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;
  static char *saved_hl = nullptr;

  if (saved_hl) {
    std::memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = nullptr;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == Key::ArrowRight || key == Key::ArrowDown) {
    direction = 1;
  } else if (key == Key::ArrowLeft || key == ArrowUp) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;

  int current = last_match;

  for (int i = 0; i < E.numRows; ++i) {
    current += direction;
    if (current == -1)
      current = E.numRows - 1;
    else if (current == E.numRows)
      current = 0;

    Row *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cursorY = current;
      E.cursorX = editorRowRxToCx(row, match - row->render);
      E.rowOffset = E.numRows;

      saved_hl_line = current;
      saved_hl = static_cast<char *>(malloc(row->rsize));
      memcpy(saved_hl, row->hl, row->rsize);
      std::memset(&row->hl[match - row->render], Highlight::Match,
                  strlen(query));
      break;
    }
  }
}

void editorSave() {
  if (E.filename == nullptr) {
    E.filename = editorPrompt(const_cast<char *>("Save as: %s"));
    if (E.filename == nullptr) {
      editorSetStatusMessage("Save aborted");
      return;
    }

    editorSelectSyntaxHighlight();
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

void initEditor() {
  E.cursorX = 0;
  E.cursorY = 0;
  E.renderX = 0;
  E.rowOffset = 0;
  E.colOffset = 0;
  E.numRows = 0;
  E.row = nullptr;
  E.dirty = 0;
  E.filename = nullptr;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = nullptr;

  if (getWindowSize(&E.screenRows, &E.screenCols) == -1)
    die("getWindowSize");

  // for the status line
  E.screenRows -= 2;
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.originalTermios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.originalTermios) == -1)
    die("tcgetattr");

  atexit(disableRawMode);

  struct termios raw = E.originalTermios;

  // --- IFLAGS ---
  // This uses ctrl-s and ctrl-q to control input and output for old days
  raw.c_iflag &= ~(IXON);
  // This flag stops the terminal from  mapping CTRL-M to CTRL-J
  raw.c_iflag &= ~(ICRNL);

  // --- LFLAGS ---
  // IEXTEN stops macOS terminal driver from getting rid of ctrl-o
  raw.c_lflag &= ~(IEXTEN);
  // Causes your input not to be echoed back
  raw.c_lflag &= ~(ECHO);
  // Causes input not to be buffered -- e.g. the char is sent right away
  // without return being pressed
  raw.c_lflag &= ~(ICANON);
  // Disables signals being sent to the child processes
  raw.c_lflag &= ~(ICANON | ISIG);

  raw.c_iflag &= ~(BRKINT);
  raw.c_iflag &= ~(INPCK);
  raw.c_iflag &= ~(ISTRIP);

  raw.c_cflag |= (CS8);

  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  // --- OFLAGS ---
  raw.c_oflag &= ~(OPOST);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

const char *controlLookup[256] = {0};

void initControlLookup() {
  controlLookup[9] = "\\t";
  controlLookup[10] = "\\r";
  controlLookup[13] = "\\n";
  controlLookup[127] = "delete";
}

struct AppendBuffer {
  char *buffer;
  int length;

  AppendBuffer() : buffer{nullptr}, length{0} {}

  void append(char const *s, int length) {
    char *n = (char *)realloc(this->buffer, this->length + length);

    if (n == nullptr)
      return;

    memcpy(&n[this->length], s, length);
    this->buffer = n;
    this->length += length;
  }

  ~AppendBuffer() { free(buffer); }
};

void editorMoveCursor(int key) {
  Row *row = (E.cursorY >= E.numRows) ? nullptr : &E.row[E.cursorY];

  switch (key) {
  case Key::End:
    E.cursorX = E.row[E.cursorY].size;
    break;
  case Key::Home:
    E.cursorX = 0;
    break;
  case Key::ArrowLeft:
    if (E.cursorX != 0)
      --E.cursorX;
    else if (E.cursorY > 0) {
      --E.cursorY;
      E.cursorX = E.row[E.cursorY].size;
    }
    break;
  case Key::ArrowRight:
    if (row && E.cursorX < row->size)
      ++E.cursorX;
    else if (row && E.cursorX == row->size) {
      ++E.cursorY;
      E.cursorX = 0;
    }
    break;
  case Key::ArrowUp:
    if (E.cursorY != 0)
      --E.cursorY;
    break;
  case Key::ArrowDown:
    if (E.cursorY < E.numRows)
      ++E.cursorY;
    break;
  }

  row = (E.cursorY >= E.numRows) ? nullptr : &E.row[E.cursorY];
  int rowLen = row ? row->size : 0;
  if (E.cursorX > rowLen)
    E.cursorX = rowLen;
}

char const *const KiloVersion = "0.0.1";
int const KiloQuitTimes = 3;

void editorDrawRows(AppendBuffer &ab) {
  for (int y = 0; y < E.screenRows; ++y) {
    int fileRow = y + E.rowOffset;
    if (fileRow >= E.numRows) {
      if (E.numRows == 0 && y == E.screenRows / 3) {
        char welcome[80];
        int welcomeLength = snprintf(welcome, sizeof(welcome),
                                     "Kilo editor -- version %s", KiloVersion);
        if (welcomeLength > E.screenCols)
          welcomeLength = E.screenCols;

        int padding = (E.screenCols - welcomeLength) / 2;
        if (padding) {
          ab.append("~", 1);
          --padding;
        }
        while (padding--)
          ab.append(" ", 1);
        ab.append(welcome, welcomeLength);
      } else {
        ab.append("~", 1);
      }
    } else {
      int len = E.row[fileRow].rsize - E.colOffset;
      if (len < 0)
        len = 0;
      if (len > E.screenCols)
        len = E.screenCols;
      char *c = &E.row[fileRow].render[E.colOffset];
      unsigned char *hl = &E.row[fileRow].hl[E.colOffset];
      int current_color = -1;
      for (int j = 0; j < len; ++j) {
        if (iscntrl(c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          ab.append(ReverseVideo, 4);
          ab.append(&sym, 1);
          ab.append(ResetColor, 3);
          if (current_color != -1) {
            char buf[16];
            int clen =
                snprintf(buf, sizeof(buf), ColorFormatString, current_color);
            ab.append(buf, clen);
          }
        } else if (hl[j] == Highlight::Normal) {
          if (current_color != -1) {
            ab.append(DefaultForegroundColor, 5);
            current_color = -1;
          }
          ab.append(&c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            current_color = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), ColorFormatString, color);
            ab.append(buf, clen);
          }
          ab.append(&c[j], 1);
        }
      }
      ab.append(DefaultForegroundColor, 5);
    }

    ab.append(ClearRow, 3);
    ab.append("\r\n", 2);
  }
}

int editorRowCxToRx(Row *row, int cursorX) {
  int renderX = 0;
  for (int j = 0; j < cursorX; ++j) {
    if (row->chars[j] == '\t')
      renderX = (TabSize - 1) - (renderX % TabSize);
    ++renderX;
  }
  return renderX;
}

int editorRowRxToCx(Row *row, int renderX) {
  int curRx = 0;
  int cx;
  for (cx = 0; cx < row->size; ++cx) {
    if (row->chars[cx] == '\t')
      curRx += (TabSize - 1) - (curRx % TabSize);
    ++curRx;

    if (curRx > renderX)
      return cx;
  }
  return cx;
}

void editorFind() {
  int saved_cx = E.cursorX;
  int saved_cy = E.cursorY;
  int saved_coloff = E.colOffset;
  int saved_rowoff = E.rowOffset;

  char *query =
      editorPrompt(const_cast<char *>("Search: %s (Use ESC/Arrows/Enter)"),
                   editorFindCallback);

  if (query)
    free(query);
  else {
    E.cursorX = saved_cx;
    E.cursorY = saved_cy;
    E.colOffset = saved_coloff;
    E.rowOffset = saved_rowoff;
  }
}

void editorScroll() {
  E.renderX = E.cursorX;
  if (E.cursorY < E.numRows)
    E.renderX = editorRowCxToRx(&E.row[E.cursorY], E.cursorX);

  if (E.cursorY < E.rowOffset)
    E.rowOffset = E.cursorY;

  if (E.cursorY >= E.rowOffset + E.screenRows)
    E.rowOffset = E.cursorY - E.screenRows + 1;

  if (E.renderX < E.colOffset)
    E.colOffset = E.renderX;

  if (E.renderX >= E.colOffset + E.screenCols)
    E.colOffset = E.renderX - E.screenCols + 1;
}

void editorDrawStatusBar(AppendBuffer &ab) {
  ab.append(ReverseVideo, 4);
  char status[80];
  char rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numRows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                      E.syntax ? E.syntax->filetype : "no ft", E.cursorY + 1,
                      E.numRows);

  if (len > E.screenCols)
    len = E.screenCols;
  ab.append(status, len);

  while (len < E.screenCols) {
    if (E.screenCols - len == rlen) {
      ab.append(rstatus, rlen);
      break;
    } else {
      ab.append(" ", 1);
      ++len;
    }
  }
  ab.append(ResetColor, 3);
  ab.append("\r\n", 2);
}

void editorDrawMessageBar(AppendBuffer &ab) {
  ab.append(EraseLine, 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screenCols)
    msglen = E.screenCols;
  if (msglen && time(nullptr) - E.statusmsg_time < 5)
    ab.append(E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  AppendBuffer ab;
  ab.append(MakeCursorInvisible, 6);
  ab.append(MoveCursorHome, 3);

  editorDrawRows(ab);
  editorDrawStatusBar(ab);
  editorDrawMessageBar(ab);

  auto cursorMove = setCursorPosition((E.cursorY - E.rowOffset) + 1,
                                      (E.renderX - E.colOffset) + 1);
  ab.append(cursorMove.c_str(), cursorMove.size());

  ab.append(MakeCursorVisible, 6);

  write(STDOUT_FILENO, ab.buffer, ab.length);
}

void editorProcessKeypress() {
  static int quitTimes = KiloQuitTimes;
  int c = editorReadKey();

  switch (c) {
  case '\r':
    editorInsertNewLine();
    break;
  case addCtrl(31):
    editorFind();
    ;
    break;
  case Key::BackSpace:
  case addCtrl('h'):
  case Key::Delete:
    if (c == Key::Delete)
      editorMoveCursor(ArrowRight);
    editorDelChar();
    break;
  case addCtrl('s'):
    editorSave();
    break;
  case addCtrl('e'):
    editorMoveCursor(Key::End);
    break;
  case addCtrl('a'):
    editorMoveCursor(Key::Home);
    break;
  case addCtrl('k'):
  case addCtrl('l'):
  case '\x1b':
    break;
  case addCtrl('q'):
    if (E.dirty && quitTimes > 0) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quitTimes);
      --quitTimes;
      return;
    }
    write(STDOUT_FILENO, ClearScreen, 4);
    write(STDOUT_FILENO, MoveCursorHome, 3);
    exit(0);
    break;
  case Key::Home:
    E.cursorX = 0;
    break;
  case Key::End:
    if (E.cursorY < E.numRows)
      E.cursorX = E.row[E.cursorY].size;
    break;
  case addCtrl('p'):
  case Key::ArrowUp:
    editorMoveCursor(Key::ArrowUp);
    break;
  case addCtrl('n'):
  case Key::ArrowDown:
    editorMoveCursor(Key::ArrowDown);
    break;
  case addCtrl('b'):
  case Key::ArrowLeft:
    editorMoveCursor(Key::ArrowLeft);
    break;
  case addCtrl('f'):
  case Key::ArrowRight:
    editorMoveCursor(Key::ArrowRight);
    break;
  case Key::PageUp:
  case Key::PageDown: {
    if (c == PageUp)
      E.cursorY = E.rowOffset;
    else if (c == PageDown) {
      E.cursorY = E.rowOffset + E.screenRows - 1;
      if (E.cursorY > E.numRows)
        E.cursorY = E.numRows;
    }

    int times = E.screenRows;
    while (times--)
      editorMoveCursor(Key::ArrowUp);
    if (E.cursorY > E.numRows)
      E.cursorY = E.numRows;
  } break;
  default:
    editorInsertChar(c);
    break;
  }
  quitTimes = KiloQuitTimes;
}

#define CTRL_KEY(k) ((k)&0x1f)

void doEchoLoop() {
  while (true) {
    char c = '\0';
    read(STDIN_FILENO, &c, 1);
    if (c == '\0')
      continue;
    if (iscntrl(c))
      printf("%d\r\n", c);
    else
      printf("%d ('%c')\r\n", c, c);
    if (c == CTRL_KEY('q'))
      break;
  }
}

int main(int argc, char **argv) {
  if (!llvm::cl::ParseCommandLineOptions(argc, argv)) {
    llvm::cl::PrintOptionValues();
  }

  enableRawMode();

  // doEchoLoop();

  if (DoLoop)
    doEchoLoop();

  initEditor();
  if (InputFilename.size() > 0)
    editorOpen(InputFilename.c_str());

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-/ = find");
  initControlLookup();

  while (true) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
