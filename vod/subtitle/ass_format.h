#ifndef __ASS_FORMAT_H__
#define __ASS_FORMAT_H__

// includes
#include "../media_format.h"

#define VALIGN_SUB 0
#define VALIGN_CENTER 8
#define VALIGN_TOP 4

#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMINMAX(c,a,b) FFMIN(FFMAX(c, a), b)

typedef enum {
    PST_UNKNOWN = 0,
    PST_INFO,
    PST_STYLES,
    PST_EVENTS,
    PST_FONTS
} ParserState;

// globals
extern media_format_t ass_format;

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


void ass_free_track(vod_pool_t* pool, ass_track_t *track);
ass_track_t *parse_memory(char *buf, int len, request_context_t* request_context);

#endif //__ASS_FORMAT_H__
