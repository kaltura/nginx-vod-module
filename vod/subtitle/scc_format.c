#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"
#include "scc_format.h"
#include <ctype.h>

#define SCC_SCRIPT_INFO_HEADER ("Scenarist_SCC V1.0")

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
#define FIXED_WEBVTT_ESCAPE_FOR_RTL_STR "&lrm;"
#define FIXED_WEBVTT_ESCAPE_FOR_RTL_WIDTH 5

#define MAX_STR_SIZE_EVNT_CHUNK         1024
#define MAX_STR_SIZE_ALL_WEBVTT_STYLES 20480

#define NUM_OF_INLINE_TAGS_SUPPORTED 3     //iub

//#define ASSUME_STYLE_SUPPORT

static const int utf8_len[80] = {
    2,   // Registered
    2,   // Ring / Angestrom
    2,   // Fraction Half
    2,   // Latin Starting Question Mark
    2,   // Trade Mark
    1,   // Cent Sign ??
    1,   // Pound Sign
    3,   // Eighth Music Note
    2,   // Letter a with grave
    1,   // Space
    2,   // Letter e with grave
    2,   // Letter a with circumflex
    2,   // Letter e with circumflex
    2,   // Letter i with circumflex
    2,   // Letter o with circumflex
    2,   // Letter u with circumflex

    2,   // Letter A with acute
    2,   // Letter E with acute
    2,   // Letter O with acute
    2,   // Letter U with acute
    2,   // Letter U with diaeresis
    2,   // Letter u with diaeresis
    2,   // Inverted apostrophe
    2,   // Inverted exclamation mark
    2,   // Astresk
    2,   // Apostrophe
    3,   // Horizontal bar
    2,   // Copyright sign
    3,   // Service mark
    3,   // Bullet
    3,   // Left double quotation
    3,   // Right double quotation

    2,   // Letter A with grave
    2,   // Letter A with circumflex
    2,   // Letter C with cedilla
    2,   // Letter E with grave
    2,   // Letter E with circumflex
    2,   // Letter E with diaeresis
    2,   // Letter e with diaeresis
    2,   // Letter I with circumflex
    2,   // Letter I with diaeresis
    2,   // Letter i with diaeresis
    2,   // Letter o with circumflex
    2,   // Letter U with grave
    2,   // Letter u with grave
    2,   // Letter U with circumflex
    3,   // Much less than
    3,   // Much greater than

    2,   // Letter A with tilde
    2,   // Letter a with tilde
    2,   // Letter I with acute
    2,   // Letter I with grave
    2,   // Letter i with grave
    2,   // Letter O with grave
    2,   // Letter o with grave
    2,   // Letter O with tilde
    2,   // Letter o with tilde
    1,   // Left curly bracket
    1,   // Right curly bracket
    1,   // Backslash
    1,   // Circumflex accent
    1,   // Low line (underscore)
    2,   // Broken bar
    1,   // Tilde

    2,   // Letter A with diaeresis
    2,   // Letter a with diaeresis
    2,   // Letter O with diaeresis
    2,   // Letter o with diaeresis
    2,   // Small letter sharp s
    2,   // Yen sign
    2,   // Currency sign
    1,   // Vertical line
    2,   // Letter A with ring above
    2,   // Letter a with ring above
    2,   // Letter O with a stroke
    2,   // Letter o with a stroke
    3,   // Box top left
    3,   // Box top right
    3,   // Box bot left
    3    // Box bot right
};

static const char* utf8_code[80] = {
    "\xc2\xae",     // Registered sign
    "\xc2\xb0",     // Ring / Angestrom
    "\xc2\xbd",     // Fraction Half
    "\xc2\xbf",     // Latin Starting Question Mark
    "\x21\x22",     // Trade Mark
    "\xa2",         // Cent Sign ??
    "\xa3",         // Pound Sign
    "\xe2\x99\xaa", // Eighth Music Note
    "\xc3\xa0",     // Letter a with grave
    " ",            // Space ??
    "\xc3\xa8",     // Letter e with grave
    "\xc3\xa2",     // Letter a with circumflex
    "\xc3\xaa",     // Letter e with circumflex
    "\xc3\xae",     // Letter i with circumflex
    "\xc3\xb4",     // Letter o with circumflex
    "\xc3\xbb",     // Letter u with circumflex

    "\xc3\x81",     // Letter A with acute
    "\xc3\x89",     // Letter E with acute
    "\xc3\x93",     // Letter O with acute
    "\xc3\x9a",     // Letter U with acute
    "\xc3\x9c",     // Letter U with diaeresis
    "\xc3\xbc",     // Letter u with diaeresis
    "\xca\xbb",     // Inverted apostrophe
    "\xc2\xa1",     // Inverted exclamation mark
    "\x2a",         // Asterisk
    "\xc3\xbc",     // Apostrophe
    "\xe2\x80\x95", // Horizontal bar
    "\xc2\xa9",     // Copyright sign
    "\xe2\x84\xa0", // Service mark
    "\xe2\x80\xa2", // Bullet
    "\xe2\x80\x9c", // Left double quotation
    "\xe2\x80\x9d", // Right double quotation

    "\xc3\x80",     // Letter A with grave
    "\xc3\x82",     // Letter A with circumflex
    "\xc3\x87",     // Letter C with cedilla
    "\xc3\x88",     // Letter E with grave
    "\xc3\x8a",     // Letter E with circumflex
    "\xc3\x8b",     // Letter E with diaeresis
    "\xc3\xab",     // Letter e with diaeresis
    "\xc3\x8e",     // Letter I with circumflex
    "\xc3\x8f",     // Letter I with diaeresis
    "\xc3\xaf",     // Letter i with diaeresis
    "\xc3\xb4",     // Letter o with circumflex
    "\xc3\x99",     // Letter U with grave
    "\xc3\xb9",     // Letter u with grave
    "\xc3\x9b",     // Letter U with circumflex
    "\xe2\x89\xaa", // Much less than
    "\xe2\x89\xab", // Much greater than

    "\xc3\x83",     // Letter A with tilde
    "\xc3\xa3",     // Letter a with tilde
    "\xc3\x8d",     // Letter I with grave
    "\xc3\x8c",     // Letter I with acute
    "\xc3\xac",     // Letter i with grave
    "\xc3\x92",     // Letter O with grave
    "\xc3\xb2",     // Letter o with grave
    "\xc3\x95",     // Letter O with tilde
    "\xc3\xb5",     // Letter o with tilde
    "\x7b",         // Left curly bracket
    "\x7d",         // Right curly bracket
    "\x5c",         // Backslash
    "\x5e",         // Circumflex accent
    "\x5f",         // Low line (underscore)
    "\xc2\xa6",     // Broken bar
    "\x7e",         // Tilde

    "\xc3\x84",     // Letter A with diaeresis
    "\xc3\xa4",     // Letter a with diaeresis
    "\xc3\x96",     // Letter O with diaeresis
    "\xc3\xb6",     // Letter o with diaeresis
    "\xc3\x9f",     // Small letter sharp s
    "\xc2\xa5",     // Yen sign
    "\xc2\xa4",     // Currency sign
    "\x7c",         // Vertical line
    "\xc3\x85",     // Letter A with ring above
    "\xc3\xa5",     // Letter a with ring above
    "\xc3\x98",     // Letter O with a stroke
    "\xc3\xb8",     // Letter o with a stroke
    "\xe2\x94\x8f", // Box top left
    "\xe2\x94\x93", // Box top right
    "\xe2\x94\x97", // Box bot left
    "\xe2\x94\x9b"  // Box bot right
};

static void scc_swap_events(scc_event_t* nxt, scc_event_t* cur)
{
    scc_event_t tmp;
    vod_memcpy(&tmp,  nxt, sizeof(scc_event_t));
    vod_memcpy( nxt,  cur, sizeof(scc_event_t));
    vod_memcpy( cur, &tmp, sizeof(scc_event_t));
}

static int convert_event_text(scc_event_t *event, char *textp, request_context_t* request_context)
{
    int colidx, rowidx, dstidx = 0;
    unsigned char iub_flags = 0; // non-italic, non-underlined, non-bold/flash font

    for (rowidx = 0; rowidx < 15; rowidx++)
    {
        if (event->row_used[rowidx] == 1) {
            for (colidx = 0; colidx < SCC_608_SCREEN_WIDTH+1; colidx++)
            {
                bool_t printable = (event->characters[rowidx][colidx] != 0) ? TRUE : FALSE;
#ifdef SCC_TEMP_VERBOSITY
                vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "convert_event_text(): rowidx=%d, colidx=%d, char=%c, iub=%d, printable=%d",
                rowidx, colidx, event->characters[rowidx][colidx], event->iub[rowidx][colidx], printable==TRUE);
#endif
                if (printable == TRUE) {
                    if (iub_flags != event->iub[rowidx][colidx]) {
                        // close b u i in order (if open)
                        if (iub_flags & 4)
                        {
                            vod_memcpy(textp + dstidx, "</b>", 4); dstidx += 4;
                        }
                        if (iub_flags & 2)
                        {
                            vod_memcpy(textp + dstidx, "</u>", 4); dstidx += 4;
                        }
                        if (iub_flags & 1)
                        {
                            vod_memcpy(textp + dstidx, "</i>", 4); dstidx += 4;
                        }
                        // set the running flags
                        iub_flags = event->iub[rowidx][colidx];

                        // open i u b in order (if open)
                        if (iub_flags & 1)
                        {
                            vod_memcpy(textp+ dstidx, "<i>", 3); dstidx += 3;
                        }
                        if (iub_flags & 2)
                        {
                            vod_memcpy(textp+ dstidx, "<u>", 3); dstidx += 3;
                        }
                        if (iub_flags & 4)
                        {
                            vod_memcpy(textp+ dstidx, "<b>", 3); dstidx += 3;
                        }
                    }

                    if (event->characters[rowidx][colidx] >= 0x80 && event->characters[rowidx][colidx] <= 0xcf)
                    {
                        // special and extended characters
                        int utf8_idx = event->characters[rowidx][colidx] - 0x80;
                        vod_memcpy(textp+dstidx, utf8_code[utf8_idx], utf8_len[utf8_idx]);
                        dstidx      += utf8_len[utf8_idx];
                        continue;
                    }
                    else if (colidx == SCC_608_SCREEN_WIDTH && event->characters[rowidx][colidx] == '\n')
                    {
                        textp[dstidx++] = '\r';
                    }
                    else if (event->characters[rowidx][colidx] == '<')
                    {   // Less than is unique in WebVTT
                        vod_memcpy(textp+dstidx, "&lt;", 4);
                        dstidx += 4;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '>')
                    {   // Greater than is unique in WebVTT
                        vod_memcpy(textp+dstidx, "&gt;", 4);
                        dstidx += 4;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '&')
                    {   // Ampersand is unique in WebVTT
                        vod_memcpy(textp+dstidx, "&amp;", 5);
                        dstidx += 5;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x2a')
                    {   // Letter a with acute
                        vod_memcpy(textp+dstidx, "\xc3\xa1", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x5c')
                    {   // Letter e with acute
                        vod_memcpy(textp+dstidx, "\xc3\xa9", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x5e')
                    {   // Letter i with acute
                        vod_memcpy(textp+dstidx, "\xc3\xad", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x5f')
                    {   // Letter o with acute
                        vod_memcpy(textp+dstidx, "\xc3\xb3", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x7b')
                    {   // Letter c withh cedilla
                        vod_memcpy(textp+dstidx, "\xc3\xa7", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x7c')
                    {   // Division sign
                        vod_memcpy(textp+dstidx, "\xc3\xb7", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x7d')
                    {   // Letter N with tilde
                        vod_memcpy(textp+dstidx, "\xc3\x91", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x7e')
                    {   // Letter n with tilde
                        vod_memcpy(textp+dstidx, "\xc3\xb1", 2);
                        dstidx += 2;
                        continue;
                    }
                    else if (event->characters[rowidx][colidx] == '\x7f')
                    {   // Cursor block full
                        vod_memcpy(textp+dstidx, "\xe2\x96\x88", 3);
                        dstidx += 3;
                        continue;
                    }

                    textp[dstidx++] = event->characters[rowidx][colidx];
                }
            }
        }
    }
    // insert closures to open spans, ordered as </b></u></i>
    if (iub_flags & 4)
    {
        vod_memcpy(textp + dstidx, "</b>", 4); dstidx += 4;
    }
    if (iub_flags & 2)
    {
        vod_memcpy(textp + dstidx, "</u>", 4); dstidx += 4;
    }
    if (iub_flags & 1)
    {
        vod_memcpy(textp + dstidx, "</i>", 4); dstidx += 4;
    }

    return dstidx;
}

static void scc_free_track(vod_pool_t* pool, scc_track_t *track, request_context_t* request_context)
{
    free(track->events);
    return;
}

static void scc_clean_known_mem(request_context_t* request_context, scc_track_t *scc_track, char* event_textp)
{
    if (scc_track != NULL)
        scc_free_track(request_context->pool, scc_track, request_context);

    if (event_textp != NULL)
    {
        vod_free(request_context->pool, event_textp);
    }

    return;
}

#ifdef ASSUME_STYLE_SUPPORT
static char* output_one_style(char* p)
{//TODO: using style index, output name an modify the rest of this function
        int len;

        vod_memcpy(p, FIXED_WEBVTT_STYLE_START_STR, FIXED_WEBVTT_STYLE_START_WIDTH);           p+=FIXED_WEBVTT_STYLE_START_WIDTH;
        len = 28; vod_memcpy(p, "RAFIK INSERT STYLE NAME HERE", len);                          p+=len;
        vod_memcpy(p, FIXED_WEBVTT_STYLE_END_STR, FIXED_WEBVTT_STYLE_END_WIDTH);               p+=FIXED_WEBVTT_STYLE_END_WIDTH;
        vod_memcpy(p, FIXED_WEBVTT_BRACES_START_STR, FIXED_WEBVTT_BRACES_START_WIDTH);         p+=FIXED_WEBVTT_BRACES_START_WIDTH;

        len = 8; vod_memcpy(p, "color: #", len);                                               p+=len;
        vod_sprintf((u_char*)p, "%08uxD;\r\n", 0xaabbccdd);                                      p+=11;


        len = 14; vod_memcpy(p, "font-family: \"", len);                                       p+=len;
        len = 27; vod_memcpy(p, "RAFIK INSERT FONT NAME HERE", len);                           p+=len;
        len = 16; vod_memcpy(p, "\", sans-serif;\r\n", len);                                   p+=len;
        vod_sprintf((u_char*)p, "font-size: %03uDpx;\r\n", 24);                                p+=19;

        {
            // webkit is not supported by all players, stick to adding outline using text-shadow
            len = 13; vod_memcpy(p, "text-shadow: ", len);                                     p+=len;
            // add outline in 4 directions with the outline color
            vod_sprintf((u_char*)p, "#%08uxD -%01uDpx 0px, #%08uxD 0px %01uDpx, #%08uxD 0px -%01uDpx, #%08uxD %01uDpx 0px, #%08uxD %01uDpx %01uDpx 0px;\r\n",
                         0xaabbccdd, 2,
                         0xaabbccdd, 2,
                         0xaabbccdd, 2,
                         0xaabbccdd, 2,
                         0x00bbcc00, 2, 2);         p+=102;

        } else {
            len = 19; vod_memcpy(p, "background-color: #", len);                               p+=len;
            vod_sprintf((u_char*)p, "%08uxD;\r\n", 0xaabbccdd);                                p+=11;
        }
        vod_memcpy(p, FIXED_WEBVTT_BRACES_END_STR, FIXED_WEBVTT_BRACES_END_WIDTH);             p+=FIXED_WEBVTT_BRACES_END_WIDTH;
        len = 2; vod_memcpy(p, "\r\n", len);                                                   p+=len;

        return p;
}
#endif //ASSUME_STYLE_SUPPORT

static vod_status_t
scc_reader_init(
    request_context_t* request_context,
    vod_str_t* buffer,
    size_t initial_read_size,
    size_t max_metadata_size,
    void** ctx)
{
    vod_status_t  ret_val;
    u_char* p = buffer->data;

    if (vod_strncmp(p, UTF8_BOM, sizeof(UTF8_BOM) - 1) == 0)
    {
        p += sizeof(UTF8_BOM) - 1;
    }

    // The line that says “Scenarist_SCC V1.0” must be the first line in a v4/v4+ script.
    if (buffer->len > 0 && vod_strncmp(p, SCC_SCRIPT_INFO_HEADER, sizeof(SCC_SCRIPT_INFO_HEADER) - 1) != 0)
    {
        return VOD_NOT_FOUND;
    }

    ret_val = subtitle_reader_init(
        request_context,
        initial_read_size,
        ctx);

    return ret_val;
}

static vod_status_t
scc_parse(
    request_context_t* request_context,
    media_parse_params_t* parse_params,
    vod_str_t* source,
    size_t metadata_part_count,
    media_base_metadata_t** result)
{
    scc_track_t *scc_track;
    vod_status_t ret_status;
    scc_track = scc_parse_memory((char *)(source->data), source->len, request_context);

    if (scc_track == NULL)
    {
        // scc_track was de-allocated already inside the function, for failure cases
        vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
            "scc_parse_memory failed");
        return VOD_BAD_DATA;
    }

    ret_status = subtitle_parse(
        request_context,
        parse_params,
        source,
        NULL,
        (uint64_t)(scc_track->max_duration),
        metadata_part_count,
        result);

#ifdef  SCC_TEMP_VERBOSITY
    vod_log_error(VOD_LOG_ERR, request_context->log, 0,
        "scc_parse(): scc_parse_memory() succeeded: len of data = %d, max_duration = %D, max_frame = %d, nEvents = %d, ret_status=%d",
        source->len, scc_track->max_duration, scc_track->max_frame_count, scc_track->n_events, ret_status);
#endif
    // now that we used max_duration, we need to free the memory used by the track
    scc_free_track(request_context->pool, scc_track, request_context);
    return ret_status;
}

// correct start_time depending on fps and initial offset, then add SCC_OFFSET_FOR_SHORTER_LEAD
static long long scale_sub_sec(long long time, long long offset, int fps)
{
    if (fps <= 0)
        return time;
    long long frame_count = time % 1000;
    long long frames_in_msec = frame_count * 1000 / fps;
    return (time + frames_in_msec - offset - frame_count + SCC_OFFSET_FOR_SHORTER_LEAD);
}
/**
 * \brief Parse the .scc file, convert to webvtt, output all cues as frames
 * In the following function event == frame == cue. All words point to the text in SCC/media-struct/WebVTT.
 *
 * \output vtt_track->media_info.extra_data   (WEBVTT header + all STYLE cues)
 * \output vtt_track->total_frames_duration   (sum of output frame durations)
 * \output vtt_track->first_frame_index       (event index for very first event output in this segment)
 * \output vtt_track->first_frame_time_offset (Start time of the very first event output in this segment)
 * \output vtt_track->total_frames_size       (Number of String Bytes used in all events that were output)
 * \output vtt_track->frame_count             (Number of events output in this segment)
 * \output vtt_track->frames.clip_to          (the upper clipping bound of this segment)
 * \output vtt_track->frames.first_frame      (pointer to first frame structure in the linked list)
 * \output vtt_track->frames.last_frame       (pointer to last frame structure in the linked list)
 * \output result (media track in the track array)
 *
 * individual cues in the frames array
 * \output cur_frame->duration                      (start time of next  output event - start time of current event)
 * if last event to be output but not last in file: (start time of next         event - start time of current event)
 * if last event in whole file:                     (end time of current output event - start time of current event)
 * \output cur_frame->offset
 * \output cur_frame->size
 * \output cur_frame->pts_delay
 * \output cur_frame->key_frame
 *
 * \return int VOD_OK or any of the VOD_ error enums
*/
static vod_status_t
scc_parse_frames(
    request_context_t* request_context,
    media_base_metadata_t* base,
    media_parse_params_t* parse_params,
    struct segmenter_conf_s* segmenter,     // unused
    read_cache_state_t* read_cache_state,   // unused
    vod_str_t* frame_data,                  // unused
    media_format_read_request_t* read_req,  // unused
    media_track_array_t* result)
{
    scc_track_t *scc_track;
    vod_array_t frames;
    subtitle_base_metadata_t* metadata
                              = vod_container_of(base, subtitle_base_metadata_t, base);
    vod_str_t*     source     = &metadata->source;
    media_track_t* vtt_track  = base->tracks.elts;
    input_frame_t* cur_frame  = NULL;
    scc_event_t*   cur_event  = NULL;
    vod_str_t* header         = &vtt_track->media_info.extra_data;
    char *p, *pfixed;
    int len, evntcounter, jj;
    uint64_t base_time, clip_to, seg_start, seg_end, last_start_time;

    vod_memzero(result, sizeof(*result));
    result->first_track       = vtt_track;
    result->last_track        = vtt_track + 1;
    result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
    result->total_track_count = 1;

    vtt_track->first_frame_index       = 0;
    vtt_track->first_frame_time_offset = -1;
    vtt_track->total_frames_size       = 0;
    vtt_track->total_frames_duration   = 0;
    last_start_time = 0;

    if ((parse_params->parse_type & (PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA | PARSE_FLAG_EXTRA_DATA_SIZE)) == 0)
    {
        return VOD_OK;
    }

    scc_track = scc_parse_memory((char *)(source->data), source->len, request_context);

    if (scc_track == NULL)
    {
        // scc_track was de-allocated already inside the function, for failure cases
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "scc_parse_frames(): failed to parse memory into scc track");
        return VOD_BAD_MAPPING;
    }
    else if (scc_track->n_events < 1)
    {
        // File had no valid events
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "scc_parse_frames(): file empty or has no valid events");
        return VOD_BAD_DATA;
    }
#ifdef  SCC_TEMP_VERBOSITY
    else
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "frames scc_parse_memory() succeeded, len of data = %d, max_frame = %d, init_offset = %D, max_duration = %D, nEvents = %d",
            source->len, scc_track->max_frame_count, scc_track->initial_offset, scc_track->max_duration, scc_track->n_events);
    }
#endif

    // Re-order events so that each event has a starting time that is bigger than or equal than the one before it.
    // This matches WebVTT expectations of cue order. And allows us to calculate frame duration correctly.
    // We don't sort it inside scc_parse_memory() because that function is called twice, and first time no sorting is needed.
    // BUBBLE SORT was chosen to optimize the best-case scenario O(n), since most scripts are already ordered.
    for (evntcounter = 0; evntcounter < scc_track->n_events - 1; evntcounter++)
    {
        // Last evntcounter elements are already in place
        for (jj = 0; jj < scc_track->n_events - evntcounter - 1; jj++)
        {
            scc_event_t*   next_event = scc_track->events + jj + 1;
                           cur_event  = scc_track->events + jj;
            // We use >= instead of > so simultaneous events get ordered first-in last-out
            // so WebVTT would move the earlier cues up instead of the newer cues.
            if (cur_event->start_time >= next_event->start_time)
            {
                //  Swap the two events
                scc_swap_events(next_event, cur_event);
            }
        }
    }

    int  fps = (scc_track->max_frame_count < 24) ? 24 :
               (scc_track->max_frame_count < 30) ? 30 :
               (scc_track->max_frame_count < 60) ? 60 :
               (scc_track->max_frame_count + 1);      // inaccurate sub-second times, just to avoid overflow

    // set the end_time of each event depending on next event's start_time, capping to 3 seconds.
    // Set duration of last event in the file to 3 seconds for any input file.
    scc_event_t*  last_event = scc_track->events + scc_track->n_events - 1;
    if (last_event != NULL && fps != 0)
    {
        // correct start_time depending on fps and initial offset, then add one second
        last_event->start_time = scale_sub_sec(last_event->start_time, scc_track->initial_offset, fps);
        last_event->end_time = last_event->start_time + SCC_MAX_CUE_DURATION_MSEC;
    }
    for (evntcounter = scc_track->n_events - 1; evntcounter > 0 ; evntcounter--)
    {
        scc_event_t*  next_event = scc_track->events + evntcounter;     // has times scaled already
                      cur_event  = scc_track->events + evntcounter - 1; // time not adjusted for start, no end calculated

        cur_event->start_time = scale_sub_sec(cur_event->start_time, scc_track->initial_offset, fps);

        if (cur_event->start_time == next_event->start_time)
            // multiple events starting at the same exact time will have same duration (appear simultaneously on screen)
            cur_event->end_time = next_event->end_time;
        else
        {
            // duration is capped to no less than 1 sec, no more than 3 seconds
            // we should cap it to no less than some value
            long long expected_end = next_event->start_time - SCC_MIN_INTER_CUE_DUR_MSEC;
            if (expected_end < (cur_event->start_time + SCC_MIN_CUE_DURATION_MSEC))
                expected_end = (cur_event->start_time + SCC_MIN_CUE_DURATION_MSEC);
            if (expected_end > (cur_event->start_time + SCC_MAX_CUE_DURATION_MSEC))
                expected_end = (cur_event->start_time + SCC_MAX_CUE_DURATION_MSEC);

            cur_event->end_time  = (expected_end > SCC_MIN_CUE_DURATION_MSEC)
                                 ? expected_end
                                 : SCC_MIN_CUE_DURATION_MSEC;
        }
    }

    // allocate initial array of cues/styles, to be augmented as needed after the first 5
    if (vod_array_init(&frames, request_context->pool, 5, sizeof(input_frame_t)) != VOD_OK)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "scc_parse_frames: vod_array_init failed");
        scc_free_track(request_context->pool, scc_track, request_context);
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
    for (evntcounter = 0; evntcounter < scc_track->n_events; evntcounter++)
    {
        cur_event = scc_track->events + evntcounter;

        // make all timing checks and clipping, before we decide to read the text or output it.
        // to make sure this event should be included in the segment.
        if (cur_event->end_time < 0 || cur_event->start_time < 0 || cur_event->start_time >= cur_event->end_time)
        {
            continue;
        }
        if ((uint64_t)cur_event->end_time < seg_start)
        {
            continue;
        }

        // apply clipping
        if (cur_event->start_time >= (int64_t)base_time)
        {
            cur_event->start_time -= base_time;
            if ((uint64_t)cur_event->start_time > clip_to)
            {
                cur_event->start_time = (long long)(clip_to);
            }
        }
        else
        {
            cur_event->start_time = 0;
        }

        cur_event->end_time -= base_time;
        if ((uint64_t)cur_event->end_time > clip_to)
        {
            cur_event->end_time = (long long)(clip_to);
        }

        if (cur_frame != NULL)
        {
            cur_frame->duration = cur_event->start_time - last_start_time;
            vtt_track->total_frames_duration += cur_frame->duration;
        }
        else
        {
            // if this is the very first event intersecting with segment, this is the first start in the segment
            vtt_track->first_frame_time_offset = cur_event->start_time;
            vtt_track->first_frame_index       = evntcounter;
        }

        if ((uint64_t)cur_event->start_time >= seg_end)
        {
            // events are already ordered by start-time
            break;
        }

        ///// This EVENT is within the segment duration. Parse its text, and output it after conversion to WebVTT valid tags./////

        // Split the event text into multiple chunks so we can insert each chunk as a separate frame in webVTT, all under a single cue
        char* event_textp = vod_alloc(request_context->pool, MAX_STR_SIZE_EVNT_CHUNK);
        if (event_textp == NULL)
        {
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "scc_parse_frames: vod_alloc failed");
            scc_clean_known_mem(request_context, scc_track, event_textp);
            return VOD_ALLOC_FAILED;
        }

        int  event_len = convert_event_text(cur_event,  event_textp, request_context);

#ifdef  SCC_TEMP_VERBOSITY
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "scc_parse_frames: event=%d len0=%d",
            evntcounter, event_len);
#endif

        // allocate the output frame
        cur_frame = vod_array_push(&frames);
        if (cur_frame == NULL)
        {
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "scc_parse_frames: vod_array_push failed");
            scc_clean_known_mem(request_context, scc_track, event_textp);
            return VOD_ALLOC_FAILED;
        }
        // allocate the text of output frame
        p = pfixed = vod_alloc(request_context->pool, MAX_STR_SIZE_EVNT_CHUNK);
        if (p == NULL)
        {
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
                "scc_parse_frames: vod_alloc failed");
            scc_clean_known_mem(request_context, scc_track, event_textp);
            return VOD_ALLOC_FAILED;
        }

        if (evntcounter == (scc_track->n_events - 1))
        {
            cur_frame->duration = cur_event->end_time - cur_event->start_time;
            vtt_track->total_frames_duration += cur_frame->duration;
        }

#ifdef  SCC_TEMP_VERBOSITY
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "UPDATEDURATION: evntCounter=%d, Start=%D, End=%D,duration=%D, total_frames_duration=%D, firstFrmIdx=%d, firstFrmOffset=%D",
            evntcounter, cur_event->start_time, cur_event->end_time, cur_frame->duration, vtt_track->total_frames_duration,
            vtt_track->first_frame_index, vtt_track->first_frame_time_offset);
#endif
        // Cues are named "c<iteration_number_in_7_digits>" starting from c0000000
        vod_sprintf((u_char*)p, FIXED_WEBVTT_CUE_FORMAT_STR, evntcounter);      p+=FIXED_WEBVTT_CUE_NAME_WIDTH;
        len = 2; vod_memcpy(p, "\r\n", len);                                    p+=len;
        // timestamps will be inserted here, we now insert positioning and alignment changes
        {
            unsigned char align;
            int kk, ll, pos, sizeH=0, line=14;
            int max_num_of_chars_per_line = 0,                    lineidx_max_num_of_chars=line;
            int min_num_of_chars_per_line = SCC_608_SCREEN_WIDTH, lineidx_min_num_of_chars=line;
            int slots_before_min_chars = 0, slots_after_min_chars = 0, slots_before_max_chars = 0, slots_after_max_chars = 0;
            for (kk=0; kk<15; kk++)
            {
                if (cur_event->row_used[kk] == 1)
                {
                    line = kk; // should never be 15
                    break;
                }
            }

            for (kk=line; kk<15; kk++)
            {
                if (cur_event->row_used[kk] == 1)
                {
                    int num_of_chars = 32;
                    for (ll=0; ll<SCC_608_SCREEN_WIDTH; ll++)
                    {
                        if (cur_event->characters[kk][ll] == SCC_UNUSED_CHAR)
                            num_of_chars--;
                    }
                    if (num_of_chars > max_num_of_chars_per_line)
                    {
                        max_num_of_chars_per_line = num_of_chars;
                        lineidx_max_num_of_chars = kk;
                    }
                    if (num_of_chars < min_num_of_chars_per_line)
                    {
                        min_num_of_chars_per_line = num_of_chars;
                        lineidx_min_num_of_chars = kk;
                    }
                }
            }
            for (ll=0; ll<(SCC_608_SCREEN_WIDTH-1); ll++)
            {
                if (cur_event->characters[lineidx_min_num_of_chars][ll] == SCC_UNUSED_CHAR)
                    slots_before_min_chars++;
                else
                    break;
            }
            for (ll=0; ll<(SCC_608_SCREEN_WIDTH-1); ll++)
            {
                if (cur_event->characters[lineidx_max_num_of_chars][ll] == SCC_UNUSED_CHAR)
                    slots_before_max_chars++;
                else
                    break;
            }
            sizeH = 3 * max_num_of_chars_per_line;
            slots_after_min_chars = SCC_608_SCREEN_WIDTH - min_num_of_chars_per_line - slots_before_min_chars;
            slots_after_max_chars = SCC_608_SCREEN_WIDTH - max_num_of_chars_per_line - slots_before_max_chars;
#ifdef  SCC_TEMP_VERBOSITY
            vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "event number %d, spaces_before=%d, max=%d, spaces_after=%d, lineidx=%d",
            evntcounter, slots_before_max_chars, max_num_of_chars_per_line, slots_after_max_chars, lineidx_max_num_of_chars);
#endif
            if ((slots_after_min_chars ==  slots_before_min_chars   ) ||
                (slots_after_min_chars == (slots_before_min_chars+1)) ||
                (slots_after_min_chars == (slots_before_min_chars-1)))
            {
                align = SCC_ALIGN_CENTER;
                pos = 50;
            }
            else if (slots_after_min_chars > slots_before_min_chars)
            {
                align = SCC_ALIGN_LEFT;
                pos = 2 + (3 * slots_before_max_chars);
            }
            else
            {
                align = SCC_ALIGN_RIGHT;
                pos = 98 - (3 * slots_after_max_chars);
            }

            len = 10; vod_memcpy(p, " position:", len);                     p+=len;
            vod_sprintf((u_char*)p, "%03uD", pos);                          p+=3;
            len =  7; vod_memcpy(p, "% size:", len);                        p+=len;
            vod_sprintf((u_char*)p, "%03uD", sizeH);                        p+=3;
            len =  7; vod_memcpy(p, "% line:", len);                        p+=len;
            vod_sprintf((u_char*)p, "%02uD", line);                         p+=2;


            len =  7; vod_memcpy(p, " align:", len);                            p+=len;
            if (align == SCC_ALIGN_CENTER) {
                len =  6; vod_memcpy(p, "center", len);                         p+=len;
            }
            else if (align == SCC_ALIGN_LEFT) {
                len =  4; vod_memcpy(p, "left", len);                           p+=len;
            }
            else {
                len =  5; vod_memcpy(p, "right", len);                          p+=len;
            }
            len = 2; vod_memcpy(p, "\r\n", len);                                p+=len;
        }
#ifdef ASSUME_STYLE_SUPPORT
        vod_memcpy(p, FIXED_WEBVTT_VOICE_START_STR, FIXED_WEBVTT_VOICE_START_WIDTH);       p+=FIXED_WEBVTT_VOICE_START_WIDTH;
        len = 28; vod_sprintf((u_char*)p, "RAFIK INSERT STYLE NAME HERE", len);            p+=len;
        vod_memcpy(p, FIXED_WEBVTT_VOICE_END_STR, FIXED_WEBVTT_VOICE_END_WIDTH);           p+=FIXED_WEBVTT_VOICE_END_WIDTH;
#endif //ASSUME_STYLE_SUPPORT

        vod_memcpy(p, event_textp, event_len);  p+=event_len;

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
        cur_frame->pts_delay = cur_event->end_time - cur_event->start_time;

        vtt_track->total_frames_size += cur_frame->size;

        last_start_time = cur_event->start_time;
    }

    //allocate memory for the style's text string
    p = pfixed = vod_alloc(request_context->pool, MAX_STR_SIZE_ALL_WEBVTT_STYLES);
    if (p == NULL)
    {
        vod_log_error(VOD_LOG_ERR, request_context->log, 0,
            "scc_parse_frames: vod_alloc failed");
        scc_free_track(request_context->pool, scc_track, request_context);
        return VOD_ALLOC_FAILED;
    }

    // We now insert header and all Style definitions
    header->data              = (u_char*)pfixed;
    len = sizeof(WEBVTT_HEADER_NEWLINES) - 1; vod_memcpy(p, WEBVTT_HEADER_NEWLINES, len);  p+=len;
#ifdef ASSUME_STYLE_SUPPORT
    int stylecounter;
    /*for (stylecounter = 0; (stylecounter < SCC_NUM_OF_STYLES_INSERTED); stylecounter++)
    {
        scc_style_t* cur_style = scc_track->styles + stylecounter;
        if (cur_style->b_output_in_cur_segment == TRUE)
            p = output_one_style(p);

    }*/
#endif //ASSUME_STYLE_SUPPORT
    header->len               = (size_t)(p - pfixed);

    // now we got all the info from scc_track, deallocate its memory
    scc_free_track(request_context->pool, scc_track, request_context);

    vtt_track->frame_count        = frames.nelts;
    vtt_track->frames.clip_to     = clip_to;
    vtt_track->frames.first_frame = frames.elts;
    vtt_track->frames.last_frame  = vtt_track->frames.first_frame + frames.nelts;

    return VOD_OK;
}

media_format_t scc_format = {
    FORMAT_ID_SCC,
    vod_string("scc CEA-608"),
    scc_reader_init,
    subtitle_reader_read,
    NULL,
    NULL,
    scc_parse,
    scc_parse_frames,
};
