# Catalog format

`catalog/catalog.json` is the only remote list consumed by the PS4 application. The app downloads the raw file from the repository's `main` branch and falls back to its bundled or cached copy when offline.

Only packages you created, open-source homebrew packages, or packages you are licensed to redistribute belong in this catalog.

## Item example

```json
{
  "id": "example-homebrew",
  "name": "Example Homebrew",
  "title_id": "BREW00001",
  "type": "app",
  "version": "1.00",
  "pkg_url": "https://huggingface.co/OWNER/REPOSITORY/resolve/main/example.pkg?download=true",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size_bytes": 12345678,
  "enabled": true,
  "notes": "Optional short note"
}
```

`type` accepts `app`, `game`, `update`, or `dlc`. `sha256` and `size_bytes` are optional but strongly recommended. Disabled entries remain in JSON but are hidden by the app.

## Hugging Face files

For a public Hugging Face repository, use its `resolve/main/...` HTTPS URL. No token is required.

Do not commit a Hugging Face token or compile one into the PKG. Public Git history and distributed PKG binaries expose embedded secrets. For a private Hugging Face repository, place a read-only token manually at `/data/OrbisShelf/hf_token.txt` on the console. OrbisShelf sends it only to the original host, not to redirected CDN hosts. A short-lived URL service is safer for broader distribution.

## Validation

Run:

```sh
python scripts/validate_catalog.py catalog/catalog.json
```
