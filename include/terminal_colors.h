#ifndef _TERMINAL_COLORS_H
#define _TERMINAL_COLORS_H

#define TERM_COLOR_RED     "\x1b[31;1m"
#define TERM_COLOR_GREEN   "\x1b[32;1m"
#define TERM_COLOR_YELLOW  "\x1b[33;1m"
#define TERM_COLOR_BLUE    "\x1b[34;1m"
#define TERM_COLOR_MAGENTA "\x1b[35;1m"
#define TERM_COLOR_CYAN    "\x1b[36;1m"
#define TERM_COLOR_WHITE   "\x1b[37;1m"
#define TERM_COLOR_RESET   "\x1b[0m"

#define RED(str)           TERM_COLOR_RED str TERM_COLOR_RESET
#define GREEN(str)         TERM_COLOR_GREEN str TERM_COLOR_RESET
#define YELLOW(str)        TERM_COLOR_YELLOW str TERM_COLOR_RESET
#define BLUE(str)          TERM_COLOR_BLUE str TERM_COLOR_RESET
#define MAGENTA(str)       TERM_COLOR_MAGENTA str TERM_COLOR_RESET
#define CYAN(str)          TERM_COLOR_CYAN str TERM_COLOR_RESET
#define WHITE(str)         TERM_COLOR_WHITE str TERM_COLOR_RESET

#endif //_TERMINAL_COLORS_H