import json
import pathlib
import runpy
import tempfile
import time
import unittest


WORLD_STATE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "world_state.py"
WORLD_STATE = runpy.run_path(str(WORLD_STATE_PATH), run_name="world_state_under_test")


class WorldEventCursorTests(unittest.TestCase):
    def make_stream(self, directory, event_count=12):
        root = pathlib.Path(directory)
        session_id = "test-session"
        group_id = "1"
        snapshot = {
            "schema_version": 1,
            "session_id": session_id,
            "group_id": group_id,
            "captured_at_unix_ms": int(time.time() * 1000),
            "snapshot_seq": event_count,
            "event_seq": event_count,
            "dropped_events_total": 0,
            "group": [],
            "nearby": [],
        }
        (root / "world-state-1.json").write_text(json.dumps(snapshot), encoding="utf-8")
        events = [
            {
                "schema_version": 1,
                "session_id": session_id,
                "group_id": group_id,
                "event_seq": sequence,
                "occurred_at": sequence,
                "type": "spell_cast",
                "actor": "Codex",
            }
            for sequence in range(1, event_count + 1)
        ]
        (root / "world-events-1.jsonl").write_text(
            "".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        return session_id, group_id

    def read_context(self, directory, cursor):
        rendered, next_cursor = WORLD_STATE["build_agent_context"](
            str(directory), cursor, event_limit=5)
        return json.loads(rendered.split("\n", 1)[1]), next_cursor

    def test_new_cursor_starts_at_latest_events_and_marks_older_history_omitted(self):
        with tempfile.TemporaryDirectory() as directory:
            self.make_stream(directory)
            context, next_cursor = self.read_context(directory, {})

        sequences = [event["event_seq"] for event in context["recent_events"]]
        self.assertEqual(sequences, [8, 9, 10, 11, 12])
        self.assertTrue(context.get("event_history_truncated"))
        self.assertFalse(context["event_backlog"])
        self.assertFalse(context["event_cursor_gap"])
        self.assertEqual(next_cursor["event_seq"], 12)

    def test_lagging_cursor_jumps_to_latest_events_instead_of_replaying_old_backlog(self):
        with tempfile.TemporaryDirectory() as directory:
            session_id, group_id = self.make_stream(directory)
            context, next_cursor = self.read_context(directory, {
                "session_id": session_id,
                "group_id": group_id,
                "event_seq": 2,
            })

        sequences = [event["event_seq"] for event in context["recent_events"]]
        self.assertEqual(sequences, [8, 9, 10, 11, 12])
        self.assertTrue(context.get("event_history_truncated"))
        self.assertFalse(context["event_backlog"])
        self.assertEqual(next_cursor["event_seq"], 12)


if __name__ == "__main__":
    unittest.main()
