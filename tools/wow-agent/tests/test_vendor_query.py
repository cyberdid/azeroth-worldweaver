"""Vendor search uses configured local stock, including referenced vendor lists."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location('vendor_test_' + name, ROOT / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


query = load('world_query')
client = load('world_query_client')
service = load('world_query_service')


class VendorQueryTests(unittest.TestCase):
    def setUp(self):
        # Execute the real emitted query against a small SQL database; only the
        # external MySQL boundary and schema discovery are replaced.
        self.db = sqlite3.connect(':memory:')
        self.db.create_function('CONCAT', -1, lambda *parts: ''.join(map(str, parts)))
        self.db.create_function('LOCATE', 2, lambda needle, haystack: str(haystack).lower().find(str(needle).lower()) + 1)
        self.db.executescript('''
            CREATE TABLE npc_vendor (entry INT, slot INT, item INT, maxcount INT, incrtime INT, ExtendedCost INT);
            CREATE TABLE item_template (entry INT, name TEXT, BuyCount INT, BuyPrice INT);
            CREATE TABLE creature_template (entry INT, name TEXT, npcflag INT);
            CREATE TABLE creature (guid INT, id1 INT, id2 INT, id3 INT, map INT, zoneId INT, areaId INT,
                position_x REAL, position_y REAL, position_z REAL, orientation REAL, phaseMask INT, spawnMask INT);
            INSERT INTO item_template VALUES (159, 'Refreshing Spring Water', 5, 25), (117, 'Tough Jerky', 1, 5);
            INSERT INTO creature_template VALUES (10, 'Water Merchant', 128), (20, 'Remote Merchant', 128),
                (30, 'Unspawned Merchant', 128);
            INSERT INTO npc_vendor VALUES (900, 0, 159, 4, 60, 3), (10, 0, -900, 999, 999, 999),
                (20, 0, -10, 0, 0, 0), (30, 0, 159, 0, 0, 0), (10, 1, 117, 0, 0, 0);
            INSERT INTO creature VALUES (100, 10, 0, 0, 0, 12, 13, 1, 2, 3, 0, 1, 1),
                (200, 20, 0, 0, 530, 14, 15, 4, 5, 6, 0, 1, 1),
                (201, 999, 20, 0, 530, 14, 15, 7, 8, 9, 0, 1, 1);
        ''')
        self.statements = []
        self.boundary = patch.object(query, '_mysql_rows', side_effect=self.mysql_rows)
        self.boundary.start()
        self.addCleanup(self.boundary.stop)
        self.addCleanup(self.db.close)

    def mysql_rows(self, statement, parameters, config_path):
        self.statements.append((statement, parameters))
        if 'information_schema.COLUMNS' in statement:
            return [{'column': row[1]} for row in self.db.execute('PRAGMA table_info(creature)')]
        return [json.loads(row[0]) for row in self.db.execute(statement, parameters)]

    def test_nested_references_inherit_leaf_stock_and_include_unspawned_vendor(self):
        result = query.run_query('vendors', {'item_id': 159})
        self.assertNotIn('error', result)
        self.assertEqual([(r['vendor_entry'], r['spawn_guid']) for r in result['results']],
                         [(10, 100), (20, 200), (20, 201), (30, None)])
        first = result['results'][0]
        self.assertEqual((first['item_id'], first['item_name'], first['buy_count'], first['base_buy_price_copper']),
                         (159, 'Refreshing Spring Water', 5, 25))
        self.assertEqual((first['maxcount'], first['incrtime_seconds'], first['extended_cost_id']), (4, 60, 3))
        self.assertEqual((first['provenance'], first['loaded'], first['current_stock']), ('static_spawn', 'unknown', 'unknown'))
        self.assertEqual((first['map_id'], first['zone_id'], first['area_id'], first['position']), (0, 12, 13, [1.0, 2.0, 3.0]))
        self.assertFalse(result['reference_resolution_truncated'])

    def test_literal_name_search_and_pagination_filters(self):
        result = query.run_query('vendors', {'name': 'spring water', 'map_id': 530, 'entry': 20, 'limit': 1})
        self.assertNotIn('error', result)
        self.assertEqual([r['spawn_guid'] for r in result['results']], [200])
        self.assertEqual(result['next_offset'], 1)
        second = query.run_query('vendors', {'name': 'spring water', 'map_id': 530, 'entry': 20, 'limit': 1, 'offset': 1})
        self.assertEqual([r['spawn_guid'] for r in second['results']], [201])
        self.assertIsNone(second['next_offset'])
        self.assertFalse(second['truncated'])
        dangerous = "water%' OR 1=1 --"
        result = query.run_query('vendors', {'name': dangerous})
        self.assertEqual(result['results'], [])
        self.assertTrue(any(dangerous in params for _, params in self.statements))
        self.assertTrue(all(dangerous not in sql for sql, _ in self.statements))
        self.assertNotIn(dangerous, query._prepared_script('SELECT ?', [dangerous]))

    def test_invalid_filters_are_rejected_before_database_access(self):
        cases = [({}, 'vendors_requires_item_id_or_name'),
                 ({'item_id': 159, 'name': 'water'}, 'vendors_requires_item_id_or_name'),
                 ({'item_id': '1 OR 1=1'}, 'invalid_item_id'),
                 ({'item_id': True}, 'invalid_item_id'),
                 ({'name': '  '}, 'invalid_name'),
                 ({'item_id': 159, 'map_id': -1}, 'invalid_map_id'),
                 ({'item_id': 159, 'offset': -1}, 'invalid_offset'),
                 ({'item_id': 159, 'limit': True}, 'invalid_limit'),
                 ({'item_id': 159, 'sql': 'DROP TABLE npc_vendor'}, 'unsupported_filter')]
        for args, error in cases:
            with self.subTest(args=args):
                self.assertEqual(query.run_query('vendors', args).get('error'), error)
        self.assertEqual(self.statements, [])

    def test_cycle_is_reported_without_repeating_vendor_results(self):
        self.db.execute('INSERT INTO npc_vendor VALUES (900, 1, -20, 0, 0, 0)')
        result = query.run_query('vendors', {'item_id': 159})
        self.assertNotIn('error', result)
        self.assertTrue(result['reference_cycle_detected'])
        self.assertTrue(result['reference_resolution_truncated'])
        self.assertEqual(len(result['results']), 4)

    def test_depth_bound_reports_incomplete_catalog(self):
        for entry in range(1000, 1020):
            self.db.execute('INSERT INTO npc_vendor VALUES (?, 0, ?, 0, 0, 0)',
                            (entry, -900 if entry == 1000 else -(entry - 1)))
        result = query.run_query('vendors', {'item_id': 159})
        self.assertNotIn('error', result)
        self.assertTrue(result['reference_resolution_truncated'])
        self.assertTrue(result['reference_depth_limit_reached'])
        self.assertTrue(result['truncated'])

    def test_optional_zone_area_schema_and_legacy_spawn_entry(self):
        self.db.executescript('''ALTER TABLE creature DROP COLUMN zoneId;
            ALTER TABLE creature DROP COLUMN areaId;
            ALTER TABLE creature DROP COLUMN id2;
            ALTER TABLE creature DROP COLUMN id3;
            ALTER TABLE creature RENAME COLUMN id1 TO id;''')
        result = query.run_query('vendors', {'item_id': 159, 'entry': 10})
        self.assertNotIn('error', result)
        self.assertIsNone(result['results'][0]['zone_id'])
        self.assertIsNone(result['results'][0]['area_id'])

    def test_output_byte_limit_keeps_pagination_consistent(self):
        with patch.object(query, 'MAX_OUTPUT_BYTES', 1600):
            result = query.run_query('vendors', {'item_id': 159})
        self.assertNotIn('error', result)
        self.assertLessEqual(len(json.dumps(result, ensure_ascii=False, separators=(',', ':')).encode()), 1600)
        self.assertEqual(result['returned_count'], len(result['results']))
        self.assertEqual(result['next_offset'], len(result['results']))


class VendorTransportTests(unittest.TestCase):
    def test_cli_carries_item_and_offset_to_service(self):
        with patch.object(sys, 'argv', ['world_query_client.py', 'vendors', '--item-id', '159', '--offset', '2']), \
             patch.object(client, 'submit', return_value={'results': []}) as submit, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(client.main(), 0)
        self.assertEqual(submit.call_args.args[:2], ('vendors', {'item_id': '159', 'limit': '20', 'offset': '2'}))

    def test_service_accepts_typed_vendor_request_and_rejects_sql_argument(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for args, expected in [({'item_id': 159, 'offset': 2}, None), ({'item_id': 159, 'sql': 'SELECT 1'}, 'invalid_request')]:
                request_id = 'a' * 32
                request = root / (request_id + '.request.json')
                request.write_text(json.dumps({'id': request_id, 'operation': 'vendors', 'args': args}))
                with patch.object(service._QUERY, 'run_query', return_value={'results': [{'item_id': 159}]}):
                    service._process_request(request, root, '/unused', lambda _: None)
                result = json.loads((root / (request_id + '.response.json')).read_text())['result']
                self.assertEqual(result.get('error'), expected)
                if expected is None:
                    self.assertEqual(result['results'], [{'item_id': 159}])


if __name__ == '__main__':
    unittest.main()
