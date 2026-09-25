# 📡 Yukon-for-Cact

<p align="center">
  <img src="https://img.shields.io/badge/version-2.0.0-green.svg?style=for-the-badge" alt="Version: 2.0.0">
  <img src="https://img.shields.io/badge/license-GPLv3-blue.svg?style=for-the-badge" alt="License: GPLv3">
  <img src="https://img.shields.io/badge/arch-i686-red.svg?style=for-the-badge" alt="Arch: i686">
  <img src="https://img.shields.io/badge/format-cctk-green.svg?style=for-the-badge" alt="Output: yukon.cctk">
  <img src="https://img.shields.io/badge/bus-PCI-blue.svg?style=for-the-badge" alt="PCI">
  <img src="https://img.shields.io/badge/irq-poll-yellow.svg?style=for-the-badge" alt="Polling">
</p>

<p align="center">
  Out-of-tree <strong>Yukon</strong> Ethernet driver → <strong><code>yukon.cctk</code></strong>.<br>
  <strong>2.0.0:</strong> include paths updated — <code>Cact/kernel/net</code> → <code>Cact/net</code> to match kernel 2.0.0 directory layout.
</p>

---

## 🔨 Building

**Recommended — full workspace**

```sh
ninja -C CactOS-x86_32/build-meson iso
```

**Standalone**

```sh
meson setup build-meson --cross-file cross/i686-cact-clang.ini
ninja -C build-meson          # → build-meson/yukon.cctk
ninja -C build-meson stage    # copy into ../LocalRepoCactOS-x86_32/lib/
ninja -C build-meson clean
```

Override paths if needed: `meson configure build-meson -Dkern_root=/custom/path -Dlocal_repo=/custom/path`.
