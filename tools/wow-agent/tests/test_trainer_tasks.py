import importlib.util
from pathlib import Path
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]


def load(name):
    spec=importlib.util.spec_from_file_location(name+'_trainer_test',ROOT/(name+'.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


class TrainerTasks(unittest.TestCase):
    def setUp(self):
        self.module=load('vendor_tasks')
        self.tmp=tempfile.TemporaryDirectory()
        self.actor=dict(name='Codex',guid='2',map_id=0,position=[0,0,0],playerbot=dict(master='Test'),
                        skills=dict(first_aid=0),all_skills=[])
        self.snap=dict(session_id='s',group_id='1',_stale=False,
                       group=[self.actor,dict(name='Test',guid='1',map_id=0,position=[0,0,0])],nearby=[])
        self.calls=[];self.events=[];self.clock=[100];self.results={}
        def dispatch(a,sender,group,**kwargs):
            self.calls.append(a)
            return self.results.get(a['type'],dict(status='accepted',action_id=str(len(self.calls))))
        self.tasks=self.module.VendorTasks(Path(self.tmp.name)/'state.json','Codex',{'Test'},lambda:self.snap,
                                           lambda:self.events,dispatch,lambda args:dict(results=[],next_offset=None),
                                           lambda:self.clock[0])

    def tearDown(self):
        self.tmp.cleanup()

    def poll(self):
        self.clock[0]+=3
        return self.tasks.poll()

    def start(self, **extra):
        return self.tasks.start(dict(actor='Codex',type='trainer_task_start',trainer_entry=5150,map_id=0,
                                     position=[100.0,0.0,0.0],**extra),'Test')

    def test_travels_to_distant_trainer_then_learns_and_follows_back(self):
        self.assertEqual(self.start()['status'],'accepted')
        self.poll();self.assertEqual(self.calls[-1],dict(actor='Codex',type='playerbot_action',action_name='nc',qualifier='-follow,-stay'))
        self.poll();self.assertEqual(self.calls[-1]['type'],'vendor_travel')
        self.assertEqual(self.calls[-1]['position'],[100.0,0.0,0.0])
        journey_id=str(len(self.calls))
        self.assertEqual(self.poll(),[])
        self.actor['position']=[99.0,0.0,0.0]
        self.snap['nearby']=[dict(guid='77',entry=5150,npc_roles=['trainer'],map_id=0,position=[100.0,0.0,0.0],alive=True)]
        self.events.append(dict(type='vendor_journey',actor='Codex',payload=dict(action_id=journey_id,status='arrived')))
        self.poll();self.poll()
        self.assertEqual(self.calls[-1],dict(actor='Codex',type='trainer_action',target_guid='77',learn=True))
        self.actor['skills']=dict(first_aid=1)
        updates=self.poll()
        self.assertEqual(self.calls[-1]['type'],'follow')
        self.assertEqual(updates[0]['event'],'trainer_task_completed')
        self.assertEqual(updates[0]['evidence']['skills_changed'],{'first_aid':[0,1]})
        self.assertIsNone(self.tasks.state)

    def test_trainer_already_in_reach_learns_without_travel(self):
        self.snap['nearby']=[dict(guid='77',entry=5150,npc_roles=['trainer'],map_id=0,position=[2.0,0.0,0.0],alive=True)]
        self.start(learn=False)
        self.poll();self.poll()
        self.assertEqual([a['type'] for a in self.calls],['playerbot_action','trainer_action'])
        self.assertFalse(self.calls[-1]['learn'])

    def test_rejected_training_reports_failure_and_restores_follow(self):
        self.snap['nearby']=[dict(guid='77',entry=5150,npc_roles=['trainer'],map_id=0,position=[2.0,0.0,0.0],alive=True)]
        self.results['trainer_action']=dict(status='rejected',reason_code='trainer_unavailable_or_out_of_range')
        self.start()
        self.poll();updates=self.poll()
        self.assertEqual(updates[0]['event'],'trainer_task_failed')
        self.assertEqual(updates[0]['reason'],'trainer_unavailable_or_out_of_range')
        self.assertEqual(self.calls[-1]['type'],'follow')

    def test_failed_journey_reports_failure(self):
        self.start()
        self.poll();self.poll()
        self.events.append(dict(type='vendor_journey',actor='Codex',payload=dict(action_id=str(len(self.calls)),status='failed',reason_code='journey_route_stalled')))
        updates=self.poll()
        self.assertEqual(updates[0]['event'],'trainer_task_failed')
        self.assertEqual(updates[0]['reason'],'journey_route_stalled')
        self.assertEqual(self.calls[-1]['type'],'follow')


class TrainerTaskProtocol(unittest.TestCase):
    def test_trainer_task_is_validated_and_kept_bridge_local(self):
        client=load('action_client')
        reply='Іду вчитися.\nACTION: {"actor":"Claude","type":"trainer_task_start","trainer_entry":5150,"map_id":0,"position":[-4876.6,-1149.1,512.5]}'
        text,action,error=client.parse_action_response(reply,bot_name='Claude',allow_action=True)
        self.assertIsNone(error);self.assertEqual(action['trainer_entry'],5150)
        with self.assertRaisesRegex(ValueError,'bridge_local_action'):
            client.build_action_command(action,sender='Test',group_id='1',action_id='0'*32)
        bad=reply.replace('"trainer_entry":5150','"trainer_entry":0')
        self.assertEqual(client.parse_action_response(bad,bot_name='Claude',allow_action=True)[2],'invalid_trainer_entry')


if __name__=='__main__':
    unittest.main()
