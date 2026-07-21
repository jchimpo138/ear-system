# Direct serial read test - bypasses PlatformIO monitor entirely
import serial
import time
import sys

port = "COM8"
baud = 115200

print(f"Opening {port} at {baud} baud...")
try:
    ser = serial.Serial(port, baud, timeout=2)
    print(f"Connected! Waiting for data...")
    
    start = time.time()
    while time.time() - start < 15:  # Read for 15 seconds
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting)
            print(f"[RAW BYTES] {data}")
            print(f"[DECODED]   {data.decode('utf-8', errors='replace')}")
        else:
            elapsed = int(time.time() - start)
            print(f"  ...waiting ({elapsed}s, no data yet)")
            time.sleep(1)
    
    ser.close()
    print("Done. No more data after 15 seconds.")
except serial.SerialException as e:
    print(f"ERROR: {e}")
except KeyboardInterrupt:
    print("Interrupted.")
