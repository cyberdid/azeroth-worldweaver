"""Regression cases for side-effect replay and missing asynchronous vendor results."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('vendor_tasks_regression', ROOT / 'vendor_tasks.py')
tasks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tasks)


class SimulatedProcessDeath(BaseException):
    pass


class VendorTaskRegressions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'state.json'
        self.clock = [100.0]
        self.calls = []
        self.action_ids = []
        self.events = []
        self.crash_after_buy = False
        self.unknown_buy_ack = False
        self.actor = dict(name='Codex', guid='2', map_id=0, position=[0, 0, 0], playerbot=dict(master='Test'))
        self.snapshot = dict(session_id='s', group_id='1', _stale=False, group=[self.actor,
            dict(name='Test', guid='1', map_id=0, position=[0, 0, 0])], nearby=[
            dict(guid='3', entry=50, npc_roles=['vendor'], map_id=0, position=[0, 0, 0], alive=True)])
        self.candidates = [dict(vendor_entry=50, map_id=0, position=[0, 0, 0])]
        self.worker = self.make_worker()
        result = self.worker.start(dict(actor='Codex', type='vendor_task_start', item_entry=159,
            quantity=5, max_total_copper=25, return_to_user=False), 'Test')
        self.assertEqual(result['status'], 'accepted')

    def dispatch(self, action, sender, group, **kwargs):
        self.calls.append(dict(action))
        self.action_ids.append(kwargs.get('action_id') or str(len(self.calls)))
        if action['type'] == 'vendor_buy' and self.crash_after_buy:
            # The server accepted/performed the side effect; the bridge died
            # before Python could receive its acknowledgement or save its state.
            raise SimulatedProcessDeath()
        if action['type'] == 'vendor_buy' and self.unknown_buy_ack:
            return dict(status='unknown', action_id=self.action_ids[-1], reason_code='server_timeout')
        return dict(status='accepted', action_id=self.action_ids[-1])

    def make_worker(self):
        return tasks.VendorTasks(self.path, 'Codex', {'Test'}, lambda: self.snapshot,
            lambda: self.events, self.dispatch,
            lambda args: dict(results=self.candidates, next_offset=None),
            lambda: self.clock[0])

    def tick(self, seconds=3):
        self.clock[0] += seconds
        return self.worker.poll()

    def reach_catalog_wait(self):
        for _ in range(5):
            self.tick()
            if self.calls and self.calls[-1]['type'] == 'vendor_inspect':
                return self.action_ids[-1]
        self.fail('worker never requested vendor inspection')

    def test_bridge_restart_does_not_repeat_purchase_when_ack_is_lost(self):
        inspect_id = self.reach_catalog_wait()
        self.events.append(dict(type='vendor_catalog', actor='Codex', payload=dict(action_id=inspect_id,
            npc_guid='3', items=[dict(entry=159, buy_count=5, price_per_bundle_copper=25, extended_cost=dict(id=0))])))
        self.crash_after_buy = True
        with self.assertRaises(SimulatedProcessDeath):
            self.tick()
        self.crash_after_buy = False
        self.worker = self.make_worker()
        self.tick()
        self.assertEqual(sum(action['type'] == 'vendor_buy' for action in self.calls), 1,
                         'a server-accepted purchase must never be resent with a new action id after restart')

    def test_missing_catalog_result_has_a_stage_deadline(self):
        self.reach_catalog_wait()
        updates = self.tick(95)
        self.assertTrue(updates, 'a missing catalog must time out promptly, rather than block the worker for one hour')
        self.assertEqual(updates[0]['event'], 'vendor_task_failed')
        self.assertIsNone(self.worker.state)

    def test_matching_late_inspection_rejection_finishes_task(self):
        inspect_id = self.reach_catalog_wait()
        self.events.append(dict(type='action_rejected', actor='Codex', payload=dict(action_id=inspect_id,
            action='vendor_inspect', status='rejected', reason_code='npc_approach_timeout')))
        updates = self.tick()
        self.assertTrue(updates, 'an explicit server-side inspection rejection must finish the waiting task')
        self.assertEqual(updates[0]['event'], 'vendor_task_failed')


    def catalog_event(self, action_id, **metadata):
        return dict(type='vendor_catalog', actor='Codex', group_id='1', session_id='s', **metadata,
            payload=dict(action_id=action_id, npc_guid='3', items=[dict(entry=159, buy_count=5,
                price_per_bundle_copper=25, extended_cost=dict(id=0))]))

    def test_unknown_purchase_ack_waits_for_success_receipt_without_rebuying(self):
        inspect_id = self.reach_catalog_wait()
        self.events.append(self.catalog_event(inspect_id))
        self.unknown_buy_ack = True
        self.assertEqual(self.tick(), [])
        purchase_id = self.action_ids[-1]
        self.assertEqual(self.calls[-1]['type'], 'vendor_buy')
        self.assertEqual(self.tick(), [])
        self.events.append(dict(type='vendor_purchase', actor='Codex', group_id='1', session_id='s',
            payload=dict(action_id=purchase_id, npc_guid='3', status='success', entry=159, quantity_units=5)))
        updates = self.tick()
        self.assertEqual(updates[0]['event'], 'vendor_task_completed')
        self.assertEqual(updates[0]['evidence']['quantity_units'], 5)
        self.assertEqual(sum(a['type'] == 'vendor_buy' for a in self.calls), 1)
        self.assertEqual(self.tick(), [])

    def test_changed_world_session_prevents_buy_even_with_matching_catalog(self):
        inspect_id = self.reach_catalog_wait()
        self.events.append(self.catalog_event(inspect_id))
        self.snapshot['session_id'] = 'new-world-session'
        updates = self.tick()
        self.assertEqual(updates[0]['event'], 'vendor_task_failed')
        self.assertEqual(updates[0]['reason'], 'world_session_changed')
        self.assertFalse(any(a['type'] == 'vendor_buy' for a in self.calls))

    def test_catalog_without_requested_item_tries_next_vendor(self):
        self.candidates.append(dict(vendor_entry=60, map_id=0, position=[5, 0, 0]))
        self.snapshot['nearby'].append(dict(guid='4', entry=60, npc_roles=['vendor'], map_id=0,
            position=[5, 0, 0], alive=True))
        first_id = self.reach_catalog_wait()
        self.events.append(dict(type='vendor_catalog', actor='Codex', group_id='1', session_id='s',
            payload=dict(action_id=first_id, npc_guid='3', items=[])))
        self.assertEqual(self.tick(), [])
        second_id = self.reach_catalog_wait()
        self.assertNotEqual(first_id, second_id)
        self.assertEqual(self.calls[-1]['target_guid'], '4')
        self.events.append(dict(type='vendor_catalog', actor='Codex', group_id='1', session_id='s',
            payload=dict(action_id=second_id, npc_guid='4', items=[dict(entry=159, buy_count=5,
                price_per_bundle_copper=25, extended_cost=dict(id=0))])))
        self.tick()
        self.assertEqual([(a['type'], a['target_guid']) for a in self.calls if a['type'] == 'vendor_buy'],
                         [('vendor_buy', '4')])

    def test_wrong_actor_or_action_id_cannot_authorize_buy(self):
        inspect_id = self.reach_catalog_wait()
        wrong_actor = self.catalog_event(inspect_id)
        wrong_actor['actor'] = 'Claude'
        self.events.extend([wrong_actor, self.catalog_event('old-unrelated-action')])
        self.assertEqual(self.tick(), [])
        self.assertFalse(any(a['type'] == 'vendor_buy' for a in self.calls))
        self.events.append(self.catalog_event(inspect_id))
        self.tick()
        self.assertEqual(sum(a['type'] == 'vendor_buy' for a in self.calls), 1)

    def assert_event_identity_rejected(self, key, value):
        inspect_id = self.reach_catalog_wait()
        event = self.catalog_event(inspect_id)
        event[key] = value
        self.events[:] = [event]
        self.assertEqual(self.tick(), [])
        self.assertFalse(any(a['type'] == 'vendor_buy' for a in self.calls),
                         'receipt identity must include the task world session and group')

    def test_wrong_session_event_cannot_authorize_buy(self):
        self.assert_event_identity_rejected('session_id', 'previous-world-session')

    def test_wrong_group_event_cannot_authorize_buy(self):
        self.assert_event_identity_rejected('group_id', '999')

    def test_existing_purchase_receipt_wins_over_elapsed_poll_deadline(self):
        inspect_id = self.reach_catalog_wait()
        self.events.append(self.catalog_event(inspect_id))
        self.tick()
        purchase_id = self.action_ids[-1]
        self.events.append(dict(type='vendor_purchase', actor='Codex', group_id='1', session_id='s',
            payload=dict(action_id=purchase_id, npc_guid='3', status='success', entry=159, quantity_units=5)))
        updates = self.tick(95)
        self.assertEqual(updates[0]['event'], 'vendor_task_completed',
                         'an observed success must not become outcome unknown because the bridge polled late')


if __name__ == '__main__':
    unittest.main()
