# Samsung Galaxy S6 Edge mainline Linux support

Independent experimental port for **Samsung SM-G925I (zerolte), Exynos7420**.
Only this model has been tested. Do not assume that another Galaxy S6 or S6 Edge
variant has identical wiring, firmware, memory layout or boot requirements.
This is not an official Nura device release.

## Scope and validation

The port is based on **Linux v7.2.12**, upstream commit
`cc9de5d4b5ab6459f5774946aa3195ad51236fa8`.
It was developed with LLVM/Clang. Hardware observations below come from the
SYSTEM2 development image and earlier recorded bring-up tests. The publication
cleanup removes obsolete diagnostics and normalizes the source; a successful
build of the cleaned tree does not itself constitute a new hardware test.

This repository contains kernel changes. A bootable installation also needs the
matching device tree, initramfs, uniLoader integration, firmware and userspace
packages. Those components are separate integration work. The current tested
userspace is Nura/postmarketOS v26.06 with OpenRC and Plasma Mobile.

## Implemented kernel changes

### Storage and boot recovery

- Exynos7420-specific UFS matching and quirks, including firmware ownership of
  secure UFSPR registers. Direct access to those registers caused an SError.
- UFS clock-gating ordering before the core clock is disabled, addressing the
  failure observed during shutdown/recovery.
- A board- and device-tree-gated boot supervisor that captures blocked tasks,
  the I2C bus owner and interrupt counts before userspace. An additional explicit
  device-tree opt-in permits one recovery restart if boot remains stuck.
- Removal of experimental framebuffer bars, direct glyph rendering, no-op stage
  markers, the obsolete `/proc/g925i_msg` bridge and temporary probe chatter.

### CPUs, clocks and thermal protection

- APOLLO (Cortex-A53) and ATLAS (Cortex-A57) clock providers using Samsung register
  layouts, bounded mux/divider waits and verified PLL/divider settings.
- Early 500 MHz cluster preparation before SMP; failure retains a single-core
  rescue boot. ATLAS preparation also checks its supply constraint.
- CPU operating points supporting low-frequency operation and controlled boosts.
  Validated rates include A53 200/400/500/1000/1500 MHz and A57
  200/400/500/800/1000 MHz. These are tested points, not an ASV or full-load
  stability qualification for every chip.
- A physical architected-counter workaround for mismatched firmware CNTVOFF
  values across CPUs, including disabling the incompatible direct vDSO path.
- Read-only TMU converter sampling with captured P0 calibration, validity checks,
  and rejection of impossible measurements.
- Automatic CPU frequency limiting and secondary-core parking at 55 C, with
  5 C hysteresis. Invalid sensors trigger conservative cooling. The 85 C critical
  trip requests clean recovery through the rescue userspace helper.
- Thermal recovery arguments have explicit lifetime management for asynchronous
  execution; the action does not wait for the recovery process to finish.

### Battery and temperature telemetry

- Read-only MAX77843 battery/USB power-supply reporting, including battery state,
  capacity, voltage, current and inherited charger settings.
- Exynos7420 ADC support for the firmware-initialized supply arrangement.
- Fresh battery and charger thermistor conversions using board-specific NTC
  tables, with interpolation and open/short/out-of-range rejection.
- No fuel-gauge model programming or active charger configuration is implemented.

### Connectivity

- Staged MAX77843 interrupt unmasking so handlers exist before pending MUIC
  sources can run; this addresses the observed boot-time I2C lockup.
- Explicit LPASS UART routing for the Bluetooth UART, with saved/restored IRQ
  routing state and reset-state validation.
- BCM4358A3 identification and reset/baud-rate ordering in the Broadcom driver.
- A board-specific PCIe WLAN root-complex implementation, Samsung PHY sequencing,
  clocks, endpoint reset, MSI and serialized DBI access.
- An opt-in USB VBUS-valid override needed by the development gadget path.
  This is a board workaround, not a generic cable-detection solution.

### Graphics and input

- A Mali-T760 G3D clock/supply provider for Panfrost, with calibration checks,
  bounded domain/PLL waits and verified 266 MHz / 693750 uV operation.
- GPU domain/rails remain on. GPU DVFS, runtime power-off and suspend are not
  implemented; intermittent scheduler timeouts remain an open issue.
- STM FTS touchscreen events, multitouch slots and separate capacitive Back and
  Recents key events. Firmware upgrades and NVM repair are deliberately absent.
- Touchscreen analog supply support with a checked inherited 3.3 V selector,
  plus an optional fixed-voltage key-backlight supply exposed through LED class.
- Matching device-tree and userspace configuration provide the GPIO Home,
  Power and Volume keys. Desktop navigation behavior is implemented in userspace.

## External userspace integration

The development installation also includes the following integration. These
files and package recipes are **not part of this kernel repository** and must
be reproduced separately rather than copied from the installed phone:

- A persistent ext4 root on USERDATA, OpenRC startup and a BusyBox/USB rescue path.
- Plasma Mobile 6.6.6 on native Wayland/KWin with Panfrost rendering, 3x display
  scaling, a working on-screen keyboard and cursor hiding during touch use.
- A dark AMOLED-friendly theme and automatic screen blanking after two minutes,
  with touch wake. This is framebuffer blanking, not electrical panel power-off.
- NetworkManager, BlueZ and restricted desktop policies so the mobile user can
  control Wi-Fi/Bluetooth. Wi-Fi reconnect restores the address-bound SSH listener;
  USB SSH remains a separate rescue channel. SSH password login stays disabled.
- Physical-key navigation, volume control, optional key backlight handling and a
  long-Power menu. A local credential-checked broker accepts only fixed shutdown,
  normal reboot and recovery actions after filesystem preparation.
- Removal of the bottom navigation panel, Back handling for panels/keyboard/
  terminal, and an installed fix for the Power wake/blank race. The latest UI
  adjustments still need explicit physical feedback on the cleaned candidate.
- Guarded on-demand CPU policy with a 200 MHz minimum and higher cool-state
  ceilings; thermal limits always take precedence over requested boosts.
- Distinct administrator/user authentication, password-protected sudo and
  fastfetch. No account passwords, SSH identities or Wi-Fi settings are shipped
  in this source tree.

## Hardware and system status

“Working” means observed on the development hardware with the required external
configuration/firmware. It is not a promise that the kernel alone enables the
feature, or that every power state and error path has been qualified.

| Feature | Status | Evidence and limitations |
| --- | --- | --- |
| Boot to Linux | Working | Requires the matching DTB, initramfs and loader; cleanup still needs hardware regression testing. |
| USB serial rescue console | Working | CDC ACM in Linux; ADB is provided by TWRP, not this Linux gadget. |
| USB networking and SSH | Working | Requires gadget and SSH userspace configuration. |
| UFS and persistent root filesystem | Working | USERDATA/ext4 tested; recovery roundtrips observed. |
| Four Cortex-A53 + four Cortex-A57 CPUs | Working with thermal limits | All eight brought online; thermal policy can park six cores. |
| CPU frequency scaling | Working with limits | Bounded transitions tested; full ASV tuning and sustained-load qualification remain. |
| Cross-CPU timekeeping | Working in tested configuration | Physical-counter workaround; direct vDSO counter reads disabled. |
| Battery and charger temperatures | Working | ADC/NTC measurements with board-specific tables. |
| SoC temperature and automatic cooling | Working with limits | TMU monitoring, frequency caps and core parking observed; this is not complete hardware-trip qualification. |
| Battery percentage/voltage/current | Working | Read-only telemetry; depends on inherited fuel-gauge state. |
| Active charging control | Not implemented | Firmware settings are preserved; telemetry is not charger management. |
| Wi-Fi | Working | BCM4358 over PCIe; association, reconnect and wireless SSH tested. |
| Bluetooth | Working with limits | Firmware initialization, hci0 and scanning tested; pairing/audio profiles not qualified. |
| Display scanout | Working with limits | Firmware-initialized framebuffer through simpledrm; native panel lifecycle is not implemented. |
| GPU acceleration | Working with known faults | Panfrost rendering improves Plasma; intermittent scheduler timeouts remain. |
| Touchscreen and on-screen keyboard | Working | Physical touch/drag and keyboard input confirmed with Plasma userspace. |
| Home, Power, Volume, Back and Recents | Working with mapping caveats | Physical events confirmed; latest Back/wake/menu adjustments require further UI feedback. |
| Capacitive key backlight | Implemented, partially verified | LED/regulator path exists; visual operation still needs explicit confirmation. |
| GUI Wi-Fi/Bluetooth controls | Working in configured userspace | NetworkManager, BlueZ, D-Bus and policy configuration required. |
| Normal reboot and reboot to TWRP | Working | Userspace-controlled clean restart paths tested. |
| Physical power-off | Not hardware-qualified | Syscon driver registered in SYSTEM2; actual power cut not tested. |
| Screen blanking | Partial | Black framebuffer/DPMS path works; panel power and refresh remain active. |
| Suspend and GPU runtime power management | Not implemented | No deep-idle battery-life claim. |
| Speaker, microphone and audio routing | Not validated | No functional audio claim. |
| Cellular modem, calls and mobile data | Not implemented/validated | WLAN root complex does not initialize the modem controller. |
| Cameras, fingerprint, NFC and other sensors | Not validated | No support claim from this bring-up. |

## Building and release limitations

Use the included `arch/arm64/configs/zerolte_defconfig` and the matching external
device tree. The defconfig reproduces the configuration used for publication
validation, including `CONFIG_WERROR=y`. Build outside the source checkout:

```sh
make O=../build-zerolte ARCH=arm64 LLVM=1 zerolte_defconfig
make O=../build-zerolte ARCH=arm64 LLVM=1 -j4 Image
```

Keep at least **12 GiB free** and record exact tool versions and image hashes.
Validation used Clang/LLD 18.1.3: the complete Image builds with the standard
upstream warning set and warnings treated as errors. All 22 modified C objects
also build with `W=1` and `CONFIG_WERROR=y`. Global `W=1` is not claimed clean:
unmodified ARM64 syscall tables intentionally override fallback initializers and
trigger that extra diagnostic. The upstream warning configuration is unchanged.
Do not infer an end-to-end installer from kernel compilation alone.

Bindings and DT clock IDs belong in a separate commit before the implementation
commit. This keeps each patch reviewable and satisfies the kernel's binding patch
separation checks. See the local validation report for the exact build command,
configuration hash, compiler version and diagnostic logs.

The device-specific fixes remain experimental. Before publishing a bootable
release, rebuild the initramfs/rootfs from clean recipes, verify firmware source
and redistribution terms, and test the new artifacts on authorized hardware.
Keep a recovery path and a local BOOT backup. Do not repartition the device or
write RECOVERY, EFS, modem or unrelated partitions as part of this port.

## Privacy and contribution provenance

Do not commit installed root filesystems, raw BOOT backups, private SSH keys,
authorized keys, Wi-Fi profiles, passwords/hashes, machine IDs or hardware-specific
installation identities. Generate credentials and host identities per install.
No credentials or personalized network configuration are needed in this document.

Development has substantial AI assistance. This is an independent downstream
experiment. Nura's current contribution policy does not accept code generated
fully or partly with generative AI; this tree must not be represented as an
accepted upstream Nura contribution or have its provenance concealed.
