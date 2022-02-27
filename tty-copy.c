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

#define PROGNAME "tty-copy"

#ifndef VERSION
  #define VERSION "0.1.0"
#endif

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
	"  " PROGNAME " (-V | -h)\n"
	"\n"
	"Copy content to the system clipboard from anywhere via terminal that supports\n"
	"ANSI OSC 52 sequence.\n"
	"\n"
	"Options:\n"
	"  -c --clear         Instead of copying anything, clear the clipboard.\n"
	"  -n --trim-newline  Do not copy the trailing newline character.\n"
	"  -o --output FILE   Write to FILE instead of /dev/tty.\n"
	"  -p --primary       Use the \"primary\" clipboard (selection) instead of the\n"
	"                     regular clipboard.\n"
	"  -T --term TERM     Type of the terminal: (default), screen, or tmux.\n"
	"  -V --version       Print program name & version and exit.\n"
	"  -h --help          Display this message and exit.\n"
	"\n"
	"Homepage: https://github.com/jirutka/tty-copy\n";

// CLI options
static struct {
	bool clear;
	bool is_screen;
	bool is_tmux;
	char *out_file;
	bool primary;
	bool trim_newline;
} opts;

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

	const char *short_opts = "cno:pT:hV";
	static struct option long_opts[] = {
		{"clear"       , no_argument      , 0, 'c'},
		{"output"      , required_argument, 0, 'o'},
		{"primary"     , no_argument      , 0, 'p'},
		{"term"        , required_argument, 0, 'T'},
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
				opts.clear = true;
				break;
			case 'n':
				opts.trim_newline = true;
				break;
			case 'o':
				opts.out_file = strdup(optarg);
				break;
			case 'p':
				opts.primary = true;
				break;
			case 'T':
				term_type = strdup(optarg);
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

	if (opts.out_file == NULL) {
		opts.out_file = _PATH_TTY;
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

int main (int argc, char * const *argv) {
	parse_opts(argc, argv);

	char seq_start[32];
	sprintf(seq_start, "%s\033]52;%c;",
		opts.is_tmux ? "\033Ptmux;\033" : "",
		opts.primary ? 'p' : 'c');

	const char *seq_end = opts.is_tmux ? "\a\033\\" : "\a";

	FILE *out = NULL;
	if ((out = fopen(opts.out_file, "w")) == NULL) {
		logerr("Failed to open %s for writing: %s", opts.out_file, strerror(errno));
		exit(ERR_IO);
	}

	int rc = EXIT_SUCCESS;
	if (opts.clear) {
		fprintf(out, "%s!%s", seq_start, seq_end);
		fflush(out);

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
					exit(ERR_GENERAL);
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
				fputs("\033P", out);
			}
			if (write_header) {
				fputs(seq_start, out);
				write_header = false;
			}
			size_t len = base64_encode(read_buf, read_len, enc_buf, sizeof(enc_buf));
			if (fwrite(enc_buf, 1, len, out) != len) {
				rc = ERR_IO;
				break;
			}
			if (opts.is_screen) {
				fputs("\033\\", out);
			}
		}
		fputs(seq_end, out);
		fflush(out);

		if (ferror(input)) {
			logerr("/dev/stdin: read error: %s", strerror(errno));
			rc = ERR_IO;
		}
		if (input_len > OSC_SAFE_LIMIT) {
			logerr("warning: Input size (%lu kiB) exceeded %d kiB, it may be truncated by some terminals",
				input_len / 1024, OSC_SAFE_LIMIT / 1024);
		}
	}

	if (ferror(out)) {
		logerr("%s: write error: %s", opts.out_file, strerror(errno));
		rc = ERR_IO;
	}

	return rc;
}
