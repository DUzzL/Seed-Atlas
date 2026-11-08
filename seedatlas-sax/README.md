# seedatlas-sax: the engine's C ABI for the mods

`seedatlas_sax.h` is a small, stable C interface to the Seed Atlas engine
(`sax_*`, ABI 6): biomes, terrain heights, the Nether floor and structure,
stronghold, spawn, slime chunk and ore vein markers of a seed. It is what the
mods built on Seed Atlas use, so that they compute exactly what the app does:

- the Fabric mod ([seed-atlas-xaero](https://github.com/DUzzL/seed-atlas-xaero))
  builds it as a shared library and calls it through Java's FFM;
- the Horizon mod (Meridian) compiles it into its Rust crate and calls it
  through Rust's FFI.

Both build the engine sources of `../seedatlas-engine` together with
`seedatlas_sax.c`, with the engine's flags (`-fwrapv -ffp-contract=off`): the
engine relies on wrapping signed arithmetic, and fused multiply-adds would make
the results depend on the platform. The engine needs C11 variable-length
arrays, so on Windows it builds with MinGW GCC or clang, not MSVC's C compiler.

```sh
make -C seedatlas-sax test   # builds libseedatlas-sax.a and runs the tests
```

Define `SAX_STATIC` when linking the library statically, `SAX_BUILD` when
building it as a shared library.

The interface moved here from the Fabric mod (`src/main/native/seedatlas_xaero.c`,
release 2.0.0), unchanged apart from the names of these files.
