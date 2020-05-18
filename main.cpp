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

struct termios originalTermios;

void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode() {
 if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios) == -1)
   die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &originalTermios) == -1)
    die("tcgetattr");

  atexit(disableRawMode);

  struct termios raw = originalTermios;

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
  // Causes input not to be buffered -- e.g. the char is sent right away without
  // return being pressed
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

inline char addCtrl(char c) {
  return c & 0x1f;
}

int main(int argc, char **argv) {
  if (!llvm::cl::ParseCommandLineOptions(argc, argv)) {
    llvm::cl::PrintOptionValues();
  }

  enableRawMode();
  initControlLookup();

  while (true) {
    char c = 0;
    int result = read(STDIN_FILENO, &c, 1);
    if (result == -1) {
      if (errno != EAGAIN)
        die("read");
      else
        continue;
    }
    if (c == 0)
      continue;

    if (iscntrl(c)) {
      auto val = controlLookup[c];
      if (val != 0)
        printf("%d ('%s')\r\n", c, controlLookup[c]);
      else
        printf("%d\r\n", c);
    } else
      printf("%d ('%c')\r\n", c, c);

    if (c == addCtrl('q'))
      break;
  }

  return 0;
}
