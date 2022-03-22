// vim: set ts=4:
// Copyright 2022 - present, Jakub Jirutka <jakub@jirutka.cz>.
// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <paths.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define PROGNAME "tty-copy"

#ifndef VERSION
  #define VERSION "0.2.2"
#endif

#define OP_CLEAR 'c'
#define OP_TEST 't'
#define OP_WRITE 'w'

#define ERR_GENERAL 1
#define ERR_WRONG_USAGE 10
#define ERR_IO 11

// The maximum length of an OSC 52 sequence is originally 100 000 bytes, of
// which 7 bytes is "\033]52;c;" header, 1 byte is "\a" footer, and 99 992
// bytes by the base64-encoded result of 74 994 bytes of copyable text.
#define OSC_SAFE_LIMIT 74994

#define logerr(format, ...) \
	fprintf(stderr, PROGNAME ": " format "\n", __VA_ARGS__)

typedef unsigned char uchar;

static const char * const help_msg =
	"Usage:\n"
	"  " PROGNAME " [options] text to copy\n"
	"  " PROGNAME " [options] < file-to-copy\n"
	"  " PROGNAME " (-t | -V | -h)\n"
	"\n"
	"Copy content to the system clipboard from anywhere via terminal that supports\n"
	"ANSI OSC 52 sequence.\n"
	"\n"
	"Options:\n"
	"  -c --clear         Instead of copying anything, clear the clipboard.\n"
	"  -n --trim-newline  Do not copy the trailing newline character.\n"
	"  -o --output FILE   Path of the terminal device (defaults to /dev/tty).\n"
	"  -p --primary       Use the \"primary\" clipboard (selection) instead of the\n"
	"                     regular clipboard.\n"
	"  -T --term TERM     Type of the terminal: (default), screen, or tmux.\n"
	"  -t --test          Test if your terminal processes OSC 52 sequence.\n"
	"  -V --version       Print program name & version and exit.\n"
	"  -h --help          Display this message and exit.\n"
	"\n"
	"Please report bugs at <https://github.com/jirutka/tty-copy/issues>\n";

// CLI options
static struct {
	char op;
	bool is_screen;
	bool is_tmux;
	bool primary;
	bool trim_newline;
	char *tty_path;
} opts = {0};

/**
 * Returns true if the given string `str` starts with the `prefix`.
 */
static bool str_startswith (const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

/**
 * Returns true if the given strings are equal.
 */
static bool str_equal (const char *str1, const char *str2) {
	return strcmp(str1, str2) == 0;
}

static void parse_opts (int argc, char * const *argv) {
	assert(argc > 0 && "given zero argc");

	const char *short_opts = "cno:pT:thV";
	static struct option long_opts[] = {
		{"clear"       , no_argument      , 0, 'c'},
		{"output"      , required_argument, 0, 'o'},
		{"primary"     , no_argument      , 0, 'p'},
		{"term"        , required_argument, 0, 'T'},
		{"test"        , no_argument      , 0, 't'},
		{"trim-newline", no_argument      , 0, 'n'},
		{"help"        , no_argument      , 0, 'h'},
		{"version"     , no_argument      , 0, 'V'},
		{0             , 0                , 0, 0  },
	};

	char *term_type = NULL;
	int optch;
	int optidx;
	while ((optch = getopt_long(argc, argv, short_opts, long_opts, &optidx)) != -1) {
		if (optch == 0) {
			optch = long_opts[optidx].val;
		}
		switch (optch) {
			case 'c':
				opts.op = OP_CLEAR;
				break;
			case 'n':
				opts.trim_newline = true;
				break;
			case 'o':
				opts.tty_path = strdup(optarg);
				break;
			case 'p':
				opts.primary = true;
				break;
			case 'T':
				term_type = strdup(optarg);
				break;
			case 't':
				opts.op = OP_TEST;
				break;
			case 'h':
				printf("%s", help_msg);
				exit(EXIT_SUCCESS);
			case 'V':
				puts(PROGNAME " " VERSION);
				exit(EXIT_SUCCESS);
			default:
				exit(ERR_WRONG_USAGE);
		}
	}

	if (!opts.op) {
		opts.op = OP_WRITE;
	}
	if (opts.tty_path == NULL) {
		opts.tty_path = _PATH_TTY;
	}

	const char *term = NULL;
	if (term_type != NULL) {
		opts.is_screen = str_equal(term_type, "screen");
		opts.is_tmux = str_equal(term_type, "tmux");

	} else if ((term = getenv("TERM")) != NULL) {
		if (str_startswith(term, "screen")) {
			// Since tmux defaults to setting TERM=screen (ugh), we need to detect
			// it here specially.
			if (getenv("TMUX") != NULL) {
				opts.is_tmux = true;
			} else {
				opts.is_screen = true;
			}
		} else if (str_startswith(term, "tmux")) {
			opts.is_tmux = true;
		}
	}
}

/**
 * Returns the exact size of the base64 encoded data as a function of the size
 * of the input data.
 */
static size_t base64_encoded_size (size_t size) {
	return (size + 2) / 3 * 4 + 1;
}

/**
 * Encodes given string to base64 (RFC 1341).
 *
 * @param src Pointer to the source of the data to be encoded.
 * @param srclen Length of the source data to be encoded.
 * @param dst Pointer to the destination buffer where the encoded data is to be
 *   written. It will be null-terminated.
 * @param dstlen Size of the `dst` buffer; only used to assert that the output
 *   buffer is large enough.
 * @return Length of the encoded data written to `dst` buffer.
 *
 * This code is based on base64 encoder from wpa_supplicant written by
 * Jouni Malinen <j@w1.fi> and distributed under the terms of the BSD-3-Clause
 * license.
 */
static size_t base64_encode (const uchar *src, size_t srclen, uchar *dst, size_t dstlen) {
	static const uchar b64_table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	                                   "abcdefghijklmnopqrstuvwxyz"
	                                   "0123456789+/";
	// We know how much we'll write, just make sure that there's space.
	assert(dstlen >= base64_encoded_size(srclen) \
		&& "not enough space provided for base64 encode");

	const uchar *end = src + srclen;
	const uchar *in = src;
	uchar *pos = dst;

	while (end - in >= 3) {
		*pos++ = b64_table[in[0] >> 2];
		*pos++ = b64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
		*pos++ = b64_table[((in[1] & 0x0f) << 2) | (in[2] >> 6)];
		*pos++ = b64_table[in[2] & 0x3f];
		in += 3;
	}

	if (end - in) {
		*pos++ = b64_table[in[0] >> 2];
		if (end - in == 1) {
			*pos++ = b64_table[(in[0] & 0x03) << 4];
			*pos++ = '=';
		} else {
			*pos++ = b64_table[((in[0] & 0x03) << 4) | (in[1] >> 4)];
			*pos++ = b64_table[(in[1] & 0x0f) << 2];
		}
		*pos++ = '=';
	}
	*pos = '\0';

	return pos - dst;
}

/**
 * Returns the next character in the input `stream`, as a value of type `int`,
 * without extracting it: the character is pushed back onto the stream so that
 * the this is available for the next read operation. If there are no more
 * characters to read in the stream, `EOF` is returned.
 */
static int fpeek (FILE *stream) {
	const int ch = fgetc(stream);
	ungetc(ch, stream);

	return ch;
}

/**
 * Returns the cursor position on the X-axis (column), or -1 on error.
 */
static int get_cursor_column (FILE *tty) {
	char buf[16] = {'\0'};

	if (fputs("\033[6n", tty) < 0) {
		return -1;
	}
	for (int i = 0, ch = 0; ch != 'R' && i < sizeof(buf) - 1; i++) {
		if ((ch = fgetc(tty)) < 0) {
			return -1;
		}
		buf[i] = ch;
	}

	int row = 0, col = 0;
	if (sscanf(buf, "\033[%d;%dR", &row, &col) != 2) {
		return -1;
	}
	return col;
}

/**
 * Changes the local modes of the terminal referred to by the open file descriptor `fd`.
 *
 * @param fd An open file descriptor associated with the terminal.
 * @param c_lflag The flags to be ANDed to `c_lflag` field in the `struct termios` structure.
 * @return 0 on success, -1 on error.
 */
static int term_change_local_modes (int fd, tcflag_t c_lflag) {
	struct termios term;

	if (tcgetattr(fd, &term) < 0) {
		return -1;
	}
	term.c_lflag &= c_lflag;
	return tcsetattr(fd, TCSANOW, &term);
}

int main (int argc, char * const *argv) {
	parse_opts(argc, argv);

	char seq_start[32];
	sprintf(seq_start, "%s\033]52;%c;",
		opts.is_tmux ? "\033Ptmux;\033" : "",
		opts.primary ? 'p' : 'c');

	const char *seq_end = opts.is_tmux ? "\a\033\\" : "\a";

	FILE *tty = NULL;
	if ((tty = fopen(opts.tty_path, "r+")) == NULL) {
		logerr("Failed to open %s: %s", opts.tty_path, strerror(errno));
		exit(ERR_IO);
	}
	int tty_fd = fileno(tty);

	struct termios term_restore;
	if (isatty(tty_fd)) {
		// Save the current terminal state so we can restore it later.
		tcgetattr(tty_fd, &term_restore);
		// Avoid mixing input with terminal output.
		term_change_local_modes(tty_fd, ~(CREAD | ECHO | ICANON));
	}

	// TODO: refactor this spaghetti

	int rc = EXIT_SUCCESS;
	if (opts.op == OP_TEST) {
		fputs("\0337", tty);  // save current terminal state

		int col = get_cursor_column(tty);
		fprintf(tty, "%s%s", seq_start, seq_end);
		int col2 = get_cursor_column(tty);

		if (col < 0 || col2 < 0 || col != col2) {
			fputs("\0338", tty);  // restore terminal state
			rc = ERR_GENERAL;
		}
	} else if (opts.op == OP_CLEAR) {
		fprintf(tty, "%s!%s", seq_start, seq_end);
		fflush(tty);

	} else {
		FILE *input = stdin;

		// There are remaining command line arguments, get the content from them
		// instead of stdin.
		if (argc > optind) {
			char buf[OSC_SAFE_LIMIT] = "\0";
			size_t buf_len = 0;

			for (int i = optind; i < argc; i++) {
				size_t arg_len = strlen(argv[i]);

				if (buf_len + arg_len + 2 > sizeof(buf)) {
					logerr("Command line is too long (limit is %lu bytes)", sizeof(buf));
					rc = ERR_GENERAL;
					goto done;
				}
				if (buf_len > 0) {
					buf[buf_len++] = ' ';
				}
				(void) strcpy(buf + buf_len, argv[i]);

				buf[(buf_len += arg_len)] = '\0';
			}
			input = fmemopen(buf, buf_len, "r");
		}

		// Screen limits the length of string sequences, so we have to break it
		// up to chunks of max 768 bytes.
		// 2048 * 3 bytes for others is just an arbitrary number.
		// IMPORTANT: The buffer size must be divisable by 3 (because of base64)!
		uchar read_buf[(opts.is_screen ? 254 : 2048) * 3];
		uchar enc_buf[base64_encoded_size(sizeof(read_buf))];

		bool write_header = true;
		size_t read_len = 0;
		size_t input_len = 0;
		while ((read_len = fread(read_buf, 1, sizeof(read_buf), input)) > 0) {
			input_len += read_len;

			if (opts.trim_newline && fpeek(input) == EOF && read_buf[read_len -1] == '\n') {
				if (--read_len == 0) {
					break;
				}
			}
			if (opts.is_screen) {
				fputs("\033P", tty);
			}
			if (write_header) {
				fputs(seq_start, tty);
				write_header = false;
			}
			size_t len = base64_encode(read_buf, read_len, enc_buf, sizeof(enc_buf));
			if (fwrite(enc_buf, 1, len, tty) != len) {
				rc = ERR_IO;
				break;
			}
			if (opts.is_screen) {
				fputs("\033\\", tty);
			}
		}
		fputs(seq_end, tty);
		fflush(tty);

		if (ferror(input)) {
			logerr("/dev/stdin: read error: %s", strerror(errno));
			rc = ERR_IO;
		}
		if (input_len > OSC_SAFE_LIMIT) {
			logerr("warning: Input size (%lu kiB) exceeded %d kiB, it may be truncated by some terminals",
				input_len / 1024, OSC_SAFE_LIMIT / 1024);
		}
	}

	if (ferror(tty)) {
		logerr("%s: write error: %s", opts.tty_path, strerror(errno));
		rc = ERR_IO;
	}

done:
	if (isatty(tty_fd)) {
		tcsetattr(tty_fd, TCSANOW, &term_restore);
	}

	return rc;
}
