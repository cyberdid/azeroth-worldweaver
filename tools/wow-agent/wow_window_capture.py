#!/usr/bin/env python3
"""Capture only a verified, mapped World of Warcraft X11 client window."""

from __future__ import annotations

import json
import fcntl
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

try:
    from PIL import Image
except ImportError:  # report a useful unavailable result to the caller
    Image = None

MAX_XWD_BYTES = 64 * 1024 * 1024
MAX_PNG_BYTES = 20 * 1024 * 1024
MAX_IMAGE_DIMENSION = 8192
MAX_IMAGE_PIXELS = 20_000_000
RETAIN_IMAGES = 6
_VISUAL_REQUEST_RE = re.compile(
    r"(?:\b(?:screenshot|screen|image|picture|visual|look\s+(?:at\s+)?(?:the\s+)?(?:screen|game|window)|"
    r"what\s+do\s+you\s+see|what(?:'s|\s+is)\s+on\s+screen)\b"
    r"|\b(?:скрін(?:шот)?|скрин(?:шот)?|фото|зображенн\w*|картинк\w*|екран\w*|"
    r"бачиш|видно|глянь|поглянь|подив(?:ись|ися)|що\s+(?:ти\s+)?бачиш|що\s+на\s+екрані|"
    r"опиши\s+(?:зображення|картинку|скрін(?:шот)?))\b)",
    re.IGNORECASE,
)
_NEGATED_VISUAL_REQUEST_RE = re.compile(
    r"\b(?:no|without|avoid|stop|don't|do\s+not)\s+(?:(?:take|capture|make|create)\s+)?"
    r"(?:a\s+)?(?:screenshot|screen\s*capture|screen(?:shot)?|image|picture)\b"
    r"|\b(?:не(?:\s+(?:роби|робіть|знімай|знімайте|створюй|створюйте))?|без|ніяких|ніякого|"
    r"не\s+(?:треба|потрібно)\s+(?:робити|знімати))\s+"
    r"(?:будь-яких\s+)?(?:скріншот\w*|скрін\w*|скриншот\w*|скрин\w*|кадр\w*|знімок\w*|фото\w*|зображенн\w*|екран\w*)\b",
    re.IGNORECASE,
)


def is_visual_request(text: str) -> bool:
    """Return whether the user explicitly asks for visual inspection of the game."""
    return (isinstance(text, str) and not _NEGATED_VISUAL_REQUEST_RE.search(text)
            and bool(_VISUAL_REQUEST_RE.search(text)))


def _run(args: list[str], timeout: float = 5.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)


def _window_ids() -> list[str]:
    for prop in ("_NET_CLIENT_LIST_STACKING", "_NET_CLIENT_LIST"):
        result = _run(["xprop", "-root", prop])
        match = re.search(r"(?:=|:)\s*(.*)$", result.stdout, re.M)
        if result.returncode == 0 and match:
            ids = re.findall(r"0x[0-9a-fA-F]+", match.group(1))
            if ids:
                return list(dict.fromkeys(ids))
    return []


def _active_window_id() -> str | None:
    result = _run(["xprop", "-root", "_NET_ACTIVE_WINDOW"])
    if result.returncode != 0:
        return None
    match = re.search(r"window id #\s*(0x[0-9a-fA-F]+|\d+)", result.stdout)
    if not match:
        return None
    try:
        return hex(int(match.group(1), 0))
    except ValueError:
        return None


def _property(output: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}(?:\([^)]*\))?\s*=\s*(.*)$", output, re.M)
    return match.group(1).strip() if match else ""


def _identify_window(window_id: str) -> tuple[dict[str, Any] | None, str | None]:
    props = _run(["xprop", "-id", window_id, "_NET_WM_NAME", "WM_NAME", "WM_CLASS", "_NET_WM_PID",
                  "_NET_WM_STATE", "WM_STATE"])
    if props.returncode != 0:
        return None, "cannot inspect X11 window properties"
    title = _property(props.stdout, "_NET_WM_NAME") or _property(props.stdout, "WM_NAME")
    wm_class = _property(props.stdout, "WM_CLASS")
    identity = f"{title} {wm_class}".lower()
    if not re.search(r"world\s+of\s+warcraft|warcraft|wow", identity):
        return None, None

    pid_value = _property(props.stdout, "_NET_WM_PID")
    pid_match = re.search(r"\b(\d+)\b", pid_value)
    if not pid_match:
        return None, "window has no verifiable owner PID"

    pid = int(pid_match.group(1))
    try:
        command_line = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace")
    except OSError:
        return None, "cannot verify the WoW window owner process"
    if not re.search(r"(?<![A-Za-z0-9_])wow\.exe(?![A-Za-z0-9_])|world\s+of\s+warcraft", command_line, re.I):
        return None, None

    info = _run(["xwininfo", "-id", window_id])
    if info.returncode != 0:
        return None, "cannot verify WoW window mapping"
    map_match = re.search(r"Map State:\s*(\w+)", info.stdout)
    width_match = re.search(r"Width:\s*(\d+)", info.stdout)
    height_match = re.search(r"Height:\s*(\d+)", info.stdout)
    if not width_match or not height_match:
        return None, "cannot read WoW window dimensions"
    width, height = int(width_match.group(1)), int(height_match.group(1))
    states = _property(props.stdout, "_NET_WM_STATE")
    return {
        "window_id": window_id,
        "title": title,
        "wm_class": wm_class,
        "pid": pid,
        "process_verified": True,
        "width": width,
        "height": height,
        "map_state": map_match.group(1) if map_match else None,
        "hidden": ("_NET_WM_STATE_HIDDEN" in states
                   or bool(re.search(r"window state:\s*Iconic(?:State)?\b", props.stdout, re.I))),
    }, None


def _read_snapshot(
    snapshot_path: str | None,
) -> tuple[int | None, str | None, float | None, str | None]:
    if not snapshot_path:
        return None, None, None, None
    try:
        snapshot_file = Path(snapshot_path)
        if snapshot_file.stat().st_size > 16 * 1024 * 1024:
            return None, None, None, None
        snapshot = json.loads(snapshot_file.read_text(encoding="utf-8"))
        if not isinstance(snapshot, dict):
            return None, None, None, None
        captured_ms = snapshot.get("captured_at_unix_ms")
        if not isinstance(captured_ms, int):
            captured_ms = int(float(snapshot.get("captured_at", 0)) * 1000)
        age = max(0.0, (time.time() * 1000 - captured_ms) / 1000)
        return (snapshot.get("snapshot_seq"), snapshot.get("session_id"), round(age, 3),
                snapshot.get("group_id"))
    except (OSError, UnicodeError, json.JSONDecodeError, TypeError, ValueError, OverflowError):
        return None, None, None, None


def _decode_xwd(raw_path: str) -> Any:
    """Decode the XWD ZPixmap layout emitted by xwd without relying on ImageMagick."""
    if Image is None:
        raise ValueError("Pillow is unavailable")
    raw = Path(raw_path).read_bytes()
    if len(raw) < 100:
        raise ValueError("XWD file is shorter than its fixed header")
    fields = struct.unpack(">25I", raw[:100])
    (header_size, version, pixmap_format, depth, width, height, xoffset, byte_order,
     _bitmap_unit, _bitmap_bit_order, _bitmap_pad, bits_per_pixel, bytes_per_line,
     visual_class, red_mask, green_mask, blue_mask, _bits_per_rgb, _colormap_entries,
     ncolors, _window_width, _window_height, _window_x, _window_y, _window_bdrwidth) = fields
    if version != 7 or pixmap_format != 2:
        raise ValueError("XWD image is not a version 7 ZPixmap")
    if not (1 <= width <= MAX_IMAGE_DIMENSION and 1 <= height <= MAX_IMAGE_DIMENSION
            and width * height <= MAX_IMAGE_PIXELS and 1 <= depth <= 32):
        raise ValueError("XWD image dimensions are outside capture limits")
    if bits_per_pixel not in (8, 16, 24, 32) or bytes_per_line < (xoffset + width) * (bits_per_pixel // 8):
        raise ValueError("XWD pixel layout is unsupported or truncated")
    color_offset = max(100, header_size)
    image_offset = color_offset + ncolors * 12
    expected_end = image_offset + bytes_per_line * height
    if header_size < 100 or image_offset > len(raw) or expected_end > len(raw):
        raise ValueError("XWD image payload is truncated")

    palette: dict[int, tuple[int, int, int]] = {}
    for index in range(ncolors):
        entry = color_offset + index * 12
        pixel, red, green, blue, _flags, _pad = struct.unpack(">IHHHBB", raw[entry:entry + 12])
        palette[pixel] = (red >> 8, green >> 8, blue >> 8)

    byteorder = "little" if byte_order == 0 else "big" if byte_order == 1 else None
    if byteorder is None:
        raise ValueError("XWD image declares an unknown pixel byte order")
    bytes_per_pixel = bits_per_pixel // 8

    # Pillow's raw decoder handles the common X11 TrueColor byte layouts in C.
    # Avoid a Python loop over every pixel for the large 24/32-bit WoW window.
    raw_mode = None
    if visual_class in (4, 5) and xoffset == 0:
        masks = (red_mask, green_mask, blue_mask)
        if bits_per_pixel == 24:
            if masks == (0x00FF0000, 0x0000FF00, 0x000000FF):
                raw_mode = "BGR" if byteorder == "little" else "RGB"
            elif masks == (0x000000FF, 0x0000FF00, 0x00FF0000):
                raw_mode = "RGB" if byteorder == "little" else "BGR"
        elif bits_per_pixel == 32:
            if masks == (0x00FF0000, 0x0000FF00, 0x000000FF):
                raw_mode = "BGRX" if byteorder == "little" else "XRGB"
            elif masks == (0x000000FF, 0x0000FF00, 0x00FF0000):
                raw_mode = "RGBX" if byteorder == "little" else "XBGR"
    if raw_mode is not None:
        return Image.frombytes(
            "RGB", (width, height), raw[image_offset:expected_end],
            "raw", raw_mode, bytes_per_line, 1)

    def channel(pixel: int, mask: int) -> int:
        if not mask:
            return 0
        shift = (mask & -mask).bit_length() - 1
        maximum = mask >> shift
        value = (pixel & mask) >> shift
        return (value * 255 + maximum // 2) // maximum

    pixels = bytearray(width * height * 3)
    source = memoryview(raw)
    dest_index = 0
    for y in range(height):
        row_offset = image_offset + y * bytes_per_line + xoffset * bytes_per_pixel
        for x in range(width):
            pixel_offset = row_offset + x * bytes_per_pixel
            value = int.from_bytes(source[pixel_offset:pixel_offset + bytes_per_pixel], byteorder)
            if visual_class in (4, 5) and (red_mask or green_mask or blue_mask):
                rgb = (channel(value, red_mask), channel(value, green_mask), channel(value, blue_mask))
            elif palette:
                rgb = palette.get(value, (0, 0, 0))
            else:
                maximum = (1 << depth) - 1
                gray = (value * 255 + maximum // 2) // maximum
                rgb = (gray, gray, gray)
            pixels[dest_index:dest_index + 3] = bytes(rgb)
            dest_index += 3
    return Image.frombytes("RGB", (width, height), bytes(pixels))


def _unavailable(reason: str) -> dict[str, Any]:
    return {"available": False, "path": None, "captured_at": None, "window_id": None,
            "snapshot_seq": None, "group_id": None, "reason": reason}


def _publish_metadata(directory: Path, metadata: dict[str, Any]) -> None:
    temp_fd, temp_name = tempfile.mkstemp(prefix=".latest-", suffix=".json.tmp", dir=directory)
    temp_metadata = Path(temp_name)
    try:
        with os.fdopen(temp_fd, "w", encoding="utf-8") as stream:
            json.dump(metadata, stream, ensure_ascii=False)
        os.chmod(temp_metadata, 0o600)
        os.replace(temp_metadata, directory / "latest.json")
    except BaseException:
        temp_metadata.unlink(missing_ok=True)
        raise


def publish_unavailable(output_dir: str, reason: str) -> dict[str, Any]:
    """Invalidate the shared latest-image pointer, including after unexpected failures."""
    metadata = _unavailable(reason)
    metadata["status_updated_at"] = time.strftime("%Y-%m-%d %H:%M:%S %Z", time.localtime())
    directory = Path(output_dir)
    metadata_path = directory / "latest.json"
    try:
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(directory, 0o700)
        _publish_metadata(directory, metadata)
    except OSError:
        try:
            metadata_path.unlink(missing_ok=True)
        except OSError:
            pass
    return metadata


def _capture_game_window(
    snapshot_path: str | None,
    output_dir: str,
    min_interval_seconds: float = 5.0,
) -> dict[str, Any]:
    """Capture the single verified WoW client window, or return why no image is safe to use."""
    directory = Path(output_dir)
    try:
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(directory, 0o700)
    except OSError as exc:
        reason = f"cannot prepare private screenshot directory: {exc}"
        return publish_unavailable(output_dir, reason)

    def unavailable(reason: str) -> dict[str, Any]:
        return publish_unavailable(output_dir, reason)

    if Image is None:
        return unavailable("Pillow is unavailable")
    if not all(shutil.which(binary) for binary in ("xprop", "xwininfo", "xwd")):
        return unavailable("xprop, xwininfo, and xwd are required for verified window capture")

    metadata_path = directory / "latest.json"
    current_snapshot_seq, current_session, current_snapshot_age, current_group_id = _read_snapshot(snapshot_path)

    if not os.environ.get("DISPLAY"):
        return unavailable("no X11 DISPLAY is available to the bridge process")
    try:
        candidates: list[dict[str, Any]] = []
        reasons: list[str] = []
        for window_id in _window_ids():
            info, reason = _identify_window(window_id)
            if info:
                candidates.append(info)
            elif reason:
                reasons.append(reason)
        if reasons:
            return unavailable("a WoW-like X11 window could not be verified: " + reasons[0])
        if not candidates:
            return unavailable(reasons[0] if reasons else "no mapped, process-verified WoW window was found")
        if len(candidates) != 1:
            return unavailable(f"found {len(candidates)} matching WoW windows; refusing ambiguous capture")
    except (OSError, subprocess.TimeoutExpired) as exc:
        return unavailable(f"X11 window inspection failed: {exc}")

    window = candidates[0]
    if _active_window_id() != window["window_id"].lower():
        return unavailable("WoW window is not the active window; refusing an occluded capture")
    if window["hidden"]:
        return unavailable("WoW window is minimized or hidden")
    if window["map_state"] != "IsViewable":
        return unavailable("WoW window is not mapped and visible")
    if (window["width"] < 1 or window["height"] < 1
            or window["width"] > MAX_IMAGE_DIMENSION or window["height"] > MAX_IMAGE_DIMENSION):
        return unavailable("WoW window dimensions are outside capture limits")

    try:
        previous = json.loads(metadata_path.read_text(encoding="utf-8"))
        if not isinstance(previous, dict):
            raise ValueError("screenshot metadata is not an object")
        age = time.time() - float(previous.get("captured_at_epoch", 0))
        image_path = Path(previous.get("path", ""))
        inside_output_dir = image_path.resolve().parent == directory.resolve()
        same_capture_target = (previous.get("available") is True
                               and previous.get("window_id") == window["window_id"]
                               and previous.get("pid") == window["pid"]
                               and previous.get("dimensions") == [window["width"], window["height"]]
                               and previous.get("capture_group_id") == current_group_id
                               and previous.get("capture_session_id") == current_session)
        if (age < max(1.0, min_interval_seconds) and inside_output_dir and image_path.is_file()
                and same_capture_target):
            current_window, _reason = _identify_window(window["window_id"])
            if (current_window is None or current_window["hidden"]
                    or current_window["map_state"] != "IsViewable"
                    or current_window["pid"] != window["pid"]
                    or (current_window["width"], current_window["height"])
                    != (window["width"], window["height"])):
                return unavailable("WoW window changed or became unavailable before cached reuse")
            cached = dict(previous)
            cached["cached"] = True
            cached["current_snapshot_seq"] = current_snapshot_seq
            cached["current_session_id"] = current_session
            cached["current_group_id"] = current_group_id
            cached["current_snapshot_age_seconds"] = current_snapshot_age
            cached["checked_at"] = time.strftime("%Y-%m-%d %H:%M:%S %Z", time.localtime())
            cached["stale"] = (previous.get("capture_session_id") != current_session
                               or previous.get("capture_group_id") != current_group_id
                               or current_snapshot_seq is None or current_snapshot_age is None
                               or age > max(5.0, min_interval_seconds)
                               or current_snapshot_age > 5.0)
            cached["reason"] = "reused recent capture after verifying the current WoW window"
            try:
                _publish_metadata(directory, cached)
            except OSError as exc:
                return unavailable(f"cannot refresh screenshot metadata: {exc}")
            return cached
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        pass

    timestamp = time.time()
    stamp = f"{time.time_ns()}-{os.getpid()}"
    image_path = directory / f"wow-window-{stamp}.png"
    try:
        fd, raw_path = tempfile.mkstemp(prefix=".wow-window-", suffix=".xwd", dir=directory)
        os.close(fd)
    except OSError as exc:
        return unavailable(f"cannot create private raw screenshot file: {exc}")
    try:
        result = _run(["xwd", "-silent", "-id", window["window_id"], "-out", raw_path], timeout=20.0)
        if result.returncode != 0:
            return unavailable("xwd could not capture the verified WoW window ID")
        if os.path.getsize(raw_path) > MAX_XWD_BYTES:
            return unavailable("WoW window capture exceeds the raw image size limit")
        with _decode_xwd(raw_path) as source:
            if source.size != (window["width"], window["height"]):
                return unavailable("captured image dimensions do not match the verified WoW window")
            source.save(image_path, format="PNG", optimize=True)
        os.chmod(image_path, 0o600)
        if image_path.stat().st_size > MAX_PNG_BYTES:
            image_path.unlink(missing_ok=True)
            return unavailable("WoW PNG exceeds the image size limit")
        final_window, _reason = _identify_window(window["window_id"])
        if (final_window is None or final_window["hidden"] or final_window["map_state"] != "IsViewable"
                or final_window["pid"] != window["pid"]
                or (final_window["width"], final_window["height"]) != (window["width"], window["height"])):
            image_path.unlink(missing_ok=True)
            return unavailable("WoW window changed or became unavailable during capture")
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired) as exc:
        image_path.unlink(missing_ok=True)
        return unavailable(f"WoW window image decoding failed: {exc}")
    finally:
        Path(raw_path).unlink(missing_ok=True)

    metadata = {
        "available": True,
        "path": str(image_path),
        "captured_at": time.strftime("%Y-%m-%d %H:%M:%S %Z", time.localtime(timestamp)),
        "captured_at_epoch": timestamp,
        "checked_at": time.strftime("%Y-%m-%d %H:%M:%S %Z", time.localtime(timestamp)),
        "window_id": window["window_id"],
        "title": window["title"],
        "wm_class": window["wm_class"],
        "pid": window["pid"],
        "process_verified": window["process_verified"],
        "dimensions": [window["width"], window["height"]],
        "snapshot_seq": current_snapshot_seq,
        "capture_snapshot_seq": current_snapshot_seq,
        "session_id": current_session,
        "capture_session_id": current_session,
        "group_id": current_group_id,
        "capture_group_id": current_group_id,
        "world_snapshot_age_seconds": current_snapshot_age,
        "stale": (current_snapshot_seq is None or current_snapshot_age is None
                  or current_snapshot_age > 5.0),
        "cached": False,
    }
    try:
        _publish_metadata(directory, metadata)
    except OSError as exc:
        image_path.unlink(missing_ok=True)
        return unavailable(f"cannot publish screenshot metadata: {exc}")

    images = sorted(directory.glob("wow-window-*.png"), key=lambda path: path.stat().st_mtime, reverse=True)
    for old_image in images[RETAIN_IMAGES:]:
        old_image.unlink(missing_ok=True)
    return metadata


def capture_game_window(
    snapshot_path: str | None,
    output_dir: str,
    min_interval_seconds: float = 5.0,
) -> dict[str, Any]:
    """Keep the shared sidecar honest even when an unexpected capture error occurs."""
    descriptor: int | None = None
    try:
        directory = Path(output_dir)
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(directory, 0o700)
        descriptor = os.open(directory / ".capture.lock", os.O_CREAT | os.O_RDWR, 0o600)
        os.fchmod(descriptor, 0o600)
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        return _capture_game_window(snapshot_path, output_dir, min_interval_seconds)
    except Exception as exc:
        return publish_unavailable(output_dir, f"unexpected WoW capture failure: {type(exc).__name__}")
    finally:
        if descriptor is not None:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            finally:
                os.close(descriptor)


def capture_for_request(
    text: str,
    snapshot_path: str | None,
    output_dir: str,
    min_interval_seconds: float = 10.0,
) -> dict[str, Any]:
    """Capture only when the user explicitly asks the agents to inspect the game image."""
    if not is_visual_request(text):
        return {"available": False, "requested": False, "path": None,
                "reason": "No explicit visual request; background capture is disabled."}
    result = dict(capture_game_window(snapshot_path, output_dir, min_interval_seconds))
    result["requested"] = True
    return result


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    print(json.dumps(capture_game_window(args.snapshot, args.output_dir), ensure_ascii=False))
