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

#define TITLE_BYTES_CONSIDERED 14

typedef enum
{
	PST_UNKNOWN = 0,
	PST_INFO,
	PST_STYLES,
	PST_EVENTS,
	PST_FONTS
} ParserState;

// globals
extern media_format_t ass_format;

/* ASS Style: line */
typedef struct ass_style
{
	char		*name;
	char		*font_name;
	int			font_size;
	uint32_t	primary_colour;
	uint32_t	secondary_colour;
	uint32_t	outline_colour;
	uint32_t	back_colour;
	bool_t		bold;
	bool_t		italic;
	bool_t		underline;
	bool_t		strike_out;
	bool_t		right_to_left_language;
	bool_t		output_in_cur_segment;
	double		scale_x;
	double		scale_y;
	double		spacing;
	double		angle;
	int			border_style;  // 1 means Outline + Shadow, 3 means Opaque box
	int			outline;
	int			shadow;
	int			alignment;
	int			margin_l;
	int			margin_r;
	int			margin_v;
	int			encoding;
} ass_style_t;

/*
 * ass_event corresponds to a single Dialogue line;
 * text is stored as-is, style overrides will be parsed later.
 */
typedef struct ass_event
{
	long long	start;	// ms
	long long	end;	// ms

	int			layer;
	int			style;
	char		*name;
	int			margin_l;
	int			margin_r;
	int			margin_v;
	char		*effect;
	char		*text;
	bool_t		right_to_left_language;
} ass_event_t;

/*
 * ass_track represent either an external script or a matroska subtitle stream
 * (no real difference between them); it can be used in rendering after the
 * headers are parsed (i.e. events format line read).
 */
typedef struct ass_track
{
	int			n_styles;			// amount used
	int			max_styles;			// amount allocated
	int			n_events;
	int			max_events;
	ass_style_t	*styles;			// array of styles, max_styles length, n_styles used
	ass_event_t	*events;			// the same as styles

	char		*style_format;		// style format line (everything after "Format: ")
	char		*event_format;		// event format line

	enum {
		TRACK_TYPE_UNKNOWN = 0,
		TRACK_TYPE_ASS,
		TRACK_TYPE_SSA
	} track_type;

	// Script header fields
	int			play_res_x;
	int			play_res_y;
	double		timer;
	int			wrap_style;
	int			scaled_border_and_shadow;
	int			kerning;
	char		*language;
	char		*title;
	bool_t		right_to_left_language;

	int			default_style;		// index of default style, defaults to zero
	char		*name;				// file name in case of external subs, 0 for streams
	ParserState	state;

	long long	max_duration;		// ms, added for needs of the vod-module
} ass_track_t;


void ass_free_track(vod_pool_t* pool, ass_track_t *track);
ass_track_t *parse_memory(char *buf, int len, request_context_t* request_context);

#endif //__ASS_FORMAT_H__
