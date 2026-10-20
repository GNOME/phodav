#!/bin/sh
# Copyright (C) 2026 Red Hat, Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, see <http://www.gnu.org/licenses/>.

# Run the litmus WebDAV compliance suite (RFC 4918) against the
# in-tree test server.
#
# The server is started on a random port, litmus is run against it
# and the server is stopped again. The script exits with the litmus
# status; note that litmus --keep-going exits successfully even when
# tests fail, so the summaries are checked as well.
#
# Usage: litmus.sh <server> <litmus>
set -eu

server=$1
litmus=$2

# make the paths absolute before changing directory
case $server in
  /*) ;;
  *) server=$(pwd)/$server ;;
esac
case $litmus in
  /*) ;;
  *) litmus=$(pwd)/$litmus ;;
esac

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

# the server creates its temporary root in the current directory,
# isolate it from other tests which may run in parallel
cd "$workdir"

# keep a fifo open on the server's stdin: closing it makes the
# server quit (see its --quit-on-stdin option)
fifo=$workdir/server-stdin
mkfifo "$fifo"
"$server" --quit-on-stdin < "$fifo" > "$workdir/server-stdout" 2> "$workdir/server-stderr" &
server_pid=$!
exec 3> "$fifo"

# the server prints "OK <port>" once it listens
port=
i=0
while [ "$i" -lt 100 ]; do
  port=$(sed -n 's/^OK //p' "$workdir/server-stdout" 2>/dev/null || true)
  [ -n "$port" ] && break
  i=$((i + 1))
  sleep 0.1
done

if [ -z "$port" ]; then
  echo "the test server did not start:" >&2
  cat "$workdir/server-stderr" >&2
  exec 3>&-
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  exit 1
fi

status=0
"$litmus" --keep-going "dav://127.0.0.1:$port/" > "$workdir/litmus.log" 2>&1 || status=$?

# litmus --keep-going returns successfully even when tests fail,
# check the suite summaries instead
if grep -q 'passed, [1-9][0-9]* failed' "$workdir/litmus.log"; then
  status=1
fi

cat "$workdir/litmus.log"

# closing the fifo makes the server quit
exec 3>&-
# a non-zero server exit means a crash or, with sanitizers enabled, a
# leaked allocation; either way the test must not pass
if wait "$server_pid" 2>/dev/null; then
  :
else
  echo "the test server exited with an error:" >&2
  cat "$workdir/server-stderr" >&2
  status=1
fi

exit $status
