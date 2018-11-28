#ifndef __SCC_FORMAT_H__
#define __SCC_FORMAT_H__

// includes
#include "../media_format.h"

//#define SCC_TEMP_VERBOSITY

#define VALIGN_SUB 0
#define VALIGN_CENTER 8
#define VALIGN_TOP 4

#define SCC_608_SCREEN_WIDTH          32
#define SCC_NUM_OF_STYLES_INSERTED    10
#define SCC_UNUSED_CHAR               0
#define SCC_MAX_LONG_LONG             0xefffffffLL
#define SCC_THRESH_LONG_LONG          (55*60*1000)

#define SCC_MAX_CUE_DURATION_MSEC     3000
#define SCC_MIN_CUE_DURATION_MSEC     1000
#define SCC_MIN_INTER_CUE_DUR_MSEC    100
#define SCC_OFFSET_FOR_SHORTER_LEAD   1000


// globals
extern media_format_t scc_format;

enum cc_text_done
{
    EVENT_TEXT_OPEN = 0,
    EVENT_TEXT_DONE = 1
};

/*
 * scc_event corresponds to a single Dialogue line;
 * text is stored as-is, style overrides gets applied later.
 */
typedef struct scc_event {
    unsigned char      characters[15][33]; // Extra char at the end for potential '\n'
    unsigned char      iub       [15][33]; // Right-most bit is Italic flag, bit 1 is Underline, bit 2 is Flash/Bold
             char      row_used  [15];     // Any data in row?
    unsigned char      color;
    unsigned char      bk_color;
    int                len_text;           // number of visible characters added to this screen
    enum cc_text_done  event_text_done;    // when set to EVENT_TEXT_DONE, no further text is added. EOC was received already.

    long long          start_time;         // ms
    long long          end_time;           // ms
} scc_event_t;

/*
 * scc_track represents a fully parsed SCC file.
 * It is entirely parsed before events are rendered into WebVTT cues.
 */
typedef struct scc_track {
    long long       max_duration;          // ms, added for needs of the vod-module
    long long       initial_offset;        // ms, sometimes Broadcast shift the cues by some 59 minutes relative to video
    int             max_frame_count;       // to identify FPS without external information

    int             n_events;
    int             max_events;
    scc_event_t    *events;

    long long       cue_time;             // ms
    int             cursor_row, cursor_column;
    unsigned char   last_c1, last_c2;
    unsigned char   current_color;        // Color we are currently using to write
    unsigned char   current_bk_color;     // Background color we are currently using to write
    unsigned char   current_iub;          // Current flag values. RMS bit is Italic flag, bit 1 is Underline, bit 2 is Flash/Bold
} scc_track_t;

scc_track_t *scc_parse_memory(char *data, int length, request_context_t* request_context);

enum scc_alignment
{
    SCC_ALIGN_CENTER = 0,
    SCC_ALIGN_LEFT   = 1,
    SCC_ALIGN_RIGHT  = 2
} scc_alignment_t;

enum scc_color_code
{
    COL_WHITE       = 0,
    COL_GREEN       = 1,
    COL_BLUE        = 2,
    COL_CYAN        = 3,
    COL_RED         = 4,
    COL_YELLOW      = 5,
    COL_MAGENTA     = 6,
    COL_USERDEFINED = 7,
    COL_BLACK       = 8,
    COL_TRANSPARENT = 9
} scc_color_code_t;

enum font_bits
{
    FONT_REGULAR                = 0,
    FONT_ITALIC                 = 1,
    FONT_UNDERLINED             = 2,
    FONT_UNDERLINED_ITALIC      = 3,
    FONT_BOLD                   = 4,
    FONT_BOLD_ITALIC            = 5,
    FONT_BOLD_UNDERLINED        = 6,
    FONT_BOLD_UNDERLINED_ITALIC = 7
};

enum command_code
{
    COM_UNKNOWN = 0,
    COM_ERASEDISPLAYEDMEMORY = 1,
    COM_RESUMECAPTIONLOADING = 2,
    COM_ENDOFCAPTION = 3,
    COM_TABOFFSET1 = 4,
    COM_TABOFFSET2 = 5,
    COM_TABOFFSET3 = 6,
    COM_ROLLUP2 = 7,
    COM_ROLLUP3 = 8,
    COM_ROLLUP4 = 9,
    COM_CARRIAGERETURN = 10,
    COM_ERASENONDISPLAYEDMEMORY = 11,
    COM_BACKSPACE = 12,
    COM_RESUMETEXTDISPLAY = 13,
    COM_ALARMOFF =14,
    COM_ALARMON = 15,
    COM_DELETETOENDOFROW = 16,
    COM_RESUMEDIRECTCAPTIONING = 17,
    COM_FLASHON = 18
};

#endif //__SCC_FORMAT_H__
