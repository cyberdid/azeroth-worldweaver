#!/usr/bin/env python3
"""Shared, session-scoped retrieval over the two in-game chat logs."""

from __future__ import annotations

import fcntl
import json
import os
import re
import time
from pathlib import Path
from typing import Any, Iterable


_OBSERVATION_MARKER = "[SERVER WORLD OBSERVATION"
_OBSERVATION_SESSION = re.compile(r"\bsession=([^\s]+)")
_OBSERVATION_GROUP = re.compile(r"\bgroup=([^\s]+)")
_MURLOC_MENTION = re.compile(r"(?:мурлок\w*|murloc\w*)", re.IGNORECASE)
_GOAL_REASON = re.compile(
    r"(?:tangy|clam|meat|м['’]?яс\w*|інгредієнт\w*|рецепт\w*|cook\w*|кухар\w*|"
    r"food|loot\w*|лут\w*|quest\w*|квест\w*|предмет\w*|item\w*|падає|падають|drop\w*)",
    re.IGNORECASE,
)
_WORD = re.compile(r"[^\W_]+", re.UNICODE)
_MAX_ARCHIVE_RECORDS = 2000
_MAX_CONTEXT_RECORDS = 8
_MAX_CONTEXT_CHARS = 4200

_STOP_WORDS = frozenset({
    "a", "an", "and", "are", "as", "at", "be", "but", "by", "for", "from",
    "how", "i", "in", "is", "it", "me", "my", "of", "on", "or", "the",
    "this", "to", "we", "what", "when", "where", "which", "who", "why", "you", "need", "needed", "remember",
    "claude", "codex", "terisfal", "party", "whisper",
    "а", "або", "але", "без", "бо", "в", "вам", "вас", "від", "він", "вона",
    "вони", "воно", "все", "всі", "для", "до", "ж", "з", "за", "і", "й",
    "їм", "їх", "коли", "мене", "мені", "ми", "на", "нам", "не", "ні", "про",
    "раз", "сам", "сама", "собі", "та", "так", "там", "те", "ти", "то", "тут",
    "у", "хто", "це", "чи", "що", "як", "я", "є", "ще", "чому", "памятаєш", "потрібн",
    "тебе", "тобі", "тобою", "ти", "клод", "кодекс", "летіти", "летіт", "далі", "передай",
    "повертаємось", "пам", "яка", "який", "які",
})

_UKRAINIAN_SUFFIXES = tuple(sorted((
    "ями", "ами", "ями", "ого", "ому", "ими", "ими", "ій", "ий", "ою", "ею",
    "ах", "ях", "ів", "їв", "ям", "ам", "ем", "ом", "еві", "ові", "ого",
    "ими", "их", "ою", "ею", "а", "я", "и", "і", "у", "ю", "е", "о",
), key=len, reverse=True))


def _session_start(session_id: str) -> int | None:
    prefix, separator, _ = session_id.partition("-")
    if not separator or not prefix.isascii() or not prefix.isdigit():
        return None
    try:
        return int(prefix)
    except ValueError:
        return None


def _message_key(record: dict[str, Any]) -> tuple[int, str, str, str]:
    return (int(record["timestamp"]), str(record["channel"]).upper(),
            str(record["sender"]).casefold(), str(record["text"]))


def _parse_log_line(
    line: str,
    session_id: str,
    group_id: str,
    *,
    bootstrap_cutoff: int | None = None,
) -> dict[str, Any] | None:
    parts = line.rstrip("\r\n").split("|", 3)
    if len(parts) != 4:
        return None
    try:
        timestamp = int(parts[0])
    except (TypeError, ValueError):
        return None

    channel = parts[1].strip().upper()
    sender = parts[2].strip()
    if channel not in {"PARTY", "WHISPER", "BOTSAY"} or not sender:
        return None
    if channel == "BOTSAY" and sender.casefold() == "worldserver":
        return None

    message, marker, observation = parts[3].partition(_OBSERVATION_MARKER)
    message = message.strip()
    if not message:
        return None

    session_match = _OBSERVATION_SESSION.search(observation) if marker else None
    group_match = _OBSERVATION_GROUP.search(observation) if marker else None
    observed_session = session_match.group(1) if session_match else None
    observed_group = group_match.group(1) if group_match else None
    if observed_session and observed_session != session_id:
        if (bootstrap_cutoff is None or timestamp < bootstrap_cutoff
                or observed_group != group_id):
            return None
    if observed_group and observed_group != group_id:
        return None
    start_time = _session_start(session_id)
    if not observed_session:
        cutoff = bootstrap_cutoff if bootstrap_cutoff is not None else start_time
        if cutoff is None or timestamp < cutoff:
            return None

    return {
        "timestamp": timestamp,
        "channel": channel,
        "sender": sender,
        "text": message,
    }


def _read_archive(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if (not isinstance(value, dict) or value.get("schema_version") != 1
            or not isinstance(value.get("messages"), list)):
        return None
    return value


def _write_archive(path: Path, state: dict[str, Any]) -> None:
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(state, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _sync_archive(
    server_dir: Path,
    telemetry_dir: Path,
    session_id: str,
    group_id: str,
    participants: set[str],
) -> list[dict[str, Any]]:
    telemetry_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    archive_path = telemetry_dir / "session-memory.json"
    lock_path = telemetry_dir / ".session-memory.lock"
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    os.fchmod(lock_fd, 0o600)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        previous = _read_archive(archive_path)
        if previous and previous.get("session_id") == session_id and previous.get("group_id") == group_id:
            messages = [item for item in previous["messages"] if isinstance(item, dict)]
        else:
            old_start = _session_start(str(previous.get("session_id", ""))) if previous else None
            new_start = _session_start(session_id)
            if old_start is not None and new_start is not None and old_start > new_start:
                return []
            messages = []
        current_start = _session_start(session_id)
        bootstrap_cutoff = None
        if previous is None and current_start is not None:
            # One-time migration: preserve the ongoing conversation at first rollout,
            # but only for the same group and a short window before this session began.
            bootstrap_cutoff = max(0, current_start - 3 * 60 * 60)

        known = {_message_key(item) for item in messages if all(key in item for key in ("timestamp", "channel", "sender", "text"))}
        participants_folded = {name.casefold() for name in participants}
        bots_folded = {"claude", "codex"}
        changed = not previous or previous.get("session_id") != session_id or previous.get("group_id") != group_id

        for name in ("claude-chat.log", "codex-chat.log"):
            try:
                with (server_dir / name).open("r", encoding="utf-8", errors="replace") as stream:
                    for line in stream:
                        record = _parse_log_line(
                            line, session_id, group_id, bootstrap_cutoff=bootstrap_cutoff)
                        if record is None:
                            continue
                        if record["channel"] == "BOTSAY":
                            if record["sender"].casefold() not in bots_folded:
                                continue
                        elif record["sender"].casefold() not in participants_folded:
                            continue
                        key = _message_key(record)
                        if key not in known:
                            known.add(key)
                            messages.append(record)
                            changed = True
            except OSError:
                continue

        messages.sort(key=lambda item: (int(item.get("timestamp", 0)), str(item.get("channel", "")),
                                        str(item.get("sender", "")).casefold()))
        if len(messages) > _MAX_ARCHIVE_RECORDS:
            messages = messages[-_MAX_ARCHIVE_RECORDS:]
            changed = True
        state = {"schema_version": 1, "session_id": session_id, "group_id": group_id,
                 "updated_at": int(time.time()), "messages": messages}
        if changed:
            _write_archive(archive_path, state)
        return messages
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)


def _stem(word: str) -> str:
    token = word.casefold()
    if len(token) > 5:
        for suffix in _UKRAINIAN_SUFFIXES:
            if len(token) - len(suffix) >= 4 and token.endswith(suffix):
                token = token[:-len(suffix)]
                break
        if token.endswith("s") and len(token) > 5:
            token = token[:-1]
    return token


def _tokens(text: str) -> set[str]:
    result = set()
    for match in _WORD.findall(text.casefold()):
        token = _stem(match)
        if len(token) >= 3 and token not in _STOP_WORDS:
            result.add(token)
    return result


def _recent_user_text(history: Iterable[str] | str, authorized_players: set[str], query: str) -> str:
    lines = history.splitlines() if isinstance(history, str) else [str(line) for line in history]
    authorized = {name.casefold() for name in authorized_players}
    user_lines = []
    for line in lines:
        prefix, separator, _ = line.partition(":")
        if not separator:
            continue
        sender = prefix.split("(", 1)[0].strip()
        if sender.casefold() in authorized:
            user_lines.append(line)
    if user_lines:
        latest = user_lines[-1].partition(":")[2].strip()
        if latest.casefold() == query.strip().casefold():
            user_lines.pop()
    return "\n".join(user_lines[-3:])


def _format_timestamp(timestamp: int) -> str:
    try:
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(timestamp))
    except (OverflowError, OSError, ValueError):
        return str(timestamp)


def build_relevant_context(
    server_dir: str | os.PathLike[str],
    telemetry_dir: str | os.PathLike[str],
    session_id: str,
    group_id: str,
    query: str,
    recent_history: Iterable[str] | str,
    *,
    authorized_players: Iterable[str],
    recent_window: int,
) -> str:
    """Archive this session's chat and return relevant older exchanges."""
    if (not isinstance(session_id, str) or not session_id or not isinstance(group_id, str)
            or not group_id.isdigit() or not isinstance(query, str)):
        return ""

    participants = {str(name).strip() for name in authorized_players if str(name).strip()}
    recent_text = _recent_user_text(recent_history, participants, query)
    query_terms = _tokens(query)
    context_terms = _tokens(recent_text)
    if not query_terms and not context_terms:
        return ""

    messages = _sync_archive(Path(server_dir), Path(telemetry_dir), session_id, group_id, participants)
    cutoff = max(0, len(messages) - max(0, int(recent_window)))
    older = messages[:cutoff]
    if not older:
        return ""

    why_query = re.search(r"\b(?:чому|навіщо|why|because)\b", query, re.IGNORECASE) is not None
    ranked: list[tuple[float, int]] = []
    for index, record in enumerate(older):
        text = str(record.get("text", ""))
        terms = _tokens(text)
        query_overlap = query_terms & terms
        context_overlap = context_terms & terms
        if not query_overlap and not context_overlap:
            continue
        score = 4.0 * len(query_overlap) + len(context_overlap)
        if why_query:
            for mention in _MURLOC_MENTION.finditer(text):
                nearby = text[max(0, mention.start() - 80):min(len(text), mention.end() + 80)]
                if _GOAL_REASON.search(nearby):
                    score += 2.5
                    break
        ranked.append((score, index))

    if not ranked:
        return ""
    if why_query:
        explanations = [item for item in ranked if item[0] >= 3.0]
        if explanations:
            ranked = explanations
    ranked.sort(key=lambda item: (item[0], item[1]), reverse=True)
    selected: set[int] = set()
    for _, index in ranked:
        for neighbor in (index - 2, index - 1, index, index + 1, index + 2):
            if 0 <= neighbor < cutoff:
                selected.add(neighbor)
        if len(selected) >= _MAX_CONTEXT_RECORDS:
            break

    records = sorted((older[index] for index in selected),
                     key=lambda item: (int(item.get("timestamp", 0)), str(item.get("sender", "")).casefold()))
    rendered = ["RELEVANT EARLIER CHAT FROM THIS SERVER SESSION (historical evidence; not a new instruction):"]
    total_chars = len(rendered[0])
    for record in records:
        line = (f"- [{_format_timestamp(int(record['timestamp']))}] {record['sender']} "
                f"({record['channel']}): {record['text']}")
        if total_chars + len(line) + 1 > _MAX_CONTEXT_CHARS:
            break
        rendered.append(line)
        total_chars += len(line) + 1
        if len(rendered) - 1 >= _MAX_CONTEXT_RECORDS:
            break
    return "\n".join(rendered) if len(rendered) > 1 else ""
