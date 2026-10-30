#!/usr/bin/env python3
"""Decode a captured PVR transfer-frag stream (the format the open Vulkan winsys writes and the
kernel's pvr_stream_defs.c parses): KMD stream header, then four 64-bit PDS/ISP base fields, nine
64-bit PBE words (pbe_wordx_mrty), the 32-bit USC/ISP/event fields and the ISP render words."""
import struct, sys

FIELDS_64 = ["pds_bgnd0_base", "pds_bgnd1_base", "pds_bgnd3_sizeinfo", "isp_mtile_base"]
FIELDS_32 = ["isp_bgobjvals", "usc_pixel_output_ctrl", "usc_clear_register0",
             "usc_clear_register1", "usc_clear_register2", "usc_clear_register3",
             "isp_mtile_size", "isp_render_origin", "isp_ctl", "isp_aa",
             "event_pixel_pds_info", "event_pixel_pds_code", "event_pixel_pds_data",
             "isp_render", "isp_rgn", "frag_screen"]

def load(path):
    data = open(path, "rb").read()
    # files written by the capture hooks carry a 4-byte length prefix
    n = struct.unpack("<I", data[:4])[0]
    raw = data[4:4 + n]
    return list(struct.unpack("<%dI" % (len(raw) // 4), raw))

def decode(path):
    w = load(path)
    main_len = w[0]
    i = 1
    regs = {}
    for name in FIELDS_64:
        regs[name] = w[i] | (w[i + 1] << 32)
        i += 2
    regs["pbe_wordx_mrty"] = [w[i + 2 * k] | (w[i + 2 * k + 1] << 32) for k in range(9)]
    i += 18
    for name in FIELDS_32:
        regs[name] = w[i]
        i += 1
    return main_len, regs, i, len(w)

if __name__ == "__main__":
    for path in sys.argv[1:]:
        main_len, regs, used, total = decode(path)
        print("== %s (stream %u bytes, used dwords %u/%u)" % (path, main_len, used, total))
        for k, v in regs.items():
            if k == "pbe_wordx_mrty":
                print("  pbe_wordx_mrty:", " ".join("%016x" % x for x in v))
            else:
                print("  %-22s = %016x" % (k, v))
