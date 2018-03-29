/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 * Copyright (C) 2018 Rafik Mikhael <rmikhael@ellation.com>
 *
 * This file was part of libass. Modified and updated for use with nginx-vod-module.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */



#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include <ctype.h>
#include <float.h>

#define ASS_SCRIPT_INFO_HEADER ("[Script Info]")

#define FIXED_WEBVTT_CUE_NAME_WIDTH 8
#define FIXED_WEBVTT_CUE_FORMAT_STR "c%07d"
#define FIXED_WEBVTT_STYLE_START_WIDTH 22
#define FIXED_WEBVTT_STYLE_START_STR "STYLE\r\n::cue(v[voice=\""
#define FIXED_WEBVTT_STYLE_END_WIDTH 4
#define FIXED_WEBVTT_STYLE_END_STR "\"]) "
#define FIXED_WEBVTT_BRACES_START_WIDTH 3
#define FIXED_WEBVTT_BRACES_START_STR "{\r\n"
#define FIXED_WEBVTT_BRACES_END_WIDTH 3
#define FIXED_WEBVTT_BRACES_END_STR "}\r\n"
#define FIXED_WEBVTT_VOICE_START_STR  "<v "
#define FIXED_WEBVTT_VOICE_START_WIDTH  3
#define FIXED_WEBVTT_VOICE_END_STR  ">"
#define FIXED_WEBVTT_VOICE_END_WIDTH  1
#define FIXED_WEBVTT_VOICE_SPANEND_STR  "</v>"
#define FIXED_WEBVTT_VOICE_SPANEND_WIDTH  4

// ignore this set for now, till we see how to support inline tags for color/shadow/outline/background
#define FIXED_WEBVTT_CLASS_NOITALIC  "STYLE\r\n::cue(.noitalic) {\r\nfont-style: normal;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_NOITALIC_WIDTH  53
#define FIXED_WEBVTT_CLASS_ITALIC    "STYLE\r\n::cue(.italic) {\r\nfont-style: italic;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_ITALIC_WIDTH  51
#define FIXED_WEBVTT_CLASS_NOBOLD    "STYLE\r\n::cue(.nobold) {\r\nfont-weight: normal;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_NOBOLD_WIDTH  52
#define FIXED_WEBVTT_CLASS_BOLD      "STYLE\r\n::cue(.bold) {\r\nfont-weight: bold;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_BOLD_WIDTH  48
#define FIXED_WEBVTT_CLASS_NOUNDER    "STYLE\r\n::cue(.nounder) {\r\ntext-decoration: none;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_NOUNDER_WIDTH  55
#define FIXED_WEBVTT_CLASS_UNDER      "STYLE\r\n::cue(.under) {\r\ntext-decoration: solid underline;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_UNDER_WIDTH  64
#define FIXED_WEBVTT_CLASS_NOSTRIKE    "STYLE\r\n::cue(.nostrike) {\r\ntext-decoration: none;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_NOSTRIKE_WIDTH  56
#define FIXED_WEBVTT_CLASS_STRIKE      "STYLE\r\n::cue(.strike) {\r\ntext-decoration: solid line-through;\r\n}\r\n\r\n"
#define FIXED_WEBVTT_CLASS_STRIKE_WIDTH  68

#define VALIGN_SUB 0
#define VALIGN_CENTER 8
#define VALIGN_TOP 4

#define ASS_STYLES_ALLOC 20
#define ASS_SIZE_MAX ((size_t)-1)
#define MAX_STR_SIZE_EVNT_CHUNK 1024
#define MAX_STR_SIZE_ALL_WEBVTT_STYLES 20480

#define ass_atof(STR) (ass_strtod((STR),NULL))

#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMINMAX(c,a,b) FFMIN(FFMAX(c, a), b)

#define NUM_OF_TAGS_ALLOWED_PER_LINE 1

//#define TEMP_VERBOSITY
#define ASSUME_STYLE_SUPPORT

typedef enum {
// all starts should be in even index, all ends should be in odd index. This logic is assumed
    TAG_TYPE_NEWLINE_SMALL  = 0,
    TAG_TYPE_NEWLINE_LARGE  = 1,
    TAG_TYPE_AMPERSANT      = 2,
    TAG_TYPE_SMALLERTHAN    = 3,
    TAG_TYPE_BIGGERTHAN     = 4,

    TAG_TYPE_OPEN_BRACES    = 5,
    TAG_TYPE_CLOSE_BRACES   = 6,

    TAG_TYPE_ITALIC_END     = 7,
    TAG_TYPE_ITALIC_START   = 8,
    TAG_TYPE_BOLD_END       = 9,
    TAG_TYPE_BOLD_START     = 10,
    TAG_TYPE_UNDER_END      = 11,
    TAG_TYPE_UNDER_START    = 12,

    TAG_TYPE_UNKNOWN_TAG    = 13, // has to be after all known braces types
    TAG_TYPE_NONE           = 14
} ass_tag_idx_t;
static const char* const tag_strings[TAG_TYPE_NONE] = {
    "\\n",
    "\\N",
    "&",
    "<",
    ">",
	
    "{",
    "}",

    "\\i0",
    "\\i",
    "\\b0",
    "\\b",
    "\\u0",
    "\\u",

    "\\"
};
static const int tag_string_len[TAG_TYPE_NONE][2] = {
    // index 0 is size of ASS tag, index 1 is size of replacement webVTT tag
    {2,2},
    {2,2},
    {1,5},
    {1,4},
    {1,4},

    {1,0},
    {1,0},

    {3,4},
    {2,3},
    {3,4},
    {2,3},
    {3,4},
    {2,3},

    {1,0}
};
static const char* tag_replacement_strings[TAG_TYPE_NONE] = {
    "\r\n",
    "\r\n",
    "&amp;",
    "&lt;",
    "&gt;",

    "",
    "",

    "</i>",
    "<i>",
    "</b>",
    "<b>",
    "</u>",
    "<u>",

    ""
};


static const unsigned char lowertab[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
    0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x61,
    0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
    0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d,
    0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
    0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
    0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
    0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
    0xfd, 0xfe, 0xff
};

char *ass_strndup(const char *s, size_t n)
{
    char *end = memchr(s, 0, n);
    int len = end ? end - s : (int)(n);
    char *newp = len < (int)(ASS_SIZE_MAX) ? malloc(len + 1) : NULL;
    if (newp) {
        vod_memcpy(newp, s, len);
        newp[len] = 0;
    }
    return newp;
}

static int ass_strcasecmp(const char *s1, const char *s2)
{
    unsigned char a, b;

    do {
        a = lowertab[(unsigned char)*s1++];
        b = lowertab[(unsigned char)*s2++];
    } while (a && a == b);

    return a - b;
}


static int ass_strncasecmp(const char *s1, const char *s2, size_t n)
{
    unsigned char a, b;
    const char *last = s1 + n;

    do {
        a = lowertab[(unsigned char)*s1++];
        b = lowertab[(unsigned char)*s2++];
    } while (s1 < last && a && a == b);

    return a - b;
}

void skip_spaces(char **str)
{
    char *p = *str;
    while ((*p == ' ') || (*p == '\t'))
        ++p;
    *str = p;
}

void rskip_spaces(char **str, char *limit)
{
    char *p = *str;
    while ((p > limit) && ((p[-1] == ' ') || (p[-1] == '\t')))
        --p;
    *str = p;
}

static inline uint32_t ass_bswap32(uint32_t x)
{
#ifdef _MSC_VER
    return _byteswap_ulong(x);
#else
    return (x & 0x000000FF) << 24 | (x & 0x0000FF00) <<  8 |
           (x & 0x00FF0000) >>  8 | (x & 0xFF000000) >> 24;
#endif
}

static inline int ass_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
           c == '\f' || c == '\r';
}

static inline int ass_isdigit(int c)
{
    return c >= '0' && c <= '9';
}


static
const size_t maxExponent = 511; /* Largest possible base 10 exponent.  Any
                                 * exponent larger than this will already
                                 * produce underflow or overflow, so there's
                                 * no need to worry about additional digits.
                                 */

static
const double powersOf10[] = {   /* Table giving binary powers of 10.  Entry */
    10.,                        /* is 10^2^i.  Used to convert decimal */
    100.,                       /* exponents into floating-point numbers. */
    1.0e4,
    1.0e8,
    1.0e16,
    1.0e32,
    1.0e64,
    1.0e128,
    1.0e256
};

static
const double negPowOf10[] = {   /* Table giving negative binary powers */
    0.1,                        /* of 10.  Entry is 10^-2^i. */
    0.01,                       /* Used to convert decimal exponents */
    1.0e-4,                     /* into floating-point numbers. */
    1.0e-8,
    1.0e-16,
    1.0e-32,
    1.0e-64,
    1.0e-128,
    1.0e-256
};

/*
 *----------------------------------------------------------------------
 *
 * strtod --
 *
 * This procedure converts a floating-point number from an ASCII
 * decimal representation to internal double-precision format.
 *
 * Results:
 * The return value is the double-precision floating-point
 * representation of the characters in string.  If endPtr isn't
 * NULL, then *endPtr is filled in with the address of the
 * next character after the last one that was part of the
 * floating-point number.
 *
 * Side effects:
 * None.
 *
 *----------------------------------------------------------------------
 */

double
ass_strtod(
    const char *string,     /* A decimal ASCII floating-point number,
                             * optionally preceded by white space.
                             * Must have form "-I.FE-X", where I is the
                             * integer part of the mantissa, F is the
                             * fractional part of the mantissa, and X
                             * is the exponent.  Either of the signs
                             * may be "+", "-", or omitted.  Either I
                             * or F may be omitted, or both.  The decimal
                             * point isn't necessary unless F is present.
                             * The "E" may actually be an "e".  E and X
                             * may both be omitted (but not just one).
                             */
    char **endPtr           /* If non-NULL, store terminating character's
                             * address here. */
    )
{
    int sign, fracExpSign, expSign;
    double fraction, dblExp;
    const double *d;
    register const char *p;
    register int c;
    size_t exp = 0;         /* Exponent read from "EX" field. */
    size_t fracExp;         /* Exponent that derives from the fractional
                             * part.  Under normal circumstatnces, it is
                             * the negative of the number of digits in F.
                             * However, if I is very long, the last digits
                             * of I get dropped (otherwise a long I with a
                             * large negative exponent could cause an
                             * unnecessary overflow on I alone).  In this
                             * case, fracExp is incremented one for each
                             * dropped digit. */
    size_t mantSize;    /* Number of digits in mantissa. */
    size_t decPt;       /* Number of mantissa digits BEFORE decimal
                         * point. */
    size_t leadZeros;   /* Number of leading zeros in mantissa. */
    const char *pExp;       /* Temporarily holds location of exponent
                             * in string. */

    /*
     * Strip off leading blanks and check for a sign.
     */

    p = string;
    while (ass_isspace(*p)) {
        p += 1;
    }
    if (*p == '-') {
        sign = 1;
        p += 1;
    } else {
        if (*p == '+') {
            p += 1;
        }
        sign = 0;
    }

    /*
     * Count the number of digits in the mantissa (including the decimal
     * point), and also locate the decimal point.
     */

    decPt = -1;
    leadZeros = -1;
    for (mantSize = 0; ; mantSize += 1)
    {
        c = *p;
        if (!ass_isdigit(c)) {
            if ((c != '.') || (decPt != (size_t) -1)) {
                break;
            }
            decPt = mantSize;
        } else if ((c != '0') && (leadZeros == (size_t) -1)) {
            leadZeros = mantSize;
        }
        p += 1;
    }

    /*
     * Now suck up the digits in the mantissa.  Use two integers to
     * collect 9 digits each (this is faster than using floating-point).
     * If the mantissa has more than 18 digits, ignore the extras, since
     * they can't affect the value anyway.
     */

    if (leadZeros == (size_t) -1) {
        leadZeros = mantSize;
    }
    pExp  = p;
    p -= mantSize - leadZeros;
    if (decPt == (size_t) -1) {
        decPt = mantSize;
    } else {
        mantSize -= 1;      /* One of the digits was the point. */
        if (decPt < leadZeros) {
            leadZeros -= 1;
        }
    }
    if (mantSize - leadZeros > 18) {
        mantSize = leadZeros + 18;
    }
    if (decPt < mantSize) {
        fracExpSign = 1;
        fracExp = mantSize - decPt;
    } else {
        fracExpSign = 0;
        fracExp = decPt - mantSize;
    }
    if (mantSize == 0) {
        fraction = 0.0;
        p = string;
        goto done;
    } else {
        int frac1, frac2, m;
        mantSize -= leadZeros;
        m = mantSize;
        frac1 = 0;
        for ( ; m > 9; m -= 1)
        {
            c = *p;
            p += 1;
            if (c == '.') {
                c = *p;
                p += 1;
            }
            frac1 = 10*frac1 + (c - '0');
        }
        frac2 = 0;
        for (; m > 0; m -= 1)
        {
            c = *p;
            p += 1;
            if (c == '.') {
                c = *p;
                p += 1;
            }
            frac2 = 10*frac2 + (c - '0');
        }
        fraction = (1.0e9 * frac1) + frac2;
    }

    /*
     * Skim off the exponent.
     */

    p = pExp;
    if ((*p == 'E') || (*p == 'e')) {
        size_t expLimit;    /* If exp > expLimit, appending another digit
                             * to exp is guaranteed to make it too large.
                             * If exp == expLimit, this may depend on
                             * the exact digit, but in any case exp with
                             * the digit appended and fracExp added will
                             * still fit in size_t, even if it does
                             * exceed maxExponent. */
        int expWraparound = 0;
        p += 1;
        if (*p == '-') {
            expSign = 1;
            p += 1;
        } else {
            if (*p == '+') {
                p += 1;
            }
            expSign = 0;
        }
        if (expSign == fracExpSign) {
            if (maxExponent < fracExp) {
                expLimit = 0;
            } else {
                expLimit = (maxExponent - fracExp) / 10;
            }
        } else {
            expLimit = fracExp / 10 + (fracExp % 10 + maxExponent) / 10;
        }
        while (ass_isdigit(*p)) {
            if ((exp > expLimit) || expWraparound) {
                do {
                    p += 1;
                } while (ass_isdigit(*p));
                goto expOverflow;
            } else if (exp > ((size_t) -1 - (*p - '0')) / 10) {
                expWraparound = 1;
            }
            exp = exp * 10 + (*p - '0');
            p += 1;
        }
        if (expSign == fracExpSign) {
            exp = fracExp + exp;
        } else if ((fracExp <= exp) || expWraparound) {
            exp = exp - fracExp;
        } else {
            exp = fracExp - exp;
            expSign = fracExpSign;
        }
    } else {
        exp = fracExp;
        expSign = fracExpSign;
    }

    /*
     * Generate a floating-point number that represents the exponent.
     * Do this by processing the exponent one bit at a time to combine
     * many powers of 2 of 10. Then combine the exponent with the
     * fraction.
     */

    if (exp > maxExponent) {
expOverflow:
        exp = maxExponent;
        if (fraction != 0.0) {
            errno = ERANGE;
        }
    }
    /* Prefer positive powers of 10 for increased precision, especially
     * for small powers that are represented exactly in floating-point. */
    if ((exp <= DBL_MAX_10_EXP) || !expSign) {
        d = powersOf10;
    } else {
        /* The floating-point format supports more negative exponents
         * than positive, or perhaps the result is a subnormal number. */
        if (exp > -DBL_MIN_10_EXP) {
            /* The result might be a valid subnormal number, but the
             * exponent underflows.  Tweak fraction so that it is below
             * 1.0 first, so that if the exponent still underflows after
             * that, the result is sure to underflow as well. */
            exp -= mantSize;
            dblExp = 1.0;
            for (d = powersOf10; mantSize != 0; mantSize >>= 1, d += 1) {
                if (mantSize & 01) {
                    dblExp *= *d;
                }
            }
            fraction /= dblExp;
        }
        d = negPowOf10;
        expSign = 0;
    }
    dblExp = 1.0;
    for (; exp != 0; exp >>= 1, d += 1) {
        if (exp & 01) {
            dblExp *= *d;
        }
    }
    if (expSign) {
        fraction /= dblExp;
    } else {
        fraction *= dblExp;
    }

done:
    if (endPtr != NULL) {
        *endPtr = (char *) p;
    }

    if (sign) {
        return -fraction;
    }
    return fraction;
}

int mystrtoi(char **p, int *res)
{
    char *start = *p;
    double temp_res = ass_strtod(*p, p);
    *res = (int) (temp_res + (temp_res > 0 ? 0.5 : -0.5));
    return *p != start;
}

int mystrtoll(char **p, long long *res)
{
    char *start = *p;
    double temp_res = ass_strtod(*p, p);
    *res = (long long) (temp_res + (temp_res > 0 ? 0.5 : -0.5));
    return *p != start;
}

int mystrtod(char **p, double *res)
{
    char *start = *p;
    *res = ass_strtod(*p, p);
    return *p != start;
}

int mystrtoi32(char **p, int base, int32_t *res)
{
    char *start = *p;
    long long temp_res = strtoll(*p, p, base);
    *res = FFMINMAX(temp_res, INT32_MIN, INT32_MAX);
    return *p != start;
}

static int read_digits(char **str, int base, uint32_t *res)
{
    char *p = *str;
    char *start = p;
    uint32_t val = 0;

    while (1) {
        int digit;
        if (*p >= '0' && *p < FFMIN(base, 10) + '0')
            digit = *p - '0';
        else if (*p >= 'a' && *p < base - 10 + 'a')
            digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p < base - 10 + 'A')
            digit = *p - 'A' + 10;
        else
            break;
        val = val * base + digit;
        ++p;
    }

    *res = val;
    *str = p;
    return p != start;
}

/*
 * \brief Convert a string to an integer reduced modulo 2**32
 * Follows the rules for strtoul but reduces the number modulo 2**32
 * instead of saturating it to 2**32 - 1.
 */
static int mystrtou32_modulo(char **p, int base, uint32_t *res)
{
    // This emulates scanf with %d or %x format as it works on
    // Windows, because that's what is used by VSFilter. In practice,
    // scanf works the same way on other platforms too, but
    // the standard leaves its behavior on overflow undefined.

    // Unlike scanf and like strtoul, produce 0 for invalid inputs.

    char *start = *p;
    int sign = 1;

    skip_spaces(p);

    if (**p == '+')
        ++*p;
    else if (**p == '-')
        sign = -1, ++*p;

    if (base == 16 && !ass_strncasecmp(*p, "0x", 2))
        *p += 2;

    if (read_digits(p, base, res)) {
        *res *= sign;
        return 1;
    } else {
        *p = start;
        return 0;
    }
}
int32_t parse_alpha_tag(char *str)
{
    int32_t alpha = 0;

    while (*str == '&' || *str == 'H')
        ++str;

    mystrtoi32(&str, 16, &alpha);
    return alpha;
}

uint32_t parse_color_tag(char *str)
{
    int32_t color = 0;

    while (*str == '&' || *str == 'H')
        ++str;

    mystrtoi32(&str, 16, &color);
    return ass_bswap32((uint32_t) color);
}

uint32_t parse_color_header(char *str)
{
    uint32_t color = 0;
    int base;

    if (!ass_strncasecmp(str, "&h", 2) || !ass_strncasecmp(str, "0x", 2)) {
        str += 2;
        base = 16;
    } else
        base = 10;

    mystrtou32_modulo(&str, base, &color);
    return ass_bswap32(color);
}

// Return a boolean value for a string
char parse_bool(char *str)
{
    skip_spaces(&str);
    return !ass_strncasecmp(str, "yes", 3) || strtol(str, NULL, 10) > 0;
}

/******* End of Utilities. Start of ASS specific structs ********/


/* ASS Style: line */
typedef struct ass_style {
    char       *Name;
    char       *FontName;
    int         FontSize;
    uint32_t    PrimaryColour;
    uint32_t    SecondaryColour;
    uint32_t    OutlineColour;
    uint32_t    BackColour;
    int         Bold;
    int         Italic;
    int         Underline;
    int         StrikeOut;
    double      ScaleX;
    double      ScaleY;
    double      Spacing;
    double      Angle;
    int         BorderStyle;
    int         Outline;
    int         Shadow;
    int         Alignment;
    int         MarginL;
    int         MarginR;
    int         MarginV;
    int         Encoding;
    int         treat_fontname_as_pattern;
    int         Justify;
} ass_style_t;

/*
 * ass_event corresponds to a single Dialogue line;
 * text is stored as-is, style overrides will be parsed later.
 */
typedef struct ass_event {
    long long   Start;    // ms
    long long   End;      // ms

    //int         ReadOrder;
    int         Layer;
    int         Style;
    char       *Name;
    int         MarginL;
    int         MarginR;
    int         MarginV;
    char       *Effect;
    char       *Text;
} ass_event_t;

typedef enum {
    PST_UNKNOWN = 0,
    PST_INFO,
    PST_STYLES,
    PST_EVENTS,
    PST_FONTS
} ParserState;


/*
 * ass_track represent either an external script or a matroska subtitle stream
 * (no real difference between them); it can be used in rendering after the
 * headers are parsed (i.e. events format line read).
 */
typedef struct ass_track {
    int             n_styles;           // amount used
    int             max_styles;         // amount allocated
    int             n_events;
    int             max_events;
    ass_style_t    *styles;             // array of styles, max_styles length, n_styles used
    ass_event_t    *events;             // the same as styles

    char           *style_format;       // style format line (everything after "Format: ")
    char           *event_format;       // event format line

    enum {
        TRACK_TYPE_UNKNOWN = 0,
        TRACK_TYPE_ASS,
        TRACK_TYPE_SSA
    } track_type;

    // Script header fields
    int             PlayResX;
    int             PlayResY;
    double          Timer;
    int             WrapStyle;
    int             ScaledBorderAndShadow;
    int             Kerning;
    char           *Language;

    int             default_style;    // index of default style, defaults to zero
    char           *name;             // file name in case of external subs, 0 for streams
    ParserState     state;

    long long       maxDuration;      // ms, added for needs of the vod-module

} ass_track_t;


int ass_alloc_style(ass_track_t *track)
{
    int sid;

    //assert(track->n_styles <= track->max_styles);

    if (track->n_styles == track->max_styles) {
        track->max_styles += ASS_STYLES_ALLOC;
        track->styles =
            (ass_style_t *) realloc(track->styles,
                                    sizeof(ass_style_t) *
                                    track->max_styles);
    }

    sid = track->n_styles++;
    vod_memset(track->styles + sid, 0, sizeof(ass_style_t));
    return sid;
}

int ass_alloc_event(ass_track_t *track)
{
    int eid;

    //assert(track->n_events <= track->max_events);

    if (track->n_events == track->max_events) {
        track->max_events = track->max_events * 2 + 1;
        track->events =
            (ass_event_t *) realloc(track->events,
                                    sizeof(ass_event_t) *
                                    track->max_events);
    }

    eid = track->n_events++;
    vod_memzero(track->events + eid, sizeof(ass_event_t));
    return eid;
}
void ass_free_event(ass_track_t *track, int eid)
{
    ass_event_t *event = track->events + eid;

    free(event->Name);
    free(event->Effect);
    free(event->Text);
}
void ass_free_style(ass_track_t *track, int sid)
{
    ass_style_t *style = track->styles + sid;

    free(style->Name);
    free(style->FontName);
}

void ass_free_track(vod_pool_t* pool, ass_track_t *track)
{
    int i;

    free(track->style_format);
    free(track->event_format);
    free(track->Language);
    if (track->styles) {
        for (i = 0; i < track->n_styles; ++i)
            ass_free_style(track, i);
    }
    free(track->styles);
    if (track->events) {
        for (i = 0; i < track->n_events; ++i)
            ass_free_event(track, i);
    }
    free(track->events);
    free(track->name);
    vod_free(pool, track);
}



/**
 * \brief Set up default style
 * \param style style to edit to defaults
 * The parameters are mostly taken directly from VSFilter source for
 * best compatibility.
 */
static void set_default_style(ass_style_t *style)
{
    style->Name             = strdup("Default");
    style->FontName         = strdup("Arial");
    style->FontSize         = 18;
    style->PrimaryColour    = 0xffffff00;
    style->SecondaryColour  = 0x00ffff00;
    style->OutlineColour    = 0x00000000;
    style->BackColour       = 0x00000080;
    style->Bold             = 200;
    style->ScaleX           = 1.0;
    style->ScaleY           = 1.0;
    style->Spacing          = 0;
    style->BorderStyle      = 1;
    style->Outline          = 2;
    style->Shadow           = 3;
    style->Alignment        = 2;
    style->MarginL = style->MarginR = style->MarginV = 20;
}

static long long string2timecode(char *p)
{
    int h, m, s, ms;
    long long tm;
    int res = sscanf(p, "%d:%d:%d.%d", &h, &m, &s, &ms);
    if (res < 4) {
        // error msg "Bad timestamp";
        return 0;
    }
    tm = ((h * 60LL + m) * 60 + s) * 1000 + ms * 10LL;
    return tm;
}
/**
 * \brief find style by name
 * \param track track
 * \param name style name
 * \return index in track->styles
 * Returns 0 if no styles found => expects at least 1 style.
 * Parsing code always adds "Default" style in the beginning.
 */
int lookup_style(ass_track_t *track, char *name)
{
    int i;
    // '*' seem to mean literally nothing;
    // VSFilter removes them as soon as it can
    while (*name == '*')
        ++name;
    // VSFilter then normalizes the case of "Default"
    // (only in contexts where this function is called)
    if (ass_strcasecmp(name, "Default") == 0)
        name = "Default";
    for (i = track->n_styles - 1; i >= 0; --i) {
        if (strcmp(track->styles[i].Name, name) == 0)
            return i;
    }
    i = track->default_style;
    return i;
}

#define NEXT(str,token) \
    token = next_token(&str); \
    if (!token) break;


#define ALIAS(alias,name) \
        if (ass_strcasecmp(tname, #alias) == 0) {tname = #name;}

/* One section started with PARSE_START and PARSE_END parses a single token
 * (contained in the variable named token) for the header indicated by the
 * variable tname. It does so by chaining a number of else-if statements, each
 * of which checks if the tname variable indicates that this header should be
 * parsed. The first parameter of the macro gives the name of the header.
 *
 * The string that is passed is in str. str is advanced to the next token if
 * a header could be parsed. The parsed results are stored in the variable
 * target, which has the type ass_style_t* or ass_event_t*.
 */
#define PARSE_START if (0) {
#define PARSE_END   }

#define ANYVAL(name,func) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        target->name = func(token);

#define STRVAL(name) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        if (target->name != NULL) free(target->name); \
        target->name = strdup(token);

#define STARREDSTRVAL(name) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        if (target->name != NULL) free(target->name); \
        while (*token == '*') ++token; \
        target->name = strdup(token);

#define COLORVAL(name) ANYVAL(name,parse_color_header)
#define INTVAL(name) ANYVAL(name,atoi)
#define FPVAL(name) ANYVAL(name,ass_atof)
#define TIMEVAL(name) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        target->name = string2timecode(token);

#define STYLEVAL(name) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        target->name = lookup_style(track, token);

static char *next_token(char **str)
{
    char *p = *str;
    char *start;
    skip_spaces(&p);
    if (*p == '\0') {
        *str = p;
        return 0;
    }
    start = p;                  // start of the token
    for (; (*p != '\0') && (*p != ','); ++p) {
    }
    if (*p == '\0') {
        *str = p;               // eos found, str will point to '\0' at exit
    } else {
        *p = '\0';
        *str = p + 1;           // ',' found, str will point to the next char (beginning of the next token)
    }
    rskip_spaces(&p, start);    // end of current token: the first space character, or '\0'
    *p = '\0';
    return start;
}

/**
 * \brief Parse the tail of Dialogue line
 * \param track track
 * \param event parsed data goes here
 * \param str string to parse, zero-terminated
*/
static int process_event_tail(ass_track_t *track, ass_event_t *event, char *str)
{
    char *token;
    char *tname;
    char *p = str;
    ass_event_t *target = event;

    char *format = strdup(track->event_format);
    char *q = format;           // format scanning pointer

    if (track->n_styles == 0) {
        // add "Default" style to the end
        // will be used if track does not contain a default style (or even does not contain styles at all)
        int sid = ass_alloc_style(track);
        set_default_style(&track->styles[sid]);
        track->default_style = sid;
    }

    while (1) {
        NEXT(q, tname);
        if (ass_strcasecmp(tname, "Text") == 0) {
            char *last;
            event->Text = strdup(p);
            if (*event->Text != 0) {
                last = event->Text + strlen(event->Text) - 1;
                if (last >= event->Text && *last == '\r')
                    *last = 0;
            }
            // need to track the largest end time in all events, since they are not in chronological order
            if (track->maxDuration < event->End) {
                track->maxDuration = event->End;
            }
            free(format);
            return 0;           // "Text" is always the last
        }
        NEXT(p, token);

        PARSE_START
            INTVAL(Layer)
            STYLEVAL(Style)
            STRVAL(Name)
            STRVAL(Effect)
            INTVAL(MarginL)
            INTVAL(MarginR)
            INTVAL(MarginV)
            TIMEVAL(Start)
            TIMEVAL(End)
        PARSE_END
    }
    free(format);
    return 1;
}

/**
 * \brief converts numpad-style align to align.
 */
int numpad2align(int val)
{
    if (val < -INT_MAX)
        // Pick an alignment somewhat arbitrarily. VSFilter handles
        // INT32_MIN as a mix of 1, 2 and 3, so prefer one of those values.
        val = 2;
    else if (val < 0)
        val = -val;
    int res = ((val - 1) % 3) + 1;  // horizontal alignment
    if (val <= 3)
        res |= VALIGN_SUB;
    else if (val <= 6)
        res |= VALIGN_CENTER;
    else
        res |= VALIGN_TOP;
    return res;
}

/**
 * \brief Parse the Style line
 * \param track track
 * \param str string to parse, zero-terminated
 * Allocates a new style struct.
*/
static int process_style(ass_track_t *track, char *str)
{

    char *token;
    char *tname;
    char *p = str;
    char *format;
    char *q;                    // format scanning pointer
    int sid;
    ass_style_t *style;
    ass_style_t *target;

    if (!track->style_format) {
        // no style format header
        // probably an ancient script version
        if (track->track_type == TRACK_TYPE_SSA)
            track->style_format =
                strdup
                ("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour,"
                 "TertiaryColour, BackColour, Bold, Italic, BorderStyle, Outline,"
                 "Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding");
        else
            track->style_format =
                strdup
                ("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour,"
                 "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut,"
                 "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow,"
                 "Alignment, MarginL, MarginR, MarginV, Encoding");
    }

    q = format = strdup(track->style_format);

    // Add default style first
    if (track->n_styles == 0) {
        // will be used if track does not contain a default style (or even does not contain styles at all)
        int sid = ass_alloc_style(track);
        set_default_style(&track->styles[sid]);
        track->default_style = sid;
    }

    sid = ass_alloc_style(track);

    style = track->styles + sid;
    target = style;

    // fill style with some default values
    style->ScaleX = 100.;
    style->ScaleY = 100.;

    while (1) {
        NEXT(q, tname);
        NEXT(p, token);

        PARSE_START
            STARREDSTRVAL(Name)
            if (strcmp(target->Name, "Default") == 0)
                track->default_style = sid;
            STRVAL(FontName)
            COLORVAL(PrimaryColour)
            COLORVAL(SecondaryColour)
            COLORVAL(OutlineColour) // TertiaryColor
            COLORVAL(BackColour)
            // SSA uses BackColour for both outline and shadow
            // this will destroy SSA's TertiaryColour, but i'm not going to use it anyway
            if (track->track_type == TRACK_TYPE_SSA)
                target->OutlineColour = target->BackColour;
            INTVAL(FontSize)
            INTVAL(Bold)
            INTVAL(Italic)
            INTVAL(Underline)
            INTVAL(StrikeOut)
            FPVAL(Spacing)
            FPVAL(Angle)
            INTVAL(BorderStyle)
            INTVAL(Alignment)
            if (track->track_type == TRACK_TYPE_ASS)
            {
                target->Alignment = numpad2align(target->Alignment);
            }
            // VSFilter compatibility
            else if (target->Alignment == 8)
                target->Alignment = 3;
            else if (target->Alignment == 4)
                target->Alignment = 11;
            INTVAL(MarginL)
            INTVAL(MarginR)
            INTVAL(MarginV)
            INTVAL(Encoding)
            FPVAL(ScaleX)
            FPVAL(ScaleY)
            INTVAL(Outline)
            INTVAL(Shadow)
        PARSE_END
    }
    style->ScaleX = FFMAX(style->ScaleX, 0.) / 100.;
    style->ScaleY = FFMAX(style->ScaleY, 0.) / 100.;
    style->Spacing = FFMAX(style->Spacing, 0.);
    style->Outline = FFMAX(style->Outline, 0);
    style->Shadow = FFMAX(style->Shadow, 0);
    style->Bold = !!style->Bold;
    style->Italic = !!style->Italic;
    style->Underline = !!style->Underline;
    style->StrikeOut = !!style->StrikeOut;
    if (!style->Name)
        style->Name = strdup("Default");
    if (!style->FontName)
        style->FontName = strdup("Arial");
    free(format);
    return 0;

}

static int process_styles_line(ass_track_t *track, char *str, request_context_t* request_context)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->style_format);
        track->style_format = strdup(p);
    } else if (!strncmp(str, "Style:", 6)) {
        char *p = str + 6;
        skip_spaces(&p);
        process_style(track, p);
    }
    return 0;
}

static int process_info_line(ass_track_t *track, char *str, request_context_t* request_context)
{
    if (!strncmp(str, "PlayResX:", 9)) {
        track->PlayResX = atoi(str + 9);
    } else if (!strncmp(str, "PlayResY:", 9)) {
        track->PlayResY = atoi(str + 9);
    } else if (!strncmp(str, "Timer:", 6)) {
        track->Timer = ass_atof(str + 6);
    } else if (!strncmp(str, "WrapStyle:", 10)) {
        track->WrapStyle = atoi(str + 10);
    } else if (!strncmp(str, "ScaledBorderAndShadow:", 22)) {
        track->ScaledBorderAndShadow = parse_bool(str + 22);
    } else if (!strncmp(str, "Kerning:", 8)) {
        track->Kerning = parse_bool(str + 8);
    } else if (!strncmp(str, "YCbCr Matrix:", 13)) {
        // ignore for now
    } else if (!strncmp(str, "Language:", 9)) {
        char *p = str + 9;
        while (*p && ass_isspace(*p)) p++;
        free(track->Language);
        track->Language = strndup(p, 2);
    }
    return 0;
}

static void event_format_fallback(ass_track_t *track)
{
    track->state = PST_EVENTS;
    if (track->track_type == TRACK_TYPE_SSA)
        track->event_format = strdup("Marked, Start, End, Style, "
            "Name, MarginL, MarginR, MarginV, Effect, Text");
    else
        track->event_format = strdup("Layer, Start, End, Style, "
            "Actor, MarginL, MarginR, MarginV, Effect, Text");
}

static int process_events_line(ass_track_t *track, char *str, request_context_t* request_context)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->event_format);
        track->event_format = strdup(p);

    } else if (!strncmp(str, "Dialogue:", 9)) {
        // This should never be reached for embedded subtitles.
        // They have slightly different format and are parsed in ass_process_chunk,
        // called directly from demuxer
        int eid;
        ass_event_t *event;

        str += 9;
        skip_spaces(&str);

        eid = ass_alloc_event(track);
        event = track->events + eid;

        // We can't parse events without event_format
        if (!track->event_format)
            event_format_fallback(track);

        process_event_tail(track, event, str);
    } else if (!strncmp(str, "Comment:", 8)) {
        // Ignore and do nothing, this is just a comment line
    } else {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "Event line not understood: %s", str);
    }
    return 0;
}

/**
 * \brief Parse a header line
 * \param track track
 * \param str string to parse, zero-terminated
*/
static int process_line(ass_track_t *track, char *str, request_context_t* request_context)
{
    int retval = 0;
    if (!ass_strncasecmp(str, "[Script Info]", 13)) {
        track->state = PST_INFO;
    } else if (!ass_strncasecmp(str, "[V4 Styles]", 11)) {
        track->state = PST_STYLES;
        track->track_type = TRACK_TYPE_SSA;
    } else if (!ass_strncasecmp(str, "[V4+ Styles]", 12)) {
        track->state = PST_STYLES;
        track->track_type = TRACK_TYPE_ASS;
    } else if (!ass_strncasecmp(str, "[Events]", 8)) {
        track->state = PST_EVENTS;
    } else if (!ass_strncasecmp(str, "[Fonts]", 7)) {
        track->state = PST_FONTS;
    } else {
        switch (track->state) {
        case PST_INFO:
            retval |= process_info_line(track, str, request_context);
            break;
        case PST_STYLES:
            retval |= process_styles_line(track, str, request_context);
            break;
        case PST_EVENTS:
            retval |= process_events_line(track, str, request_context);
            break;
        case PST_FONTS:
            // ignore for now
            break;
        default:
            break;
        }
    }
    return 0;
}

/**
 * \brief Process all text in an ASS/SSA file
 * \param track output ass_track_t pointer
 * \param str utf-8 string to parse, zero-terminated
*/
static int process_text(ass_track_t *track, char *str, request_context_t* request_context)
{
    int retval = 0;
    char *p = str;

    while (1) {
        char *q;
        while (1) {
            if ((*p == '\r') || (*p == '\n'))
                ++p;
            else if (p[0] == '\xef' && p[1] == '\xbb' && p[2] == '\xbf')
                p += 3;         // U+FFFE (BOM)
            else
                break;
        }
        for (q = p; ((*q != '\0') && (*q != '\r') && (*q != '\n')); ++q) {
        };
        if (q == p)
            break;
        if (*q != '\0')
            *(q++) = '\0';
        retval |= process_line(track, p, request_context);
        if (*q == '\0')
            break;
        p = q;
    }
    return retval;
}

/*
 * \param buf pointer to subtitle text in utf-8
 */
static ass_track_t *parse_memory(char *buf, int len, request_context_t* request_context)
{
    ass_track_t *track;
    int bfailed;
    // copy the input buffer, as the parsing is destructive.
    char *pcopy = vod_alloc(request_context->pool, len+1);
    if (pcopy == NULL)
    {
        vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "ass_parse_frames: vod_alloc failed");
        return NULL;
    }
    vod_memcpy(pcopy, buf, len+1);

    // initializes all fields to zero. If that doesn't suit your need, use another track_init function.
    track = vod_calloc(request_context->pool, sizeof(ass_track_t));
    if (!track)
    {
        vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "vod_calloc() failed");
        vod_free(request_context->pool, pcopy);
        return NULL;
    }

    // destructive parsing of pcopy
    bfailed = process_text(track, pcopy, request_context);
    if (bfailed == 1)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "process_text failed, track_type = %d", track->track_type);

    }
    vod_free(request_context->pool, pcopy); // not needed anymore either way

    if (track->track_type == TRACK_TYPE_UNKNOWN) {
        ass_free_track(request_context->pool, track);
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "track_type unknown");
        return NULL;
    }

    // external SSA/ASS subs does not have ReadOrder field
    //for (i = 0; i < track->n_events; ++i)
    //    track->events[i].ReadOrder = i;

    // call ass_free_track outside, after info has been used
    return track;
}

void ass_add_biu_ends(uint32_t* finalstring, ass_tag_idx_t targetidx, int badd)
{
    if (badd)
    {
        *finalstring <<= 8;
        *finalstring |= (uint32_t)(targetidx);
    }
    else
    {
        *finalstring >>= 8;
    }
    return;
}

uint32_t ass_add_biu_starts(uint32_t initorder, ass_tag_idx_t targetidx)
{
    return ((initorder << 8) | (uint32_t)(targetidx));
}

static int split_event_text_to_chunks(char *src, int srclen, char **textp, int *evlen, uint32_t *evorder, int iinitneeded, int binitneeded, int uinitneeded, request_context_t* request_context)
{
    // a chunk is part of the text that will be added with a specific voice/style. So we increment chunk only when we need a different style applied
    // Number of chunks is at least 1 if len is > 0
    int srcidx = 0, dstidx = 0, tagidx = 0, bBracesOpen = 0, chunkidx = 0;
    // (x)openneeded and (x)closeneeded are used to store all tags within braces, so we can order them correctly at the closing of the tag
    int iopenneeded = 0, icloseneeded = 0, iopened = 0, bopenneeded = 0, bcloseneeded = 0, bopened = 0, uopenneeded = 0, ucloseneeded = 0, uopened = 0;
    // initial string can only hold starts, finalstring can only hold closures
    uint32_t initialstring = 0, finalstring = 0;

    // Basic sanity checking for inputs
    if ((src == NULL) || (srclen < 1) || (srclen > MAX_STR_SIZE_EVNT_CHUNK))
    {
        return 0;
    }

    while (srcidx < srclen)
    {
        for (tagidx = 0; tagidx < TAG_TYPE_NONE; tagidx++)
        {
            if (vod_strncmp(src+srcidx, tag_strings[tagidx], tag_string_len[tagidx][0]) == 0)
            {
                char* curloc;
                srcidx += tag_string_len[tagidx][0]; //tag got read from input
                curloc = src + srcidx;

                switch (tagidx) {
                    case (TAG_TYPE_ITALIC_END): {
                        if (iinitneeded)
                        {
                            // insert <i> to initial string and reset the bool
                            initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_ITALIC_START);
                            iinitneeded = 0; // can never be set again, only at start of the event
                            iopened = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output

                        }
                        else if ((iopened != 0) && ((finalstring & 0xFF) == TAG_TYPE_ITALIC_END))
                        {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            iopened = 0;
                            // insert the string into output
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output
                        }
                        else if (iopened != 0)
                        {
                            icloseneeded = 1; // to be closed by end of braces, or at the very end of the line (worst case)
                        }
                        // else ignore it, as we can't close a span that was not opened
                    } break;

                    case (TAG_TYPE_ITALIC_START): {
                        if (iopened == 0)
                        {
                            iopenneeded = 1; //at the ending brace
                        }
                    } break;

                    case (TAG_TYPE_BOLD_END): {
                        if (binitneeded)
                        {
                            // insert <i> to initial string and reset the bool
                            initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_BOLD_START);
                            binitneeded = 0;
                            bopened = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output

                        }
                        else if ((bopened != 0) && ((finalstring & 0xFF) == TAG_TYPE_BOLD_END))
                        {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            bopened = 0;
                            // insert the string into output
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output
                        }
                        else if (bopened != 0)
                        {
                            bcloseneeded = 1; // to be closed by end of braces, or at the very end of the line (worst case)
                        }
                        // else ignore it, as we can't close a span that was not opened
                    } break;

                    case (TAG_TYPE_BOLD_START): {
                        if (bopened == 0)
                        {
                            bopenneeded = 1; //at the ending brace
                        }
                    } break;

                    case (TAG_TYPE_UNDER_END): {
                        if (uinitneeded)
                        {
                            // insert <i> to initial string and reset the bool
                            initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_UNDER_START);
                            uinitneeded = 0;
                            uopened = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output

                        }
                        else if ((uopened != 0) && ((finalstring & 0xFF) == TAG_TYPE_UNDER_END))
                        {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            uopened = 0;
                            // insert the string into output
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                            dstidx += tag_string_len[tagidx][1]; //tag got written to output
                        }
                        else if (uopened != 0)
                        {
                            ucloseneeded = 1; // to be closed by end of braces, or at the very end of the line (worst case)
                        }
                        // else ignore it, as we can't close a span that was not opened
                    } break;
                    case (TAG_TYPE_UNDER_START): {
                        if (uopened == 0)
                        {
                            uopenneeded = 1; //at the ending brace
                        }
                    } break;

                    default: {
                        // replace the string with its equivalent in target string
                        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
                        dstidx += tag_string_len[tagidx][1]; //tag got written to output
                    }
                }

#ifdef  TEMP_VERBOSITY
                vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                    "tagidx=%d, srcidx=%d, dstidx=%d, icloseneeded=%d, bcloseneeded=%d, ucloseneeded=%d, iopened=%d, bopened=%d, uopened=%d, iinitneeded=%d, uinitneeded=%d, initialstring=%d, finalstring=%d",
                    tagidx, srcidx, dstidx, icloseneeded, bcloseneeded, ucloseneeded, iopened, bopened, uopened, iinitneeded, uinitneeded, initialstring, finalstring);
#endif
                if (tagidx == TAG_TYPE_CLOSE_BRACES)
                {
                    int iii;
                    bBracesOpen = 0;
                    // problem here is inside braces, tags could come in any order.
                    for (iii=0; iii<3; iii++) {
                        uint32_t temp = (finalstring & 0xFF);
                        if ((temp == TAG_TYPE_ITALIC_END) && icloseneeded) {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            icloseneeded = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_ITALIC_END], tag_string_len[TAG_TYPE_ITALIC_END][1]);
                            dstidx += tag_string_len[TAG_TYPE_ITALIC_END][1];
                        }
                        else if ((temp == TAG_TYPE_BOLD_END) && bcloseneeded) {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            bcloseneeded = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_BOLD_END], tag_string_len[TAG_TYPE_BOLD_END][1]);
                            dstidx += tag_string_len[TAG_TYPE_BOLD_END][1];
                        }
                        else if ((temp == TAG_TYPE_UNDER_END) && ucloseneeded) {
                            ass_add_biu_ends(&finalstring, 0, 0); //remove
                            ucloseneeded = 0;
                            // replace the string with its equivalent in target string
                            vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_UNDER_END], tag_string_len[TAG_TYPE_UNDER_END][1]);
                            dstidx += tag_string_len[TAG_TYPE_UNDER_END][1];
                        }
                    }
                    if (iopenneeded) {
                        iopened = 1;
                        iopenneeded = 0;
                        ass_add_biu_ends(&finalstring, TAG_TYPE_ITALIC_END, 1); //add
                        // replace the string with its equivalent in target string
                        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_ITALIC_START], tag_string_len[TAG_TYPE_ITALIC_START][1]);
                        dstidx += tag_string_len[TAG_TYPE_ITALIC_START][1]; //tag got written to output
                    }
                    if (bopenneeded) {
                        bopened = 1;
                        bopenneeded = 0;
                        ass_add_biu_ends(&finalstring, TAG_TYPE_BOLD_END, 1); //add
                        // replace the string with its equivalent in target string
                        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_BOLD_START], tag_string_len[TAG_TYPE_BOLD_START][1]);
                        dstidx += tag_string_len[TAG_TYPE_BOLD_START][1]; //tag got written to output
                    }
                    if (uopenneeded) {
                        uopened = 1;
                        uopenneeded = 0;
                        ass_add_biu_ends(&finalstring, TAG_TYPE_UNDER_END, 1); //add
                        // replace the string with its equivalent in target string
                        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_UNDER_START], tag_string_len[TAG_TYPE_UNDER_START][1]);
                        dstidx += tag_string_len[TAG_TYPE_UNDER_START][1]; //tag got written to output
                    }

                }
                // if the next char is not "\\" or "}", then ignore all characters between here and then
                // (case of \b400) or unsupported \xxxxxxx tag
                else if (bBracesOpen && (*curloc != '}') && (*curloc != '\\'))
                {
                    char*  nearest;
                    char*  nearslash = vod_strchr(curloc, '\\'); // NULL or value
                    char*  nearbrace = vod_strchr(curloc, '}');  // NULL or value
                    if (nearslash == NULL)  nearslash = nearbrace;
                    if (nearbrace == NULL)  nearbrace = nearslash;
                    nearest = FFMIN(nearslash, nearbrace);
                    srcidx = (int)(FFMAX(nearest, curloc+1) - src);
                }
                else if (tagidx == TAG_TYPE_OPEN_BRACES)
                    bBracesOpen = 1;

                tagidx = -1; //start all tags again, cause they can come in any order
            }
        }
        // none of the tags matched this character
        if (tagidx == TAG_TYPE_NONE)
        {
            vod_memcpy(textp[chunkidx] + dstidx, src + srcidx, 1);
            srcidx++;
            dstidx++;
        }
    }

    // We now close b/i/u where close is needed
    if (finalstring) {
        int iii;
        for (iii=0; iii<3; iii++) {
            uint32_t temp = (finalstring & 0xFF);
            finalstring >>= 8;
            if (temp == TAG_TYPE_ITALIC_END) {
                ass_add_biu_ends(&finalstring, 0, 0); //remove
                icloseneeded = 0;
                // replace the string with its equivalent in target string
                vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_ITALIC_END], tag_string_len[TAG_TYPE_ITALIC_END][1]);
                dstidx += tag_string_len[TAG_TYPE_ITALIC_END][1];
            }
            else if (temp == TAG_TYPE_BOLD_END) {
                ass_add_biu_ends(&finalstring, 0, 0); //remove
                bcloseneeded = 0;
                // replace the string with its equivalent in target string
                vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_BOLD_END], tag_string_len[TAG_TYPE_BOLD_END][1]);
                dstidx += tag_string_len[TAG_TYPE_BOLD_END][1];
            }
            else if (temp == TAG_TYPE_UNDER_END) {
                ass_add_biu_ends(&finalstring, 0, 0); //remove
                ucloseneeded = 0;
                // replace the string with its equivalent in target string
                vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_UNDER_END], tag_string_len[TAG_TYPE_UNDER_END][1]);
                dstidx += tag_string_len[TAG_TYPE_UNDER_END][1];
            }
        }
    }
    // style b/i/u that was not served yet, or one opened and not closed yet
    if (iinitneeded)
    {
        initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_ITALIC_START);
        iinitneeded = 0;
        // replace the string with its equivalent in target string
        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_ITALIC_END], tag_string_len[TAG_TYPE_ITALIC_END][1]);
        dstidx += tag_string_len[TAG_TYPE_ITALIC_END][1];
    }
    if (binitneeded)
    {
        initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_BOLD_START);
        binitneeded = 0;
        // replace the string with its equivalent in target string
        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_BOLD_END], tag_string_len[TAG_TYPE_BOLD_END][1]);
        dstidx += tag_string_len[TAG_TYPE_BOLD_END][1];
    }
    if (uinitneeded)
    {
        initialstring = ass_add_biu_starts(initialstring, TAG_TYPE_UNDER_START);
        uinitneeded = 0;
        // replace the string with its equivalent in target string
        vod_memcpy(textp[chunkidx] + dstidx, tag_replacement_strings[TAG_TYPE_UNDER_END], tag_string_len[TAG_TYPE_UNDER_END][1]);
        dstidx += tag_string_len[TAG_TYPE_UNDER_END][1];
    }

#ifdef  TEMP_VERBOSITY
    vod_log_error(VOD_LOG_ERR, request_context->log, 0,
        "FINAL: srcidx=%d, dstidx=%d, icloseneeded=%d, bcloseneeded=%d, ucloseneeded=%d, iopened=%d, bopened=%d, uopened=%d, iinitneeded=%d, uinitneeded=%d, initialstring=%d, finalstring=%d",
        srcidx, dstidx, icloseneeded, bcloseneeded, ucloseneeded, iopened, bopened, uopened, iinitneeded, uinitneeded, initialstring, finalstring);
#endif

    evorder[chunkidx] = initialstring;
    evlen[chunkidx]   = dstidx;

    return chunkidx + 1;
}

static void ass_clean_known_mem(request_context_t* request_context, ass_track_t *ass_track, char** event_textp)
{
    int chunkidx;
    if (ass_track != NULL)
        ass_free_track(request_context->pool, ass_track);

    if (event_textp != NULL)
    {
        for (chunkidx = 0; chunkidx < NUM_OF_TAGS_ALLOWED_PER_LINE; chunkidx++)
        {
            if (event_textp[chunkidx] != NULL)
                vod_free(request_context->pool, event_textp[chunkidx]);
        }
    }

    return;
}

static vod_status_t
ass_reader_init(
    request_context_t* request_context,
    vod_str_t* buffer,
    size_t initial_read_size,
    size_t max_metadata_size,
    void** ctx)
{
    u_char* p = buffer->data;

    if (vod_strncmp(p, UTF8_BOM, sizeof(UTF8_BOM) - 1) == 0)
    {
        p += sizeof(UTF8_BOM) - 1;
    }

    // The line that says “[Script Info]” must be the first line in a v4/v4+ script.
    if (buffer->len > 0 && vod_strncmp(p, ASS_SCRIPT_INFO_HEADER, sizeof(ASS_SCRIPT_INFO_HEADER) - 1) != 0)
    {
        return VOD_NOT_FOUND;
    }

    return subtitle_reader_init(
        request_context,
        initial_read_size,
        ctx);
}

static vod_status_t
ass_parse(
    request_context_t* request_context,
    media_parse_params_t* parse_params,
    vod_str_t* source,
    size_t metadata_part_count,
    media_base_metadata_t** result)
{
    ass_track_t *ass_track;
    vod_status_t ret_status;
    ass_track = parse_memory((char *)(source->data), source->len, request_context);

    if (ass_track == NULL)
    {
        // ass_track was de-allocated already inside the function, for failure cases
        vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "ass_parse failed");
        return VOD_BAD_DATA;
    }

    ret_status = subtitle_parse(
        request_context,
        parse_params,
        source,
        NULL,
        (uint64_t)(ass_track->maxDuration),
        metadata_part_count,
        result);
#ifdef  TEMP_VERBOSITY
    vod_log_error(VOD_LOG_ERR, request_context->log, 0,
        "ass_parse(): parse_memory() succeeded, sub_parse succeeded, len of data = %d, maxDuration = %D, nEvents = %d, nStyles = %d",
        source->len, ass_track->maxDuration, ass_track->n_events, ass_track->n_styles);
#endif
    // now that we used maxDuration, we need to free the memory used by the track
    ass_free_track(request_context->pool, ass_track);
    return ret_status;
}

/**
 * \brief Parse the .ass/.ssa file, convert to webvtt, output all cues as frames
 *
 * common for all frames
 * \output vtt_track->media_info.extra_data (WEBVTT header + all STYLE cues)
 * \output vtt_track->total_frames_duration
 * \output vtt_track->first_frame_time_offset
 * \output vtt_track->total_frames_size
 * \output vtt_track->frame_count
 * \output vtt_track->frames.clip_to
 * \output vtt_track->frames.first_frame
 * \output vtt_track->frames.last_frame
 * \output vtt_track->first_frame_index
 * \output result (media track in the track array)
 *
 * individual cues in the frames array
 * \output cur_frame->duration
 * \output cur_frame->offset
 * \output cur_frame->size
 * \output cur_frame->pts_delay
 * \output cur_frame->key_frame
 *
 * \return int VOD_OK or any of the VOD_ error enums
*/
static vod_status_t
ass_parse_frames(
    request_context_t* request_context,
    media_base_metadata_t* base,
    media_parse_params_t* parse_params,
    struct segmenter_conf_s* segmenter,     // unused
    read_cache_state_t* read_cache_state,   // unused
    vod_str_t* frame_data,                  // unused
    media_format_read_request_t* read_req,  // unused
    media_track_array_t* result)
{
    ass_track_t *ass_track;
    vod_array_t frames;
    int evntcounter, chunkcounter;
    subtitle_base_metadata_t* metadata
                              = vod_container_of(base, subtitle_base_metadata_t, base);
    vod_str_t*     source     = &metadata->source;
    media_track_t* vtt_track  = base->tracks.elts;
    input_frame_t* cur_frame  = NULL;
    ass_event_t*   cur_event  = NULL;
    char *p, *pfixed;
    vod_str_t* header         = &vtt_track->media_info.extra_data;
    int len;

    vod_memzero(result, sizeof(*result));
    result->first_track       = vtt_track;
    result->last_track        = vtt_track + 1;
    result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
    result->total_track_count = 1;

    vtt_track->first_frame_index = 0;
    vtt_track->total_frames_size = 0;

    if ((parse_params->parse_type & (PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_EXTRA_DATA_SIZE)) == 0)
    {
        return VOD_OK;
    }

    ass_track = parse_memory((char *)(source->data), source->len, request_context);
    if (ass_track == NULL)
    {
        // ass_track was de-allocated already inside the function, for failure cases
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_parse_frames: failed to parse memory into ass track");
        return VOD_BAD_MAPPING;
    }
#ifdef  TEMP_VERBOSITY
    else
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "frames parse_memory() succeeded, len of data = %d, maxDuration = %D, nEvents = %d, nStyles = %d",
            source->len, ass_track->maxDuration, ass_track->n_events, ass_track->n_styles);
    }
#endif
    // allocate initial array of cues/styles, to be augmented as needed after the first 5
    if (vod_array_init(&frames, request_context->pool, 5, sizeof(input_frame_t)) != VOD_OK)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_parse_frames: vod_array_init failed");
        ass_free_track(request_context->pool, ass_track);
        return VOD_ALLOC_FAILED;
    }

    //allocate memory for the style's text string
    p = pfixed = vod_alloc(request_context->pool, MAX_STR_SIZE_ALL_WEBVTT_STYLES);
    if (p == NULL)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_parse_frames: vod_alloc failed");
        ass_free_track(request_context->pool, ass_track);
        return VOD_ALLOC_FAILED;
    }

    // We now insert header and all Style definitions
    header->data              = (u_char*)pfixed;
    len = sizeof(WEBVTT_HEADER_NEWLINES) - 1; vod_memcpy(p, WEBVTT_HEADER_NEWLINES, len);  p+=len;
#ifdef ASSUME_STYLE_SUPPORT
    int stylecounter;
    bool_t fewstyles = TRUE;
    for (stylecounter = (ass_track->default_style ? 1 : 0); (stylecounter < ass_track->n_styles) && fewstyles; stylecounter++)
    {
        ass_style_t* cur_style = ass_track->styles + stylecounter;

        vod_memcpy(p, FIXED_WEBVTT_STYLE_START_STR, FIXED_WEBVTT_STYLE_START_WIDTH);           p+=FIXED_WEBVTT_STYLE_START_WIDTH;
        len = vod_strlen(cur_style->Name); vod_memcpy(p, cur_style->Name, len);                p+=len;
        vod_memcpy(p, FIXED_WEBVTT_STYLE_END_STR, FIXED_WEBVTT_STYLE_END_WIDTH);               p+=FIXED_WEBVTT_STYLE_END_WIDTH;
        vod_memcpy(p, FIXED_WEBVTT_BRACES_START_STR, FIXED_WEBVTT_BRACES_START_WIDTH);         p+=FIXED_WEBVTT_BRACES_START_WIDTH;

        len = 8; vod_memcpy(p, "color: #", len);                                               p+=len;
        vod_sprintf((u_char*)p, "%08uxD;\r\n", cur_style->PrimaryColour);                      p+=11;


        len = 14; vod_memcpy(p, "font-family: \"", len);                                       p+=len;
        len = vod_strlen(cur_style->FontName); vod_memcpy(p, cur_style->FontName, len);        p+=len;
        len = 16; vod_memcpy(p, "\", sans-serif;\r\n", len);                                   p+=len;
        vod_sprintf((u_char*)p, "font-size: %03uDpx;\r\n", cur_style->FontSize);               p+=19;

        /*if (cur_style->Bold) {
            len = 20; vod_memcpy(p, "font-weight: bold;\r\n", len);                            p+=len;
        }
        if (cur_style->Italic) {
            len = 21; vod_memcpy(p, "font-style: italic;\r\n", len);                           p+=len;
        }
        // This will inherit the OutlineColour (and shadow) if BorderStyle==1, otherwise it inherits PrimaryColour
        if (cur_style->Underline) {
            // available styles are: solid | double | dotted | dashed | wavy
            // available lines are: underline || overline || line-through || blink
            len = 35; vod_memcpy(p, "text-decoration: solid underline;\r\n", len);             p+=len;
        }
        else if (cur_style->StrikeOut) {
            // available lines are: underline || overline || line-through || blink
            len = 38; vod_memcpy(p, "text-decoration: solid line-through;\r\n", len);          p+=len;
        }*/

        if (cur_style->BorderStyle == 1 /*&& ass_track->type == TRACK_TYPE_ASS*/)
        {
            // webkit is not supported by all players, stick to adding outline using text-shadow
#if 0
            len = 22; vod_memcpy(p, "-webkit-text-stroke: #", len);                            p+=len;
            vod_sprintf((u_char*)p, "%08uxD %01uDpx;\r\n", cur_style->OutlineColour, cur_style->Outline); p+=15;

            len = 14; vod_memcpy(p, "text-shadow: #", len);                                    p+=len;
            vod_sprintf((u_char*)p, "%08uxD %01uDpx %01uDpx 0px;\r\n", //* always very sharp non-blurred shadows */
                         cur_style->BackColour, cur_style->Shadow, cur_style->Shadow);         p+=23;
#else
            len = 13; vod_memcpy(p, "text-shadow: ", len);                                     p+=len;
            // add outline in 4 directions with the outline color
            vod_sprintf((u_char*)p, "#%08uxD -%01uDpx 0px, #%08uxD 0px %01uDpx, #%08uxD 0px -%01uDpx, #%08uxD %01uDpx 0px, #%08uxD %01uDpx %01uDpx 0px;\r\n",
                         cur_style->OutlineColour, cur_style->Outline,
                         cur_style->OutlineColour, cur_style->Outline,
                         cur_style->OutlineColour, cur_style->Outline,
                         cur_style->OutlineColour, cur_style->Outline,
                         cur_style->BackColour, cur_style->Shadow, cur_style->Shadow);         p+=102;

#endif
        } else {
            len = 19; vod_memcpy(p, "background-color: #", len);                               p+=len;
            vod_sprintf((u_char*)p, "%08uxD;\r\n", cur_style->BackColour);                     p+=11;
        }
#if 0
		len =  12; vod_memcpy(p, "text-align: ", len);                                         p+=len;
		if ((cur_style->Alignment & 1) == 0) {              //center Alignment  2/6/10
			len =  6; vod_memcpy(p, "center", len);                                            p+=len;
		} else if (((cur_style->Alignment - 1) & 3) == 0) { //left   Alignment  1/5/9
			len =  4; vod_memcpy(p, "left", len);                                              p+=len;
		} else {                                            //right  Alignment  3/7/11
			len =  5; vod_memcpy(p, "right", len);                                             p+=len;
		}
		len = 3; vod_memcpy(p, ";\r\n", len);                                                  p+=len;
#endif
        vod_memcpy(p, FIXED_WEBVTT_BRACES_END_STR, FIXED_WEBVTT_BRACES_END_WIDTH);             p+=FIXED_WEBVTT_BRACES_END_WIDTH;
        len = 2; vod_memcpy(p, "\r\n", len);                                                   p+=len;

        // check that we are not getting too close to memory allocated. If too many styles, ignore the rest.
        fewstyles = (8 * (int)(p - pfixed)) < (7 * MAX_STR_SIZE_ALL_WEBVTT_STYLES);
    }
#endif //ASSUME_STYLE_SUPPORT
    header->len               = (size_t)(p - pfixed);

    // We now insert all cues that include their positioning info
    for (evntcounter = 0; evntcounter < ass_track->n_events; evntcounter++)
    {
        // Split the event text into multiple chunks so we can insert each chunk as a separate frame in webVTT, all under a single cue
        char*          event_textp[NUM_OF_TAGS_ALLOWED_PER_LINE];
        int            event_len  [NUM_OF_TAGS_ALLOWED_PER_LINE];
        uint32_t       eventprestring[NUM_OF_TAGS_ALLOWED_PER_LINE];
        int            margL, margR, margV; // all of these are integer percentage values

        ass_event_t*   prev_event = ass_track->events + evntcounter - 1;
                       cur_event  = ass_track->events + evntcounter;
        ass_style_t*   cur_style = ass_track->styles + cur_event->Style; //cur_event->Style will be zero for an unknown Style name

        // allocate memory for the chunk pointer itself first
        for (chunkcounter = 0; chunkcounter<NUM_OF_TAGS_ALLOWED_PER_LINE; chunkcounter++)
        {
            // now allocate string memory for each chunk
            event_textp[chunkcounter] = vod_alloc(request_context->pool, MAX_STR_SIZE_EVNT_CHUNK);
            if (event_textp[chunkcounter] == NULL)
            {
                vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                    "ass_parse_frames: vod_alloc failed");
                ass_clean_known_mem(request_context, ass_track, event_textp);
                return VOD_ALLOC_FAILED;
            }
            eventprestring[chunkcounter] = 0;
            event_len[chunkcounter] = 0;
        }

        int  num_chunks_in_text = split_event_text_to_chunks(cur_event->Text, vod_strlen(cur_event->Text),
                                      event_textp, event_len, eventprestring, cur_style->Italic, cur_style->Bold, cur_style->Underline, request_context);

#ifdef  TEMP_VERBOSITY
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_parse_frames: event=%d num_chunks=%d len0=%d order0=%d",
            evntcounter, num_chunks_in_text, event_len[0], eventprestring[0]);
#endif
        if (evntcounter > 0 && cur_frame != NULL)
        {
            cur_frame->duration = cur_event->Start - prev_event->Start;
        }

        // allocate the output frame
        cur_frame = vod_array_push(&frames);
        if (cur_frame == NULL)
        {
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "ass_parse_frames: vod_array_push failed");
            ass_clean_known_mem(request_context, ass_track, event_textp);
            return VOD_ALLOC_FAILED;
        }
        // allocate the text of output frame
        p = pfixed = vod_alloc(request_context->pool, MAX_STR_SIZE_EVNT_CHUNK);
        if (p == NULL)
        {
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "ass_parse_frames: vod_alloc failed");
            ass_clean_known_mem(request_context, ass_track, event_textp);
            return VOD_ALLOC_FAILED;
        }

        // Cues are named "c<iteration_number_in_7_digits>" starting from c0000000
        vod_sprintf((u_char*)p, FIXED_WEBVTT_CUE_FORMAT_STR, evntcounter);      p+=FIXED_WEBVTT_CUE_NAME_WIDTH;
        len = 2; vod_memcpy(p, "\r\n", len);                                    p+=len;
        // timestamps will be inserted here, we now insert positioning and alignment changes
        {
            margL = ((cur_event->MarginL > 0) ? cur_event->MarginL : cur_style->MarginL) * 100 / ass_track->PlayResX;
            margR = (ass_track->PlayResX - ((cur_event->MarginR > 0) ? cur_event->MarginR : cur_style->MarginR)) * 100 / ass_track->PlayResX;
            margV = ((cur_event->MarginV > 0) ? cur_event->MarginV : cur_style->MarginV) * 100 / ass_track->PlayResY; // top assumed
            // All the following variables are percentages in rounded integer values
            if (margL || margR || margV)
            {
                // center/middle means we are giving the coordinate of the center/middle point
                int line, sizeH, pos;
                if (cur_style->Alignment >= VALIGN_CENTER) {   //middle Alignment  for values 9,10,11
                    line = FFMINMAX(margV - 4, 8, 84);
                } else if (cur_style->Alignment < VALIGN_TOP) { //bottom Alignment  for values 1, 2, 3
                    margV = 100 - margV;
                    line = FFMINMAX(margV - 8, 8, 84);
                } else {                                        //top alignment is the default assumption
                    line = FFMINMAX(margV, 8, 84);
                }

                sizeH = FFMINMAX(margR - margL, 30, 100 - margL);
                if ((cur_style->Alignment & 1) == 0) {              //center Alignment  2/6/10
                    pos = FFMINMAX((margR + margL + 1)/2, sizeH/2, 100 - sizeH/2);
                } else if (((cur_style->Alignment - 1) & 3) == 0) { //left   Alignment  1/5/9
                    pos = FFMINMAX(margL, 3, 100 - sizeH);
                } else {                                            //right  Alignment  3/7/11
                    pos = FFMINMAX(margR, sizeH, 97);
                }

                len = 10; vod_memcpy(p, " position:", len);                     p+=len;
                vod_sprintf((u_char*)p, "%03uD", pos);                          p+=3;
                len =  7; vod_memcpy(p, "% size:", len);                        p+=len;
                vod_sprintf((u_char*)p, "%03uD", sizeH);                        p+=3;
                len =  7; vod_memcpy(p, "% line:", len);                        p+=len;
                vod_sprintf((u_char*)p, "%03uD", line);                         p+=3;
            }
            // We should only insert this if an alignment override tag {\a...}is in the text, otherwise follow the style's alignment
            // but for now, insert it all the time till all players can read styles
            len =  8; vod_memcpy(p, "% align:", len);                           p+=len;
            if ((cur_style->Alignment & 1) == 0) {              //center Alignment  2/6/10
                len =  6; vod_memcpy(p, "center", len);                         p+=len;
            } else if (((cur_style->Alignment - 1) & 3) == 0) { //left   Alignment  1/5/9
                len =  5; vod_memcpy(p, "start", len);                          p+=len;
            } else {                                            //right  Alignment  3/7/11
                len =  3; vod_memcpy(p, "end", len);                            p+=len;
            }
            len = 2; vod_memcpy(p, "\r\n", len);                                p+=len;
        }
#ifdef ASSUME_STYLE_SUPPORT
        vod_memcpy(p, FIXED_WEBVTT_VOICE_START_STR, FIXED_WEBVTT_VOICE_START_WIDTH);       p+=FIXED_WEBVTT_VOICE_START_WIDTH;
        len = vod_strlen(cur_style->Name); vod_sprintf((u_char*)p, cur_style->Name, len);  p+=len;
        vod_memcpy(p, FIXED_WEBVTT_VOICE_END_STR, FIXED_WEBVTT_VOICE_END_WIDTH);           p+=FIXED_WEBVTT_VOICE_END_WIDTH;
#endif //ASSUME_STYLE_SUPPORT


        for (chunkcounter = 0; chunkcounter < num_chunks_in_text; chunkcounter++)
        {
            // insert initial webvtt tags here in reverse order to that in eventprestring
            if (eventprestring[chunkcounter]) {
                int iii;
                for (iii=0; iii<3; iii++) {
                    uint32_t temp = (eventprestring[chunkcounter] & 0xFF);
                    eventprestring[chunkcounter] >>= 8;
                    if (temp == TAG_TYPE_ITALIC_START) {
                        vod_memcpy(p, tag_replacement_strings[TAG_TYPE_ITALIC_START], tag_string_len[TAG_TYPE_ITALIC_START][1]);
                        p += tag_string_len[TAG_TYPE_ITALIC_START][1];
                    }
                    else if (temp == TAG_TYPE_BOLD_START) {
                        vod_memcpy(p, tag_replacement_strings[TAG_TYPE_BOLD_START], tag_string_len[TAG_TYPE_BOLD_START][1]);
                        p += tag_string_len[TAG_TYPE_BOLD_START][1];
                    }
                    else if (temp == TAG_TYPE_UNDER_START) {
                        vod_memcpy(p, tag_replacement_strings[TAG_TYPE_UNDER_START], tag_string_len[TAG_TYPE_UNDER_START][1]);
                        p += tag_string_len[TAG_TYPE_UNDER_START][1];
                    }
                }
            }
            vod_memcpy(p, event_textp[chunkcounter], event_len[chunkcounter]);  p+=event_len[chunkcounter];
        }

        len = 2; vod_memcpy(p, "\r\n", len);                                    p+=len;
        // we still need an empty line after each event/cue
        len = 2; vod_memcpy(p, "\r\n", len);                                    p+=len;


        // Note: mapping of cue into input_frame_t:
        // - offset = pointer to buffer containing: cue id, cue settings list, cue payload
        // - size = size of data pointed by offset
        // - key_frame = cue id length
        // - duration = start time of next event - start time of current event
        // - pts_delay = end time - start time = duration this subtitle event is on screen

        cur_frame->offset    = (uintptr_t)pfixed;
        cur_frame->size      = (uint32_t)(p - pfixed);
        cur_frame->key_frame = FIXED_WEBVTT_CUE_NAME_WIDTH + 2; // cue name + \r\n
        cur_frame->pts_delay = cur_event->End - cur_event->Start;
        if (evntcounter == 0)
        {
            vtt_track->first_frame_time_offset = cur_event->Start;
        }

        vtt_track->total_frames_duration = cur_event->End - vtt_track->first_frame_time_offset;
        vtt_track->total_frames_size += cur_frame->size;
    }
    if ((cur_frame != NULL) && (cur_event != NULL))
    {
        cur_frame->duration = cur_event->End - cur_event->Start; // correct last event's duration
    }

    // now we got all the info from ass_track, deallocate its memory
    ass_free_track(request_context->pool, ass_track);

    vtt_track->frame_count        = frames.nelts;
    vtt_track->frames.clip_to     = UINT_MAX;
    vtt_track->frames.first_frame = frames.elts;
    vtt_track->frames.last_frame  = vtt_track->frames.first_frame + frames.nelts;

    return VOD_OK;
}

media_format_t ass_format = {
    FORMAT_ID_ASS,
    vod_string("ass_or_ssa"),
    ass_reader_init,
    subtitle_reader_read,
    NULL,
    NULL,
    ass_parse,
    ass_parse_frames,
};
