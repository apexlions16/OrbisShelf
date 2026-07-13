# OrbisShelf

OrbisShelf is a minimal PlayStation 4 homebrew catalog that reads a JSON list from GitHub, downloads an authorized PKG from Hugging Face, verifies it, and queues installation through PS4 BGFT/AppInstUtil.

> Use OrbisShelf only for software you created, open-source homebrew, or PKGs you have explicit permission to redistribute. It is not intended for copyrighted commercial game distribution.

## Download

Install the latest `OrbisShelf-v0.1.0.pkg` from the GitHub Releases page on a jailbroken/homebrew-enabled PS4.

## Current flow

1. Load the bundled/cached catalog.
2. Refresh `catalog/catalog.json` from this repository.
3. Select an enabled entry with D-pad and press Cross.
4. Follow HTTPS redirects and download the PKG to `/data/OrbisShelf/downloads`.
5. Verify `size_bytes` and optional SHA-256.
6. Register the local PKG with BGFT and monitor installation.
7. Delete the downloaded PKG after a successful install.

Triangle refreshes the catalog. Circle exits while no job is running.

## Hugging Face token

A public Hugging Face repository needs no token.

For a private repository, create a fine-grained read-only Hugging Face token limited to the required repository. Launch OrbisShelf once so `/data/OrbisShelf` is created, enable an FTP server on the jailbroken PS4, and upload this plain-text file:

```text
/data/OrbisShelf/hf_token.txt
```

Example file contents:

```text
hf_example_read_only_token
```

Do not add quotes and do not add the word `Bearer`. Never commit the real token to GitHub or include it in `catalog.json`.

OrbisShelf reads this file when a download starts. It sends the token only to the original Hugging Face origin and removes it when following redirects to the storage CDN.

## Build

The `Build PS4 PKG` GitHub Actions workflow downloads OpenOrbis v0.5.4, compiles with LLVM 18, creates the PKG, uploads a workflow artifact, and publishes `v0.1.0` when changes reach `main`.

For a local build, install the OpenOrbis PS4 Toolchain, export `OO_PS4_TOOLCHAIN`, and run:

```sh
make
```

## Catalog

See [`docs/catalog.md`](docs/catalog.md). The empty initial catalog is intentional. Add only authorized packages and provide SHA-256/size metadata whenever possible.

## Status

The PKG build pipeline is validated. Runtime behavior still needs testing on the target jailbroken PS4 because firmware and payload implementations can differ.
