"""Build a per-bot purchase worker from trusted sibling modules (Python -I compatible)."""
import importlib.util
import json
from pathlib import Path


def _load(name):
    spec = importlib.util.spec_from_file_location('vendor_runtime_'+name, Path(__file__).with_name(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def create(server_dir, telemetry_dir, bot, authorized, config):
    state = _load('world_state')
    actions = _load('action_client')
    query = _load('world_query')
    tasks = _load('vendor_tasks')
    def snapshot():
        return state.read_latest_snapshot(str(telemetry_dir),member_name=bot)
    def events():
        current = snapshot()
        if not current:
            if not worker.state:
                return []
            current=dict(group_id=worker.state['group'],session_id=worker.state['session'])
        group=current['group_id']
        if not group.isascii() or not group.isdigit():
            return []
        pending=worker.state.get('action_id') if worker.state else None
        if not pending:
            return []
        found=[]
        for suffix in ('.1',''):
            path=Path(telemetry_dir)/f'world-events-{group}.jsonl{suffix}'
            try:
                with path.open('rb') as stream:
                    size=path.stat().st_size
                    stream.seek(max(0,size-state.MAX_EVENT_FILE_BYTES))
                    if size>state.MAX_EVENT_FILE_BYTES:
                        stream.readline()
                    data=stream.read(state.MAX_EVENT_FILE_BYTES)
                for line in data.splitlines():
                    if len(line)>state.MAX_EVENT_LINE_BYTES:
                        continue
                    try:
                        event=json.loads(line)
                    except (ValueError,UnicodeError):
                        continue
                    if (isinstance(event,dict) and event.get('session_id')==current['session_id']
                        and event.get('group_id')==group and event.get('actor')==bot
                        and isinstance(event.get('payload'),dict) and event['payload'].get('action_id')==pending):
                        found.append(event)
            except OSError:
                continue
        return sorted(found,key=lambda e:e.get('event_seq',0))
    def dispatch(action,sender,group,action_id=None):
        return actions.dispatch_action(action,sender=sender,group_id=group,authorized_players=authorized,
                                       server_dir=server_dir,inventory_snapshot=snapshot(),action_id=action_id)
    worker = tasks.VendorTasks(Path(telemetry_dir)/'.vendor-tasks'/f'{bot.casefold()}.json',bot,authorized,
                             snapshot,events,dispatch,lambda args:query.run_query('vendors',args,str(config)))

    return worker
