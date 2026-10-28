import importlib.util
import json
import pathlib
import subprocess
import unittest
from unittest import mock


CLIENT_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "wow-agent" / "action_client.py"


def load_client():
    if not CLIENT_PATH.is_file():
        return None
    spec = importlib.util.spec_from_file_location("wow_agent_action_client", CLIENT_PATH)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CLIENT = load_client()


class ActionReplyParsingTests(unittest.TestCase):
    def test_extracts_one_allowed_action_and_keeps_only_player_facing_reply(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        reply, action, error = CLIENT.parse_action_response(
            'Піду за тобою.\nACTION: {"actor":"Codex","type":"follow"}',
            bot_name="Codex",
        )
        self.assertEqual(reply, "Піду за тобою.")
        self.assertEqual(action, {"actor": "Codex", "type": "follow"})
        self.assertIsNone(error)

    def test_rejects_multiple_action_blocks(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        reply, action, error = CLIENT.parse_action_response(
            'Виконую.\nACTION: {"actor":"Codex","type":"follow"}\n'
            'ACTION: {"actor":"Codex","type":"stay"}',
            bot_name="Codex",
        )
        self.assertEqual(reply, "Виконую.")
        self.assertIsNone(action)
        self.assertEqual(error, "multiple_actions")

    def test_rejects_cross_bot_unknown_action_and_extra_fields(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        invalid = (
            ('ACTION: {"actor":"Claude","type":"follow"}', "actor_mismatch"),
            ('ACTION: {"actor":"Codex","type":"run_shell"}', "unsupported_action"),
            ('ACTION: {"actor":"Codex","type":"follow","command":"anything"}', "extra_fields"),
        )
        for text, expected_error in invalid:
            with self.subTest(text=text):
                _reply, action, error = CLIENT.parse_action_response(text, bot_name="Codex")
                self.assertIsNone(action)
                self.assertEqual(error, expected_error)

    def test_unaddressed_group_reply_cannot_dispatch_even_if_model_emits_action(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        reply, action, error = CLIENT.parse_action_response(
            'ACTION: {"actor":"Codex","type":"fish_here"}',
            bot_name="Codex",
            allow_action=False,
        )
        self.assertEqual(reply, "")
        self.assertIsNone(action)
        self.assertIsNone(error)


class ActionDispatchTests(unittest.TestCase):
    def test_dispatch_passes_only_allowlisted_fields_to_fixed_console_command(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        response = {
            "action_id": "a" * 32,
            "status": "accepted",
            "reason_code": "started",
            "actor": "Codex",
            "type": "follow",
        }
        runner = mock.Mock(return_value=subprocess.CompletedProcess(
            args=[], returncode=0, stdout="ACTION_RESULT: " + json.dumps(response), stderr=""))
        result = CLIENT.dispatch_action(
            {"actor": "Codex", "type": "follow"},
            sender="Terisfal",
            group_id="1",
            authorized_players={"Terisfal", "Lartician"},
            server_dir="/srv/wow",
            runner=runner,
            action_id="a" * 32,
        )
        self.assertEqual(result["status"], "accepted")
        self.assertEqual(result["action_id"], "a" * 32)
        command = runner.call_args.args[0]
        self.assertEqual(command[0], "/srv/wow/claude-cmd.sh")
        self.assertEqual(command[1], "claude action Codex Terisfal 1 " + "a" * 32 + " follow")
        self.assertNotIn("shell", runner.call_args.kwargs)

    def test_rejects_untrusted_sender_or_injected_target_before_console_call(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        runner = mock.Mock()
        result = CLIENT.dispatch_action(
            {"actor": "Codex", "type": "follow"},
            sender="OtherPlayer",
            group_id="1",
            authorized_players={"Terisfal", "Lartician"},
            server_dir="/srv/wow",
            runner=runner,
            action_id="b" * 32,
        )
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(result["reason_code"], "unauthorized_sender")
        runner.assert_not_called()

        with self.assertRaises(ValueError):
            CLIENT.build_action_command(
                {"actor": "Codex", "type": "follow"},
                sender="Terisfal; .server shutdown",
                group_id="1",
                action_id="b" * 32,
            )

    def test_rejects_mismatched_or_unstructured_server_result(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        self.assertIsNone(CLIENT.parse_action_result("ACTION_RESULT: not-json", "c" * 32))
        self.assertIsNone(CLIENT.parse_action_result(
            'ACTION_RESULT: {"action_id":"' + "d" * 32 + '","status":"accepted"}',
            "c" * 32,
        ))

    def test_does_not_accept_acknowledgement_for_a_different_actor_or_action(self):
        self.assertIsNotNone(CLIENT, "action_client.py must implement the typed chat protocol")
        response = {
            "action_id": "e" * 32,
            "status": "accepted",
            "reason_code": "started",
            "actor": "Claude",
            "type": "stay",
        }
        runner = mock.Mock(return_value=subprocess.CompletedProcess(
            args=[], returncode=0, stdout="ACTION_RESULT: " + json.dumps(response), stderr=""))
        result = CLIENT.dispatch_action(
            {"actor": "Codex", "type": "follow"},
            sender="Terisfal",
            group_id="1",
            authorized_players={"Terisfal", "Lartician"},
            server_dir="/srv/wow",
            runner=runner,
            action_id="e" * 32,
        )
        self.assertEqual(result["status"], "unknown")
        self.assertEqual(result["reason_code"], "mismatched_server_ack")


if __name__ == "__main__":
    unittest.main()
