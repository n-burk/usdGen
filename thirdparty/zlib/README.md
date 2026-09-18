# zlib vendoring

This directory vendors zlib from the `v1.3.1` tag (annotated tag object
`925af44f3cde53c6b076611c297850091b5dc7bb`, commit
`51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf`) of
`https://github.com/madler/zlib`.

It exists only because the vendored Ptex 2.4.3 (`thirdparty/ptex`) compresses
its face data with zlib, and the OpenUSD prefix this project builds against
does not ship zlib (see the `find_package(Threads)` note in the top-level
`CMakeLists.txt`).

Contents: the C sources and headers upstream's static library compiles
(`adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast
inflate inftrees trees uncompr zutil`), with `zconf.h` exactly as shipped. The
upstream license is preserved in `LICENSE` (zlib license). No source is
modified; the build system, contrib, examples, tests and platform ports are
not vendored.

CMake builds them as the private static target `usdGen_zlib` (hidden
visibility, warnings off, `_CRT_SECURE_NO_DEPRECATE` /
`_CRT_NONSTDC_NO_DEPRECATE` on MSVC, `HAVE_UNISTD_H` elsewhere, which is what
upstream's configure step would have written into `zconf.h`). Its include
directory is visible to `usdGen_ptex` only; zlib is never installed or
exported.
