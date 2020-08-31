#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include "ass_format.h"
#include <ctype.h>

#define ASS_SCRIPT_INFO_HEADER ("[Script Info]")

#define MAX_STR_SIZE_EVNT_CHUNK 1024
#define MAX_STR_SIZE_ALL_WEBVTT_STYLES 20480

#define NUM_OF_INLINE_TAGS_SUPPORTED 3	 //ibu
#define NUM_OF_TAGS_ALLOWED_PER_LINE 1

//#define ASSUME_STYLE_SUPPORT
//#define ASSUME_CSS_SUPPORT
#ifdef ASSUME_STYLE_SUPPORT
#undef ASSUME_CSS_SUPPORT
#endif

#define PAIROF(type, name, w, str)											\
static const int      FIXED_WEBVTT_ ## type ## _ ## name ## _WIDTH = w;		\
static const char*    FIXED_WEBVTT_ ## type ## _ ## name ## _STR   = str;

PAIROF(CUE, 	NAME, 		8,	"c%07d");
PAIROF(ESCAPE,	FOR_RTL,	5,	"&lrm;")
PAIROF(HEADER,	NEWLINES,	10,	WEBVTT_HEADER_NEWLINES)


// STYLE_SUPPORT will insert dynamic styles with the names used in the ASS/SSA file
// CSS_SUPPORT will use static style classes defined in ./css/crstyles.css
// They are mutually exclusive
#ifdef ASSUME_STYLE_SUPPORT
PAIROF(STYLE, 	START, 		22,	"STYLE\r\n::cue(v[voice=\"")
PAIROF(STYLE, 	END,		4,	"\"]) ")
PAIROF(BRACES,	START,		3,	"{\r\n")
PAIROF(BRACES,	END,		3,	"}\r\n")
PAIROF(VOICE,	START,		3,	"<v ")
PAIROF(VOICE,	END,		1,	">")
#endif
#ifdef ASSUME_CSS_SUPPORT
PAIROF(CLASSOPEN,	START,	2,	"<c")
PAIROF(CLASSOPEN,	END,	1,	">")
PAIROF(CLASSCLOSE,	FULL,	4,	"</c>")
PAIROF(COLOR,		FULL,	7,	".color_")
PAIROF(BACKGROUND,	FULL,	9,	".bkcolor_")
PAIROF(OUTLINE,		FULL,	8,	".outline")
PAIROF(STRIKE,		THRU,	14,	".strikethrough")
PAIROF(FONT,		FAM,	8,	".fontfam")
PAIROF(FONT,		SIZE,	9,	".fontsize")
PAIROF(ALPHA,		FULL,	5,	"_full")
PAIROF(ALPHA,		HALF,	5,	"_half")
PAIROF(TIMES,		FONT,	5,	"times")
PAIROF(ARIAL,		FONT,	5,	"arial")
PAIROF(COMIC,		FONT,	5,	"comic")
PAIROF(LUCIDA,		FONT,	6,	"lucida")
PAIROF(COURIER,		FONT,	7,	"courier")
PAIROF(ARABIC,		FONT,	6,	"arabic")
PAIROF(RUSSIAN,		FONT,	7,	"russian")
PAIROF(TREBUCHET,	FONT,	9,	"trebuchet")
#endif


typedef enum
{
	TAG_TYPE_NEWLINE_SMALL	= 0,
	TAG_TYPE_NEWLINE_LARGE	= 1,
	TAG_TYPE_AMPERSANT		= 2,
	TAG_TYPE_SMALLERTHAN	= 3,
	TAG_TYPE_BIGGERTHAN		= 4,

	TAG_TYPE_OPEN_BRACES	= 5,
	TAG_TYPE_CLOSE_BRACES	= 6,

// all starts should be in even index, all ends should be in odd index. This logic is assumed
	TAG_TYPE_IBU_DATUM		= 7,
	TAG_TYPE_ITALIC_END		= 7,
	TAG_TYPE_ITALIC_START	= 8,
	TAG_TYPE_BOLD_END		= 9,
	TAG_TYPE_BOLD_START		= 10,
	TAG_TYPE_UNDER_END		= 11,
	TAG_TYPE_UNDER_START	= 12,

	TAG_TYPE_UNKNOWN_TAG	= 13,
	TAG_TYPE_NONE			= 14
} ass_tag_idx_t;

static const char* tag_strings[TAG_TYPE_NONE] =
{
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
static const int tag_string_len[TAG_TYPE_NONE][2] =
{
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
static const char* tag_replacement_strings[TAG_TYPE_NONE] =
{
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

#ifdef ASSUME_CSS_SUPPORT
static const char*  palette[/*Red*/ 5][/*Green*/ 5][/*Blue*/ 5] =
{
	{
		{"black",			"stratos",			"navy",				"mediumblue",		"blue"			},
		{"deepfir",			"cyprus",			"congressblue",		"cobalt",			"blueribbon"	},
		{"japlaurel",		"fungreen",			"teal",				"lochmara",			"azure"			},
		{"forest",			"malachite",		"caribbean",		"eggblue",			"cerulean"		},
		{"green",			"lime",				"springgreen",		"brightturq",		"cyan"			}
	},
	{
		{"temptress",		"blackberry",		"indigo",			"purpleblue",		"redblue"		},
		{"verdun",			"tundora",			"eastbay",			"governor",			"royalblue"		},
		{"limeade",			"goblin",			"fadedjade",		"steelblue",		"dodger"		},
		{"harlequin",		"emerald",			"shamrock",			"pelorous",			"dodgerblue"	},
		{"brightgreen",		"pastelgreen",		"screaminggreen",	"aquamarine",		"aqua"			}
	},
	{
		{"maroon",			"siren",			"eggplant",			"purple",			"electric"		},
		{"cinnamon",		"lotus",			"cannonpink",		"fuchsiablue",		"heliotrope"	},
		{"olive",			"yellowmetal",		"gray",				"yonder",			"malibu"		},
		{"pistachio",		"wasabi",			"deyork",			"neptune",			"anakiwa"		},
		{"chartreuse",		"greenyellow",		"mintgreen",		"magicmint",		"turquoise"		}
	},
	{
		{"guardsman",		"monza",			"flirt",			"cerise",			"purpleheart"	},
		{"sharonrose",		"crail",			"mulberry",			"amethyst",			"violet"		},
		{"pirategold",		"tussock",			"voldrose",			"viola",			"orchid"		},
		{"buddhagold",		"turmeric",			"pineglade",		"silver",			"melrose"		},
		{"laspalmas",		"starship",			"reef",				"snowymint",		"onahau"		}
	},
	{
		{"red",				"torchred",			"rose",				"pizzazz",			"magenta"		},
		{"vermilion",		"coral",			"strawberry",		"razzledazzle",		"flamingo"		},
		{"flushorange",		"crusta",			"vividtangerine",	"hotpink",			"blushpink"		},
		{"amber",			"yelloworange",		"macncheese",		"yourpink",			"pinklace"		},
		{"yellow",			"goldenfizz",		"dolly",			"shalimar",			"white"			}
	}
};
static const int color_name_size[/*Red*/ 5][/*Green*/ 5][/*Blue*/ 5] =
{
	{
		{5,					7,					4,					10,					4				},
		{7,					6,					12,					6,					10				},
		{9,					8,					4,					8,					5				},
		{6,					9,					9,					7,					8				},
		{5,					4,					11,					10,					4				}
	},
	{
		{9,					10,					6,					10,					7				},
		{6,					7,					7,					8,					9				},
		{7,					6,					9,					9,					6				},
		{9,					7,					8,					8,					10				},
		{11,				11,					14,					10,					4				}
	},
	{
		{6,					5,					8,					6,					8				},
		{8,					5,					10,					11,					10				},
		{5,					11,					4,					6,					6				},
		{9,					6,					6,					7,					7				},
		{10,				11,					9,					9,					9				}
	},
	{
		{9,					5,					5,					6,					11				},
		{10,				5,					8,					8,					6				},
		{10,				7,					8,					5,					6				},
		{10,				8,					9,					6,					7				},
		{9,					8,					4,					9,					6				}
	},
	{
		{3,					8,					4,					7,					7				},
		{9,					5,					10,					12,					8				},
		{11,				6,					14,					7,					9				},
		{5,					12,					10,					8,					8				},
		{6,					10,					5,					8,					5				}
	}
};

static u_char* insert_color_string(u_char *p, uint32_t abgr_word, request_context_t* request_context)
{
	uint32_t red, green, blue;
	blue	= (((abgr_word >> 16) & 0xFF) + 32) >> 6;
	green	= (((abgr_word >>  8) & 0xFF) + 32) >> 6;
	red		= (((abgr_word      ) & 0xFF) + 32) >> 6;
	return vod_sprintf(p, palette[red][green][blue], color_name_size[red][green][blue]);
}
#endif   //ASSUME_CSS_SUPPORT

void swap_events(ass_event_t* nxt, ass_event_t* cur)
{
	ass_event_t tmp;
	vod_memcpy(&tmp,  nxt, sizeof(ass_event_t));
	vod_memcpy( nxt,  cur, sizeof(ass_event_t));
	vod_memcpy( cur, &tmp, sizeof(ass_event_t));
}

static int split_event_text_to_chunks(char *src, int srclen, bool_t rtl, u_char **textp, int *evlen, bool_t *ibu_flags, uint32_t *max_run, request_context_t* request_context)
{
	// a chunk is part of the text that will be added with a specific voice/style. So we increment chunk only when we need a different style applied
	// Number of chunks is at least 1 if len is > 0
	int tagidx;
	int srcidx 			= 0;
	int bBracesOpen		= 0;
	int chunkidx 		= 0;
	uint32_t cur_run	= 0;
	u_char *text_start	= textp[chunkidx];

	if ((src == NULL) || (srclen < 1) || (srclen > MAX_STR_SIZE_EVNT_CHUNK))
	{
		return 0;
	}

	if (rtl)
	{
		 textp[chunkidx] = vod_copy(textp[chunkidx], FIXED_WEBVTT_ESCAPE_FOR_RTL_STR, FIXED_WEBVTT_ESCAPE_FOR_RTL_WIDTH);
	}

	// insert openers to currently open modes, ordered as <i><b><u>
	if (ibu_flags[0])
	{
		textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_ITALIC_START], tag_string_len[TAG_TYPE_ITALIC_START][1]);
	}
	if (ibu_flags[1])
	{
		textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_BOLD_START], tag_string_len[TAG_TYPE_BOLD_START][1]);
	}
	if (ibu_flags[2])
	{
		textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_UNDER_START], tag_string_len[TAG_TYPE_UNDER_START][1]);
	}

	while (srcidx < srclen)
	{
		for (tagidx = 0; tagidx < TAG_TYPE_NONE; tagidx++)
		{
			if (vod_strncmp(src+srcidx, tag_strings[tagidx], tag_string_len[tagidx][0]) == 0)
			{
				char* curloc;
				srcidx += tag_string_len[tagidx][0];
				curloc = src + srcidx;

				switch (tagidx) {
					case (TAG_TYPE_ITALIC_END):
					case (TAG_TYPE_BOLD_END):
					case (TAG_TYPE_UNDER_END):
					case (TAG_TYPE_ITALIC_START):
					case (TAG_TYPE_BOLD_START):
					case (TAG_TYPE_UNDER_START): {
						int ibu_idx = (tagidx - TAG_TYPE_IBU_DATUM) >> 1;
						bool_t opposite = (tagidx & 1) == 1;

						if (ibu_flags[ibu_idx] == opposite)
						{
							// insert closures to open spans, ordered as </u></b></i>
							if (ibu_flags[2])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_UNDER_END], tag_string_len[TAG_TYPE_UNDER_END][1]);
							}
							if (ibu_flags[1])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_BOLD_END], tag_string_len[TAG_TYPE_BOLD_END][1]);
							}
							if (ibu_flags[0])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_ITALIC_END], tag_string_len[TAG_TYPE_ITALIC_END][1]);
							}

							ibu_flags[ibu_idx] = !ibu_flags[ibu_idx];

							if (ibu_flags[0])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_ITALIC_START], tag_string_len[TAG_TYPE_ITALIC_START][1]);
							}
							if (ibu_flags[1])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_BOLD_START], tag_string_len[TAG_TYPE_BOLD_START][1]);
							}
							if (ibu_flags[2])
							{
								textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_UNDER_START], tag_string_len[TAG_TYPE_UNDER_START][1]);
							}
						}
					} break;

					case (TAG_TYPE_NEWLINE_LARGE):
					case (TAG_TYPE_NEWLINE_SMALL):
					{
						if (cur_run > *max_run)
						{
							*max_run = cur_run; // we don't add the size of \r\n since they are not visible on screen.
							cur_run = 0;		// max_run holds the longest run of visible characters on any line.
						}

						textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
						if (rtl)
						{
							 textp[chunkidx] = vod_copy(textp[chunkidx], FIXED_WEBVTT_ESCAPE_FOR_RTL_STR, FIXED_WEBVTT_ESCAPE_FOR_RTL_WIDTH);
						}
					} break;

					case (TAG_TYPE_AMPERSANT):
					case (TAG_TYPE_BIGGERTHAN):
					case (TAG_TYPE_SMALLERTHAN):
					{
						cur_run++;
						textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
					} break;

					case (TAG_TYPE_OPEN_BRACES):
					case (TAG_TYPE_CLOSE_BRACES):
					{
						bBracesOpen = (tagidx & 1);
						textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
					} break;

					default:
					{
						textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[tagidx], tag_string_len[tagidx][1]);
					}
				}

				// if the next char is not "\\" or "}", then ignore all characters between here and then
				// (case of \b400) or unsupported \xxxxxxx tag
				if (bBracesOpen && (*curloc != '}') && (*curloc != '\\'))
				{
					char*  nearest;
					char*  nearslash = vod_strchr(curloc, '\\');
					char*  nearbrace = vod_strchr(curloc, '}');
					if (nearslash == NULL)  nearslash = nearbrace;
					if (nearbrace == NULL)  nearbrace = nearslash;
					nearest = FFMIN(nearslash, nearbrace);
					srcidx = (int)(FFMAX(nearest, curloc+1) - src);
				}

				tagidx = -1; //start all tags again, cause they can come in any order
			}
		}
		// none of the tags matched this character
		if (tagidx == TAG_TYPE_NONE)
		{
			// for Arabic language, we want to increment number of characters only once every 2 bytes
			// Arabic utf8 chars all start with 0xD8 or 0xD9, different from all western languages
			unsigned char cur_char = (unsigned char)(*(src + srcidx));
			if (cur_char != 0xD8 && cur_char != 0xD9)
				cur_run++;

			textp[chunkidx] = vod_copy(textp[chunkidx], src + srcidx, 1);
			srcidx++;
		}
	}

	if (ibu_flags[2])
	{
		textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_UNDER_END], tag_string_len[TAG_TYPE_UNDER_END][1]);
	}
	if (ibu_flags[1])
	{
		 textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_BOLD_END], tag_string_len[TAG_TYPE_BOLD_END][1]);
	}
	if (ibu_flags[0])
	{
		textp[chunkidx] = vod_copy(textp[chunkidx], tag_replacement_strings[TAG_TYPE_ITALIC_END], tag_string_len[TAG_TYPE_ITALIC_END][1]);
	}

	if (cur_run > *max_run)
	{
		*max_run = cur_run;
	}

	evlen[chunkidx] = textp[chunkidx] - text_start;
	textp[chunkidx] = text_start;

	return chunkidx + 1;
}

static void ass_clean_known_mem(request_context_t* request_context, ass_track_t *ass_track, u_char** event_textp)
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

#ifdef ASSUME_STYLE_SUPPORT
static u_char* output_one_style(ass_style_t* cur_style, u_char* p)
{
		p = vod_copy(p, FIXED_WEBVTT_STYLE_START_STR, FIXED_WEBVTT_STYLE_START_WIDTH);
		p = vod_copy(p, cur_style->name, vod_strlen(cur_style->name));
		p = vod_copy(p, FIXED_WEBVTT_STYLE_END_STR, FIXED_WEBVTT_STYLE_END_WIDTH);
		p = vod_copy(p, FIXED_WEBVTT_BRACES_START_STR, FIXED_WEBVTT_BRACES_START_WIDTH);

		p = vod_copy(p, "color: #", 8);
		p = vod_sprintf(p, "%08uxD;\r\n", cur_style->primary_colour);


		p = vod_copy(p, "font-family: \"", 14);
		p = vod_copy(p, cur_style->font_name, vod_strlen(cur_style->font_name));
		p = vod_copy(p, "\", sans-serif;\r\n", 16);
		p = vod_sprintf(p, "font-size: %03uDpx;\r\n", cur_style->font_size);

		/*if (cur_style->bold)
		{
			p = vod_copy(p, "font-weight: bold;\r\n", 20);
		}
		if (cur_style->italic)
		{
			p = vod_copy(p, "font-style: italic;\r\n", 21);
		}
		// This will inherit the outline_colour (and shadow) if border_style==1, otherwise it inherits primary_colour
		if (cur_style->underline)
		{
			// available styles are: solid | double | dotted | dashed | wavy
			// available lines are: underline || overline || line-through || blink
			p = vod_copy(p, "text-decoration: solid underline;\r\n", 35);
		}
		else if (cur_style->strike_out)
		{
			// available lines are: underline || overline || line-through || blink
			p = vod_copy(p, "text-decoration: solid line-through;\r\n", 38);
		}*/

		if (cur_style->border_style == 1 /*&& ass_track->type == TRACK_TYPE_ASS*/)
		{
			// webkit is not supported by all players, stick to adding outline using text-shadow
			p = vod_copy(p, "text-shadow: ", 13);
			// add outline in 4 directions with the outline color
			p = vod_sprintf(p,
				"#%08uxD -%01uDpx 0px, #%08uxD 0px %01uDpx, #%08uxD 0px -%01uDpx, #%08uxD %01uDpx 0px, #%08uxD %01uDpx %01uDpx 0px;\r\n",
				cur_style->outline_colour, cur_style->outline,
				cur_style->outline_colour, cur_style->outline,
				cur_style->outline_colour, cur_style->outline,
				cur_style->outline_colour, cur_style->outline,
				cur_style->back_colour, cur_style->shadow, cur_style->shadow);
		}
		else
		{
			p = vod_copy(p, "background-color: #", 19);
			p = vod_sprintf(p, "%08uxD;\r\n", cur_style->back_colour);
		}
		p = vod_copy(p, FIXED_WEBVTT_BRACES_END_STR, FIXED_WEBVTT_BRACES_END_WIDTH);
		p = vod_copy(p, "\r\n", 2);

		return p;
}
#endif //ASSUME_STYLE_SUPPORT

static vod_status_t
ass_reader_init(
	request_context_t* request_context,
	vod_str_t* buffer,
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
		(uint64_t)(ass_track->max_duration),
		metadata_part_count,
		result);

	// now that we used max_duration, we need to free the memory used by the track
	ass_free_track(request_context->pool, ass_track);
	return ret_status;
}

/**
 * \brief Parse the .ass/.ssa file, convert to webvtt, output all cues as frames
 * In the following function event == frame == cue. All words point to the text in ASS/media-struct/WebVTT.
 *
 * \output vtt_track->media_info.extra_data		(WEBVTT header + all STYLE cues)
 * \output vtt_track->total_frames_duration		(sum of output frame durations)
 * \output vtt_track->first_frame_index			(event index for very first event output in this segment)
 * \output vtt_track->first_frame_time_offset	(Start time of the very first event output in this segment)
 * \output vtt_track->total_frames_size			(Number of String Bytes used in all events that were output)
 * \output vtt_track->frame_count				(Number of events output in this segment)
 * \output vtt_track->frames.clip_to			(the upper clipping bound of this segment)
 * \output vtt_track->frames.first_frame		(pointer to first frame structure in the linked list)
 * \output vtt_track->frames.last_frame			(pointer to last frame structure in the linked list)
 * \output result								(media track in the track array)
 *
 * individual cues in the frames array
 * \output cur_frame->duration					(start time of next  output event - start time of current event)
 * if last event to be output but not last in file:	(start time of next		 event - start time of current event)
 * if last event in whole file:					(end time of current output event - start time of current event)
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
	struct segmenter_conf_s* segmenter,		// unused
	read_cache_state_t* read_cache_state,	// unused
	vod_str_t* frame_data,					// unused
	media_format_read_request_t* read_req,	// unused
	media_track_array_t* result)
{
	ass_track_t *ass_track;
	vod_array_t frames;
	subtitle_base_metadata_t* metadata
								= vod_container_of(base, subtitle_base_metadata_t, base);
	vod_str_t* 		source		= &metadata->source;
	media_track_t*	vtt_track	= base->tracks.elts;
	input_frame_t*	cur_frame	= NULL;
	ass_event_t*	cur_event	= NULL;
	vod_str_t* header			= &vtt_track->media_info.extra_data;
	u_char *p, *pfixed;
	int evntcounter;
	int chunkcounter;
	uint64_t base_time;
	uint64_t clip_to;
	uint64_t seg_start;
	uint64_t seg_end;
	uint64_t last_start_time;

	vod_memzero(result, sizeof(*result));
	result->first_track			= vtt_track;
	result->last_track			= vtt_track + 1;
	result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
	result->total_track_count	= 1;

	vtt_track->first_frame_index		= 0;
	vtt_track->first_frame_time_offset	= -1;
	vtt_track->total_frames_size		= 0;
	vtt_track->total_frames_duration 	= 0;
	last_start_time = 0;

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

	// Re-order events so that each event has a starting time that is bigger than or equal than the one before it.
	// This matches WebVTT expectations of cue order. And allows us to calculate frame duration correctly.
	// We don't sort it inside parse_memory() because that function is called twice, and first time no sorting is needed.
	// BUBBLE SORT was chosen to optimize the best-case scenario O(n), since most scripts are already ordered.
	for (evntcounter = 0; evntcounter < ass_track->n_events - 1; evntcounter++)
	{
		// Last evntcounter elements are already in place
		for (chunkcounter = 0; chunkcounter < ass_track->n_events - evntcounter - 1; chunkcounter++)
		{
			ass_event_t* next_event = ass_track->events + chunkcounter + 1;
						 cur_event  = ass_track->events + chunkcounter;
			if (cur_event->start > next_event->start)
			{
				//  Swap the two events
				swap_events(next_event, cur_event);
			}
		}
	}
	// allocate initial array of cues/styles, to be augmented as needed after the first 5
	if (vod_array_init(&frames, request_context->pool, 5, sizeof(input_frame_t)) != VOD_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"ass_parse_frames: vod_array_init failed");
		ass_free_track(request_context->pool, ass_track);
		return VOD_ALLOC_FAILED;
	}

	seg_start = parse_params->range->start + parse_params->clip_from;

	if ((parse_params->parse_type & PARSE_FLAG_RELATIVE_TIMESTAMPS) != 0)
	{
		base_time = seg_start;
		clip_to = parse_params->range->end - parse_params->range->start;
		seg_end = clip_to;
	}
	else
	{
		base_time = parse_params->clip_from;
		clip_to = parse_params->clip_to;
		seg_end = parse_params->range->end;
	}

	// We now insert all cues that include their positioning info
	// Events are assumed already ordered by their start time. As required for WebVTT output Cues.
	for (evntcounter = 0; evntcounter < ass_track->n_events; evntcounter++)
	{
		cur_event = ass_track->events + evntcounter;
		ass_style_t* cur_style = ass_track->styles + cur_event->style; //cur_event->style will be zero for an unknown style name

		// make all timing checks and clipping, before we decide to read the text or output it.
		// to make sure this event should be included in the segment.
		if (cur_event->end < 0 || cur_event->start < 0 || cur_event->start >= cur_event->end)
		{
			continue;
		}
		if ((uint64_t)cur_event->end < seg_start)
		{
			continue;
		}

		// apply clipping
		if (cur_event->start >= (int64_t)base_time)
		{
			cur_event->start -= base_time;
			if ((uint64_t)cur_event->start > clip_to)
			{
				cur_event->start = (long long)(clip_to);
			}
		}
		else
		{
			cur_event->start = 0;
		}

		cur_event->end -= base_time;
		if ((uint64_t)cur_event->end > clip_to)
		{
			cur_event->end = (long long)(clip_to);
		}

		if (cur_frame != NULL)
		{
			cur_frame->duration = cur_event->start - last_start_time;
			vtt_track->total_frames_duration += cur_frame->duration;
		}
		else
		{
			// if this is the very first event intersecting with segment, this is the first start in the segment
			vtt_track->first_frame_time_offset	= cur_event->start;
			vtt_track->first_frame_index		= evntcounter;
		}

		if ((uint64_t)cur_event->start >= seg_end)
		{
			// events are already ordered by start-time
			break;
		}


		///// This EVENT is within the segment duration. Parse its text, and output it after conversion to WebVTT valid tags./////

		// Split the event text into multiple chunks so we can insert each chunk as a separate frame in webVTT, all under a single cue
		u_char*	event_textp[NUM_OF_TAGS_ALLOWED_PER_LINE];
		int		event_len  [NUM_OF_TAGS_ALLOWED_PER_LINE];
		int		marg_l, marg_r, marg_v; // all of these are integer percentage values

		// allocate memory for the chunk pointer itself first
		for (chunkcounter = 0; chunkcounter < NUM_OF_TAGS_ALLOWED_PER_LINE; chunkcounter++)
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
			event_len[chunkcounter] = 0;
		}

		bool_t  ibu_flags[NUM_OF_INLINE_TAGS_SUPPORTED] = {cur_style->italic, cur_style->bold, cur_style->underline};
		uint32_t max_run = 0;
		int  num_chunks_in_text =	split_event_text_to_chunks(cur_event->text, vod_strlen(cur_event->text),
									cur_style->right_to_left_language, event_textp, event_len,
									ibu_flags, &max_run, request_context);

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
		p = pfixed = (u_char *)vod_alloc(request_context->pool, MAX_STR_SIZE_EVNT_CHUNK);
		if (p == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"ass_parse_frames: vod_alloc failed");
			ass_clean_known_mem(request_context, ass_track, event_textp);
			return VOD_ALLOC_FAILED;
		}

		if (evntcounter == (ass_track->n_events - 1))
		{
			cur_frame->duration = cur_event->end - cur_event->start;
			vtt_track->total_frames_duration += cur_frame->duration;
		}

		// Cues are named "c<iteration_number_in_7_digits>" starting from c0000000
		p = vod_sprintf(p, FIXED_WEBVTT_CUE_NAME_STR, evntcounter);
		p = vod_copy(p, "\r\n", 2);
		// timestamps will be inserted here, we now insert positioning and alignment changes
		{
			bool_t	bleft = FALSE, bright = FALSE;
			if ((cur_style->alignment & 1) == 0)			//center alignment  2/6/10
			{
				// do nothing
			}
			else if (((cur_style->alignment - 1) & 3) == 0)	//left	 alignment  1/5/9
			{
				bleft  = TRUE;
			}
			else
			{												//right	 alignment  3/7/11
				bright = TRUE;
			}

			marg_l = ((cur_event->margin_l > 0) ? cur_event->margin_l : cur_style->margin_l) * 100 / ass_track->play_res_x;
			marg_r = (ass_track->play_res_x - ((cur_event->margin_r > 0) ? cur_event->margin_r : cur_style->margin_r)) * 100 / ass_track->play_res_x;
			marg_v = ((cur_event->margin_v > 0) ? cur_event->margin_v : cur_style->margin_v) * 100 / ass_track->play_res_y; // top assumed
			// All the margX variables are percentages in rounded integer values.
			// line is integer in range of [0 - 12] given 16 rows of lines in the frame.
			if (marg_l || marg_r || marg_v)
			{
				// center/middle means we are giving the coordinate of the center/middle point
				int line, sizeH, pos;

				if (cur_style->alignment >= VALIGN_CENTER)		//middle alignment  for values 9,10,11
				{
					line = 7;
				} else if (cur_style->alignment < VALIGN_TOP)	//bottom alignment  for values 1, 2, 3
				{
					marg_v = 100 - marg_v;
					line = FFMINMAX((marg_v+4) >> 3, 0, 12);
				}
				else											//top alignment is the default assumption
				{
					line = FFMINMAX((marg_v+4) >> 3, 0, 12);
				}

				// cap horizontal size to more than 2x to make sure we don't break lines with generic font
				// cap horizontal size to no more than 3x to make sure we allow right and left aligned events on same line
				sizeH = FFMINMAX(marg_r - marg_l, (int)(max_run*2), (int)(max_run*3));
				if ((!bleft) && (!bright))						//center alignment  2/6/10
				{
					pos = FFMINMAX((marg_r + marg_l + 1)/2, sizeH/2, 100 - sizeH/2);
				}
				else if (bleft)									//left   alignment  1/5/9
				{
					pos = FFMINMAX(marg_l, 0, 100 - sizeH);
				}
				else											//right  alignment  3/7/11
				{
					pos = FFMINMAX(marg_r, sizeH, 100);
				}

				p = vod_copy(p, " position:", 10);
				p = vod_sprintf(p, "%03uD", pos);
				p = vod_copy(p, "% size:", 7);
				p = vod_sprintf(p, "%03uD", sizeH);
				p = vod_copy(p, "% line:", 7);
				p = vod_sprintf(p, "%02uD", line);
			}
			// We should only insert this if an alignment override tag {\a...}is in the text, otherwise follow the style's alignment
			// but for now, insert it all the time till all players can read styles
			p = vod_copy(p, " align:", 7);
			if ((!bleft) && (!bright))							//center alignment  2/6/10
			{
				p = vod_copy(p, "center", 6);
			}
			else if (bleft)										//left	 alignment  1/5/9
			{
				p = vod_copy(p, "left", 4);
			}
			else												//right  alignment  3/7/11
			{
				p = vod_copy(p, "right", 5);
			}
			p = vod_copy(p, "\r\n", 2);
		}
#ifdef ASSUME_STYLE_SUPPORT
		p = vod_copy(p, FIXED_WEBVTT_VOICE_START_STR, FIXED_WEBVTT_VOICE_START_WIDTH);
		p = vod_sprintf(p, cur_style->name, vod_strlen(cur_style->name));
		p = vod_copy(p, FIXED_WEBVTT_VOICE_END_STR, FIXED_WEBVTT_VOICE_END_WIDTH);
#endif //ASSUME_STYLE_SUPPORT

#ifdef ASSUME_CSS_SUPPORT
		p = vod_copy(p, FIXED_WEBVTT_CLASSOPEN_START_STR, FIXED_WEBVTT_CLASSOPEN_START_WIDTH);
		// insert primary color class
		{
			uint32_t alpha = ((cur_style->primary_colour >> 24) & 0xFF);
			p = vod_copy(p, FIXED_WEBVTT_COLOR_FULL_STR, FIXED_WEBVTT_COLOR_FULL_WIDTH);
			p = insert_color_string(p, cur_style->primary_colour, request_context);
			if (alpha <= 64)
				p = vod_copy(p, FIXED_WEBVTT_ALPHA_FULL_STR, FIXED_WEBVTT_ALPHA_FULL_WIDTH);
			else
				p = vod_copy(p, FIXED_WEBVTT_ALPHA_HALF_STR, FIXED_WEBVTT_ALPHA_HALF_WIDTH);
		}
		// insert outline or background color
		if (cur_style->border_style == 3)
		{
			// Opaque box with background color
			uint32_t alpha = ((cur_style->back_colour >> 24) & 0xFF);
			p = vod_copy(p, FIXED_WEBVTT_BACKGROUND_FULL_STR, FIXED_WEBVTT_BACKGROUND_FULL_WIDTH);
			p = insert_color_string(p, cur_style->back_colour, request_context);
			if (alpha <= 64)
				p = vod_copy(p, FIXED_WEBVTT_ALPHA_FULL_STR, FIXED_WEBVTT_ALPHA_FULL_WIDTH);
			else
				p = vod_copy(p, FIXED_WEBVTT_ALPHA_HALF_STR, FIXED_WEBVTT_ALPHA_HALF_WIDTH);
		}
		else if (cur_style->outline > 0 && cur_style->outline <= 4)
		{
			// Outline color as dictated, with size in pixels as dictated
			p = vod_copy(p, FIXED_WEBVTT_OUTLINE_FULL_STR, FIXED_WEBVTT_OUTLINE_FULL_WIDTH);
			if (cur_style->outline == 1)
				p 	= vod_copy(p, "1", 1);
			else if (cur_style->outline == 2)
				p 	= vod_copy(p, "2", 1);
			else
				p 	= vod_copy(p, "3", 1);
			p = insert_color_string(p, cur_style->outline_colour, request_context);
		}

		// insert strikethrough
		if (cur_style->strike_out)
			p = vod_copy(p, FIXED_WEBVTT_STRIKE_THRU_STR, FIXED_WEBVTT_STRIKE_THRU_WIDTH);

		// insert font-family
		p = vod_copy(p, FIXED_WEBVTT_FONT_FAM_STR, FIXED_WEBVTT_FONT_FAM_WIDTH);

		if (cur_style->right_to_left_language)
			p = vod_copy(p, FIXED_WEBVTT_ARABIC_FONT_STR, FIXED_WEBVTT_ARABIC_FONT_WIDTH);
		else if ( !vod_strncmp(ass_track->title,	"Русский", TITLE_BYTES_CONSIDERED) ||
				  !vod_strncmp(ass_track->language,	"Русский", TITLE_BYTES_CONSIDERED) )
			p = vod_copy(p, FIXED_WEBVTT_RUSSIAN_FONT_STR, FIXED_WEBVTT_RUSSIAN_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Times New Roman", 15) )
			p = vod_copy(p, FIXED_WEBVTT_TIMES_FONT_STR, FIXED_WEBVTT_TIMES_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Trebuchet MS", 12) )
			p = vod_copy(p, FIXED_WEBVTT_TREBUCHET_FONT_STR, FIXED_WEBVTT_TREBUCHET_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Comic Sans MS", 13) )
			p = vod_copy(p, FIXED_WEBVTT_COMIC_FONT_STR, FIXED_WEBVTT_COMIC_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Lucida Console", 14) )
			p = vod_copy(p, FIXED_WEBVTT_LUCIDA_FONT_STR, FIXED_WEBVTT_LUCIDA_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Courier New", 11) )
			p = vod_copy(p, FIXED_WEBVTT_COURIER_FONT_STR, FIXED_WEBVTT_COURIER_FONT_WIDTH);
		else if ( !vod_strncmp(cur_style->font_name, "Times New Roman", 15) )
			p = vod_copy(p, FIXED_WEBVTT_TIMES_FONT_STR, FIXED_WEBVTT_TIMES_FONT_WIDTH);
		else
			p = vod_copy(p, FIXED_WEBVTT_ARIAL_FONT_STR, FIXED_WEBVTT_ARIAL_FONT_WIDTH);

				vod_log_error(VOD_LOG_ERR, request_context->log, 0,
        			"title=%s, language=%s", ass_track->title, ass_track->language);
		// insert font-size
		p = vod_copy(p, FIXED_WEBVTT_FONT_SIZE_STR, FIXED_WEBVTT_FONT_SIZE_WIDTH);
		if (cur_style->font_size < 16)
			p = vod_snprintf(p, 2, "%d", 16);
		else if (cur_style->font_size > 45)
			p = vod_snprintf(p, 2, "%d", 45);
		else
			p = vod_snprintf(p, 2, "%d", cur_style->font_size);

		p = vod_copy(p, FIXED_WEBVTT_CLASSOPEN_END_STR, FIXED_WEBVTT_CLASSOPEN_END_WIDTH);
#endif

		for (chunkcounter = 0; chunkcounter < num_chunks_in_text; chunkcounter++)
		{
			 p = vod_copy(p, event_textp[chunkcounter], event_len[chunkcounter]);
		}
#ifdef ASSUME_CSS_SUPPORT
		p = vod_copy(p, FIXED_WEBVTT_CLASSCLOSE_FULL_STR, FIXED_WEBVTT_CLASSCLOSE_FULL_WIDTH);
#endif
		p = vod_copy(p, "\r\n", 2);
		// we still need an empty line after each event/cue
		p = vod_copy(p, "\r\n", 2);

		// Note: mapping of cue into input_frame_t:
		// - offset = pointer to buffer containing: cue id, cue settings list, cue payload
		// - size = size of data pointed by offset
		// - key_frame = cue id length
		// - duration = start time of next event - start time of current event
		// - pts_delay = end time - start time = duration this subtitle event is on screen

		cur_frame->offset		= (uintptr_t)pfixed;
		cur_frame->size			= (uint32_t)(p - pfixed);
		cur_frame->key_frame	= FIXED_WEBVTT_CUE_NAME_WIDTH + 2; // cue name + \r\n
		cur_frame->pts_delay	= cur_event->end - cur_event->start;

		vtt_track->total_frames_size += cur_frame->size;
		cur_style->output_in_cur_segment = TRUE; // output this style as part of this segment

		last_start_time = cur_event->start;
	}

	//allocate memory for the style's text string
	p = pfixed = (u_char *)vod_alloc(request_context->pool, MAX_STR_SIZE_ALL_WEBVTT_STYLES);
	if (p == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"ass_parse_frames: vod_alloc failed");
		ass_free_track(request_context->pool, ass_track);
		return VOD_ALLOC_FAILED;
	}

	// We now insert header and all style definitions
	header->data = pfixed;
	p = vod_copy(p, FIXED_WEBVTT_HEADER_NEWLINES_STR, FIXED_WEBVTT_HEADER_NEWLINES_WIDTH);
#ifdef ASSUME_STYLE_SUPPORT
	int stylecounter;
	for (stylecounter = (ass_track->default_style ? 1 : 0); (stylecounter < ass_track->n_styles); stylecounter++)
	{
		ass_style_t* cur_style = ass_track->styles + stylecounter;
		if (cur_style->output_in_cur_segment)
			p = output_one_style(cur_style, p);

	}
#endif //ASSUME_STYLE_SUPPORT
	header->len						= (size_t)(p - pfixed);

	// now we got all the info from ass_track, deallocate its memory
	ass_free_track(request_context->pool, ass_track);

	vtt_track->frame_count			= frames.nelts;
	vtt_track->frames.clip_to		= clip_to;
	vtt_track->frames.first_frame	= frames.elts;
	vtt_track->frames.last_frame	= vtt_track->frames.first_frame + frames.nelts;

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
