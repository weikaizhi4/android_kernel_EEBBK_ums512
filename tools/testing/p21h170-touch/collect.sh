#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run with root on the device. Reads diagnostics only.
umask 077
touch_tag="$(date +%Y%m%d_%H%M%S)_$$"
touch_dir="/sdcard/A7_touch_diag_${touch_tag}"
mkdir -p "$touch_dir" || exit 1
cat /proc/cmdline > "$touch_dir/cmdline.txt" 2>&1
cat /proc/bus/input/devices > "$touch_dir/input_devices.txt" 2>&1
cat /proc/interrupts > "$touch_dir/interrupts.txt" 2>&1
getevent -pl > "$touch_dir/getevent_caps.txt" 2>&1
dmesg > "$touch_dir/dmesg.txt" 2>&1
{
    for touch_dev in /sys/bus/i2c/devices/*-0062 /sys/bus/i2c/devices/*-0038; do
        [ -d "$touch_dev" ] || continue
        echo "--- $touch_dev ---"
        cat "$touch_dev/name" 2>/dev/null
        readlink "$touch_dev/driver" 2>/dev/null
        cat "$touch_dev/nvt_status" 2>/dev/null
        for touch_prop in compatible reg novatek,reset-gpio \
                novatek,irq-gpio interrupts; do
            touch_file="$touch_dev/of_node/$touch_prop"
            [ -f "$touch_file" ] || continue
            echo "$touch_prop (hex bytes, DT cells are big-endian):"
            od -An -tx1 "$touch_file"
        done
    done
} > "$touch_dir/i2c_touch.txt" 2>&1
cat /sys/kernel/debug/gpio > "$touch_dir/gpio.txt" 2>&1
tar -czf "$touch_dir.tar.gz" -C /sdcard "${touch_dir##*/}" || exit 1
echo "Saved $touch_dir.tar.gz"
