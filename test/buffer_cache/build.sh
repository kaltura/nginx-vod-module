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

$CC -Wall $NGX_ROOT/src/core/ngx_palloc.c $NGX_ROOT/src/os/unix/ngx_alloc.c $NGX_ROOT/src/core/ngx_string.c $NGX_ROOT/src/core/ngx_crc32.c $NGX_ROOT/src/core/ngx_rbtree.c $VOD_ROOT/ngx_buffer_cache.c $VOD_ROOT/test/buffer_cache/main.c -o bctest -I $VOD_ROOT/test/buffer_cache -I $NGX_ROOT/src/core -I $NGX_ROOT/src/event -I $NGX_ROOT/src/event/modules -I $NGX_ROOT/src/os/unix -I $NGX_ROOT/objs -I $VOD_ROOT -g
