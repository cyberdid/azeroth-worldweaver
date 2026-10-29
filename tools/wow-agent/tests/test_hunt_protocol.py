"""Regression checks for collection intent, large context and natural replies."""
import importlib.util
import json
import sys
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


CLIENT = load('hunt_test_client', 'action_client.py')
CLAUDE = load('hunt_test_claude', 'claude-bridge.py')


class HuntProtocol(unittest.TestCase):
    def test_collect_without_trade(self):
        action = dict(actor='Codex', type='hunt_start', creature_entries=[1024, 1028],
                      map_id=0, zone_id=11, kill_count=20, collect_loot=True, deliver_loot=False)
        _, parsed, error = CLIENT.parse_action_response('ACTION: ' + json.dumps(action), bot_name='Codex')
        self.assertIsNone(error)
        command = CLIENT.build_action_command(parsed, sender='Terisfal', group_id='1', action_id='a' * 32)
        self.assertTrue(command.endswith(':20:0:1'))

    def test_delivery_still_collects(self):
        action = dict(creature_entries=[1024], map_id=0, zone_id=11, kill_count=2,
                      collect_loot=False, deliver_loot=True)
        self.assertTrue(CLIENT._encode_hunt_request(action).endswith(':2:1:1'))

    def test_claude_large_prompt_uses_stdin(self):
        prompt = 'x' * 200000
        with patch.object(CLAUDE.shutil, 'which', return_value='/usr/bin/claude'), \
             patch.object(CLAUDE.subprocess, 'run', return_value=SimpleNamespace(
                 returncode=0, stdout='ok', stderr='')) as run:
            self.assertEqual(CLAUDE.ask_claude(prompt, 'xhigh'), 'ok')
            self.assertFalse(prompt in run.call_args.args[0])
            self.assertEqual(run.call_args.kwargs.get('input'), prompt)

    def test_group_assignment_preserves_natural_reply(self):
        draft = 'Добре, піду вздовж берега й збиратиму здобич у сумки.'
        action = dict(actor='Claude', type='hunt_start', creature_entries=[1024],
                      map_id=0, zone_id=11, kill_count=20, collect_loot=True, deliver_loot=False)
        with ExitStack() as stack:
            stack.enter_context(patch.object(CLAUDE, 'AUTHORIZED', {'terisfal'}))
            stack.enter_context(patch.object(CLAUDE, 'read_context', return_value=('world', {'group_id': '1'})))
            stack.enter_context(patch.object(CLAUDE, 'visual_context', return_value='{}'))
            stack.enter_context(patch.object(CLAUDE.SESSION_MEMORY, 'build_relevant_context', return_value=''))
            stack.enter_context(patch.object(CLAUDE, 'ask_claude', return_value=draft + '\nACTION: ' + json.dumps(action)))
            dispatch = stack.enter_context(patch.object(CLAUDE.ACTION_CLIENT, 'dispatch_action', return_value={
                'status': 'accepted', 'reason_code': 'hunt_started'}))
            rewrite = stack.enter_context(patch.object(CLAUDE, 'synthesize_action_reply'))
            speak = stack.enter_context(patch.object(CLAUDE, 'speak', return_value=True))
            stack.enter_context(patch.object(CLAUDE, 'save_history'))
            stack.enter_context(patch.object(CLAUDE.WORLD_STATE, 'save_cursor'))
            stack.enter_context(patch.object(CLAUDE, 'log'))
            CLAUDE.handle('PARTY', 'Terisfal', 'завдання для всіх — полюйте по 20 мурлоків', [], 'xhigh')
            dispatch.assert_called_once()
            rewrite.assert_not_called()
            self.assertEqual(speak.call_args.args[0], draft)


if __name__ == '__main__':
    unittest.main()
