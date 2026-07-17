# Siflower Linux kernel

This is a Linux kernel tree with out-of-tree patches adding support for
Siflower SoCs. The target used here is the NOR-boot Banana Pi BPI-RV2 with
an SF21H8898 SoC.

## Reference source trees

- Vendor SDK kernel:
  `/home/gch981213/src/siflower/h8898/Openwrt-master/sf_kernel/linux-5.10`
- Vendor Ethernet driver:
  `/home/gch981213/src/siflower/h8898/Openwrt-master/package/kernel/siflower/sf_eth`
- Alpine rootfs, initramfs, and FIT-image material:
  `/home/gch981213/src/kernel/bpi-rv2-its`
- OpenWrt FIT ITS generator:
  `/home/gch981213/src/openwrt/openwrt/scripts/mkits.sh`

Use the vendor trees as references only unless a task explicitly asks for
changes there.

## Build the kernel

The working tree's `.config` is the board configuration. Build `Image` and
the DTBs from the repository root with:

```sh
CCACHE_DISABLE=1 make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j$(nproc)
```

The required outputs are:

```text
arch/riscv/boot/Image
arch/riscv/boot/dts/siflower/sf21h8898_bananapi_bpi-rv2-nor.dtb
```

Always package the current `-nor.dtb`. Do not use the stale
`sf21h8898_bananapi_bpi-rv2.dtb`, which is no longer built by the Siflower
DTB Makefile.

## Build and deploy kernel.itb

The Alpine initramfs is
`../bpi-rv2-its/initrd.cpio.zst`. Build an external-data FIT image and copy it
to the local TFTP root with:

```sh
KERNEL_DIR=$PWD
ITB_DIR=$(realpath ../bpi-rv2-its)
KERNEL_VERSION=$(make -s ARCH=riscv kernelversion)

gzip -c -n "$KERNEL_DIR/arch/riscv/boot/Image" > "$ITB_DIR/kernel.Image.gz"

"$HOME/src/openwrt/openwrt/scripts/mkits.sh" \
  -D bananapi_bpi-rv2-nor \
  -o "$ITB_DIR/kernel.bin.its" \
  -k "$ITB_DIR/kernel.Image.gz" -C gzip \
  -d "$KERNEL_DIR/arch/riscv/boot/dts/siflower/sf21h8898_bananapi_bpi-rv2-nor.dtb" \
  -i "$ITB_DIR/initrd.cpio.zst" \
  -a 0x20000000 -e 0x20000000 \
  -c config-1 -A riscv -v "$KERNEL_VERSION"

mkimage -E -B 0x1000 -p 0x1000 \
  -f "$ITB_DIR/kernel.bin.its" "$ITB_DIR/kernel.itb"
install -m 0644 "$ITB_DIR/kernel.itb" /var/lib/tftpboot/kernel.itb
restorecon -Rv /var/lib/tftpboot/kernel.itb 2>/dev/null || true
dumpimage -l /var/lib/tftpboot/kernel.itb
```

`dumpimage` can print `Truncated file` for this external-data FIT layout while
still listing all images correctly. Confirm that it lists the kernel,
initramfs, and the current NOR DTB.

## BPI-RV2 host setup

Hardware connections and host resources:

- Relay channel 1 controls board power through
  `~/src/device_specific_stuff/relay.py`; the relay is `/dev/ttyUSB0`.
- The board console is `/dev/ttyACM0`, 115200 8N1.
- Board `eth0` (QSGMII port 0) connects to host `enp4s0`.
- Board `eth5` (RGMII) connects to host `eno1`.
- Use the existing NetworkManager connections only:
  `enp4s0-shared` on `enp4s0` and `eno1-192-168-1` on `eno1`.
- `enp4s0-shared` gives the host `10.42.0.1/24`; U-Boot normally uses
  `10.42.0.2` and TFTP server `10.42.0.1`.
- `eno1-192-168-1` gives the host `192.168.1.253/24`. Its profile is not
  bound to an interface, so always activate it with `ifname eno1`.

The boot script activates both existing connections with explicit interface
names. To do it manually:

```sh
nmcli connection up eno1-192-168-1 ifname eno1
nmcli connection up enp4s0-shared ifname enp4s0
ip -brief address show dev eno1
ip -brief address show dev enp4s0
```

The system TFTP socket must be active and UDP TFTP must be allowed into the
`nm-shared` firewalld zone:

```sh
systemctl is-active tftp.socket
ss -lunp | grep ':69 '
```

Starting the system socket or changing permanent firewalld rules may require
host administrator authentication. Do not create a replacement NetworkManager
connection. A local TFTP download can be checked with:

```sh
curl --fail --max-time 20 tftp://10.42.0.1/kernel.itb -o /tmp/kernel.itb.test
cmp /tmp/kernel.itb.test /var/lib/tftpboot/kernel.itb
```

Close `gtkterm`, `minicom`, or other programs using `/dev/ttyACM0` before
running automation; the script takes an exclusive serial lock.

## Automated TFTP boot and serial interaction

`tools/bpi-rv2-tftpboot.py` uses pyserial and performs this sequence:

1. Activates `eno1-192-168-1` on `eno1` and `enp4s0-shared` on `enp4s0`.
2. Powers the board off, opens `/dev/ttyACM0`, and powers it on.
3. Detects `Hit any key to stop autoboot` and interrupts U-Boot.
4. Runs `setenv ipaddr 10.42.0.2`, `setenv serverip 10.42.0.1`,
   `tftpboot kernel.itb`, and `bootm`.
5. Streams serial output for `--timeout` seconds, optionally saves a raw log,
   and can log in to Alpine and run a command.

Stream boot logs for two minutes and save them:

```sh
./tools/bpi-rv2-tftpboot.py \
  --timeout 120 --log /tmp/bpi-rv2-boot.log
```

Perform a full boot/login test. Alpine uses user `root`, password `123456`;
these are the script defaults. With `--command`, the script exits with the
remote command's status:

```sh
./tools/bpi-rv2-tftpboot.py \
  --timeout 120 \
  --log /tmp/bpi-rv2-boot.log \
  --command 'uname -a; cat /etc/alpine-release; ip addr show'
```

Attach to an already running board without changing power or networking:

```sh
./tools/bpi-rv2-tftpboot.py --monitor-only --timeout 60
```

Run a command on an already running Alpine console:

```sh
./tools/bpi-rv2-tftpboot.py \
  --monitor-only --timeout 30 --command 'dmesg | tail -50'
```

Use `--timeout 0` to stream forever. Use `--skip-network` when the two host
connections are already active and must not be reactivated. After many rapid
power cycles, U-Boot may reuse a stale TFTP/conntrack tuple and print repeated
`T` timeouts. Wait for the old flow to expire and retry, or temporarily select
another unused address in `10.42.0.0/24`, for example:

```sh
./tools/bpi-rv2-tftpboot.py --board-ip 10.42.0.3 --timeout 120
```

The automation treats FIT hash failures and failed U-Boot image loads as
errors. U-Boot's two `reserving fdt memory region failed` messages are known
to appear before a successful load on this board; rely on the later FIT hash
checks and `Starting kernel` line to determine success.

Always power the board off after finishing the work:

```sh
~/src/device_specific_stuff/relay.py 1 off
```
