#!/usr/bin/env python3
"""Own the Pharos serial port: log everything, inject commands from a file."""
import os, time, datetime, serial
PORT='/dev/cu.usbmodem2101'; LOG='/tmp/pharos_live.log'; CMD='/tmp/pharos_cmd'
def stamp(m,fh):
    fh.write("\n===== %s @ %s =====\n"%(m,datetime.datetime.now().strftime('%H:%M:%S'))); fh.flush()
fh=open(LOG,'a',buffering=1); stamp('logger started',fh)
ser=None
while ser is None:
    try: ser=serial.Serial(PORT,115200,timeout=0.2)
    except Exception: time.sleep(1)
stamp('port opened',fh)
if os.path.exists(CMD): os.remove(CMD)
buf=b''
while True:
    try: data=ser.read(4096)
    except Exception: stamp('port lost',fh); break
    if data:
        buf+=data
        while b'\n' in buf:
            line,buf=buf.split(b'\n',1)
            fh.write(line.decode('utf-8','replace').rstrip('\r')+'\n')
    if os.path.exists(CMD):
        try:
            c=open(CMD).read().strip(); os.remove(CMD)
        except Exception: c=''
        if c:
            stamp('sending: '+c,fh); ser.write((c+'\r\n').encode()); ser.flush()
