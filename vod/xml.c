#include "xml.h"

// a simplified version of xmlGetProp that does not copy memory, but is also limited to a single child
xmlChar*
xml_get_prop(xmlNode* node, char* name)
{
	xmlAttrPtr prop;

	prop = xmlHasProp(node, (xmlChar*) name);
	if (prop == NULL ||
		prop->children == NULL ||
		prop->children->next != NULL ||
		(prop->children->type != XML_TEXT_NODE && prop->children->type != XML_CDATA_SECTION_NODE))
	{
		return NULL;
	}

	return prop->children->content;
}

void
xml_get_prop_str(xmlNode* node, char* name, vod_str_t* dest)
{
	xmlChar* attr;

	attr = xml_get_prop(node, name);
	if (attr == NULL)
	{
		dest->len = 0;
		return;
	}

	dest->data = attr;
	dest->len = vod_strlen(attr);
}

vod_int_t
xml_get_prop_int(request_context_t* request_context, xmlNode* node, char* name)
{
	vod_str_t attr;
	vod_int_t val;

	xml_get_prop_str(node, name, &attr);
	if (attr.len == 0)
	{
		return -1;
	}

	val = vod_atoi(attr.data, attr.len);
	if (val < 0)
	{
		vod_log_error(VOD_LOG_WARN, request_context->log, 0,
			"xml_get_prop_int: invalid %s \"%V\" attribute", name, &attr);
		return -1;
	}

	return val;
}

vod_int_t
xml_parse_duration(vod_str_t* str)
{
	vod_int_t val;
	vod_int_t res;
	u_char* end;
	u_char* p;

	// limitations:
	// 1. expecting only time here - no years/months/days (PT[n]H[n]M[n]S)
	// 2. currently no support for fractional seconds

	if (str->len < 4 || str->data[0] != 'P' || str->data[1] != 'T')
	{
		return NGX_ERROR;
	}

	res = 0;
	end = str->data + str->len;
	for (p = str->data + 2; p < end; p++)
	{
		if (*p < '0' || *p > '9')
		{
			return NGX_ERROR;
		}

		val = *p - '0';
		for (p++; p < end && *p >= '0' && *p <= '9'; p++)
		{
			val = val * 10 +  (*p - '0');
		}

		if (p >= end)
		{
			return NGX_ERROR;
		}

		switch (*p)
		{
		case 'H':
			res += val * 3600;
			break;
		case 'M':
			res += val * 60;
			break;
		case 'S':
			res += val;
			break;
		default:
			return NGX_ERROR;
		}
	}

	return res;
}

static xmlChar*
xml_get_text_element_content(xmlNode* node)
{
	if (node->children == NULL ||
		node->children->next != NULL ||
		(node->children->type != XML_TEXT_NODE && node->children->type != XML_CDATA_SECTION_NODE))
	{
		return NULL;
	}

	return node->children->content;
}

void
xml_get_text_element_content_str(xmlNode* node, vod_str_t* dest)
{
	xmlChar* content;

	content = xml_get_text_element_content(node);
	if (content == NULL)
	{
		dest->len = 0;
		return;
	}

	dest->data = content;
	dest->len = vod_strlen(content);
}

vod_status_t
xml_get_text_element_content_base64(request_context_t* request_context, xmlNode* node, vod_str_t* dest)
{
	vod_str_t base64;

	xml_get_text_element_content_str(node, &base64);
	if (base64.len == 0)
	{
		dest->len = 0;
		return VOD_OK;
	}

	dest->data = vod_alloc(request_context->pool, vod_base64_decoded_length(base64.len));
	if (dest->data == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	if (vod_decode_base64(dest, &base64) != VOD_OK)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"xml_get_text_element_content_base64: invalid base64 string");
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

vod_status_t
xml_parse_nodes(void* ctx, xmlNode* node, xml_node_handler_t* handlers)
{
	xml_node_handler_t* handler;
	vod_status_t rc;
	xmlNode* cur;

	for (cur = node; cur; cur = cur->next)
	{
		if (cur->type != XML_ELEMENT_NODE)
		{
			continue;
		}

		for (handler = handlers; handler->name; handler++)
		{
			if (vod_strcmp(cur->name, handler->name) != 0)
			{
				continue;
			}

			if (handler->handler)
			{
				rc = handler->handler(ctx, cur);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			if (handler->children)
			{
				rc = xml_parse_nodes(ctx, cur->children, handler->children);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}
		}
	}

	return VOD_OK;
}

static void
xml_strip_new_lines(u_char* buf, size_t n)
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
xml_sax_error(void* data, const char* msg, ...)
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

	xml_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_ERR, request_context->log, 0,
		"xml_sax_error: libxml2 error: %*s", n + 1, buf);
}

static void vod_cdecl
xml_schema_error(void* data, const char* msg, ...)
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

	xml_strip_new_lines(buf, n);

	vod_log_error(VOD_LOG_WARN, request_context->log, 0,
		"xml_schema_error: libxml2 error: %*s", n + 1, buf);
}

static void
xml_free_doc(void* data)
{
	xmlFreeDoc(data);
}

vod_status_t
xml_parse(request_context_t* request_context, u_char* source, xmlDoc** res)
{
	vod_pool_cleanup_t* cln;
	xmlParserCtxtPtr ctxt;
	xmlDoc* doc;

	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"xml_parse: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	ctxt = xmlCreateDocParserCtxt(source);
	if (ctxt == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"xml_parse: xmlCreateDocParserCtxt failed");
		return VOD_ALLOC_FAILED;
	}

	xmlCtxtUseOptions(ctxt, XML_PARSE_RECOVER | XML_PARSE_NOWARNING | XML_PARSE_NONET);

	ctxt->sax->setDocumentLocator = NULL;
	ctxt->sax->error = xml_sax_error;
	ctxt->sax->fatalError = xml_sax_error;
	ctxt->vctxt.error = xml_schema_error;
	ctxt->sax->_private = request_context;

	if (xmlParseDocument(ctxt) != 0 ||
		ctxt->myDoc == NULL ||
		(!ctxt->wellFormed && !ctxt->recovery))
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"xml_parse: parsing failed");
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

	cln->handler = xml_free_doc;
	cln->data = doc;

	*res = doc;

	return VOD_OK;
}