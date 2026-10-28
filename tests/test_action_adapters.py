import pathlib
import runpy
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
CODEX = runpy.run_path(str(ROOT / "tools" / "wow-agent" / "codex-bridge.py"),
                      run_name="codex_bridge_action_test")
CLAUDE = runpy.run_path(str(ROOT / "tools" / "wow-agent" / "claude-bridge.py"),
                       run_name="claude_bridge_action_test")


class ActionAdapterTests(unittest.TestCase):
    def test_codex_dispatches_one_action_only_for_its_directly_addressed_user_request(self):
        self.assertIn("ACTION_CLIENT", CODEX)
        bridge = CODEX
        namespace = bridge["handle"].__globals__
        namespace["USER_CHARACTERS"] = {"Terisfal", "Lartician"}
        history = []
        cursor = {"group_id": "1", "session_id": "live", "snapshot_seq": 20,
                  "event_seq": 4, "snapshot_available": True, "snapshot_stale": False}
        accepted = {"action_id": "a" * 32, "status": "accepted", "reason_code": "started",
                    "actor": "Codex", "type": "follow"}
        with (mock.patch.object(bridge["_WORLD_STATE"], "build_agent_context", return_value=("world", cursor)),
              mock.patch.object(bridge["_WORLD_STATE"], "load_cursor", return_value={}),
              mock.patch.object(bridge["_WORLD_STATE"], "save_cursor"),
              mock.patch.object(bridge["ACTION_CLIENT"], "dispatch_action", return_value=accepted) as dispatch,
              mock.patch.dict(namespace, capture_latest_visual=lambda _text: {"available": False, "requested": False}),
              mock.patch.dict(namespace, ask_codex=lambda *_args: 'Піду за тобою.\nACTION: {"actor":"Codex","type":"follow"}'),
              mock.patch.object(bridge["subprocess"], "run") as run,
              mock.patch.dict(namespace, save_history=lambda _history: None, log=lambda _message: None)):
            bridge["handle"]("gpt-6-luna", "xhigh", "PARTY", "Terisfal", "Codex, follow me", history)

        dispatch.assert_called_once_with(
            {"actor": "Codex", "type": "follow"},
            sender="Terisfal",
            group_id="1",
            authorized_players={"Terisfal", "Lartician"},
            server_dir=namespace["DIR"],
        )
        self.assertEqual(run.call_count, 1)
        self.assertIn("прийнято сервером", run.call_args.args[0][2])

    def test_claude_does_not_dispatch_an_action_from_unaddressed_party_chat(self):
        self.assertIn("ACTION_CLIENT", CLAUDE)
        bridge = CLAUDE
        namespace = bridge["handle"].__globals__
        namespace["AUTHORIZED"] = {"terisfal", "lartician"}
        history = []
        accepted = {"action_id": "b" * 32, "status": "accepted", "reason_code": "started",
                    "actor": "Claude", "type": "fish_here"}
        with (mock.patch.object(bridge["WORLD_STATE"], "save_cursor"),
              mock.patch.object(bridge["ACTION_CLIENT"], "dispatch_action", return_value=accepted) as dispatch,
              mock.patch.dict(namespace, read_context=lambda: ("world", {
                  "group_id": "1", "session_id": "live", "snapshot_seq": 30,
                  "snapshot_available": True, "snapshot_stale": False,
              })),
              mock.patch.dict(namespace, visual_context=lambda *_args: '{"requested":false,"available":false}'),
              mock.patch.dict(namespace, ask_claude=lambda *_args: 'ACTION: {"actor":"Claude","type":"fish_here"}'),
              mock.patch.dict(namespace, save_history=lambda _history: None, log=lambda _message: None)):
            bridge["handle"]("PARTY", "Terisfal", "Давайте порибалимо", history, "xhigh")

        dispatch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
