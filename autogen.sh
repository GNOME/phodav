#!/bin/sh

set -e # exit on errors

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

mkdir -p $srcdir/m4

. gnome-autogen.sh --enable-gtk-doc "$@"
