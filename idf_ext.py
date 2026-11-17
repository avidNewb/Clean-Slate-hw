"""Project-local idf.py extension adding the optional `idf.py push` OTA upload action."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

from idf_py_actions.constants import GENERATORS
from idf_py_actions.errors import FatalError
from idf_py_actions.global_options import global_options
from idf_py_actions.tools import PropertyDict, ensure_build_directory, run_target


def action_extensions(base_actions: dict, project_path: str) -> dict:
    def push_callback(
        action: str,
        ctx,
        args: PropertyDict,
        url: str,
        token: str,
        notes: str,
        allow_downgrade: bool,
        no_build: bool,
        check: bool,
    ) -> None:
        if not no_build:
            ensure_build_directory(args, ctx.info_name)
            run_target(
                "app",
                args,
                force_progression=GENERATORS[args.generator].get("force_progression", False),
            )

        proj = Path(args.project_dir).resolve()
        build_dir = Path(args.build_dir).resolve()
        script = proj / "scripts" / "push_firmware.py"
        if not script.is_file():
            raise FatalError(f"Uploader script not found: {script}")

        bin_path = build_dir / "clean_slate_hw.bin"
        desc_path = build_dir / "project_description.json"
        if desc_path.is_file():
            try:
                app_bin = json.loads(desc_path.read_text(encoding="utf-8")).get("app_bin")
                if app_bin:
                    bin_path = build_dir / app_bin
            except (OSError, ValueError):
                pass

        cmd = [sys.executable, str(script), "--bin", str(bin_path)]

        if url:
            cmd.extend(["--url", url])
        if token:
            cmd.extend(["--token", token])
        if notes:
            cmd.extend(["--notes", notes])
        if allow_downgrade:
            cmd.append("--allow-downgrade")
        if check:
            cmd.append("--dry-run")

        res = subprocess.run(cmd, check=False)
        if res.returncode != 0:
            raise FatalError(f"Firmware push failed with exit code {res.returncode}")

    return {
        "version": "1.0.0",
        "actions": {
            "push": {
                "callback": push_callback,
                "help": "Build app (unless --no-build) and upload firmware .bin to the Clean Slate OTA server.",
                "options": global_options
                + [
                    {
                        "names": ["--url"],
                        "help": "Clean Slate server base URL (overrides CS_OTA_SERVER_URL).",
                        "default": os.environ.get("CS_OTA_SERVER_URL", ""),
                    },
                    {
                        "names": ["--token"],
                        "help": "Admin token for X-Admin-Token (overrides CS_OTA_ADMIN_TOKEN).",
                        "default": os.environ.get("CS_OTA_ADMIN_TOKEN", ""),
                    },
                    {
                        "names": ["--notes"],
                        "help": "Optional release notes for the OTA manifest.",
                        "default": os.environ.get("CS_OTA_NOTES", ""),
                    },
                    {
                        "names": ["--allow-downgrade"],
                        "help": "Set allow_downgrade=true in the OTA manifest.",
                        "is_flag": True,
                        "default": False,
                    },
                    {
                        "names": ["--no-build"],
                        "help": "Skip building the app before uploading the existing .bin.",
                        "is_flag": True,
                        "default": False,
                    },
                    {
                        "names": ["--check"],
                        "help": "Print version, hash, and target URL without uploading.",
                        "is_flag": True,
                        "default": False,
                    },
                ],
            }
        }
    }
