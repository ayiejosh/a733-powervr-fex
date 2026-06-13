#!/usr/bin/env bash
# a733-powervr-fex installer — applies the automatable parts on a Radxa A733
# (Cubie A7A/A7S, Debian 11 BSP). Safe: confirms each section, dry-runs the patch,
# never enables services without asking. Vendor blobs + the Mesa build are manual
# (see README / gpu/README.md) — this can't ship those.
#
#   ./install.sh            # guided (all sections, with prompts)
#   ./install.sh kernel     # just the pvrsrvkm PRIME patch
#   ./install.sh sway       # just the GPU sway desktop
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
say(){ printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn(){ printf '\033[1;33m!! %s\033[0m\n' "$*"; }
ask(){ read -r -p "$1 [y/N] " a; [[ "$a" =~ ^[Yy]$ ]]; }

install_kernel(){
  say "Kernel: pvrsrvkm DRM PRIME-import patch"
  local src; src="$(ls -d /usr/src/img-bxm-dkms-*/img-bxm/linux/rogue_km/services/server/env/linux 2>/dev/null | head -1)"
  if [ -z "$src" ]; then
    warn "img-bxm-dkms source not found. Install it first:  sudo apt-get install img-bxm-dkms"
    return 1
  fi
  echo "Target: $src"
  echo "Dry-run:"; ( cd "$src" && patch -p1 --dry-run < "$HERE/kernel/pvrsrvkm-drm-prime-import.patch" )
  if ask "Apply the patch and rebuild the module?"; then
    ( cd "$src" && sudo patch -p1 < "$HERE/kernel/pvrsrvkm-drm-prime-import.patch" )
    local bd; bd="$(ls -d /usr/src/img-bxm-dkms-*/img-bxm/build/linux/sunxi_linux 2>/dev/null | head -1)"
    if [ -n "$bd" ]; then
      ( cd "$bd" && sudo make BUILD=release KERNEL_CC=gcc KERNELDIR=/usr/src/linux-headers-"$(uname -r)" )
      warn "Built. Load when pvrsrvkm refcnt is 0:  sudo rmmod pvrsrvkm && sudo insmod <path>/pvrsrvkm.ko"
      warn "Persist:  xz -c <.ko> > /lib/modules/\$(uname -r)/updates/dkms/pvrsrvkm.ko.xz && sudo depmod -a"
    else
      warn "Build dir not found — rebuild via your DKMS flow."
    fi
  fi
}

install_sway(){
  say "GPU sway + wayvnc desktop"
  command -v sway >/dev/null   || warn "sway not installed (sudo apt-get install sway)"
  command -v wayvnc >/dev/null || warn "wayvnc not installed (build it or apt)"
  command -v waybar >/dev/null || warn "waybar not installed (sudo apt-get install waybar wofi)"
  if ask "Install sway/waybar configs + user services to ~/.config?"; then
    mkdir -p ~/.config/sway ~/.config/waybar ~/.config/systemd/user
    cp "$HERE/gpu/sway/sway.config"        ~/.config/sway/config
    cp "$HERE/gpu/sway/waybar.config"      ~/.config/waybar/config
    cp "$HERE/gpu/sway/waybar-style.css"   ~/.config/waybar/style.css
    cp "$HERE/gpu/sway/sway-headless.service" "$HERE/gpu/sway/wayvnc.service" ~/.config/systemd/user/
    systemctl --user daemon-reload
    echo "Installed. Start with:  systemctl --user enable --now sway-headless wayvnc"
    echo "(needs seatd running + 'loginctl enable-linger \$USER'). Then point a websockify/noVNC at 127.0.0.1:5901."
  fi
}

say "a733-powervr-fex installer"
warn "Prerequisite NOT handled here: the proprietary PowerVR userspace blobs + firmware"
warn "(libGLESv2_PVR_MESA, libVK_IMG, rgx.fw.*) from radxa/allwinner-target, and the"
warn "Zink Mesa build (gpu/README.md). See README.md."
case "${1:-all}" in
  kernel) install_kernel ;;
  sway)   install_sway ;;
  all)    install_kernel; install_sway
          say "Done. For Zink-GL and the FEX Vulkan thunk, follow gpu/README.md and fex/README.md." ;;
  *) echo "usage: $0 [kernel|sway|all]"; exit 1 ;;
esac
