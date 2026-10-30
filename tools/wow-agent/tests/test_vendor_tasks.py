import importlib.util
from pathlib import Path
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class VendorTasks(unittest.TestCase):
    def test_authorized_plan_only_buys_after_matching_catalog_and_reports_once(self):
        spec=importlib.util.spec_from_file_location('vendor_tasks_test',ROOT/'vendor_tasks.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            actor=dict(name='Codex',guid='2',map_id=0,position=[0,0,0],playerbot=dict(master='Test'))
            snap=dict(session_id='s',group_id='1',_stale=False,group=[actor,dict(name='Test',guid='1',map_id=0,position=[0,0,0])],nearby=[dict(guid='3',entry=50,npc_roles=['vendor'],map_id=0,position=[0,0,0],alive=True)])
            calls=[];events=[]; clock=[100]
            def dispatch(a, sender, group, **kwargs):
                calls.append(a);return dict(status='accepted',action_id=str(len(calls)))
            m=module.VendorTasks(Path(tmp)/'state.json','Codex',{'Test'},lambda:snap,lambda:events,dispatch,lambda args:dict(results=[dict(vendor_entry=50,map_id=0,position=[0,0,0])],next_offset=None),lambda:clock[0])
            m.start(dict(actor='Codex',type='vendor_task_start',item_entry=159,quantity=5,max_total_copper=25,return_to_user=False),'Test')
            m.poll(); clock[0]+=3;m.poll(); self.assertEqual(calls[-1]['type'],'stay')
            clock[0]+=3;m.poll();self.assertEqual(calls[-1]['type'],'vendor_inspect')
            events.append(dict(type='vendor_catalog',actor='Claude',payload=dict(action_id='2',items=[dict(entry=159)])))
            clock[0]+=3;m.poll(); self.assertFalse(any(a['type']=='vendor_buy' for a in calls))
            events.append(dict(type='vendor_catalog',actor='Codex',payload=dict(action_id='2',npc_guid='3',items=[dict(entry=159,buy_count=5,price_per_bundle_copper=25,extended_cost=dict(id=0))])))
            clock[0]+=3;m.poll();self.assertEqual(calls[-1]['type'],'vendor_buy')
            events.append(dict(type='vendor_purchase',actor='Codex',payload=dict(action_id='3',status='success',quantity_units=5)))
            clock[0]+=3;updates=m.poll();self.assertEqual(updates[0]['event'],'vendor_task_completed');self.assertEqual(m.poll(),[])

class NativePriceRounding(unittest.TestCase):
    def test_total_matches_gold_price_before_rounding_bundles(self):
        spec=importlib.util.spec_from_file_location('vendor_rounding',ROOT/'vendor_tasks.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        offer=dict(buy_count=5,price_per_bundle_copper=4,base_price_per_bundle_copper=5,reputation_discount=0.9)
        self.assertEqual(module.vendor_total_copper(offer,15),13)
