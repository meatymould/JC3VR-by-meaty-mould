"""Summarise a completed read-only hijack trace; no phase/safety inference."""
import argparse,json,re
from pathlib import Path
from extract_weapon_icons import hash_name

def analyse(text,labels=None):
    labels=labels or {};complete=[];current=None
    for line in text.splitlines():
        if 'hijack state BEGIN ' in line:
            fields=dict(re.findall(r'(\w+)=([^\s;]+)',line));current={'header':fields,'samples':[]}
        elif current is not None and 'hijack state sample=' in line:
            current['samples'].append(dict(re.findall(r'(\w+)=([^\s;]+)',line)))
        elif current is not None and 'hijack state END' in line:
            complete.append(current);current=None
    if not complete:raise ValueError('no complete hijack trace; no results inferred')
    c=complete[-1];samples=c['samples'];header=c['header'];freq=int(header['qpc_frequency'])
    if freq<=0 or len(samples)!=int(header['count']):raise ValueError('trace frequency/count mismatch')
    if not samples:raise ValueError('capture never saw a validated helicopter')
    if not int(samples[0]['flags'])&16 or int(samples[0]['vehicle'],16)==0:
        raise ValueError('capture does not start with a validated helicopter')
    first=int(samples[0]['qpc']);previous=first;changes=[];last=None
    for row in samples:
        now=int(row['qpc'])
        if now<previous:raise ValueError('nonmonotonic trace; inspect raw log')
        previous=now;flags=int(row['flags']);state=row['hash'].upper() if flags&1 else 'UNKNOWN'
        identity=(state,row['node'],row['vehicle'],row['seat'])
        if identity!=last:
            changes.append({'seconds':(now-first)/freq,'state_hash':state,'state_label':labels.get(state),
                'node':row['node'],'vehicle':row['vehicle'],'native_seat':int(row['seat']),'read_flags':flags})
            last=identity
    return {'samples':len(samples),'buffer_overflow':int(header['overflow']),
        'qpc_frequency':freq,'observed_seconds':(previous-first)/freq,'changes':changes,
        'limitations':['Snapshots, not exact event times or a proof of a safe pause point.',
                      'Fraction fields are candidates. Thread contention drops are not counted.',
                      'Native seat matching is graph-node matching, not headset/camera validation.']}

def state_labels(root):
    labels={}
    for path in root.glob('rico_*/rtpc_*.json'):
        for node in json.loads(path.read_text())['nodes']:
            for p in node['props']:
                s=p['value']
                if p['key']==0x3a4ca003 and isinstance(s,str) and s.startswith('S_'):
                    if hash_name(s)!=node['key']:raise ValueError('asset state-name/hash mismatch')
                    labels[f'{node["key"]:08X}']=s
    return labels

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('log',type=Path);p.add_argument('output',type=Path);p.add_argument('--assets',type=Path)
    a=p.parse_args();result=analyse(a.log.read_text(errors='replace'),state_labels(a.assets) if a.assets else None)
    a.output.write_text(json.dumps(result,indent=2),encoding='utf8')
    print('Complete trace:',result['samples'],'samples;',len(result['changes']),'state/node/seat changes; no pause safety conclusion.')
