import sys
import time
from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementPublisher,
    BluetoothLEManufacturerData,
    BluetoothLEAdvertisementPublisherStatus
)
from winrt.windows.storage.streams import DataWriter

def on_status_changed(publisher, args):
    status = args.status
    if status == BluetoothLEAdvertisementPublisherStatus.STARTED:
        print("   ✅ Radio Status: ACTIVE & BROADCASTING!")
    elif status == BluetoothLEAdvertisementPublisherStatus.ABORTED:
        print("   ❌ Radio Status: ABORTED (Your PC's BT driver blocked the custom packet).")
    elif status == BluetoothLEAdvertisementPublisherStatus.STOPPED:
        print("   ⏹️ Radio Status: STOPPED.")

COMMANDS = {
    "1": ("E100E908000ED2557C007CB0", "Magenta Glow (E9 08)"),
    "2": ("E100E90500090E13A0",     "Orange Spin (E9 05)"),
    "3": ("E200E90600220F444170",   "Dual Purple/Blue Flash (E9 06)"),
}

def broadcast_packet(hex_data, description="Show Packet"):
    publisher = BluetoothLEAdvertisementPublisher()
    publisher.add_status_changed(on_status_changed)
    
    writer = DataWriter()
    payload_bytes = bytes.fromhex(hex_data)
    writer.write_bytes(payload_bytes)
    
    manufacturer_data = BluetoothLEManufacturerData()
    manufacturer_data.company_id = 0x0183  # Disney Company ID
    manufacturer_data.data = writer.detach_buffer()
    
    publisher.advertisement.manufacturer_data.append(manufacturer_data)
    
    print(f"\n📡 Requesting Broadcast: {description}...")
    print(f"   Payload: 0183{hex_data}")
    print("   (Press Ctrl+C to stop)\n")
    
    publisher.start()
    
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        publisher.stop()
        print("\n⏹️ Broadcast Stopped.")

if __name__ == "__main__":
    print("=== DISNEY SHOW BEACON CONTROLLER ===")
    print("1: Magenta Glow")
    print("2: Orange Spin")
    print("3: Dual Flash")
    
    choice = input("\nSelect command to transmit (1-3): ").strip()
    if choice in COMMANDS:
        hex_str, desc = COMMANDS[choice]
        broadcast_packet(hex_str, desc)