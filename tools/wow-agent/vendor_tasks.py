"""Private, bounded purchase tasks; LLM chooses intent, native server performs mechanics.

Only a current authorized user request can call start(). Observed events may advance
that exact task, never create or expand one. No dialogue templates are generated here.
"""
import json
import math
import os
from pathlib import Path
import time
import uuid
import struct

# Server interaction distance; journeys end within 5.5 yards of the target.
TRAINER_REACH = 5.5


def vendor_total_copper(offer, quantity):
    bundles=quantity//offer['buy_count']
    if offer.get('gold_required') is False:
        return 0
    if 'base_price_per_bundle_copper' not in offer or 'reputation_discount' not in offer:
        return offer['price_per_bundle_copper']*bundles
    def f32(value):
        return struct.unpack('f',struct.pack('f',value))[0]
    return math.floor(f32(f32(offer['base_price_per_bundle_copper']*bundles)*f32(offer['reputation_discount'])))


class VendorTasks:
    def __init__(self, path, bot, authorized, snapshot, events, dispatch, query, clock=time.time):
        self.path = Path(path)
        self.bot = bot
        self.authorized = {name.casefold() for name in authorized}
        self.snapshot = snapshot
        self.events = events
        self.dispatch = dispatch
        self.query = query
        self.clock = clock
        self.state = None
        try:
            state = json.loads(self.path.read_text())
            if (isinstance(state, dict) and {'action','sender','session','group','stage','started','bot','actor_guid'}.issubset(state)
                    and isinstance(state.get('action'),dict) and state.get('bot')==bot
                    and isinstance(state.get('sender'),str)):
                self.state = state
        except (OSError, ValueError):
            pass
        self.next_poll = 0

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        temp = self.path.with_suffix('.tmp')
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as out:
            json.dump(self.state, out, ensure_ascii=False)
        os.replace(temp, self.path)

    def context(self, sender):
        snap = self.snapshot()
        if (not snap or snap.get('_stale', True)
                or (isinstance(snap.get('captured_at'),(int,float)) and snap['captured_at']>self.clock()+5)):
            raise ValueError('fresh_world_required')
        actor = next((m for m in snap.get('group', []) if m.get('name') == self.bot), None)
        user = next((m for m in snap.get('group', []) if m.get('name', '').casefold() == sender.casefold()), None)
        if not actor or not user or actor.get('playerbot', {}).get('master', '').casefold() != sender.casefold():
            raise ValueError('owner_or_group_changed')
        return snap, actor, user

    def start(self, action, sender):
        if sender.casefold() not in self.authorized:
            return dict(status='rejected', reason_code='unauthorized_sender')
        if self.state:
            return dict(status='rejected', reason_code='vendor_task_already_running')
        try:
            snap, actor, user = self.context(sender)
        except ValueError as error:
            return dict(status='rejected', reason_code=str(error))
        trainer = action.get('type') == 'trainer_task_start'
        self.state = dict(action=action, sender=sender, session=snap['session_id'], group=snap['group_id'],
                          stage='trainer_hold' if trainer else 'search', stage_at=self.clock(), started=self.clock(),
                          bot=self.bot, actor_guid=actor['guid'], attempts=0)
        if trainer:
            self.state['skills_before'] = dict(actor.get('skills') or {})
        self.save()
        return dict(status='accepted', reason_code='trainer_task_started' if trainer else 'vendor_task_started')

    def watch(self, action, result, sender):
        if self.state or action['type'] not in {'vendor_inspect', 'vendor_buy', 'vendor_travel'} or result.get('status') not in {'accepted','unknown'}:
            return
        snap, actor, user = self.context(sender)
        self.state = dict(action=action, sender=sender, session=snap['session_id'], group=snap['group_id'],
                          stage='report', expected={'vendor_inspect':'vendor_catalog','vendor_buy':'vendor_purchase','vendor_travel':'vendor_journey'}[action['type']],
                          action_id=result['action_id'], stage_at=self.clock(), started=self.clock(), bot=self.bot, actor_guid=actor['guid'])
        self.save()

    def cancel(self, sender):
        if sender.casefold() not in self.authorized or (self.state and self.state['sender'].casefold()!=sender.casefold()):
            return dict(status='rejected', reason_code='unauthorized_sender')
        if self.state:
            receipt=self.matching_event('vendor_purchase')
            if receipt and receipt.get('status')=='success':
                self.state=None
                self.save()
                return dict(status='rejected',reason_code='purchase_already_completed',evidence=receipt)
            result = self.dispatch(dict(actor=self.bot,type='vendor_travel_cancel'),sender,self.state['group'])
            if result.get('status') != 'accepted':
                return result
            self.release_hold()
            self.state = None
            self.save()
        return dict(status='accepted', reason_code='vendor_task_cancelled')

    def release_hold(self):
        # A trainer task stops the bot following so it does not walk back to the
        # user on arrival; give following back when the task ends.
        if not self.state.get('held'):
            return
        try:
            self.dispatch(dict(actor=self.bot,type='follow'),self.state['sender'],self.state['group'])
        except (OSError,ValueError,KeyError,TypeError):
            pass

    def finish(self, event, reason, evidence=None):
        state = self.state
        if state['action'].get('type') == 'trainer_task_start':
            event = event.replace('vendor_task', 'trainer_task')
            self.release_hold()
        update = dict(event=event, actor=self.bot, requester=state['sender'], reason=reason,
                      request=state['action'], evidence=evidence)
        self.state = None
        self.save()
        return [update]

    def transition(self, stage):
        self.state['stage'] = stage
        self.state['stage_at'] = self.clock()
        self.save()

    def send(self, kind, **args):
        action = dict(actor=self.bot, type=kind, **args)
        previous = self.state.get('outbound', {})
        if previous.get('action') == action and previous.get('stage') == self.state['stage']:
            action_id = previous['action_id']
        else:
            action_id = uuid.uuid4().hex
        self.state['outbound'] = dict(action=action, action_id=action_id, stage=self.state['stage'])
        self.state['action_id'] = action_id
        # Persist waiting state BEFORE a monetary side effect. On a bridge crash,
        # wait for its receipt; never issue a new purchase automatically.
        if kind == 'vendor_buy':
            self.state['stage'] = 'purchase'
            self.state['stage_at'] = self.clock()
        self.save()
        result = self.dispatch(action,self.state['sender'],self.state['group'],action_id=action_id)
        if kind == 'vendor_buy' and result.get('status') == 'unknown':
            self.state['transport_uncertain'] = result
            self.save()
            return dict(status='accepted',action_id=action_id,reason_code='awaiting_purchase_receipt')
        if result.get('status') == 'accepted':
            self.state['action_id'] = result.get('action_id')
        return result

    def retry_vendor(self, reason, evidence=None):
        state=self.state
        index=state.get('candidate_index',0)+1
        candidates=state.get('candidates',[])
        if index>=len(candidates):
            return self.finish('vendor_task_failed',reason,evidence)
        state['candidate_index']=index
        state['vendor']=candidates[index]
        state.pop('action_id',None)
        self.transition('approach')
        return []

    def live_trainers(self, snap, actor):
        entry=self.state['action']['trainer_entry']
        live=[o for o in snap.get('nearby',[]) if o.get('entry')==entry and 'trainer' in o.get('npc_roles',[])
              and o.get('map_id')==actor['map_id'] and o.get('alive') is True]
        return sorted(live,key=lambda o:math.dist(o['position'],actor['position']))

    def matching_event(self, kind):
        for event in reversed(self.events()):
            payload = event.get('payload', {})
            if (event.get('type') == kind and event.get('actor') == self.bot
                and event.get('session_id',self.state['session']) == self.state['session']
                and event.get('group_id',self.state['group']) == self.state['group']
                and payload.get('action_id') == self.state.get('action_id')):
                if kind == 'vendor_journey' and payload.get('status') not in {'arrived','failed'}:
                    continue
                return payload
        return None

    def poll(self):
        if not self.state or self.clock() < self.next_poll:
            return []
        self.next_poll = self.clock()+2.5
        state = self.state
        if state['sender'].casefold() not in self.authorized:
            return self.finish('vendor_task_failed','authorization_removed')
        try:
            snap, actor, user = self.context(state['sender'])
            if (snap['session_id'] != state['session'] or snap['group_id'] != state['group']
                    or actor['guid'] != state['actor_guid']):
                raise ValueError('world_session_changed')
            if self.clock()-state['started'] > 3600:
                raise ValueError('vendor_task_timeout')
            state.pop('missing_context_since',None)
            action=state['action']
            stage=state['stage']
            rejection=self.matching_event('action_rejected')
            if rejection and stage=='catalog' and state.get('candidates'):
                return self.retry_vendor(rejection.get('reason_code','inspection_rejected'),rejection)
            if rejection:
                return self.finish('vendor_task_failed',rejection.get('reason_code','server_rejected'),rejection)
            expected = {'catalog':'vendor_catalog','purchase':'vendor_purchase','report':state.get('expected')}.get(stage)
            if expected and expected!='vendor_journey' and not self.matching_event(expected) and self.clock()-state.get('stage_at',state['started'])>90:
                reason='purchase_outcome_unknown' if stage=='purchase' or state.get('expected')=='vendor_purchase' else 'vendor_result_timeout'
                if reason == 'purchase_outcome_unknown':
                    if not state.get('uncertainty_reported'):
                        state['uncertainty_reported']=True
                        self.save()
                        return [dict(event='vendor_task_pending',actor=self.bot,requester=state['sender'],reason=reason,request=state['action'],evidence=state.get('outbound'))]
                    return []
                return self.finish('vendor_task_failed',reason,state.get('outbound'))
            if stage=='report':
                evidence=self.matching_event(state['expected'])
                if evidence:
                    return self.finish('vendor_observation_ready',state['expected'],evidence)
                if self.clock()-state['started']>90:
                    return self.finish('vendor_task_failed','vendor_result_timeout')
            elif stage=='search':
                # Page every local candidate up to a bounded search budget. Static rows
                # only locate candidates; eligibility and stock are checked at arrival.
                rows=[]; seen=set()
                for map_filter in ({'map_id':actor['map_id']}, {}):
                    offset=0
                    for _ in range(10):
                        page=self.query(dict(item_id=action['item_entry'],limit=100,offset=offset,**map_filter))
                        if page.get('error'):
                            raise ValueError('vendor_lookup_failed')
                        for row in page.get('results',[]):
                            key=(row.get('vendor_entry'),row.get('spawn_guid'),row.get('map_id'),str(row.get('position')))
                            if row.get('position') is not None and key not in seen:
                                rows.append(row);seen.add(key)
                        if page.get('next_offset') is None:
                            break
                        offset=page['next_offset']
                if not rows:
                    return self.finish('vendor_task_failed','no_configured_vendor_spawn_found')
                rows.sort(key=lambda r: (r['map_id']!=actor['map_id'],
                    math.dist(r['position'],actor['position']) if r['map_id']==actor['map_id'] else 0))
                state['candidates']=rows[:16];state['candidate_index']=0;state['vendor']=rows[0];self.transition('approach')
            elif stage=='approach':
                vendor=state['vendor']
                live=[o for o in snap.get('nearby',[]) if o.get('entry')==vendor['vendor_entry']
                      and 'vendor' in o.get('npc_roles',[]) and o.get('map_id')==actor['map_id']
                      and o.get('alive') is True and math.dist(o['position'],actor['position'])<=95]
                if live:
                    live.sort(key=lambda o:math.dist(o['position'],actor['position']))
                    state['npc_guid']=live[0]['guid']
                    result=self.send('stay');self.transition('inspect')
                else:
                    result=self.send('vendor_travel',map_id=vendor['map_id'],position=vendor['position'],
                                     purchase_reserve_copper=action['max_total_copper'])
                    self.transition('journey')
                if result.get('status')!='accepted':
                    return self.finish('vendor_task_failed',result.get('reason_code','action_failed'),result)
            elif stage=='journey':
                evidence=self.matching_event('vendor_journey')
                if evidence:
                    if evidence.get('status')!='arrived':
                        return self.retry_vendor(evidence.get('reason_code','route_failed'),evidence)
                    self.transition('approach')
            elif stage=='inspect':
                result=self.send('vendor_inspect',target_guid=state['npc_guid'])
                if result.get('status')!='accepted':
                    return self.retry_vendor(result.get('reason_code','inspect_failed'),result)
                self.transition('catalog')
            elif stage=='catalog':
                evidence=self.matching_event('vendor_catalog')
                if evidence:
                    offers=[i for i in evidence.get('items',[]) if i.get('entry')==action['item_entry']]
                    if len(offers)!=1:
                        return self.retry_vendor('item_unavailable_or_ambiguous',evidence)
                    offer=offers[0]; bundle=offer['buy_count']
                    if bundle<=0 or action['quantity']%bundle:
                        return self.finish('vendor_task_failed','quantity_not_multiple_of_bundle',offer)
                    if offer.get('valid_gold_price') is False:
                        return self.retry_vendor('vendor_invalid_price',offer)
                    if vendor_total_copper(offer,action['quantity'])>action['max_total_copper']:
                        return self.retry_vendor('price_exceeds_authorized_budget',offer)
                    if offer.get('extended_cost',{}).get('id') and not action.get('allow_extended_cost',False):
                        return self.finish('vendor_task_failed','extended_cost_not_authorized',offer)
                    result=self.send('vendor_buy',target_guid=state['npc_guid'],item_entry=action['item_entry'],
                                     quantity=action['quantity'],max_total_copper=action['max_total_copper'],
                                     allow_extended_cost=action.get('allow_extended_cost',False))
                    if result.get('status')!='accepted':
                        return self.finish('vendor_task_failed',result.get('reason_code','buy_rejected'),result)
                    self.transition('purchase')
            elif stage=='purchase':
                evidence=self.matching_event('vendor_purchase')
                if evidence:
                    if evidence.get('status')!='success':
                        return self.finish('vendor_task_failed',evidence.get('reason_code','purchase_failed'),evidence)
                    state['purchase']=evidence
                    if not action.get('return_to_user',True):
                        return self.finish('vendor_task_completed','purchase_confirmed',evidence)
                    result=self.send('vendor_travel',map_id=user['map_id'],position=user['position'],purchase_reserve_copper=0)
                    if result.get('status')!='accepted':
                        return self.finish('vendor_task_completed','purchased_return_not_started',evidence)
                    self.transition('return')
            elif stage=='return':
                evidence=self.matching_event('vendor_journey')
                if evidence:
                    if evidence.get('status')=='arrived':
                        self.send('follow')
                    return self.finish('vendor_task_completed','purchased_return_'+str(evidence.get('status')),state['purchase'])
            elif stage=='trainer_hold':
                # Drop following without 'stay': 'stay' walks the bot back to the
                # spot where it was issued, i.e. away from the trainer on arrival.
                result=self.send('playerbot_action',action_name='nc',qualifier='-follow,-stay')
                if result.get('status')!='accepted':
                    return self.finish('vendor_task_failed',result.get('reason_code','stay_rejected'),result)
                state['held']=True
                self.transition('trainer_approach')
            elif stage=='trainer_approach':
                live=self.live_trainers(snap,actor)
                if live and math.dist(live[0]['position'],actor['position'])<=TRAINER_REACH:
                    result=self.send('trainer_action',target_guid=live[0]['guid'],learn=action.get('learn',True))
                    if result.get('status')!='accepted':
                        return self.finish('vendor_task_failed',result.get('reason_code','trainer_rejected'),result)
                    state['trainer_result']=result
                    self.transition('trainer_report')
                elif state.get('trainer_arrivals',0)>=2:
                    return self.finish('vendor_task_failed','trainer_not_in_reach_after_arrival')
                else:
                    # Walk to the live trainer when it is visible; its static spawn
                    # point only locates it and may differ from where it stands.
                    position=live[0]['position'] if live else action['position']
                    result=self.send('vendor_travel',map_id=action['map_id'],position=list(position),purchase_reserve_copper=0)
                    if result.get('status')!='accepted':
                        return self.finish('vendor_task_failed',result.get('reason_code','route_rejected'),result)
                    self.transition('trainer_journey')
            elif stage=='trainer_journey':
                evidence=self.matching_event('vendor_journey')
                if evidence:
                    if evidence.get('status')!='arrived':
                        return self.finish('vendor_task_failed',evidence.get('reason_code','route_failed'),evidence)
                    state['trainer_arrivals']=state.get('trainer_arrivals',0)+1
                    self.transition('trainer_approach')
            elif stage=='trainer_report':
                before=state.get('skills_before',{})
                changed={name:[before.get(name,0),value] for name,value in (actor.get('skills') or {}).items()
                         if not name.endswith('_max') and before.get(name,0)!=value}
                reason='trainer_learning_done' if action.get('learn',True) else 'trainer_inspected'
                return self.finish('trainer_task_completed',reason,dict(result=state['trainer_result'],skills_changed=changed))
            self.save()
        except (OSError,ValueError,KeyError,TypeError,IndexError) as error:
            if (self.state['stage'] in {'journey','return','trainer_journey'} or self.state.get('expected')=='vendor_journey') and str(error) in {'fresh_world_required','owner_or_group_changed'}:
                evidence=self.matching_event('vendor_journey')
                if evidence and evidence.get('status')=='failed':
                    return self.finish('vendor_task_failed',evidence.get('reason_code','journey_failed'),evidence)
                since=self.state.setdefault('missing_context_since',self.clock())
                if self.clock()-since<60:
                    self.save()
                    return []
            return self.finish('vendor_task_failed',str(error))
        return []
