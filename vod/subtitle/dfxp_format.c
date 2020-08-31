#include "../media_format.h"
#include "../media_clip.h"
#include "../media_set.h"
#include "subtitle_format.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#define DFXP_PREFIX "<tt"
#define DFXP_XML_PREFIX1 "<?xml"
#define DFXP_XML_PREFIX2 "<xml"

#define DFXP_DURATION_ESTIMATE_NODES (10)
#define DFXP_MAX_STACK_DEPTH (10)
#define DFXP_FRAME_RATE (30)

#define DFXP_ELEMENT_P (u_char*)"p"
#define DFXP_ELEMENT_BR (u_char*)"br"
#define DFXP_ELEMENT_SPAN (u_char*)"span"

#define DFXP_ATTR_BEGIN (u_char*)"begin"
#define DFXP_ATTR_END (u_char*)"end"
#define DFXP_ATTR_DUR (u_char*)"dur"

static vod_status_t
dfxp_reader_init(
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

	if ((vod_strncmp(p, (u_char*)DFXP_XML_PREFIX1, sizeof(DFXP_XML_PREFIX1) - 1) == 0 ||
		vod_strncmp(p, (u_char*)DFXP_XML_PREFIX2, sizeof(DFXP_XML_PREFIX2) - 1) == 0))
	{
		// vod_strstrn requires an extra -1
		if (vod_strstrn(p, DFXP_PREFIX, sizeof(DFXP_PREFIX) - 1 - 1) == NULL)
		{
			return VOD_NOT_FOUND;
		}
	}
	else if (vod_strncmp(p, (u_char*)DFXP_PREFIX, sizeof(DFXP_PREFIX) - 1) != 0)
	{
		return VOD_NOT_FOUND;
	}

	return subtitle_reader_init(
		request_context,
		ctx);
}

// a simplified version of xmlGetProp that does not copy memory, but is also limited to a single child
static xmlChar* 
dfxp_get_xml_prop(xmlNode* node, xmlChar* name)
{
	xmlAttrPtr prop;

	prop = xmlHasProp(node, name);
	if (prop == NULL ||
		prop->children == NULL ||
		prop->children->next != NULL ||
		(prop->children->type != XML_TEXT_NODE && prop->children->type != XML_CDATA_SECTION_NODE))
	{
		return NULL;
	}

	return prop->children->content;
}

static int64_t 
dfxp_parse_timestamp(u_char* ts)
{
	u_char* p = ts;
	int64_t frames;
	int64_t num = 0;
	int64_t den;
	int64_t mul;
	
	// Note: according to spec, hours must be at least 2 digits, but some samples have only one
	//		so this is not enforced
	if (!isdigit(*p))
	{
		return -1;
	}

	do
	{
		num = num * 10 + (*p++ - '0');
	} while (isdigit(*p));

	if (*p == ':')
	{
		// clock time
		p++;	// skip the :
		
		// minutes / seconds
		if (!isdigit(p[0]) || !isdigit(p[1]) ||
			p[2] != ':' ||
			!isdigit(p[3]) || !isdigit(p[4]))
		{
			return -1;
		}
		
		num = num * 3600 + 								// hours
			((p[0] - '0') * 10 + (p[1] - '0')) * 60 +	// min
			((p[3] - '0') * 10 + (p[4] - '0'));			// sec
		p += 5;
		
		switch (*p)
		{
		case '\0':
			return num * 1000;

		case '.':
			// fraction
			p++;	// skip the .
			if (!isdigit(*p))
			{
				return -1;
			}
			
			den = 1;
			do
			{
				num = num * 10 + (*p++ - '0');
				den *= 10;
			} while (isdigit(*p));
			
			if (*p != '\0')
			{
				return -1;
			}

			return (num * 1000) / den;
			
		case ':':
			// frames
			p++;	// skip the :
			if (!isdigit(*p))
			{
				return -1;
			}
			
			frames = 0;
			do
			{
				frames = frames * 10 + (*p++ - '0');
			} while (isdigit(*p));
			
			if (*p != '\0')
			{
				return -1;
			}

			return (num * 1000) + (frames * 1000) / DFXP_FRAME_RATE;
		}
	}
	else
	{
		// offset time
		den = 1;
		if (*p == '.')
		{
			// fraction
			p++;	// skip the .
			if (!isdigit(*p))
			{
				return -1;
			}
			do
			{
				num = num * 10 + (*p++ - '0');
				den *= 10;
			} while (isdigit(*p));
		}
		
		// metric
		switch (*p)
		{
		case 'h':
			mul = 3600000;
			break;
			
		case 'm':
			if (p[1] == 's')
			{
				mul = 1;
				p++;
			}
			else
			{
				mul = 60000;
			}
			break;
			
		case 's':
			mul = 1000;
			break;
			
		case 'f':
			mul = 1000;
			den *= DFXP_FRAME_RATE;
			break;
			
		default:
			return -1;
		}
		
		if (p[1] != '\0')
		{
			return -1;
		}
		
		return (num * mul) / den;
	}

	return -1;
}

static int64_t 
dfxp_get_end_time(xmlNode *cur_node)
{
	xmlChar* attr;
	int64_t begin;
	int64_t dur;
	
	// prefer the end attribute
	attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_END);
	if (attr != NULL)
	{
		return dfxp_parse_timestamp(attr);
	}
	
	// fall back to dur + start
	attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_DUR);
	if (attr == NULL)
	{
		return -1;
	}
	
	dur = dfxp_parse_timestamp(attr);
	if (dur < 0)
	{
		return -1;
	}
	
	attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_BEGIN);
	if (attr == NULL)
	{
		return -1;
	}
	
	begin = dfxp_parse_timestamp(attr);
	if (begin < 0)
	{
		return -1;
	}
	
	return begin + dur;
}

static uint64_t
dfxp_get_duration(xmlDoc *doc)
{
	xmlNode* node_stack[DFXP_MAX_STACK_DEPTH];
	xmlNode* cur_node;
	xmlNode temp_node;
	unsigned node_stack_pos = 0;
	int nodes_left = DFXP_DURATION_ESTIMATE_NODES;
	int64_t result = 0;
	int64_t cur;

	for (cur_node = xmlDocGetRootElement(doc); ; cur_node = cur_node->prev)
	{
		// traverse the tree dfs order (reverse child order)
		if (cur_node == NULL)
		{
			if (node_stack_pos <= 0)
			{
				break;
			}

			cur_node = node_stack[--node_stack_pos];
			continue;
		}

		if (cur_node->type != XML_ELEMENT_NODE)
		{
			continue;
		}

		// recurse into non-p nodes
		if (vod_strcmp(cur_node->name, DFXP_ELEMENT_P) != 0)
		{
			if (cur_node->last == NULL || 
				node_stack_pos >= vod_array_entries(node_stack))
			{
				continue;
			}

			node_stack[node_stack_pos++] = cur_node;
			temp_node.prev = cur_node->last;
			cur_node = &temp_node;
			continue;
		}

		// get the end time of this p node
		cur = dfxp_get_end_time(cur_node);
		if (cur > result)
		{
			result = cur;
		}

		nodes_left--;
		if (nodes_left <= 0)
		{
			break;
		}
	}
	
	return result;
}

static void
dfxp_strip_new_lines(u_char* buf, size_t n)
{
	u_char* end;
	u_char* p;

	end = buf + n;

	for (p = buf; p < end; p++)
	{
		if (*p == CR || *p == LF)
		{
			*p = ' ';
		}
	}
}

// copied from ngx_http_xslt_sax_error
static void vod_cdecl
dfxp_xml_sax_error(void *data, const char *msg, ...)
{
	xmlParserCtxtPtr ctxt = data;
	request_context_t* request_context;
	va_list args;
	u_char buf[VOD_MAX_ERROR_STR];
	size_t n;

	request_context = ctxt->sax->_private;

	buf[0] = '\0';

	va_start(args, msg);
	n = (size_t)vsnprintf((char *)buf, VOD_MAX_ERROR_STR, msg, args);
	va_end(args);

	while (--n && (buf[n] == CR || buf[n] == LF)) { /* void */ }

	dfxp_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"dfxp_xml_sax_error: libxml2 error: %*s", n + 1, buf);
}

static void vod_cdecl
dfxp_xml_schema_error(void *data, const char *msg, ...)
{
	xmlParserCtxtPtr ctxt = data;
	request_context_t* request_context;
	va_list args;
	u_char buf[VOD_MAX_ERROR_STR];
	size_t n;

	request_context = ctxt->sax->_private;

	buf[0] = '\0';

	va_start(args, msg);
	n = (size_t)vsnprintf((char *)buf, VOD_MAX_ERROR_STR, msg, args);
	va_end(args);

	while (--n && (buf[n] == CR || buf[n] == LF)) { /* void */ }

	dfxp_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_WARN, request_context->log, 0,
		"dfxp_xml_schema_error: libxml2 error: %*s", n + 1, buf);
}

static void
dfxp_free_xml_doc(void *data)
{
	xmlFreeDoc(data);
}

static vod_status_t
dfxp_parse(
	request_context_t* request_context,
	media_parse_params_t* parse_params,
	vod_str_t* source,
	size_t metadata_part_count,
	media_base_metadata_t** result)
{
	vod_pool_cleanup_t *cln;
	xmlParserCtxtPtr ctxt;
	xmlDoc *doc;

	// parse the xml
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	ctxt = xmlCreateDocParserCtxt(source->data);
	if (ctxt == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dfxp_parse: xmlCreateDocParserCtxt failed");
		return VOD_ALLOC_FAILED;
	}

	xmlCtxtUseOptions(ctxt, XML_PARSE_RECOVER | XML_PARSE_NOWARNING | XML_PARSE_NONET);
	
	ctxt->sax->setDocumentLocator = NULL;
	ctxt->sax->error = dfxp_xml_sax_error;
	ctxt->sax->fatalError = dfxp_xml_sax_error;
	ctxt->vctxt.error = dfxp_xml_schema_error;
	ctxt->sax->_private = request_context;

	if (xmlParseDocument(ctxt) != 0 ||
		ctxt->myDoc == NULL ||
		(!ctxt->wellFormed && !ctxt->recovery))
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse: xml parsing failed");
		if (ctxt->myDoc != NULL)
		{
			xmlFreeDoc(ctxt->myDoc);
		}
		xmlFreeParserCtxt(ctxt);
		return VOD_BAD_DATA;
	}

	doc = ctxt->myDoc;
	ctxt->myDoc = NULL;

	xmlFreeParserCtxt(ctxt);

	cln->handler = dfxp_free_xml_doc;
	cln->data = doc;

	return subtitle_parse(
		request_context,
		parse_params,
		source,
		doc,
		dfxp_get_duration(doc),
		metadata_part_count,
		result);
}

static u_char* 
dfxp_append_string(u_char* p, u_char* s)
{
	// Note: not using strcpy as it would require an extra strlen to get the end position
	while (*s)
	{
		*p++ = *s++;
	}
	return p;
}

static size_t
dfxp_get_text_content_len(xmlNode* cur_node)
{
	xmlNode* node_stack[DFXP_MAX_STACK_DEPTH];
	unsigned node_stack_pos = 0;
	size_t result = 0;

	for (;;)
	{
		// traverse the tree dfs order
		if (cur_node == NULL)
		{
			if (node_stack_pos <= 0)
			{
				break;
			}

			cur_node = node_stack[--node_stack_pos];
			continue;
		}

		switch (cur_node->type)
		{
		case XML_TEXT_NODE:
		case XML_CDATA_SECTION_NODE:
			result += vod_strlen(cur_node->content);
			break;

		case XML_ELEMENT_NODE:
			if (vod_strcmp(cur_node->name, DFXP_ELEMENT_BR) == 0)
			{
				result++;		// \n
				break;
			}

			if (vod_strcmp(cur_node->name, DFXP_ELEMENT_SPAN) != 0 ||
				cur_node->children == NULL ||
				node_stack_pos >= vod_array_entries(node_stack))
			{
				break;
			}

			node_stack[node_stack_pos++] = cur_node->next;
			cur_node = cur_node->children;
			continue;

		default:
			break;
		}

		cur_node = cur_node->next;
	}

	return result;
}

static u_char* 
dfxp_append_text_content(xmlNode* cur_node, u_char* p)
{
	xmlNode* node_stack[DFXP_MAX_STACK_DEPTH];
	unsigned node_stack_pos = 0;

	for (;;)
	{
		// traverse the tree dfs order
		if (cur_node == NULL)
		{
			if (node_stack_pos <= 0)
			{
				break;
			}

			cur_node = node_stack[--node_stack_pos];
			continue;
		}

		switch (cur_node->type)
		{
		case XML_TEXT_NODE:
		case XML_CDATA_SECTION_NODE:
			p = dfxp_append_string(p, cur_node->content);
			break;

		case XML_ELEMENT_NODE:
			if (vod_strcmp(cur_node->name, DFXP_ELEMENT_BR) == 0)
			{
				*p++ = '\n';
				break;
			}

			if (vod_strcmp(cur_node->name, DFXP_ELEMENT_SPAN) != 0 ||
				cur_node->children == NULL ||
				node_stack_pos >= vod_array_entries(node_stack))
			{
				break;
			}

			node_stack[node_stack_pos++] = cur_node->next;
			cur_node = cur_node->children;
			continue;

		default:
			break;
		}

		cur_node = cur_node->next;
	}

	return p;
}

static vod_status_t
dfxp_get_frame_body(
	request_context_t* request_context, 
	xmlNode* cur_node, 
	vod_str_t* result)
{
	size_t alloc_size;
	u_char* start;
	u_char* end;
	
	// get the buffer length
	alloc_size = dfxp_get_text_content_len(cur_node);
	if (alloc_size == 0)
	{
		return VOD_NOT_FOUND;
	}

	alloc_size += 3;		// \n * 3

	// get the text content
	start = vod_alloc(request_context->pool, alloc_size);
	if (start == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_get_frame_body: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	start++;	// save space for prepending \n

	end = dfxp_append_text_content(cur_node, start);
	if ((size_t)(end - start + 2) > alloc_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"dfxp_get_frame_body: result length %uz exceeded allocated length %uz",
			(size_t)(end - start + 2), alloc_size);
		return VOD_UNEXPECTED;
	}

	// trim spaces
	for (;;)
	{
		if (start >= end)
		{
			return VOD_NOT_FOUND;
		}

		if (!isspace(start[0]))
		{
			break;
		}

		start++;
	}

	for (;;)
	{
		if (start >= end)
		{
			return VOD_NOT_FOUND;
		}

		if (!isspace(end[-1]))
		{
			break;
		}

		end--;
	}

	// add leading/trailing newlines
	start--;
	*start = '\n';
	*end++ = '\n';
	*end++ = '\n';

	result->data = start;
	result->len = end - start;

	return VOD_OK;
}

static vod_status_t
dfxp_parse_frames(
	request_context_t* request_context,
	media_base_metadata_t* base,
	media_parse_params_t* parse_params,
	struct segmenter_conf_s* segmenter,
	read_cache_state_t* read_cache_state,
	vod_str_t* frame_data,
	media_format_read_request_t* read_req,
	media_track_array_t* result)
{
	subtitle_base_metadata_t* metadata = vod_container_of(base, subtitle_base_metadata_t, base);
	media_track_t* track = base->tracks.elts;
	input_frame_t* cur_frame = NULL;
	vod_array_t frames;
	vod_str_t* header = &track->media_info.extra_data;
	uint64_t base_time;
	uint64_t clip_to;
	uint64_t start;
	uint64_t end;
	int64_t last_start_time = 0;
	int64_t start_time = 0;
	int64_t end_time = 0;
	int64_t duration;
	xmlNode* node_stack[DFXP_MAX_STACK_DEPTH];
	xmlNode* cur_node;
	xmlNode temp_node;
	xmlChar* attr;
	unsigned node_stack_pos = 0;
	vod_str_t text;
	vod_status_t rc;

	// initialize the result
	vod_memzero(result, sizeof(*result));
	result->first_track = track;
	result->last_track = track + 1;
	result->track_count[MEDIA_TYPE_SUBTITLE] = 1;
	result->total_track_count = 1;

	header->len = sizeof(WEBVTT_HEADER_NEWLINES) - 1;
	header->data = (u_char*)WEBVTT_HEADER_NEWLINES;
	
	if ((parse_params->parse_type & PARSE_FLAG_FRAMES_ALL) == 0)
	{
		return VOD_OK;
	}

	// init the frames array
	if (vod_array_init(&frames, request_context->pool, 5, sizeof(*cur_frame)) != VOD_OK)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"dfxp_parse_frames: vod_array_init failed");
		return VOD_ALLOC_FAILED;
	}

	// get the start / end offsets
	start = parse_params->range->start + parse_params->clip_from;

	if ((parse_params->parse_type & PARSE_FLAG_RELATIVE_TIMESTAMPS) != 0)
	{
		base_time = start;
		clip_to = parse_params->range->end - parse_params->range->start;
		end = clip_to;
	}
	else
	{
		base_time = parse_params->clip_from;
		clip_to = parse_params->clip_to;
		end = parse_params->range->end;		// Note: not adding clip_from, since end is checked after the clipping is applied to the timestamps
	}

	for (cur_node = xmlDocGetRootElement(metadata->context); ; cur_node = cur_node->next)
	{
		// traverse the tree dfs order
		if (cur_node == NULL)
		{
			if (node_stack_pos <= 0)
			{
				if (cur_frame != NULL)
				{
					cur_frame->duration = end_time - start_time;
					track->total_frames_duration = end_time - track->first_frame_time_offset;
				}
				break;
			}

			cur_node = node_stack[--node_stack_pos];
			continue;
		}

		if (cur_node->type != XML_ELEMENT_NODE)
		{
			continue;
		}

		if (vod_strcmp(cur_node->name, DFXP_ELEMENT_P) != 0)
		{
			if (cur_node->children == NULL ||
				node_stack_pos >= vod_array_entries(node_stack))
			{
				continue;
			}

			node_stack[node_stack_pos++] = cur_node;
			temp_node.next = cur_node->children;
			cur_node = &temp_node;
			continue;
		}

		// handle p element
		attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_END);
		if (attr != NULL)
		{
			// has end time
			end_time = dfxp_parse_timestamp(attr);
			if (end_time < 0)
			{
				continue;
			}

			if ((uint64_t)end_time < start)
			{
				track->first_frame_index++;
				continue;
			}

			attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_BEGIN);
			if (attr == NULL)
			{
				continue;
			}

			start_time = dfxp_parse_timestamp(attr);
			if (start_time < 0)
			{
				continue;
			}
		}
		else
		{
			// no end time
			attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_DUR);
			if (attr == NULL)
			{
				continue;
			}

			duration = dfxp_parse_timestamp(attr);
			if (duration < 0)
			{
				continue;
			}

			attr = dfxp_get_xml_prop(cur_node, DFXP_ATTR_BEGIN);
			if (attr == NULL)
			{
				continue;
			}

			start_time = dfxp_parse_timestamp(attr);
			if (start_time < 0)
			{
				continue;
			}

			end_time = start_time + duration;
			if ((uint64_t)end_time < start)
			{
				track->first_frame_index++;
				continue;
			}
		}

		if (start_time >= end_time)
		{
			continue;
		}

		// apply clipping
		if (start_time >= (int64_t)base_time)
		{
			start_time -= base_time;
			if ((uint64_t)start_time > clip_to)
			{
				start_time = clip_to;
			}
		}
		else
		{
			start_time = 0;
		}

		end_time -= base_time;
		if ((uint64_t)end_time > clip_to)
		{
			end_time = clip_to;
		}

		// get the text
		rc = dfxp_get_frame_body(
			request_context,
			cur_node->children,
			&text);
		switch (rc)
		{
		case VOD_NOT_FOUND:
			continue;

		case VOD_OK:
			break;

		default:
			return rc;
		}

		// adjust the duration of the previous frame
		if (cur_frame != NULL)
		{
			cur_frame->duration = start_time - last_start_time;
		}
		else
		{
			track->first_frame_time_offset = start_time;
		}

		if ((uint64_t)start_time >= end)
		{
			track->total_frames_duration = start_time - track->first_frame_time_offset;
			break;
		}

		// add the frame
		cur_frame = vod_array_push(&frames);
		if (cur_frame == NULL)
		{
			vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
				"dfxp_parse_frames: vod_array_push failed");
			return VOD_ALLOC_FAILED;
		}

		cur_frame->offset = (uintptr_t)text.data;
		cur_frame->size = text.len;
		cur_frame->pts_delay = end_time - start_time;
		cur_frame->key_frame = 0;
		track->total_frames_size += cur_frame->size;

		last_start_time = start_time;
	}

	track->frame_count = frames.nelts;
	track->frames.first_frame = frames.elts;
	track->frames.last_frame = track->frames.first_frame + frames.nelts;

	return VOD_OK;
}

void
dfxp_init_process()
{
	xmlInitParser();
}

void
dfxp_exit_process()
{
	xmlCleanupParser();
}

media_format_t dfxp_format = {
	FORMAT_ID_DFXP,
	vod_string("dfxp"),
	dfxp_reader_init,
	subtitle_reader_read,
	NULL,
	NULL,
	dfxp_parse,
	dfxp_parse_frames,
};
