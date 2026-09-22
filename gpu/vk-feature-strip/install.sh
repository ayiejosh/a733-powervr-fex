#!/bin/sh
# install.sh — install VK_LAYER_PVR_strip as an IMPLICIT Vulkan layer, so that setting
# PVR_FAKE_GS=1 is enough to enable it (no VK_LAYER_PATH / VK_INSTANCE_LAYERS needed).
#
#   ./install.sh              # build + install into ${XDG_DATA_HOME:-~/.local/share}
#   ./install.sh --uninstall  # remove what this script installed
#
# Refuses to install when another manifest already claims the layer name: two manifests with
# the same name mean the loader picks one arbitrarily and silently ignores the other's env
# vars (see README).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
NAME=VK_LAYER_PVR_strip
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
DEST="$DATA_DIR/vulkan/implicit_layer.d"
MANIFEST="$DEST/VkLayer_PVR_strip.json"
LIB="$DEST/libVkLayer_PVR_strip.so"

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$MANIFEST" "$LIB"
    echo "removed $MANIFEST and $LIB"
    exit 0
fi

# --- find any other manifest claiming the same layer name ---
conflict=""
for f in "$DEST"/*.json \
         "${XDG_DATA_HOME:-$HOME/.local/share}"/vulkan/implicit_layer.d/*.json \
         /usr/local/share/vulkan/implicit_layer.d/*.json \
         /usr/share/vulkan/implicit_layer.d/*.json \
         /usr/local/etc/vulkan/implicit_layer.d/*.json \
         /etc/vulkan/implicit_layer.d/*.json \
         /etc/xdg/vulkan/implicit_layer.d/*.json; do
    [ -f "$f" ] || continue
    [ "$f" = "$MANIFEST" ] && continue
    grep -q "\"$NAME\"" "$f" 2>/dev/null || continue
    conflict="$f"
    break
done
if [ -n "$conflict" ]; then
    cat >&2 <<EOF
!! another manifest already claims the layer name $NAME:
     $conflict
   Two manifests with one name -> the loader uses one and silently ignores the other's
   env vars, so the layer will appear to do nothing. Remove or rename that manifest first
   (if it is the older reference layer, this one replaces it), then re-run.
EOF
    exit 1
fi

# --- build + install ---
mkdir -p "$DEST"
gcc -shared -fPIC -fvisibility=hidden -O2 -o "$LIB.tmp" "$HERE/vk_layer_pvr_strip.c"
mv "$LIB.tmp" "$LIB"

cat > "$MANIFEST" <<EOF
{
    "file_format_version": "1.2.0",
    "layer": {
        "name": "$NAME",
        "type": "GLOBAL",
        "library_path": "$LIB",
        "api_version": "1.3.277",
        "implementation_version": "1",
        "description": "Fakes geometryShader (and, with PVR_FAKE_R2=1, VK_EXT_robustness2.nullDescriptor) so zink accepts the closed PowerVR BXM-4-64 driver; strips both before vkCreateDevice. Enabled by PVR_FAKE_GS=1, disable with PVR_STRIP_DISABLE=1.",
        "enable_environment": {
            "PVR_FAKE_GS": "1"
        },
        "disable_environment": {
            "PVR_STRIP_DISABLE": "1"
        }
    }
}
EOF

echo "installed:"
echo "  $LIB"
echo "  $MANIFEST"
echo
echo "enable per process:   PVR_FAKE_GS=1 <your GL app>"
echo "verify it loaded:     VK_LOADER_DEBUG=layer PVR_FAKE_GS=1 <app> 2>&1 | grep -i pvr_strip"
echo "uninstall:            $0 --uninstall"
