import serial, time, sys

PORT = "COM10"
try:
    p = serial.Serial(PORT, 115200, timeout=0.1)
except Exception as e:
    print("OPEN_FAIL", e)
    sys.exit(1)

# Hard-reset the ESP32-S3 (RTS->EN). DTR low = GPIO0 high = normal app boot.
p.setDTR(False)
p.setRTS(True)   # EN low: assert reset
time.sleep(0.2)
p.setRTS(False)  # release: boot the app
buf = bytearray()
end = time.time() + 25
while time.time() < end:
    d = p.read(4096)
    if d:
        buf.extend(d)
p.close()

with open(r"C:\Users\stone\momentum-mob\boot_capture.log", "wb") as f:
    f.write(buf)

txt = buf.decode("utf-8", "replace")
print("CAPTURED", len(buf), "bytes")
# echo lines mentioning storage / SD / mount / partition / keystore
for line in txt.splitlines():
    low = line.lower()
    if any(k in low for k in ("storage", "sd ", "virtual sd", "mount", "partition", "wl_", "keystore", "fatfs", "f_mkfs", "littlefs", "animation", "scene", "popup", "enclave", "mismatch", "fault", "belong", "crypto", "desktop", "select", "database", "factory", "resource", "manifest", "dialog")):
        print("LOG>", line.strip())
