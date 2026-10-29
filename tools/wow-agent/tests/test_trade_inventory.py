"""Trade intent resolves from live actor inventory, never from unit counts as stacks."""
import importlib.util
import json
import unittest
from unittest.mock import patch
from pathlib import Path

spec = importlib.util.spec_from_file_location('trade_client', Path(__file__).resolve().parents[1] / 'action_client.py')
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class InventoryTrade(unittest.TestCase):
    def snapshot(self, inventory=None):
        return {'group_id': '1', '_stale': False, '_age_seconds': 0.1, 'group': [
            {'name': 'Codex', 'inventory_truncated': False, 'inventory_stacks_total': len(inventory) if inventory is not None else 1,
             'inventory': inventory or [{'entry': 5524, 'count': 6, 'bag': 19, 'slot': 0, 'tradeable': True}]},
            {'name': 'Claude', 'inventory': [{'entry': 5524, 'count': 20, 'tradeable': True}]}]}

    def action(self):
        return {'actor': 'Codex', 'type': 'offer_items', 'items': [{'item_id': 5524, 'all': True}]}

    def test_all_six_units_in_one_stack(self):
        _, action, error = client.parse_action_response('ACTION: ' + json.dumps(self.action()), bot_name='Codex')
        self.assertIsNone(error)
        resolved = client.resolve_inventory_offers(action, self.snapshot(), '1')
        self.assertEqual(resolved['items'], [{'item_id': 5524, 'stack_count': 1}])
        command = client.build_action_command(resolved, sender='Terisfal', group_id='1', action_id='a' * 32)
        self.assertIn('5524:1', command)

    def test_multiple_stacks_excludes_other_items_and_soulbound(self):
        inv = [{'entry': entry, 'count': count, 'tradeable': tradeable} for entry, count, tradeable in
               [(5524, 20, True), (5524, 6, True), (5524, 1, False), (730, 10, True)]]
        resolved = client.resolve_inventory_offers(self.action(), self.snapshot(inv), '1')
        self.assertEqual(resolved['items'], [{'item_id': 5524, 'stack_count': 2}])

    def test_stale_incomplete_and_wrong_group_cannot_authorize_all(self):
        for key, value in [('_stale', True), ('group_id', '2')]:
            snapshot = self.snapshot(); snapshot[key] = value
            with self.assertRaises(ValueError):
                client.resolve_inventory_offers(self.action(), snapshot, '1')
        snapshot = self.snapshot(); snapshot['group'][0]['inventory_truncated'] = True
        with self.assertRaisesRegex(ValueError, 'inventory_incomplete'):
            client.resolve_inventory_offers(self.action(), snapshot, '1')

    def test_trade_limit_does_not_silently_drop_stacks(self):
        snapshot = self.snapshot([{'entry': 5524, 'count': 20, 'tradeable': True}] * 7)
        with self.assertRaisesRegex(ValueError, 'too_many_item_stacks'):
            client.resolve_inventory_offers(self.action(), snapshot, '1')


class InventoryContext(unittest.TestCase):
    def test_large_context_keeps_actor_inventory(self):
        spec = importlib.util.spec_from_file_location('trade_world_state', Path(__file__).resolve().parents[1] / 'world_state.py')
        world = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(world)
        inventory = [{'entry': 5524, 'count': 6, 'tradeable': True}]
        snapshot = {'schema_version': 1, 'group_id': '1', 'session_id': 's', 'snapshot_seq': 1,
                    'event_seq': 0, '_stale': False, '_age_seconds': 0.1,
                    'group': [{'name': 'Codex', 'inventory': inventory, 'inventory_truncated': False,
                               'inventory_stacks_total': 1}],
                    'nearby': [{'name': 'x' * 1000} for _ in range(50)]}
        with patch.object(world, 'read_latest_snapshot', return_value=snapshot), \
             patch.object(world, 'read_events', return_value=([], 0, False, 0, False)):
            rendered, _ = world.build_agent_context('/unused', member_name='Codex')
        result = json.loads(rendered.split('\n', 1)[1])
        self.assertEqual(result['snapshot']['group'][0].get('inventory'), inventory)


if __name__ == '__main__':
    unittest.main()
