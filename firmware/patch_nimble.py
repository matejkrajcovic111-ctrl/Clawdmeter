"""PlatformIO pre-build script: patch NimBLE-Arduino's ble_gap.c (idempotent).

Why: as a peripheral, NimBLE reads the central's version/features right after
connecting and only reports the connection when that finishes. On a bonded
reconnect Windows starts encryption at the same moment; on some adapters
(seen with a CSR-style USB dongle) the two collide and the link dies with
HCI 0x08. Result: after any board reboot Windows could not reconnect until
the user re-paired. We report the connection immediately instead.
"""
import os

Import("env")  # noqa: F821  (provided by PlatformIO)

MARK = "clawdmeter2: report the connection now"
OLD = """    if (evt->role == BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE) {
        ble_gap_rd_rem_ver_tx(evt->connection_handle);
    } else {"""
NEW = """    if (evt->role == BLE_HCI_LE_CONN_COMPLETE_ROLE_SLAVE) {
        /* clawdmeter2: report the connection now instead of after a
         * peripheral-initiated remote version/feature read; that read
         * collides with a bonded central's immediate LL_ENC_REQ on some
         * Windows adapters and kills the link (HCI 0x08). */
        ble_gap_event_connect_call(evt->connection_handle, 0);
        slave_conn[evt->connection_handle] = 1;
    } else {"""

path = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),  # noqa: F821
                    "NimBLE-Arduino", "src", "nimble", "nimble", "host", "src", "ble_gap.c")
if os.path.isfile(path):
    with open(path, encoding="utf-8", newline="") as f:
        src = f.read()
    if MARK in src:
        pass
    elif src.count(OLD) == 1:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(OLD, NEW))
        print("patch_nimble: patched " + path)
    else:
        print("*" * 70 + "\nWARNING patch_nimble: ble_gap.c changed upstream, NOT patched;\n"
              "Windows may need a re-pair after board reboots. Update patch_nimble.py.\n" + "*" * 70)
