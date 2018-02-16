#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
//#include "../common.h"
#include "subtitle_format.h"
#include <ctype.h>
#include <float.h>

#define ASS_SCRIPT_INFO_HEADER ("[Script Info]")

#define VALIGN_SUB 0
#define VALIGN_CENTER 8
#define VALIGN_TOP 4
#define HALIGN_LEFT 1
#define HALIGN_CENTER 2
#define HALIGN_RIGHT 3
#define ASS_JUSTIFY_AUTO 0
#define ASS_JUSTIFY_LEFT 1
#define ASS_JUSTIFY_CENTER 2
#define ASS_JUSTIFY_RIGHT 3

#define FONT_WEIGHT_LIGHT  300
#define FONT_WEIGHT_MEDIUM 400
#define FONT_WEIGHT_BOLD   700
#define FONT_SLANT_NONE    0
#define FONT_SLANT_ITALIC  100
#define FONT_SLANT_OBLIQUE 110
#define FONT_WIDTH_CONDENSED 75
#define FONT_WIDTH_NORMAL    100
#define FONT_WIDTH_EXPANDED  125

#define ASS_STYLES_ALLOC 20
#define ASS_SIZE_MAX ((size_t)-1)

#define ass_atof(STR) (ass_strtod((STR),NULL))

#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMINMAX(c,a,b) FFMIN(FFMAX(c, a), b)

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
        memcpy(newp, s, len);
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

typedef struct {
	uint8_t  wrap_style;
	uint8_t  script_type;
	uint32_t player_res_x;
	uint32_t player_res_y;
} ass_script_info_t;


/* ASS Style: line */
typedef struct ass_style {
    char       *Name;
    char       *FontName;
    double      FontSize;
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
    double      Outline;
    double      Shadow;
    int         Alignment;
    int         MarginL;
    int         MarginR;
    int         MarginV;
    int         Encoding;
    int         treat_fontname_as_pattern;
    double      Blur;
    int         Justify;
} ass_style_t;

/*
 * ass_event corresponds to a single Dialogue line;
 * text is stored as-is, style overrides will be parsed later.
 */
typedef struct ass_event {
    long long   Start;    // ms
    long long   Duration; // ms

    int         ReadOrder;
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
    ass_style_t    *styles;    // array of styles, max_styles length, n_styles used
    ass_event_t    *events;    // the same as styles

    char           *style_format;     // style format line (everything after "Format: ")
    char           *event_format;     // event format line

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

    int             default_style;    // index of default style
    char           *name;             // file name in case of external subs, 0 for streams
    ParserState     state;

    long long       maxDuration;      // ms

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
    memset(track->styles + sid, 0, sizeof(ass_style_t));
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
    memset(track->events + eid, 0, sizeof(ass_event_t));
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

void ass_free_track(ass_track_t *track)
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
    free(track);
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
    vod_log_debug3(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
       "[%p]: Warning: no style named '%s' found, using '%s'", track, name, track->styles[i].Name);
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
            if (track->maxDuration < event->Duration) {
                track->maxDuration = event->Duration;
            }
            event->Duration -= event->Start;
            free(format);
            return 0;           // "Text" is always the last
        }
        NEXT(p, token);

        ALIAS(End, Duration)    // temporarily store end timecode in event->Duration
        PARSE_START
            INTVAL(Layer)
            STYLEVAL(Style)
            STRVAL(Name)
            STRVAL(Effect)
            INTVAL(MarginL)
            INTVAL(MarginR)
            INTVAL(MarginV)
            TIMEVAL(Start)
            TIMEVAL(Duration)
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

    vod_log_debug2(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
        "[%p] Style: %s", track, str);

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
            FPVAL(FontSize)
            INTVAL(Bold)
            INTVAL(Italic)
            INTVAL(Underline)
            INTVAL(StrikeOut)
            FPVAL(Spacing)
            FPVAL(Angle)
            INTVAL(BorderStyle)
            INTVAL(Alignment)
            if (track->track_type == TRACK_TYPE_ASS)
                target->Alignment = numpad2align(target->Alignment);
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
            FPVAL(Outline)
            FPVAL(Shadow)
        PARSE_END
    }
    style->ScaleX = FFMAX(style->ScaleX, 0.) / 100.;
    style->ScaleY = FFMAX(style->ScaleY, 0.) / 100.;
    style->Spacing = FFMAX(style->Spacing, 0.);
    style->Outline = FFMAX(style->Outline, 0.);
    style->Shadow = FFMAX(style->Shadow, 0.);
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
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "Styles Format: %s", track->style_format);
    } else if (!strncmp(str, "Style:", 6)) {
        char *p = str + 6;
        skip_spaces(&p);
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "Styles Style: '%.30s'", str);
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
    vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
        "No event format found, using fallback");
}

static int process_events_line(ass_track_t *track, char *str, request_context_t* request_context)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->event_format);
        track->event_format = strdup(p);
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "Event format: %s", track->event_format);

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

        // We can't parse events with event_format
        if (!track->event_format)
            event_format_fallback(track);

        process_event_tail(track, event, str);

    } else {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "Event line not understood: '%.30s'", str);
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
            process_info_line(track, str, request_context);
            break;
        case PST_STYLES:
            process_styles_line(track, str, request_context);
            break;
        case PST_EVENTS:
            process_events_line(track, str, request_context);
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

static int process_text(ass_track_t *track, char *str, request_context_t* request_context)
{
    int retval;
    char *p = str;
    retval = 1; // initially bad

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
        retval = process_line(track, p, request_context);
        if (*q == '\0')
            break;
        p = q;
    }
    return retval;
}

/*
 * \param buf pointer to subtitle text in utf-8
 */
static ass_track_t *parse_memory(char *buf, request_context_t* request_context)
{
    ass_track_t *track;
    int i;

    // initializes all fields to zero. If that doesn't suit your need, use another track_init function.
    track = calloc(1, sizeof(ass_track_t));
    if (!track)
        return NULL;

    // process header
    process_text(track, buf, request_context);

    // external SSA/ASS subs does not have ReadOrder field
    for (i = 0; i < track->n_events; ++i)
        track->events[i].ReadOrder = i;

    if (track->track_type == TRACK_TYPE_UNKNOWN) {
        ass_free_track(track);
        return NULL;
    }

    return track;
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

    // The line that says “[Script Info]” must be the first line in a v4/v4+ script.
    if (buffer->len > 0 && vod_strncmp(p, ASS_SCRIPT_INFO_HEADER, sizeof(ASS_SCRIPT_INFO_HEADER) - 1) != 0)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_reader_init failed, len=%d", buffer->len);
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
	ass_track_t *track;
	vod_status_t ret_status;

	char* p = (char *)(source->data);
	track = parse_memory(p, request_context);

    if (track == NULL)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "ass_parse failed");
        return VOD_BAD_MAPPING;
    }

	ret_status = subtitle_parse(
		request_context,
		parse_params,
		source,
		NULL,
		(uint64_t)(track->maxDuration),
		metadata_part_count,
		result);

    vod_log_error(VOD_LOG_ERR, request_context->log, 0,
        "parse_memory() succeeded, len of data = %d, maxDuration = %D, nEvents = %d, nStyles = %d",
        source->len, track->maxDuration, track->n_events, track->n_styles);
        
    ass_free_track(track);

	return ret_status;
}

static vod_status_t
ass_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
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
