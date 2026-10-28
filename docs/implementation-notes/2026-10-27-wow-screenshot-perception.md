# WoW Window Screenshot Perception Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Capture only the verified game window.

**Goal:** Attach a current image of the WoW client to Codex prompts and make the same image available to Claude.

**Architecture:** A Python X11 helper verifies the WoW window title/class and owning process, captures its exact window ID, and writes a bounded PNG with group/session/snapshot metadata. Codex CLI receives the PNG through `-i`; Claude gets the stable metadata path inside the compact observation attached to the same real user message. The current adapter refreshes at a five-second cadence.

**Tech Stack:** Python 3, X11 (`xprop`/`xwd` or Xlib), Pillow, Codex CLI image input, Claude Code local image reading.

**Spec:** `../design/2026-10-27-wow-screenshot-perception.md`

## Global Constraints

- Never capture the desktop or capture another application as a fallback.
- Reject absent, minimized, ambiguous, or process-unverified windows.
- Keep captures rate-limited and retain only a bounded recent set.
- Do not restart worldserver for screenshot-only changes.

---

### Task 1: Implement verified window capture

**Files:**
- Create: `tools/wow-agent/wow_window_capture.py`
- Create: `tools/wow-agent/README.md`

**Interface:**
- `capture_game_window(snapshot_path, output_dir, min_interval_seconds=5) -> dict` returns availability, PNG path, capture time, window/process identity, group/session/snapshot identity, freshness, and a failure reason.

- [x] Enumerate mapped top-level X11 windows; retain only those whose title/class identifies World of Warcraft and whose `_NET_WM_PID` command line identifies the WoW Wine client.
- [x] Return an unavailable result when zero or multiple candidates remain, the window is hidden/minimized/occluded, or process identity cannot be verified; atomically invalidate the shared sidecar on failure.
- [x] Capture pixels from the selected X11 window ID only; do not call root-window capture APIs. Use `xwd -id <window-id>` and decode TrueColor/DirectColor channels by their visual masks.
- [x] Convert to RGB PNG, associate group/session/snapshot identity, revalidate the window after capture, and atomically write a JSON sidecar.
- [x] Enforce minimum capture interval, image size limit, restrictive file permissions, and bounded retention.

### Task 2: Deliver the image to both agents

**Files:**
- Modify: `tools/wow-agent/codex-bridge.py`
- Modify: `tools/wow-agent/world_state.py`
- Modify: `tools/wow-agent/README.md`

- [x] Refresh a shared screenshot on a bounded five-second cadence while the Codex bridge runs and before a user request; include the capture result/reason in Codex's next prompt and leave the shared metadata path available to Claude.
- [x] Pass the image path as a `-i <path>` argument to `codex exec` without shell interpolation; `codex exec --help` on this machine confirms the supported image option.
- [x] Put the stable `screenshots/latest.json` path in the compact observation attached to Claude's incoming message; keep Codex's capture metadata in the same prompt and emit no standalone chat record.
- [x] Add a Claude CLI chat adapter that reads the same state/event cursor and passes the sidecar to a read-only image-capable prompt; constrain shell access to the typed query client and game directives to an explicit allowlist.
- [x] Keep the refresh on the shared five-second file-only cadence; action-milestone capture remains a later extension because the action dispatcher is not part of this phase.
- [x] Include capture and prompt snapshot sequences and their delta; do not mark a recent frame stale solely because the one-second world snapshot advanced after image capture.

### Task 3: Live visual validation

- [x] Ask the user to open the WoW client when ready; the user opened it for a live capture.
- [x] Confirm the selected X11 ID, title/class, PID, and command line belong to the WoW client.
- [x] Capture once and inspect the PNG dimensions and content; confirm no desktop or neighboring window is present.
- [ ] Ask Codex and Claude independently to describe the same visible scene and compare their sequence/time references.
- [ ] Minimize/close the client and confirm the helper reports unavailable rather than capturing another window.
