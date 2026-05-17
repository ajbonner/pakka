#!/bin/sh
# Boot the NetBSD 3.0/sparc install CD under qemu-system-sparc and
# step the operator through sysinst. Creates a fresh qcow2 target
# disk at sd0; the install media at sd2 (CD-ROM). After sysinst
# completes, the qcow2 is bootable on its own — run-vm.sh.
#
# NetBSD 3.0's sysinst pre-dates scripted-install (`-j` came in
# NetBSD 6). The install is interactive but short — most prompts
# accept defaults. Inline hints print at start.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=${PAKKA_NBSD_WORKDIR:-/private/tmp/pakka-netbsd-sparc}
DISK_SIZE=${PAKKA_NBSD_DISK_SIZE:-2G}
RAM_MB=${PAKKA_NBSD_RAM:-256}
SSH_PORT=${PAKKA_NBSD_SSH_PORT:-10022}

"$SCRIPT_DIR/fetch-iso.sh"

ISO="$WORKDIR/iso/sparccd-3.0.iso"
TARGET_DISK="$WORKDIR/netbsd-sparc.qcow2"

if [ ! -f "$ISO" ]; then
    echo "Missing $ISO" >&2; exit 1
fi

if [ -f "$TARGET_DISK" ]; then
    echo "==> Target disk $TARGET_DISK already exists; reusing"
else
    echo "==> Creating $TARGET_DISK ($DISK_SIZE)"
    qemu-img create -f qcow2 "$TARGET_DISK" "$DISK_SIZE"
fi

cat <<'NOTES'
== sysinst hints (NetBSD 3.0/sparc) ==

* When the microroot setup utility asks which medium to load install
  utilities from, choose `1) cdrom` and accept the defaults
  (/dev/cd0a, /cdrom/sparc/installation/bootfs/instfs.tgz).
* Terminal type: vt100.
* At the install/halt/shell prompt: `I` (Install).
* Main menu: `a` (Install NetBSD to hard disk), then `b` (Yes).
* Disk: sd0 (the empty 2 GB target). Accept default partition
  sizes — Ctrl-N navigates rows, Enter activates.
* Distribution: `a` (Full installation — from CD it's fast).
* Install source: `a` (CD-ROM / DVD), then `c` (Continue) on
  the defaults.
* Set the root password to "pakka" (the run-build.sh default;
  override via PAKKA_NBSD_ROOT_PASSWORD). NetBSD warns it's
  short but accepts it.
* Root shell: `a` (/bin/sh).
* Hit Enter to leave the post-install configuration screens.
* From the main menu pick `e: Utility menu` → `a: Run /bin/sh`,
  then run these (one line at a time — the install-env shell lacks
  tail/grep and the heredoc form is paste-fragile):

    mount /dev/sd0a /mnt
    echo 'hostname=pakka-netbsd-sparc' >> /mnt/etc/rc.conf
    echo 'sshd=YES'                    >> /mnt/etc/rc.conf
    echo 'dhclient=YES'                >> /mnt/etc/rc.conf
    echo 'dhclient_flags=le0'          >> /mnt/etc/rc.conf
    echo 'PermitRootLogin yes'         >> /mnt/etc/ssh/sshd_config
    umount /mnt
    exit

* From the Utility menu choose `x: Exit`, then `d: Reboot the
  computer`. QEMU exits with -no-reboot; hand off to run-vm.sh.
NOTES

# Target disk at SCSI unit 0 (sd0), CD-ROM at unit 2 (cd0).
# -boot d boots from CD-ROM.
qemu-system-sparc \
    -M SS-5 \
    -m "$RAM_MB" \
    -nographic \
    -drive file="$TARGET_DISK",format=qcow2,if=scsi,bus=0,unit=0,media=disk \
    -drive file="$ISO",format=raw,if=scsi,bus=0,unit=2,media=cdrom,readonly=on \
    -boot d \
    -net nic,model=lance \
    -net user,hostfwd=tcp::"$SSH_PORT"-:22 \
    -no-reboot
qemu_rc=$?
exit "$qemu_rc"
