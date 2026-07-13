# OrbisShelf

OrbisShelf is a minimal PlayStation 4 homebrew catalog that reads a JSON list from GitHub, downloads an authorized PKG from Hugging Face, verifies it, and queues installation through PS4 BGFT/AppInstUtil.

> Use OrbisShelf only for software you created, open-source homebrew, or PKGs you have explicit permission to redistribute. It is not intended for copyrighted commercial game distribution.

## Current flow

1. Load the bundled/cached catalog.
2. Refresh `catalog/catalog.json` from this repository.
3. Select an enabled entry with D-pad and press Cross.
4. Follow HTTPS redirects and download the PKG to `/data/OrbisShelf/downloads`.
5. Verify `size_bytes` and optional SHA-256.
6. Register the local PKG with BGFT and monitor installation.
7. Delete the downloaded PKG after a successful install.

Triangle refreshes the catalog. Circle exits while no job is running.

## Build

Install the OpenOrbis PS4 Toolchain and export `OO_PS4_TOOLCHAIN`, then run:

```sh
make
```

The Makefile validates the catalog, builds the ELF/FSELF, generates `sce_sys` metadata and icon assets, and produces `IV0000-ORBS00001_00-ORBISSHELF000001.pkg`.

The build currently reuses `right.sprx`, `libSceFios2.prx`, and `libc.prx` from the OpenOrbis SDL2 sample directory. The source links SDL2, SceHttp/Ssl/Net, AppInstUtil, BGFT, UserService, and Sysmodule stubs supplied by OpenOrbis.

## Catalog

See [`docs/catalog.md`](docs/catalog.md). The empty initial catalog is intentional. Add only authorized packages and provide SHA-256/size metadata whenever possible.

## Token policy

Public Hugging Face repositories need no token. Secrets are never committed or embedded. A private repository can be accessed with a console-local read-only token at `/data/OrbisShelf/hf_token.txt`, but a short-lived URL proxy is recommended for distributed builds.

## Status

This is the first bootstrap implementation. PS4 firmware/jailbreak environments differ, so BGFT behavior and error codes must be tested on the target console before publishing a release.
