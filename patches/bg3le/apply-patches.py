#!/usr/bin/env python3
"""Apply the runtime patches only to their exact pinned source versions (BG3LE_SRC).

Patches apply in lock order. A file may be touched by several patches; a later patch's
before_sha256 is the earlier one's after_sha256. A target already at a later patch's
after_sha256 counts as applied for the earlier patch.
"""
from pathlib import Path
import hashlib,json,os,subprocess
here=Path(__file__).resolve().parent
patches=json.loads((here/'build.lock.json').read_text())['runtime_patches']
source=Path(os.environ.get('BG3LE_SRC') or here.parents[1]/'build/src/bg3le')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
final={}
for i,lock in enumerate(patches):
    patch=here/Path(lock['file']).name
    assert sha(patch)==lock['sha256'],lock['file']
    targets=lock['targets']
    later=[{t['after_sha256'] for l in patches[i+1:] for t in l['targets'] if t['path']==x['path']} for x in targets]
    current=[sha(source/t['path']) for t in targets]
    done=[c==t['after_sha256'] or c in l for c,t,l in zip(current,targets,later)]
    if not all(done):
        # All targets must be pristine; a partly applied or edited source is refused.
        assert not any(done) and current==[t['before_sha256'] for t in targets],'Source changed outside '+lock['file']
        subprocess.run(['git','-C',str(source),'apply','--check',str(patch)],check=True)
        subprocess.run(['git','-C',str(source),'apply',str(patch)],check=True)
        assert [sha(source/t['path']) for t in targets]==[t['after_sha256'] for t in targets],lock['file']
    for t in targets:
        final[t['path']]=t['after_sha256']
    print('Verified',lock['file'],'-',lock['purpose'])
for path,want in final.items():
    assert sha(source/path)==want,'Final state differs: '+path
