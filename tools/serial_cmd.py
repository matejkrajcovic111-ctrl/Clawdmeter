"""Talk to the board's serial console: optional reset, send commands, print output.
usage: serial_cmd.py PORT SECONDS [--reset] [cmd ...]"""
import sys, time
import serial

port, secs = sys.argv[1], float(sys.argv[2])
args = sys.argv[3:]
reset = "--reset" in args
cmds = [a for a in args if a not in ("--reset", "--ts")]
s = serial.Serial(None, 115200, timeout=0.1)
s.port = port; s.dtr = False; s.rts = False  # never pulse the USB reset lines
s.open()
if reset:
    # usbser.sys only applies an RTS change on the next DTR write (esptool trick)
    s.dtr = False; s.rts = True; s.dtr = s.dtr; time.sleep(0.1)
    s.rts = False; s.dtr = s.dtr
time.sleep(0.5)
for c in cmds:
    s.write((c + "\n").encode())
    time.sleep(0.4)
t0 = time.time()
end = t0 + secs
buf = b""
ts = "--ts" in args  # prefix each line with seconds since the port opened
while time.time() < end:
    chunk = s.read(4096)
    if ts and chunk:
        stamp = ("[%6.2f] " % (time.time() - t0)).encode()
        chunk = chunk.replace(b"\n", b"\n" + stamp)
    buf += chunk
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))

