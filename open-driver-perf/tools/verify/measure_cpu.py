import os,resource,subprocess,sys,time
env=dict(os.environ); env['VK_ICD_FILENAMES']=sys.argv[1]; env['PVR_I_WANT_A_BROKEN_VULKAN_DRIVER']='1'
args=sys.argv[2:]
t0=time.time(); p=subprocess.run(args,env=env,capture_output=True,text=True); t1=time.time()
r=resource.getrusage(resource.RUSAGE_CHILDREN)
out=[l for l in (p.stdout+p.stderr).splitlines() if 'ms/frame' in l]
print("wall %.3fs | cpu(user+sys) %.3fs (%.1f%%) | %s"%(t1-t0, r.ru_utime+r.ru_stime, 100*(r.ru_utime+r.ru_stime)/(t1-t0), out[-1] if out else 'no output'))
