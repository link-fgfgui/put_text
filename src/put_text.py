#!/usr/bin/env python3
"""HTTP -> SayIt Linux text commit bridge.

Accepts an HTTP POST and commits the body into the currently focused input
field, using the exact mechanism SayIt-Linux uses on Linux
(client/src-tauri/src/linux/text_input.rs + linux/fcitx5/sayitcommit.cpp):

  1. Fcitx5 module : $XDG_RUNTIME_DIR/sayit-linux/fcitx5.sock
  2. IBus engine   : $XDG_RUNTIME_DIR/sayit-linux/ibus.sock
  3. Compatibility : clipboard + synthetic Ctrl+V (wtype / ydotool / xdotool)

Socket wire format: 4-byte big-endian payload length + UTF-8 text, then a short
response line: OK / NO_FOCUS / INVALID / TIMEOUT / ERROR.

Usage:
    ./put_text.py                      # listen on 127.0.0.1:8787
    ./put_text.py --port 9000
    curl -s localhost:8787/text --data-binary '你好，世界'
    curl -s localhost:8787/text -H 'Content-Type: application/json' \
         -d '{"text":"hello"}'
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import shutil
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

SOCKET_DIRECTORY = "sayit-linux"
ENDPOINTS = (
    ("fcitx5_commit", "fcitx5.sock", "Text committed by the SayIt Fcitx5 module"),
    ("ibus_commit", "ibus.sock", "Text committed by the SayIt IBus engine"),
)
MAX_TEXT_BYTES = 1024 * 1024
IO_TIMEOUT = 2.5  # SayIt uses 750ms; the IM side waits up to 2s for its turn.

RESPONSE_REASONS = {
    "NO_FOCUS": "no_focused_input_context",
    "INVALID": "input_method_rejected_request",
    "TIMEOUT": "input_method_commit_timeout",
    "ERROR": "input_method_commit_failed",
}

log = logging.getLogger("put_text")


# ─── Input-method commit ───


def socket_parent() -> Path | None:
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime_dir:
        return None
    return Path(runtime_dir) / SOCKET_DIRECTORY


def commit_to_socket(path: Path, text: str) -> tuple[bool, str]:
    """Send one commit request. Returns (ok, reason_or_response)."""
    payload = text.encode("utf-8")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(IO_TIMEOUT)
            sock.connect(str(path))
            sock.sendall(len(payload).to_bytes(4, "big") + payload)
            raw = sock.recv(128).decode("utf-8", "replace").strip()
    except FileNotFoundError:
        return False, "socket_missing"
    except (OSError, TimeoutError) as error:
        return False, f"io:{error.__class__.__name__}"

    if raw == "OK":
        return True, "OK"
    if not raw:
        return False, "empty_input_method_response"
    return False, RESPONSE_REASONS.get(raw, f"unexpected_input_method_response:{raw}")


# ─── Compatibility fallback: clipboard + synthetic Ctrl+V ───


def is_wayland() -> bool:
    session = os.environ.get("XDG_SESSION_TYPE", "").lower()
    return session == "wayland" or bool(os.environ.get("WAYLAND_DISPLAY"))


def paste_candidates() -> list[tuple[str, list[str]]]:
    """Mirrors SayIt's ordering: wtype/ydotool on Wayland, xdotool on X11."""
    if is_wayland():
        names = ("wtype", "ydotool", "xdotool")
    else:
        names = ("xdotool", "ydotool", "wtype")
    commands = {
        # wtype types literal text; "-k v" is for special keys and fails
        # on some builds, so use plain "v" with the ctrl modifier held.
        "wtype": ["wtype", "-M", "ctrl", "v", "-m", "ctrl"],
        "ydotool": ["ydotool", "key", "29:1", "47:1", "47:0", "29:0"],
        "xdotool": ["xdotool", "key", "--clearmodifiers", "ctrl+v"],
    }
    return [(name, commands[name]) for name in names]


def clipboard_tools() -> tuple[tuple[str, list[str]], tuple[str, list[str]], list[str]]:
    """(write_command, read_command, clear_command) for the current session type.

    read uses ``wl-paste -n`` on purpose: without ``-n`` wl-paste appends an
    extra newline, which would corrupt the ``current == injected`` comparison
    in restore and mangle a restored trailing newline.
    """
    if is_wayland():
        return (("wl-copy", ["wl-copy"]), ("wl-paste", ["wl-paste", "-n"]), ["wl-copy", "-c"])
    return (
        ("xclip", ["xclip", "-selection", "clipboard"]),
        ("xclip", ["xclip", "-selection", "clipboard", "-o"]),
        # xclip has no explicit clear flag; feeding empty stdin is the
        # portable equivalent.
        ["xclip", "-selection", "clipboard"],
    )


def run(command: list[str], stdin: str | None = None, timeout: float = 5.0) -> tuple[bool, str]:
    try:
        result = subprocess.run(
            command,
            input=stdin,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
        )
    except FileNotFoundError:
        return False, "not_found"
    except subprocess.TimeoutExpired:
        return False, "timeout"
    if result.returncode == 0:
        return True, result.stdout
    return False, f"exit={result.returncode}"


PASTE_GENERATION = 0
PASTE_LOCK = threading.Lock()


def paste_via_clipboard(text: str, restore: bool) -> dict:
    (_, write_command), (_, read_command), _ = clipboard_tools()

    previous = ""
    if restore and shutil.which(read_command[0]):
        ok, output = run(read_command, timeout=0.5)
        if ok:
            previous = output

    ok, error = run(write_command, stdin=text)
    if not ok:
        return {
            "ok": False,
            "strategy": "clipboard_write",
            "reason": "clipboard_write_failed",
            "detail": f"{write_command[0]}:{error}",
        }

    global PASTE_GENERATION
    with PASTE_LOCK:
        PASTE_GENERATION += 1
        paste_id = PASTE_GENERATION

    attempts = []
    for name, command in paste_candidates():
        if not shutil.which(name):
            attempts.append(f"{name}:not_found")
            continue
        ok, error = run(command)
        if ok:
            if restore:
                threading.Thread(
                    target=restore_clipboard,
                    args=(paste_id, previous, text),
                    daemon=True,
                ).start()
            return {
                "ok": True,
                "strategy": f"clipboard_{name}",
                "reason": None,
                # Synthetic input only reports that the command succeeded.
                "detail": f"uncertain; attempts=[{', '.join(attempts)}]",
            }
        attempts.append(f"{name}:{error}")

    return {
        "ok": False,
        "strategy": "clipboard_only",
        "reason": "linux_paste_tool_unavailable",
        "detail": f"text copied; attempts=[{', '.join(attempts)}]",
    }


def restore_clipboard(paste_id: int, previous: str, injected: str) -> None:
    """Put the user's earlier clipboard content back, unless it moved on.

    When the clipboard was empty before the paste, clear it instead of
    leaving the injected text behind.
    """
    time.sleep(0.5)
    with PASTE_LOCK:
        stale = PASTE_GENERATION != paste_id
    if stale:
        return
    (_, write_command), (_, read_command), clear_command = clipboard_tools()
    ok, current = run(read_command, timeout=0.5)
    if ok and current == injected:
        if previous:
            run(write_command, stdin=previous)
        else:
            if is_wayland():
                run(clear_command)
            else:
                run(write_command, stdin="")


# ─── Commit chain ───


def commit_text(text: str, restore_clipboard: bool = True) -> dict:
    if not text:
        return {"ok": True, "strategy": "empty_text", "reason": None, "detail": "nothing to commit"}
    if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
        return {
            "ok": False,
            "strategy": None,
            "reason": "text_too_large",
            "detail": f"{len(text.encode('utf-8'))} bytes",
        }

    parent = socket_parent()
    input_method_errors = []
    if parent is None:
        input_method_errors.append("xdg_runtime_dir_unavailable")
        log.info("xdg_runtime_dir unavailable, falling back to clipboard")
    else:
        for strategy, socket_name, description in ENDPOINTS:
            ok, result = commit_to_socket(parent / socket_name, text)
            if ok:
                log.info("commit succeeded strategy=%s", strategy)
                return {"ok": True, "strategy": strategy, "reason": None, "detail": description}
            input_method_errors.append(f"{strategy}={result}")
            log.info("commit unavailable strategy=%s reason=%s", strategy, result)

    log.info("falling back to clipboard: %s", ";".join(input_method_errors))
    result = paste_via_clipboard(text, restore_clipboard)
    result["detail"] = f"{result['detail']}; input_method=[{';'.join(input_method_errors)}]"
    return result


# ─── HTTP ───


def extract_text(content_type: str, body: bytes) -> str:
    content_type = content_type.split(";")[0].strip().lower()
    if content_type == "application/json":
        try:
            data = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError(f"invalid_json:{error}") from error
        if isinstance(data, str):
            return data
        if isinstance(data, dict):
            for key in ("text", "content", "body"):
                value = data.get(key)
                if isinstance(value, str):
                    return value
            raise ValueError("json_missing_text_field")
        raise ValueError("json_body_not_object_or_string")
    if content_type == "application/x-www-form-urlencoded":
        # curl --data defaults to this type, so treat a keyless body as plain text.
        values = parse_qs(body.decode("utf-8", "replace"))
        for key in ("text", "content", "body"):
            if values.get(key):
                return values[key][0]
    return body.decode("utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "put_text"
    restore_clipboard = True
    # POST accepts /text (documented) plus / for backward compatibility;
    # GET serves status on / (plus /text for convenience). Anything else 404s.
    POST_ROUTES = ("/text", "/")
    GET_ROUTES = ("/", "/text")

    @staticmethod
    def _route(path: str) -> str:
        return path.split("?", 1)[0].rstrip("/") or "/"

    def _finish(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        if self._route(self.path) not in self.POST_ROUTES:
            self._finish(404, {"ok": False, "reason": "not_found"})
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            self._finish(400, {"ok": False, "reason": "bad_content_length"})
            return
        if length > MAX_TEXT_BYTES + 8192:
            self._finish(413, {"ok": False, "reason": "payload_too_large"})
            return
        raw = self.rfile.read(length) if length else b""

        try:
            text = extract_text(self.headers.get("Content-Type", ""), raw)
        except ValueError as error:
            self._finish(400, {"ok": False, "reason": str(error)})
            return

        result = commit_text(text, self.restore_clipboard)
        log.info("POST %s chars=%d ok=%s strategy=%s", self.path, len(text), result["ok"], result["strategy"])
        self._finish(200 if result["ok"] else 502, result)

    def do_GET(self) -> None:
        if self._route(self.path) not in self.GET_ROUTES:
            self._finish(404, {"ok": False, "reason": "not_found"})
            return
        parent = socket_parent()
        self._finish(
            200,
            {
                "ok": True,
                "service": "put_text",
                "socket_directory": str(parent) if parent else None,
                "input_methods": [
                    {
                        "strategy": strategy,
                        "socket": str(parent / socket_name) if parent else None,
                        "available": bool(parent) and (parent / socket_name).exists(),
                    }
                    for strategy, socket_name, _ in ENDPOINTS
                ],
                "paste_tools": [name for name, _ in paste_candidates() if shutil.which(name)],
            },
        )

    def log_message(self, fmt: str, *args) -> None:
        log.debug("%s %s", self.address_string(), fmt % args)


def main() -> None:
    parser = argparse.ArgumentParser(description="HTTP POST -> focused input field (SayIt Linux mechanism)")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8787, help="bind port (default: 8787)")
    parser.add_argument("--no-restore-clipboard", action="store_true", help="leave the pasted text on the clipboard")
    parser.add_argument("--verbose", action="store_true", help="log every request")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="[put_text] %(levelname)s %(message)s",
    )

    Handler.restore_clipboard = not args.no_restore_clipboard
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    log.info("listening on http://%s:%d (POST /text, GET / for status)", args.host, args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
