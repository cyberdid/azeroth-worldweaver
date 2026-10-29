#!/bin/bash
# Speak as a bridge bot, splitting long text into WoW-sized chunks
# (110 chars: Cyrillic is 2 bytes per char and the chat limit is 255 bytes).
# Usage: bot-say.sh <Bot> 'text' [party|whisper|say] [whisper_target]
set -euo pipefail
BOT=$1
MODE=${3:-party}
DIR=${WOW_AGENT_SERVER_DIR:-$(cd -- "$(dirname -- "$0")" && pwd)}
LOG="$DIR/$(printf '%s' "$BOT" | tr '[:upper:]' '[:lower:]')-chat.log"
# whisper target: 4th arg, else whoever wrote to this bot last
TARGET=${4:-$(tail -n 1 "$LOG" 2>/dev/null | cut -d'|' -f3)}
TARGET=${TARGET:-}
python3 -I - "$2" <<'EOF' | while IFS= read -r chunk; do
import sys, textwrap
for line in sys.argv[1].splitlines() or ['']:
    for part in textwrap.wrap(line, 110) or ['']:
        if part.strip():
            print(part)
EOF
    case "$MODE" in
        whisper) out=$("$DIR/claude-cmd.sh" "claude whisper $BOT $TARGET $chunk") ;;
        say)     out=$("$DIR/claude-cmd.sh" "claude say $BOT $chunk") ;;
        *)       out=$("$DIR/claude-cmd.sh" "claude party $BOT $chunk")
                 case "$out" in FAULT*) out=$("$DIR/claude-cmd.sh" "claude whisper $BOT $TARGET $chunk") ;; esac ;;
    esac
    case "$out" in FAULT*) echo "$out" >&2 ;; esac
    sleep 0.4
done
# let the other bridge bots see what this bot said (context only, they do not reply to BOTSAY lines)
FLAT=$(printf '%s' "$2" | tr '\n\r' '  ')
for other in claude codex; do
    [ "$other" = "$(printf '%s' "$BOT" | tr '[:upper:]' '[:lower:]')" ] && continue
    [ -e "$DIR/$other-chat.log" ] && printf '%s|BOTSAY|%s|%s\n' "$(date +%s)" "$BOT" "$FLAT" >> "$DIR/$other-chat.log"
done
