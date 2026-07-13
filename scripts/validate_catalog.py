#!/usr/bin/env python3
"""Dependency-free validation for OrbisShelf's public catalog."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import urlparse

ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{1,63}$")
TITLE_ID_RE = re.compile(r"^[A-Z]{4}[0-9]{5}$")
SHA_RE = re.compile(r"^[a-fA-F0-9]{64}$")
TYPES = {"app", "game", "update", "dlc"}
ALLOWED_KEYS = {
    "id", "name", "title_id", "type", "version", "pkg_url",
    "sha256", "size_bytes", "enabled", "notes",
}


def fail(message: str) -> None:
    raise ValueError(message)


def validate(path: Path) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        fail("catalog root must be an object")
    if data.get("schema_version") != 1:
        fail("schema_version must be 1")
    if not isinstance(data.get("generated_at"), str) or not data["generated_at"]:
        fail("generated_at must be a non-empty ISO-8601 string")
    items = data.get("items")
    if not isinstance(items, list):
        fail("items must be an array")

    seen: set[str] = set()
    for index, item in enumerate(items):
        prefix = f"items[{index}]"
        if not isinstance(item, dict):
            fail(f"{prefix} must be an object")
        unknown = set(item) - ALLOWED_KEYS
        if unknown:
            fail(f"{prefix} has unknown keys: {sorted(unknown)}")
        for key in ("id", "name", "type", "version", "pkg_url", "enabled"):
            if key not in item:
                fail(f"{prefix}.{key} is required")
        item_id = item["id"]
        if not isinstance(item_id, str) or not ID_RE.fullmatch(item_id):
            fail(f"{prefix}.id is invalid")
        if item_id in seen:
            fail(f"duplicate id: {item_id}")
        seen.add(item_id)
        if not isinstance(item["name"], str) or not 1 <= len(item["name"]) <= 96:
            fail(f"{prefix}.name must be 1-96 characters")
        if item["type"] not in TYPES:
            fail(f"{prefix}.type must be one of {sorted(TYPES)}")
        if not isinstance(item["version"], str) or not 1 <= len(item["version"]) <= 16:
            fail(f"{prefix}.version must be 1-16 characters")
        parsed = urlparse(item["pkg_url"])
        if parsed.scheme != "https" or not parsed.netloc:
            fail(f"{prefix}.pkg_url must be an absolute HTTPS URL")
        if "title_id" in item and (not isinstance(item["title_id"], str) or not TITLE_ID_RE.fullmatch(item["title_id"])):
            fail(f"{prefix}.title_id is invalid")
        sha = item.get("sha256", "")
        if sha and (not isinstance(sha, str) or not SHA_RE.fullmatch(sha)):
            fail(f"{prefix}.sha256 must be empty or 64 hexadecimal characters")
        size = item.get("size_bytes", 0)
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            fail(f"{prefix}.size_bytes must be a non-negative integer")
        if not isinstance(item["enabled"], bool):
            fail(f"{prefix}.enabled must be boolean")


if __name__ == "__main__":
    target = Path(sys.argv[1] if len(sys.argv) > 1 else "catalog/catalog.json")
    try:
        validate(target)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"catalog validation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
    print(f"catalog OK: {target}")
