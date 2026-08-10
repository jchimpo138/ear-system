import socket
import sys

UDP_PORT = 4210

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Enable address reuse
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', UDP_PORT))

    print("==================================================")
    print(f"📡 [WIRELESS UDP LOG LISTENER] RUNNING ON PORT {UDP_PORT}")
    print("   Listening for untethered ESP32 logs over Wi-Fi...")
    print("   Press Ctrl+C to stop.")
    print("==================================================\n")

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            message = data.decode('utf-8', errors='replace')
            sys.stdout.write(message)
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n\nStopped listener.")
    finally:
        sock.close()

if __name__ == '__main__':
    main()
