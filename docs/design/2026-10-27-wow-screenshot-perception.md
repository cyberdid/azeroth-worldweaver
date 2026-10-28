# WoW Window Screenshot Perception

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Give both agents a current image of the WoW client when visual context helps answer a user request or evaluate an action.

## Design

- Locate the actual WoW client window through X11 metadata and verify its window title/class and owning process command line. Capture that window ID only; never capture the desktop and crop afterward.
- Require the window to be mapped and non-minimized. If it is missing or ambiguous, return an explicit unavailable result.
- Convert the selected window to PNG in a private rotating screenshot directory; include capture time, window ID, group, session, and matching world snapshot sequence in a sidecar record.
- Before reusing a rate-limited image, revalidate the visible WoW window and require the same window ID, process, dimensions, group, and server session. Preserve both capture and prompt snapshot sequences; a sequence difference alone is not staleness because image and world snapshots update on independent cadences. Mark it stale when its age exceeds the limit or its session/group/state is missing or mismatched.
- On every failed/ambiguous/unavailable capture, atomically publish `available: false` to `screenshots/latest.json`; if that status cannot be written, remove the old pointer so neither agent mistakes an earlier frame for current.
- Capture only when an authenticated user's message explicitly asks for visual inspection. Keep the one-second structured world telemetry running independently. Do not capture on ordinary chat, companion speech, or telemetry; do not attach an earlier frame when visual inspection was not requested.
- Both adapters may process the same party message concurrently. Serialize capture through a private inter-process lock and reuse the resulting verified frame for up to ten seconds so both agents inspect the same image without duplicate X11/PNG work.
- Codex CLI supports `codex exec -i <file>`; attach the capture to the prompt. Claude receives the stable `screenshots/latest.json` path inside the compact observation attached to that same real message and can inspect the referenced PNG through its local image-reading capability. Do not emit a separate `WORLD` chat event.
- Do not describe pixels as current game state when the image is stale or belongs to another session/group. Report the capture sequence delta and age when the frame is older or newer than the snapshot under discussion.

## Known environment state

`xwd`, `xprop`, and Pillow are installed. No WoW client window was present during design, so actual game-window capture and both model input paths still require live validation.

## Acceptance

- Captured PNG dimensions and visible contents belong to the verified WoW client window only; XWD decoding does not rely on Pillow having an XWD plugin.
- Both agents can inspect the same retained image and cite its timestamp, capture sequence, prompt sequence, and delta; freshness is decided from age and stream identity rather than exact sequence equality.
- Hidden, minimized, absent, or ambiguous windows yield no image, an explicit reason, and an unavailable shared sidecar.
- Capture rate and storage remain bounded.
