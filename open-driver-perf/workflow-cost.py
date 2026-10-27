import json, glob, os, collections
base="/Users/fadhlisyukri/.claude/projects/-Users-fadhlisyukri-Desktop-Projects/79ac0184-53f2-4e7e-913c-4091ecfee1cb/subagents/workflows"
agg=collections.defaultdict(lambda:[0,0,0,0,0])
for f in glob.glob(base+"/*/agent-*.jsonl"):
    wf=f.split("/")[-2]
    meta=json.load(open(f.replace(".jsonl",".meta.json")))
    label=(meta.get("description") or meta.get("label") or "?")
    stage=label.split(":")[0]
    model=None; inp=cr=out=0
    for line in open(f):
        try: d=json.loads(line)
        except: continue
        m=d.get("message") or {}; u=m.get("usage")
        if u and d.get("type")=="assistant":
            inp+=u.get("input_tokens",0)+u.get("cache_creation_input_tokens",0); cr+=u.get("cache_read_input_tokens",0); out+=u.get("output_tokens",0); model=m.get("model")
    a=agg[(wf[:11],stage,(model or "")[7:])]; a[0]+=1; a[1]+=inp; a[2]+=cr; a[3]+=out
tot=collections.defaultdict(lambda:[0,0,0,0])
for k,v in sorted(agg.items()):
    print("%-12s %-12s %-12s n=%d new_in=%6.2fM cache_read=%7.1fM out=%5.0fk"%(k[0],k[1],k[2],v[0],v[1]/1e6,v[2]/1e6,v[3]/1e3))
    t=tot[k[0]]; t[0]+=v[0]; t[1]+=v[1]; t[2]+=v[2]; t[3]+=v[3]
print()
for k,v in tot.items(): print(k,"agents=%d new_in=%.1fM cache_read=%.1fM out=%.0fk"%(v[0],v[1]/1e6,v[2]/1e6,v[3]/1e3))
