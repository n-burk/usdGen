from pxr import Usd
s = Usd.Stage.Open('scene_100k_B.usdc')
prim = s.GetPrimAtPath('/World/Hair')
for name in ('hairTangent', 'primvars:hairTangent'):
    a = prim.GetAttribute(name)
    print(repr(name), '-> valid:', bool(a), 'name:', a.GetName() if a else None)
# also check default type
a = prim.GetAttribute('primvars:hairTangent')
if a:
    v = a.Get()
    print('len:', len(v), 'first:', v[0] if len(v) else None)
