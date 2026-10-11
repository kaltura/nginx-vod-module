#ifndef __XML_H__
#define __XML_H__

#include "common.h"

#include <libxml/parser.h>
#include <libxml/tree.h>


#define XML_NODE_HANDLER(name, handler) { name, handler, NULL }
#define XML_NODE_CONTAINER(name, children) { name, NULL, children }
#define XML_NODE_LAST { NULL, NULL, NULL }


typedef struct xml_node_handler_s {
	char* name;
	vod_status_t (*handler)(void* ctx, xmlNode* node);
	struct xml_node_handler_s* children;
} xml_node_handler_t;


xmlChar* xml_get_prop(xmlNode* node, char* name);

void xml_get_prop_str(xmlNode* node, char* name, vod_str_t* dest);

vod_int_t xml_get_prop_int(request_context_t* request_context, xmlNode* node, char* name);

vod_int_t xml_parse_duration(vod_str_t* str);

void xml_get_text_element_content_str(xmlNode* node, vod_str_t* dest);

vod_status_t xml_get_text_element_content_base64(request_context_t* request_context, xmlNode* node, vod_str_t* dest);

vod_status_t xml_parse_nodes(void* ctx, xmlNode* node, xml_node_handler_t* handlers);

vod_status_t xml_parse(request_context_t* request_context, u_char* source, xmlDoc** res);

#endif // __XML_H__
