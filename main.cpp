#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <iostream>

#include <Person.hpp>
#include <Utility.hpp>

#include <dbg.h>

#include <llvm/Support/CommandLine.h>

static llvm::cl::OptionCategory MuffinCategory("muffin");
llvm::cl::opt<bool> MuffinIsCool("muffin-is-cool",
                                 llvm::cl::desc("Muffin is a cool dog."),
                                 llvm::cl::init(true));

struct editorConfig {
  int cursorX;
  int cursorY;
  int screenRows;
  int screenCols;
  struct termios originalTermios;
};

struct editorConfig E;

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

enum Key {
  ArrowLeft = 1000,
  ArrowRight,
  ArrowUp,
  ArrowDown,
  Delete,
  PageUp,
  PageDown,
  Home,
  End
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
            return PageUp;
          case '6':
            return PageDown;
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

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

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
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

void initEditor() {
  E.cursorX = 0;
  E.cursorY = 0;

  if (getWindowSize(&E.screenRows, &E.screenCols) == -1)
    die("getWindowSize");
}

inline constexpr char addCtrl(char c) { return c & 0x1f; }

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
  // This flag swaps CR to NL
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
  switch (key) {
  case Key::ArrowLeft:
    if (E.cursorX != 0)
      --E.cursorX;
    break;
  case Key::ArrowRight:
    if (E.cursorX != E.screenCols - 1)
      ++E.cursorX;
    break;
  case Key::ArrowUp:
    if (E.cursorY != 0)
      --E.cursorY;
    break;
  case Key::ArrowDown:
    if (E.cursorY != E.screenRows - 1)
      ++E.cursorY;
    break;
  }
}

char const *const KiloVersion = "0.0.1";

void editorDrawRows(AppendBuffer *ab) {
  for (int y = 0; y < E.screenRows; ++y) {
    if (y == E.screenRows / 3) {
      char welcome[80];
      int welcomeLength = snprintf(welcome, sizeof(welcome),
                                   "Kilo editor -- version %s", KiloVersion);
      if (welcomeLength > E.screenCols)
        welcomeLength = E.screenCols;

      int padding = (E.screenCols - welcomeLength) / 2;
      if (padding) {
        ab->append("~", 1);
        --padding;
      }
      while (padding--)
        ab->append(" ", 1);
      ab->append(welcome, welcomeLength);
    } else {
      ab->append("~", 1);
    }

    ab->append("\x1b[K", 3);
    if (y < E.screenRows - 1)
      ab->append("\r\n", 2);
  }
}

void editorRefreshScreen() {
  AppendBuffer ab;
  ab.append("\x1b[?25l", 6);
  ab.append("\x1b[H", 3);

  editorDrawRows(&ab);

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", E.cursorY + 1, E.cursorX + 1);
  ab.append(buffer, strlen(buffer));

  ab.append("\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.buffer, ab.length);
}

void editorProcessKeypress() {
  int c = editorReadKey();

  switch (c) {
  case addCtrl('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  case Key::Home:
    E.cursorX = 0;
    break;
  case Key::End:
    E.cursorX = E.screenCols - 1;
    break;
  case Key::ArrowUp:
    editorMoveCursor(c);
    break;
  case Key::ArrowDown:
    editorMoveCursor(c);
    break;
  case Key::ArrowLeft:
    editorMoveCursor(c);
    break;
  case Key::ArrowRight:
    editorMoveCursor(c);
    break;
  case Key::PageUp: {
    int times = E.screenRows;
    while (times--)
      editorMoveCursor(Key::ArrowUp);
  }
  case Key::PageDown: {
    int times = E.screenRows;
    while (times--)
      editorMoveCursor(Key::ArrowDown);
  }
  }
}

int main(int argc, char **argv) {
  if (!llvm::cl::ParseCommandLineOptions(argc, argv)) {
    llvm::cl::PrintOptionValues();
  }

  enableRawMode();
  initEditor();
  initControlLookup();

  while (true) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
