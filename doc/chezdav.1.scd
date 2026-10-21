chezdav(1)

# NAME

chezdav - simple WebDAV server

# SYNOPSIS

*chezdav* [_OPTIONS_...]

# DESCRIPTION

The *chezdav* command starts a WebDAV server. This allows clients to access
your files, and modify them. A simple Web browser is sufficient to browse for
files. The default shared folder is the home directory, but you may specify a
different folder with the *-P* option.

# OPTIONS

*-P*, *--path*=_PATH_
	Path to export.

*-p*, *--port*=_PORT_
	Port to listen from. The default is 8080.

*--local*
	Listen on loopback interface only.

*--public*
	Listen on all network interfaces.

*--no-mdns*
	Don't broadcast the share on the local network. By default, shares are
	broadcast with mDNS/DNS-SD (when compiled with Avahi).

*-d*, *--htdigest*=_PATH_
	Path to a htdigest file, to secure the server with DIGEST authentication.

*--realm*=_REALM_
	The DIGEST realm string (must be identical to the string used in the
	htdigest file). The default value is "USER's public share" (where USER is
	the owner's username of the current process).

*-r*, *--readonly*
	Read-only access.

*--max-upload*=_SIZE_
	Refuse PUT uploads larger than SIZE bytes, answering them with 413 Request
	Entity Too Large and discarding the partial file. By default, the upload
	size is not limited.

*-v*, *--verbose*
	Verbosely print running information.

*--version*
	Print program version number.

# EXIT STATUS

*0*
	Success

*1*
	Failure (syntax or usage error; unexpected error).

# SEE ALSO

*htdigest*(1)

# AUTHORS

chezdav was originally written by Marc-André Lureau.

# RESOURCES

Main web site: https://gitlab.gnome.org/GNOME/phodav

# COPYING

Copyright (C) 2013-2014 Red Hat, Inc. Free use of this software is granted
under the terms of the GNU Lesser General Public License (LGPL).
