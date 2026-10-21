# phởdav

## About

phởdav is a WebDAV server implementation using libsoup (RFC 4918) and
GIO. It provides:

* libphodav, a GObject WebDAV server library
* chezdav, a small tool to share a directory (with optional digest
  authentication and TLS)
* spice-webdavd, the Spice *webdav* daemon, which shares a folder
  between a host and its guests

## Building

phởdav uses the [meson](https://mesonbuild.com/) build system:

    meson setup build
    meson compile -C build
    meson test -C build
    meson install -C build

The build is configurable through meson options (Avahi/mDNS support,
GObject introspection, API documentation...), see *meson_options.txt*.

## Documentation

The API reference is generated with
[gi-docgen](https://gitlab.gnome.org/GNOME/gi-docgen):

    meson compile -C build phodav-3.0-doc

The *chezdav*(1) manual page is installed in the usual locations.

## Releases

Releases are uploaded to [GNOME's FTP](https://download.gnome.org/sources/phodav/)
and published in the [tags section](https://gitlab.gnome.org/GNOME/phodav/-/tags).

## Notes

phởdav was initially developed as a filesharing mechanism for Spice,
but it is generic enough to be reused in other projects, in particular
in the GNOME desktop. Further integration work would be a welcome
contribution!

## Contributing

    git clone https://gitlab.gnome.org/GNOME/phodav.git

The sources are formatted with clang-format and linted with
[gobject-linter](https://github.com/bilelmoussaoui/gobject-linter);
both are run by [pre-commit](https://pre-commit.com) hooks:

    cargo install gobject-linter   # Rust tool, not managed by pre-commit
    pre-commit install

Note that Jujutsu does not run Git hooks: `jj` users should run
`pre-commit run --all-files` before pushing instead.

Please report bugs and open merge requests on GitLab:
https://gitlab.gnome.org/GNOME/phodav/

Opening merge requests is recommended, as it runs the CI tests.
