#!/bin/bash

if [ -z "$NGX_ROOT" ]; then
	echo "NGX_ROOT not set"
	exit 1
fi

if [ -z "$VOD_ROOT" ]; then
	echo "VOD_ROOT not set"
	exit 1
fi

if [ -z "$CC" ]; then
	CC=cc
fi

$CC -Wall -g -ojsontest $VOD_ROOT/vod/json_parser.c $VOD_ROOT/vod/parse_utils.c $VOD_ROOT/test/json_parser/main.c $NGX_ROOT/src/core/ngx_string.c $NGX_ROOT/src/core/ngx_hash.c $NGX_ROOT/src/core/ngx_palloc.c $NGX_ROOT/src/os/unix/ngx_alloc.c -I $NGX_ROOT/src/core  -I $NGX_ROOT/src/event -I $NGX_ROOT/src/event/modules -I $NGX_ROOT/src/os/unix -I $NGX_ROOT/objs -I $VOD_ROOT
