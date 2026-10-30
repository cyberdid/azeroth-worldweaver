import importlib.util
import json
import pathlib
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('vendor_protocol_client', ROOT/'action_client.py')
client = importlib.util.module_from_spec(spec); spec.loader.exec_module(client)
class VendorProtocol(unittest.TestCase):
    def parse(self, **args):
        action = dict(actor='Codex', type='vendor_buy', target_guid='123', item_entry=159,
                      quantity=10, max_total_copper=50, **args)
        return client.parse_action_response('ACTION: '+json.dumps(action), bot_name='Codex')
    def test_native_buy_exact_units_and_budget(self):
        _, action, error = self.parse()
        self.assertIsNone(error)
        self.assertTrue(client.build_action_command(action,sender='Test',group_id='1',action_id='a'*32).endswith('v:159:10:50:0'))
    def test_invalid_boolean_quantity_rejected(self):
        action=dict(actor='Codex', type='vendor_buy',target_guid='123',item_entry=159,quantity=True,max_total_copper=50)
        _, parsed, error=client.parse_action_response('ACTION: '+json.dumps(action),bot_name='Codex')
        self.assertIsNone(parsed); self.assertIsNotNone(error)
    def test_inspection_accepts_vendor_without_purchase_parameters(self):
        _, action, error=client.parse_action_response('ACTION: '+json.dumps(dict(actor='Claude',type='vendor_inspect',target_guid='123')),bot_name='Claude')
        self.assertIsNone(error); self.assertEqual(action['type'],'vendor_inspect')
    def test_purchase_task_has_explicit_quantity_budget(self):
        _, action, error=client.parse_action_response('ACTION: '+json.dumps(dict(actor='Codex',type='vendor_task_start',item_entry=159,quantity=5,max_total_copper=100)),bot_name='Codex')
        self.assertIsNone(error); self.assertEqual(action['quantity'],5)

class VendorTravelProtocol(unittest.TestCase):
    def test_nonfinite_coordinate_is_rejected(self):
        action=dict(actor='Codex',type='vendor_travel',map_id=0,position=[float('nan'),0,0],purchase_reserve_copper=0)
        _,parsed,error=client.parse_action_response('ACTION: '+json.dumps(action),bot_name='Codex')
        self.assertIsNone(parsed);self.assertEqual(error,'invalid_vendor_position')

class ActionDirectiveSanitization(unittest.TestCase):
    def test_inline_action_is_extracted_and_never_left_in_speech(self):
        action = dict(actor='Codex', type='offer_items', items=[dict(item_id=2678, all=True)])
        reply, parsed, error = client.parse_action_response(
            'Виставляю тобі Mild Spices у трейд. ACTION: ' + json.dumps(action, ensure_ascii=False),
            bot_name='Codex')
        self.assertEqual(reply, 'Виставляю тобі Mild Spices у трейд.')
        self.assertEqual(parsed, action)
        self.assertIsNone(error)

    def test_multiline_fenced_action_is_extracted_and_removed(self):
        action = dict(actor='Codex', type='vendor_inspect', target_guid='123')
        response = 'Перевірю товар. ACTION:\n```json\n' + json.dumps(action) + '\n```'
        reply, parsed, error = client.parse_action_response(response, bot_name='Codex')
        self.assertEqual(reply, 'Перевірю товар.')
        self.assertEqual(parsed, action)
        self.assertIsNone(error)

    def test_malformed_inline_directive_is_removed_before_error_reply(self):
        reply, parsed, error = client.parse_action_response(
            'Перевірю. ACTION: {"actor":"Codex", bad json}\nвнутрішні дані', bot_name='Codex')
        self.assertEqual(reply, 'Перевірю.')
        self.assertIsNone(parsed)
        self.assertEqual(error, 'invalid_action_json')
        self.assertNotIn('ACTION', reply)
        self.assertNotIn('actor', reply)

    def test_multiple_inline_actions_are_all_removed_and_rejected(self):
        first = dict(actor='Codex', type='follow')
        second = dict(actor='Codex', type='stay')
        response = 'Готово. ACTION: ' + json.dumps(first) + ' ACTION: ' + json.dumps(second)
        reply, parsed, error = client.parse_action_response(response, bot_name='Codex')
        self.assertEqual(reply, 'Готово.')
        self.assertIsNone(parsed)
        self.assertEqual(error, 'multiple_actions')

    def test_unauthorized_inline_action_is_stripped(self):
        action = dict(actor='Codex', type='follow')
        reply, parsed, error = client.parse_action_response(
            'Я не можу це зробити. ACTION: ' + json.dumps(action),
            bot_name='Codex', allow_action=False)
        self.assertEqual(reply, 'Я не можу це зробити.')
        self.assertIsNone(parsed)
        self.assertEqual(error, 'action_not_authorized')
