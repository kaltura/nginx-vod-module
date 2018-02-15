#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include <ctype.h>

#define ASS_SCRIPT_INFO_HEADER ("[Script Info]")

// utf8 functions
#define CHAR_TYPE u_char
#define METHOD(x) x
#include "webvtt_format_template.h"
#undef CHAR_TYPE
#undef METHOD

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

    int             default_style;      // index of default style
    char           *name;             // file name in case of external subs, 0 for streams
    ParserState     state;
} ass_track_t;



/**
 * \brief Set up default style
 * \param style style to edit to defaults
 * The parameters are mostly taken directly from VSFilter source for
 * best compatibility.
 */
static void set_default_style(ASS_Style *style)
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

static long long string2timecode(ASS_Library *library, char *p)
{
    int h, m, s, ms;
    long long tm;
    int res = sscanf(p, "%d:%d:%d.%d", &h, &m, &s, &ms);
    if (res < 4) {
        ass_msg(library, MSGL_WARN, "Bad timestamp");
        return 0;
    }
    tm = ((h * 60LL + m) * 60 + s) * 1000 + ms * 10LL;
    return tm;
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
 * target, which has the type ASS_Style* or ass_even_t*.
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
		target->name = string2timecode(track->library, token);

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
 * \param n_ignored number of format options to skip at the beginning
*/
static int process_event_tail(ass_track_t *track, ass_even_t *event,
                              char *str, int n_ignored)
{
    char *token;
    char *tname;
    char *p = str;
    int i;
    ass_even_t *target = event;

    char *format = strdup(track->event_format);
    char *q = format;           // format scanning pointer

    if (track->n_styles == 0) {
        // add "Default" style to the end
        // will be used if track does not contain a default style (or even does not contain styles at all)
        int sid = ass_alloc_style(track);
        set_default_style(&track->styles[sid]);
        track->default_style = sid;
    }

    for (i = 0; i < n_ignored; ++i) {
        NEXT(q, tname);
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
 * \brief Parse command line style overrides (--ass-force-style option)
 * \param track track to apply overrides to
 * The format for overrides is [StyleName.]Field=Value
 */
void ass_process_force_style(ass_track_t *track)
{
    char **fs, *eq, *dt, *style, *tname, *token;
    ASS_Style *target;
    int sid;
    char **list = track->library->style_overrides;

    if (!list)
        return;

    for (fs = list; *fs; ++fs) {
        eq = strrchr(*fs, '=');
        if (!eq)
            continue;
        *eq = '\0';
        token = eq + 1;

        if (!ass_strcasecmp(*fs, "PlayResX"))
            track->PlayResX = atoi(token);
        else if (!ass_strcasecmp(*fs, "PlayResY"))
            track->PlayResY = atoi(token);
        else if (!ass_strcasecmp(*fs, "Timer"))
            track->Timer = ass_atof(token);
        else if (!ass_strcasecmp(*fs, "WrapStyle"))
            track->WrapStyle = atoi(token);
        else if (!ass_strcasecmp(*fs, "ScaledBorderAndShadow"))
            track->ScaledBorderAndShadow = parse_bool(token);
        else if (!ass_strcasecmp(*fs, "Kerning"))
            track->Kerning = parse_bool(token);
        else if (!ass_strcasecmp(*fs, "YCbCr Matrix"))
            track->YCbCrMatrix = parse_ycbcr_matrix(token);

        dt = strrchr(*fs, '.');
        if (dt) {
            *dt = '\0';
            style = *fs;
            tname = dt + 1;
        } else {
            style = NULL;
            tname = *fs;
        }
        for (sid = 0; sid < track->n_styles; ++sid) {
            if (style == NULL
                || ass_strcasecmp(track->styles[sid].Name, style) == 0) {
                target = track->styles + sid;
                PARSE_START
                    STRVAL(FontName)
                    COLORVAL(PrimaryColour)
                    COLORVAL(SecondaryColour)
                    COLORVAL(OutlineColour)
                    COLORVAL(BackColour)
                    FPVAL(FontSize)
                    INTVAL(Bold)
                    INTVAL(Italic)
                    INTVAL(Underline)
                    INTVAL(StrikeOut)
                    FPVAL(Spacing)
                    FPVAL(Angle)
                    INTVAL(BorderStyle)
                    INTVAL(Alignment)
                    INTVAL(Justify)
                    INTVAL(MarginL)
                    INTVAL(MarginR)
                    INTVAL(MarginV)
                    INTVAL(Encoding)
                    FPVAL(ScaleX)
                    FPVAL(ScaleY)
                    FPVAL(Outline)
                    FPVAL(Shadow)
                    FPVAL(Blur)
                PARSE_END
            }
        }
        *eq = '=';
        if (dt)
            *dt = '.';
    }
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
    ASS_Style *style;
    ASS_Style *target;

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

    ass_msg(track->library, MSGL_V, "[%p] Style: %s", track, str);

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

static int process_styles_line(ass_track_t *track, char *str)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->style_format);
        track->style_format = strdup(p);
        ass_msg(track->library, MSGL_DBG2, "Style format: %s",
               track->style_format);
    } else if (!strncmp(str, "Style:", 6)) {
        char *p = str + 6;
        skip_spaces(&p);
        process_style(track, p);
    }
    return 0;
}

static int process_info_line(ass_track_t *track, char *str)
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
        // log error here
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
    ass_msg(track->library, MSGL_V,
            "No event format found, using fallback");
}

static int process_events_line(ass_track_t *track, char *str)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->event_format);
        track->event_format = strdup(p);
        ass_msg(track->library, MSGL_DBG2, "Event format: %s", track->event_format);
    } else if (!strncmp(str, "Dialogue:", 9)) {
        // This should never be reached for embedded subtitles.
        // They have slightly different format and are parsed in ass_process_chunk,
        // called directly from demuxer
        int eid;
        ass_even_t *event;

        str += 9;
        skip_spaces(&str);

        eid = ass_alloc_event(track);
        event = track->events + eid;

        // We can't parse events with event_format
        if (!track->event_format)
            event_format_fallback(track);

        process_event_tail(track, event, str, 0);
    } else {
        ass_msg(track->library, MSGL_V, "Not understood: '%.30s'", str);
    }
    return 0;
}

/*
 * \param buf pointer to subtitle text in utf-8
 */
static ass_track_t *parse_memory(char *buf)
{
    ass_track_t *track;
    int i;

    track = calloc(1, sizeof(ass_track_t));
    if (!track)
        return NULL;;

    // process header
    process_text(track, buf);

    // external SSA/ASS subs does not have ReadOrder field
    for (i = 0; i < track->n_events; ++i)
        track->events[i].ReadOrder = i;

    if (track->track_type == TRACK_TYPE_UNKNOWN) {
        ass_free_track(track);
        return 0;
    }

    ass_process_force_style(track);

    return track;
}

static int process_text(ass_track_t *track, char *str)
{
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
        process_line(track, p);
        if (*q == '\0')
            break;
        p = q;
    }
    return 0;
}

/**
 * \brief Parse a header line
 * \param track track
 * \param str string to parse, zero-terminated
*/
static int process_line(ass_track_t *track, char *str)
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
            process_info_line(track, str);
            break;
        case PST_STYLES:
            process_styles_line(track, str);
            break;
        case PST_EVENTS:
            process_events_line(track, str);
            break;
        case PST_FONTS:
            // log some error
            break;
        default:
            break;
        }
    }
    return 0;
}

static vod_status_t
ass_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
	size_t initial_read_size,
	size_t max_metadata_size,
	void** ctx)
{
    // RAFIK: here we should read the entire [SCRIPT_INFO] and [V4+ FORMAT] sections, in addition to header of Dialogue
	u_char* p = buffer->data;

    if (buffer->len > 0 &&
        vod_strncmp(p, ASS_SCRIPT_INFO_HEADER, sizeof(ASS_SCRIPT_INFO_HEADER) - 1) != 0)
    {
        vod_log_debug1(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "ass_reader_init failed, len=%d", buffer->len);
        return VOD_NOT_FOUND;
    }
    vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
        "ass_reader_init passed and called subtitle_reader_init");


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
	return subtitle_parse(
		request_context,
		parse_params,
		source,
		NULL,
		ass_estimate_duration(source),
		metadata_part_count,
		result);
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
