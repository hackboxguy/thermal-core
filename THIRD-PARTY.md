# Third-Party Notices

This repository is MIT licensed under `LICENSE`. The portable
`core/`, `protocol/`, and `support/` layers are repository code; the
items below are vendored or pinned helper components used by platform
or tooling paths.

## jsmn

- Path: `platform/linux/jsmn.{c,h}`
- Use: small JSON tokenizer for the Linux/QNX-style config loader
- License: MIT
- Copyright: 2010 Serge Zaitsev

The full notice is preserved at the top of both vendored source files.

## ch32fun

- Path: `platform/ch32v003/ch32fun`
- Use: CH32V003 reference BSP/build support
- License: MIT, with additional bundled notices under that submodule
- Pin: recorded by the git submodule commit

The submodule carries its own `LICENSE` and `misc/LIBGCC_LICENSE`
files. Distributions that include the CH32V003 firmware support should
include those notices as well.

## car-can-emulator

- Path: `tools/car-can-emulator`
- Use: optional developer-side CAN traffic emulator
- Pin: recorded by the git submodule commit when initialized

This optional submodule is not part of the portable runtime. Consult the
submodule's own license before redistributing it.
