#!/bin/bash

if [ -z "$NGX_ROOT" ]; then 
	echo "NGX_ROOT not set"
	exit 1
fi

if [ -z "$VOD_ROOT" ]; then 
	echo "VOD_ROOT not set"
	exit 1
fi

cc -Wall -ojsontest $VOD_ROOT/ngx_simple_json_parser.c $VOD_ROOT/test/json_parser/main.c -I $NGX_ROOT/src/core  -I $NGX_ROOT/src/event -I $NGX_ROOT/src/event/modules -I $NGX_ROOT/src/os/unix -I $NGX_ROOT/objs -I $VOD_ROOT
