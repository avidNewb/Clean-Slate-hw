#!/usr/bin/env python3
"""Upload built Clean Slate ESP32 firmware binary to the OTA server."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path

DEFAULT_AZURE_URL = "https://clean-slate-cce5b9f3g2ggf5cv.westus3-01.azurewebsites.net"
PLACEHOLDER_API_URL = "http://192.168.1.100:8000"


def read_sdkconfig_value(sdkconfig_path: Path, key: str) -> str:
    if not sdkconfig_path.is_file():
        return ""
    pattern = re.compile(rf'^{re.escape(key)}="(.*)"\s*$')
    for line in sdkconfig_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        m = pattern.match(line)
        if m:
            return m.group(1)
    return ""


def resolve_server_url(explicit_url: str | None, sdkconfig_path: Path) -> str:
    if explicit_url and explicit_url.strip():
        return explicit_url.strip().rstrip("/")
    env_url = os.environ.get("CS_OTA_SERVER_URL", "").strip()
    if env_url:
        return env_url.rstrip("/")
    cfg_url = read_sdkconfig_value(sdkconfig_path, "CONFIG_CS_API_BASE_URL").strip().rstrip("/")
    if cfg_url and cfg_url != PLACEHOLDER_API_URL:
        return cfg_url
    return DEFAULT_AZURE_URL


def build_multipart_body(
    fields: dict[str, str],
    file_field: str,
    upload_filename: str,
    file_bytes: bytes,
) -> tuple[bytes, str]:
    boundary = f"----CleanSlateBoundary{uuid.uuid4().hex}"
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.append(
            (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                f"{value}\r\n"
            ).encode("utf-8")
        )
    chunks.append(
        (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{file_field}"; filename="{upload_filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8")
    )
    chunks.append(file_bytes)
    chunks.append(f"\r\n--{boundary}--\r\n".encode("utf-8"))
    body = b"".join(chunks)
    content_type = f"multipart/form-data; boundary={boundary}"
    return body, content_type


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    sdkconfig_path = root / "sdkconfig"

    parser = argparse.ArgumentParser(description="Push built firmware .bin to Clean Slate OTA server.")
    parser.add_argument(
        "--bin",
        type=Path,
        default=root / "build" / "clean_slate_hw.bin",
        help="Path to compiled firmware binary (default: build/clean_slate_hw.bin)",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Firmware version string (default: CONFIG_CS_FIRMWARE_VERSION from sdkconfig)",
    )
    parser.add_argument(
        "--url",
        default="",
        help="Server base URL (default: $CS_OTA_SERVER_URL or Azure server)",
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("CS_OTA_ADMIN_TOKEN", ""),
        help="Admin token for X-Admin-Token header (default: $CS_OTA_ADMIN_TOKEN)",
    )
    parser.add_argument(
        "--notes",
        default=os.environ.get("CS_OTA_NOTES", ""),
        help="Optional release notes (default: $CS_OTA_NOTES)",
    )
    parser.add_argument(
        "--allow-downgrade",
        action="store_true",
        default=os.environ.get("CS_OTA_ALLOW_DOWNGRADE", "").lower() in ("1", "true", "yes"),
        help="Pass allow_downgrade=true to server",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print upload metadata without sending")
    args = parser.parse_args()

    bin_path = args.bin.resolve()
    if not bin_path.is_file():
        print(f"ERROR: Firmware binary not found: {bin_path}", file=sys.stderr)
        return 1

    version = args.version.strip() or read_sdkconfig_value(sdkconfig_path, "CONFIG_CS_FIRMWARE_VERSION").strip()
    if not version:
        print("ERROR: Could not determine CONFIG_CS_FIRMWARE_VERSION from sdkconfig.", file=sys.stderr)
        return 1

    server_url = resolve_server_url(args.url, sdkconfig_path)
    endpoint = f"{server_url}/api/v1/firmware/upload"
    fw_bytes = bin_path.read_bytes()
    sha256 = hashlib.sha256(fw_bytes).hexdigest()
    upload_name = f"cleanslate-{version}.bin"

    print(f"Pushing Clean Slate firmware {version} ({len(fw_bytes)} bytes, sha256={sha256[:12]}...)")
    print(f"Target: {endpoint} as {upload_name}")

    if args.dry_run:
        return 0

    body, content_type = build_multipart_body(
        fields={
            "version": version,
            "release_notes": args.notes,
            "allow_downgrade": "true" if args.allow_downgrade else "false",
        },
        file_field="file",
        upload_filename=upload_name,
        file_bytes=fw_bytes,
    )

    headers = {
        "Content-Type": content_type,
        "Content-Length": str(len(body)),
        "Accept": "application/json",
    }
    if args.token.strip():
        headers["X-Admin-Token"] = args.token.strip()

    req = urllib.request.Request(
        endpoint,
        data=body,
        method="POST",
        headers=headers,
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            resp_body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        err_body = exc.read().decode("utf-8", errors="replace")
        print(f"ERROR: Upload failed with HTTP {exc.code}: {err_body}", file=sys.stderr)
        if exc.code in (401, 403):
            print(
                "Hint: Set $env:CS_OTA_ADMIN_TOKEN or pass --token <admin-token>.",
                file=sys.stderr,
            )
        return 1
    except urllib.error.URLError as exc:
        print(f"ERROR: Could not reach {endpoint}: {exc.reason}", file=sys.stderr)
        return 1

    try:
        parsed = json.loads(resp_body)
    except json.JSONDecodeError:
        print(f"Upload returned non-JSON response:\n{resp_body}")
        return 0

    srv_ver = parsed.get("latest_version")
    srv_sha = parsed.get("sha256")
    srv_size = parsed.get("size_bytes")
    print(
        f"Upload succeeded: latest_version={srv_ver}, filename={parsed.get('filename')}, "
        f"size={srv_size}, sha256={srv_sha}"
    )
    if srv_sha and srv_sha.lower() != sha256.lower():
        print("WARNING: Server SHA-256 mismatch!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
