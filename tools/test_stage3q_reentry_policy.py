# Pure policy simulation for Stage3Q's generic selector. This does not model
# memory reads; it pins winner/retention semantics once each title's read-only
# probe has reported fresh activity.
NONE=0; H3=1; ODST=2; REACH=3; H4=4; CE=5; H2=6; UNKNOWN=7
SUPPORTED={H3,ODST,REACH,H4,H2}

def resolve(modules, retained=NONE, present_hint=NONE, fresh_activity=()):
    if present_hint in SUPPORTED and present_hint in modules:
        return present_hint
    fresh=[t for t in fresh_activity if t in SUPPORTED and t in modules]
    if len(fresh)==1:
        return fresh[0]
    if retained in SUPPORTED and retained in modules:
        return retained
    return NONE

def check(name, got, want):
    assert got==want, f'{name}: got {got}, wanted {want}'
    print('PASS',name,'->',got)

mods={H3,ODST,REACH,H4,H2}
check('initial multi-resident ambiguity fails closed', resolve(mods), NONE)
check('fresh Reach activity acquires Reach', resolve(mods,fresh_activity=[REACH]), REACH)
check('static next sample retains selected Reach raw adapter', resolve(mods,retained=REACH), REACH)
check('fresh H3 uniquely preempts retained Reach', resolve(mods,retained=REACH,fresh_activity=[H3]), H3)
check('simultaneous H3+H4 activity does not choose a new winner', resolve(mods,retained=H3,fresh_activity=[H3,H4]), H3)
check('once old H3 goes stale, fresh H4 preempts', resolve(mods,retained=H3,fresh_activity=[H4]), H4)
check('fresh H2 singleton activity preempts H4', resolve(mods,retained=H4,fresh_activity=[H2]), H2)
check('Present caller hint can immediately select Reach', resolve(mods,retained=H3,present_hint=REACH), REACH)
check('unsupported CE hint is ignored', resolve(mods|{CE},retained=H3,present_hint=CE), H3)
check('removed retained module cannot be retained', resolve({REACH,H4},retained=H3), NONE)
print('Stage3Q re-entry policy simulation PASS')
