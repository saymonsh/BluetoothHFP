"""Print the SCO/role-related HCI traffic from a BTstack PacketLogger file (hci_dump.pklg)."""
import struct
import sys

OPS = {0x0428: "Setup_Sync", 0x0429: "Accept_Sync", 0x043D: "Enh_Setup_Sync", 0x043E: "Enh_Accept_Sync",
       0x042A: "Reject_Sync", 0x080B: "Switch_Role", 0x0C26: "Write_Voice_Setting"}
EVENTS = {0x2C: "Sync_Conn_Complete", 0x0F: "Command_Status", 0x12: "Role_Change", 0x04: "Connection_Request",
          0x14: "Mode_Change", 0x03: "Connection_Complete", 0x05: "Disconnection_Complete"}

data = open(sys.argv[1], "rb").read()
pos = 0
while pos + 13 <= len(data):
    length = struct.unpack(">I", data[pos:pos + 4])[0]
    kind = data[pos + 12]
    payload = data[pos + 13:pos + 4 + length]
    pos += 4 + length
    if kind == 0x00 and len(payload) >= 3:
        op = payload[0] | payload[1] << 8
        if op in OPS:
            print(f"CMD {OPS[op]:<18} {payload[3:].hex(' ')}")
    elif kind == 0x01 and len(payload) >= 2:
        code = payload[0]
        if code == 0x0F and len(payload) >= 6:
            op = payload[4] | payload[5] << 8
            if op in OPS:
                print(f"EVT Command_Status     {OPS[op]} status=0x{payload[2]:02x}")
        elif code in EVENTS and code != 0x0F:
            print(f"EVT {EVENTS[code]:<18} {payload[2:].hex(' ')}")
