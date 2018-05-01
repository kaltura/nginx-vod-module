#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include "ass_format.h"
#include <ctype.h>

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

#define MAX_STR_SIZE_EVNT_CHUNK 1024
#define MAX_STR_SIZE_ALL_WEBVTT_STYLES 20480

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
