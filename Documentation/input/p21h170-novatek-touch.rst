EEBBK A7 (P21H170) Novatek touch port
=====================================

Problem and evidence
--------------------

At repository base d2eca349462371b083958584049a1601d675fb62,
ums512-p21h170_defconfig enables FTS/Goodix but contains no Novatek driver.
The supplied A7 runtime log instead records NVT-ts bound at i2c-3/0x62
and lcd_name=lcd_nt36532_tianma_mipi_fhd_bbk_H170. The H170 board DTS
only describes FTS and other alternative controllers. The DTS Makefile
builds the H180 overlay; the previously unlisted H170 overlay is now
also built. Both existing overlays include the shared NVT touch node.
The base tree's existing vbmeta node receives its missing label so the
overlays' pre-existing &vbmeta reference can resolve. Its path and
properties are unchanged.

This patch is a source port of the finger-touch protocol and lifecycle,
not recovered original C source or a binary driver transplanted into C.

Sources
-------

Protocol sources (GPL notices retained in the new driver/header):

* LineageOS/android_kernel_xiaomi_sdm660,
  365e6cbdef02a80f31b5b0f9629b07a3701e98dc,
  drivers/input/touchscreen/nt36xxx/nt36xxx.c and nt36xxx.h:
  I2C addresses/transactions, six-byte finger records, pressure extension,
  event offsets and reset polling.
* MiCode/Xiaomi_Kernel_OpenSource,
  77dc8e2290f3f5f15861821564f68d30ea193449 (pipa-t-oss),
  drivers/input/touchscreen/nt36532/nt36xxx_mem_map.h and nt36xxx.c:
  NT36532 trim ID, reset/CRC addresses, cascade selection and FW info.
  That platform uses SPI. Its transport/firmware loader is not imported.
* Supplied A7_boot_touchfix_KSU_SUSFS.img,
  SHA256 f9de1c2e5568b536e606504543364f5de4af8ee706a9586d7682dc553f6bc895:
  raw kernel offsets 0xc09b50 (register tuples), 0xc09f68 (NT36532 trim
  table entry), 0xc0a0cc/0xc0a100 (single/cascade maps), and the NVT
  initialization/display callback disassembly. The image uses the I2C
  command fallback for the two legacy register maps, rather than the
  SPI reset addresses used by the MiCode driver.

The first 13 fields of both NT36532 memory maps, cascade bit at 0x1fb12c,
new reset address 0x1fb43e, trim address 0x1fb104 and CRC register
0x1fb535 were cross-checked against the supplied working image.

Behavior and boundaries
-----------------------

The driver is built in with CONFIG_TOUCHSCREEN_NT36XXX=y and uses a
late_initcall. It rejects a known non-NVT panel before requesting shared
GPIOs. It uses the existing panel firmware and reads the valid firmware
coordinate range. No FTS coordinate constants or screen-size guesses
are used. A malformed firmware response does not create an input device.

The probe registers the Spreadtrum display notifier and performs an
initial attempt, covering a display-on event that already happened.
On a transient failure it remains bound, retries at most five times,
and can retry again on a later DISPC_POWER_ON. Successful repeated
power-on notifications do not allocate another input device or IRQ.

DISPC_POWER_OFF disables/synchronizes the IRQ before taking the I2C
mutex, sends sleep to the selected event buffer, and releases contacts.
DISPC_POWER_ON waits for firmware readiness and resets/retries if needed.
Removal unregisters the notifier, cancels pending work synchronously,
then releases IRQ and input resources. Lock ordering avoids waiting for
the IRQ while holding the IRQ thread's I2C mutex.

Scope: finger multi-touch (10 slots), display sleep/wake, identification
and read-only status. Pen, wake gestures, factory MP/raw-data interfaces
and firmware flashing are not implemented. This is not a claim of feature
parity with the factory pen driver. Existing FTS panels retain their
driver. No camera, charging, audio, KSU or SUSFS code changes are included.

Device-tree caveats
-------------------

The added GPIO numbers come from the repository's existing FTS shared
touch wiring: reset AP GPIO 145, interrupt EIC async 0. The supplied
runtime logs prove the NVT bus/address and IRQ thread, but do not dump
the NVT GPIO properties. Verify that wiring on the target hardware.
Both candidates must not request the same pins at once.

This patch keeps the existing panel commands/timings. The repository's
H170 overlay lists FTS/other panels and lacks a Tianma NT36532 LCD
definition; the old runtime log names that Tianma panel. The user confirms
that display works with the repository-built DTBO, with only touch
failing. No new LCD command table is added; the working display
configuration must be preserved when integrating this touch port.

The DTBO files produced by ``make dtbs`` are individual overlays, not
Android DT table partition images. Preserve the bootloader's board IDs
and entry ordering when packaging a DTBO partition. A stock DTBO that
already supplies the NVT node can be used with the built-in driver.

Validation and device checks
----------------------------

Host protocol test::

    bash tools/testing/p21h170-touch/run.sh

Use the repository's existing P21H170 build/toolchain for release.
Compile checks are not evidence of operation on a physical A7.

After boot, with root, inspect::

    cat /sys/bus/i2c/devices/3-0062/nvt_status
    readlink /sys/bus/i2c/devices/3-0062/driver
    cat /proc/bus/input/devices
    getevent -lp
    dmesg | grep -iE 'NVT|NT36532|touch'

``ready=1 awake=1 irq_enabled=1 last_error=0`` while the display is on
indicates successful initialization. ``ready=0`` with a bound NVT-ts
driver alone is insufficient. Check taps, all screen edges, dragging,
multiple fingers, cold boot, repeated screen off/on, and unbind/rebind.
Record nvt_status and kernel logs if initialization/resume fails. The
provided collect.sh gathers status without resetting or flashing hardware.
