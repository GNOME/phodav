# phởdav

## About

phởdav is a WebDAV server implementation using libsoup (RFC 4918) and
GIO. It provides:

* libphodav, a GObject WebDAV server library
* chezdav, a small tool to share a directory (with optional digest
  authentication and TLS)
* spice-webdavd, the [Spice](https://www.spice-space.org/) WebDAV daemon,
  which shares a folder between a host and its guests

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

A release is made by pushing a signed tag to the repository:

    git evtag sign v3.1   # or: git tag -s v3.1
    git push origin v3.1

The tag must match a protected tag pattern (such as `v*`) in the project
settings, and the version in *NEWS* must already be updated. The tarball
version is derived from the tag with `git describe`.

The tag triggers a CI pipeline which builds and tests the release,
generates the tarball and uploads it to
[download.gnome.org](https://download.gnome.org/sources/phodav/) and
to the
[releases section](https://gitlab.gnome.org/GNOME/phodav/-/releases),
as described in the
[maintainer handbook](https://handbook.gnome.org/maintainers/release-pipeline.html).
The tag message becomes the release notes, and the *NEWS* file of the
tarball is published alongside the tarball. The Windows installers built
by the CI are attached to the release as additional assets.

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
