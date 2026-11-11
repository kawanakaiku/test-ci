import socket, json, sys, time
s=socket.socket(socket.AF_UNIX); s.connect('' + __import__('os').environ.get('QMP_SOCK','/dev/shm/winvhd/qmp.sock') + ''); f=s.makefile('rw')
f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute":c,"arguments":a})+"\n"); f.flush()
    while True:
        r=json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
if sys.argv[1]=='shot': print(cmd('screendump', filename=sys.argv[2], format='ppm'))
elif sys.argv[1]=='keys':
    for k in sys.argv[2:]:
        print(cmd('send-key', keys=[{"type":"qcode","data":x} for x in k.split('+')])); time.sleep(0.3)
elif sys.argv[1]=='click':
    x=int(float(sys.argv[2])*32767); y=int(float(sys.argv[3])*32767)
    ev=[{"type":"abs","data":{"axis":"x","value":x}},{"type":"abs","data":{"axis":"y","value":y}}]
    print(cmd('input-send-event', events=ev)); time.sleep(0.2)
    print(cmd('input-send-event', events=[{"type":"btn","data":{"down":True,"button":"left"}}])); time.sleep(0.1)
    print(cmd('input-send-event', events=[{"type":"btn","data":{"down":False,"button":"left"}}]))
elif sys.argv[1]=='type':
    import itertools
    for ch in sys.argv[2]:
        m={' ':'spc','.':'dot',',':'comma','/':'slash','\\\\':'yen','-':'minus',':':'apostrophe',';':'semicolon','"':'shift+2','=':'shift+minus','_':'shift+ro','%':'shift+5','(':'shift+8',')':'shift+9','&':'shift+6','>':'shift+dot','<':'shift+comma','|':'shift+yen','*':'shift+apostrophe','+':'shift+semicolon',"'":'shift+7','@':'bracketleft','^':'equal','!':'shift+1','~':'shift+equal','[':'bracketright',']':'backslash'}
        k=m.get(ch, ('shift+'+ch.lower()) if ch.isupper() else ch)
        cmd('send-key', keys=[{"type":"qcode","data":x} for x in k.split('+')]); time.sleep(0.05)
    cmd('send-key', keys=[{"type":"qcode","data":"ret"}])
else: print(cmd(sys.argv[1]))
