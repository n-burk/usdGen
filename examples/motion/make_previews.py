"""Assemble captured Storm frames and validate trace_playback logs (requires Pillow).

Capture with check_animated_sphere.py first. Run bin/trace_playback.ps1 for each
scene with -Frames 1:100 -Loops 1 -Pull and redirect stdout/stderr to
build/motion_<scene>_trace.log.
"""
import collections
import json
from pathlib import Path
import re
from PIL import Image

HERE=Path(__file__).resolve().parent
BUILD=HERE.parents[1]/'build'
NAMES=('felt_sphere_animated','felt_sphere_groom_outside_xform',
       'braid_animated_guides','braid_animated_guides_and_surface',
       'braid_simulated_center_curve','two_curves_ptex_regions',
       'two_braids_ptex_regions')


def main():
    result={'renderer':'HdStormRendererPlugin','frames':100,'scenes':{}}
    for name in NAMES:
        log=(BUILD/('motion_'+name+'_trace.log')).read_text(encoding='utf-8-sig')
        assert 'commit rejected' not in log
        assert 'Warning' not in log
        rows=[line.split() for line in log.splitlines()
              if re.match(r'^\s+1\s+\d+\s+\d+\.\d+\s+',line)]
        assert len(rows)==100
        assert [int(r[1]) for r in rows]==list(range(1,101))
        point_counts={int(r[-2]) for r in rows}
        assert len(point_counts)==1 and next(iter(point_counts))>0
        assert len({r[-1] for r in rows})>20
        operators={}
        for line in log.splitlines():
            m=re.match(r'usdGen schedule\s+(\w+)\s+\w+\s+(.+?)\s+prepare.*?eval\s+(\d+)/(\d+)',line)
            if not m: continue
            op,reason,evaluated,total=m.groups()
            counts=operators.setdefault(op,collections.Counter())
            counts[reason]+=1
            if evaluated=='0': counts['zeroChunkEvaluations']+=1
        static=('scatter','grow','curl','width') if name.startswith('felt_sphere') else ('roots','guideGrow','width')
        for op in static:
            assert operators[op]['no capture yet']==1,(name,op,operators[op])
            assert operators[op]['capture reused']==99,(name,op,operators[op])
            assert operators[op]['zeroChunkEvaluations']==99,(name,op,operators[op])
        files=[BUILD/'traces'/name/'frames'/('%03d.png'%f) for f in range(1,101)]
        frames=[Image.open(p).convert('RGB') for p in files]
        frames[0].save(HERE/(name+'.gif'),save_all=True,append_images=frames[1:],
                       duration=42,loop=0,optimize=False)
        frames[25].save(HERE/(name+'.png'))
        for frame in frames: frame.close()
        result['scenes'][name]={'controlVertices':next(iter(point_counts)),
            'strands':next(iter(point_counts))//(8 if name.startswith('felt_sphere') else 64),
            'capturedViewportFrames':len(files),'rejectedCooks':0,'operators':operators}
    (HERE/'motion_validation.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v['controlVertices'] for k,v in result['scenes'].items()}))


if __name__=='__main__': main()
