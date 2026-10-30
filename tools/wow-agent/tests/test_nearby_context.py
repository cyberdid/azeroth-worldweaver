import json
import tempfile
import time
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import world_state


class NearbyContext(unittest.TestCase):
    def test_nearby_trainer_survives_large_spellbooks(self):
        spells = [{'spell_id': i, 'name': 'Spell ' + str(i), 'active': True} for i in range(1, 601)]
        snapshot = {'schema_version': 1, 'group_id': '1', 'session_id': 'session',
                    'snapshot_seq': 1, 'event_seq': 0, 'captured_at': time.time(),
                    'group': [{'name': name, 'known_spells': spells,
                               'playerbot': {'available_actions': ['action'] * 500},
                               'inventory': [{'entry': 42, 'count': 1}]}
                              for name in ('Codex', 'Claude', 'Terisfal')],
                    'nearby': [{'guid': '123', 'name': 'Nissa Firestone',
                                'type': 'creature', 'npc_roles': ['trainer']}]}
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, 'world-state-1.json').write_text(json.dumps(snapshot))
            rendered, _ = world_state.build_agent_context(directory, member_name='Codex')
        context = json.loads(rendered.split('\n', 1)[1])['snapshot']
        self.assertTrue(any(item['name'] == 'Nissa Firestone' for item in context['nearby']))
        self.assertEqual(context['group'][0]['inventory'], [{'entry': 42, 'count': 1}])
        self.assertTrue(context['group'][0]['known_spells_truncated'])
        self.assertLessEqual(len(rendered), world_state.MAX_CONTEXT_CHARS)


if __name__ == '__main__':
    unittest.main()
