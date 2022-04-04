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

$CC -Wall -g -obitsettest -DNGX_HAVE_LIB_AV_CODEC=0 $VOD_ROOT/vod/common.c $VOD_ROOT/test/bitset/main.c -I $NGX_ROOT/src/core -I $NGX_ROOT/src/event -I $NGX_ROOT/src/event/modules -I $NGX_ROOT/src/os/unix -I $NGX_ROOT/objs -I $VOD_ROOT
