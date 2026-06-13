/*
$info$
tags: thunklibs|va
desc: VAAPI thunk — lets an x86 guest (e.g. Chrome Remote Desktop under FEX) use the
      native ARM libva + the sunxi_ve VE2 H.264 encoder driver. See NOTES.md.
$end_info$
*/
#include <common/GeneratorInterface.h>

#include <va/va.h>
#include <va/va_drm.h>

template<auto>
struct fex_gen_config {
  unsigned version = 2;
};

template<typename>
struct fex_gen_type {};

// VA structs appearing in thunked signatures: x86 and ARM share identical data layout
// (LP64, same packing) -> tell the generator they're compatible (no repacking needed).
template<> struct fex_gen_type<VAGenericValue> : fexgen::assume_compatible_data_layout {};
template<> struct fex_gen_type<VASurfaceAttrib> : fexgen::assume_compatible_data_layout {};
template<> struct fex_gen_type<VAConfigAttrib> : fexgen::assume_compatible_data_layout {};
template<> struct fex_gen_type<VAImage> : fexgen::assume_compatible_data_layout {};
template<> struct fex_gen_type<VAImageFormat> : fexgen::assume_compatible_data_layout {};

// VADisplay is void* and OPAQUE to the client (only libva dereferences it). The guest
// holds the HOST's VADisplay pointer as an opaque token and passes it back unchanged —
// no struct marshalling needed. All VA*ID handles are plain unsigned int (driver-assigned),
// so they marshal as integers automatically.

// --- lifecycle / query (simple scalar+pointer args, auto-marshalled) ---
template<> struct fex_gen_config<vaInitialize> {};
template<> struct fex_gen_config<vaTerminate> {};
template<> struct fex_gen_config<vaQueryConfigProfiles> {};
template<> struct fex_gen_config<vaQueryConfigEntrypoints> {};
template<> struct fex_gen_config<vaGetConfigAttributes> {};
template<> struct fex_gen_config<vaQuerySurfaceAttributes> {};
template<> struct fex_gen_config<vaQueryImageFormats> {};
template<> struct fex_gen_config<vaMaxNumProfiles> {};
template<> struct fex_gen_config<vaMaxNumEntrypoints> {};
template<> struct fex_gen_config<vaMaxNumConfigAttributes> {};
template<> struct fex_gen_config<vaMaxNumImageFormats> {};
template<> struct fex_gen_config<vaErrorStr> {};               // returns const char* (static)
template<> struct fex_gen_config<vaQueryVendorString> {};      // returns const char* (static)
template<> struct fex_gen_config<vaSetDriverName> {};

// --- config / context / surfaces (int handles, simple) ---
template<> struct fex_gen_config<vaCreateConfig> {};
template<> struct fex_gen_config<vaDestroyConfig> {};
template<> struct fex_gen_config<vaQueryConfigAttributes> {};
template<> struct fex_gen_config<vaCreateContext> {};
template<> struct fex_gen_config<vaDestroyContext> {};
template<> struct fex_gen_config<vaCreateSurfaces> {};
template<> struct fex_gen_config<vaDestroySurfaces> {};
template<> struct fex_gen_config<vaSyncSurface> {};
template<> struct fex_gen_config<vaQuerySurfaceStatus> {};

// --- buffers: param buffers carry a `data` pointer to copy IN; coded/image buffers are
//     read back via vaMapBuffer. vaMapBuffer returns a host pointer -> needs custom impl
//     to repack the buffer contents into guest-visible memory (copy on map, sync on unmap). ---
template<> struct fex_gen_config<vaCreateBuffer> {};
template<> struct fex_gen_config<vaBufferSetNumElements> {};
template<> struct fex_gen_config<vaMapBuffer> {};    // TODO: custom buffer repacking for encode (guest-visible map)
template<> struct fex_gen_config<vaUnmapBuffer> {};
template<> struct fex_gen_config<vaDestroyBuffer> {};

// --- images (upload path) ---
template<> struct fex_gen_config<vaCreateImage> {};
template<> struct fex_gen_config<vaDeriveImage> {};
template<> struct fex_gen_config<vaDestroyImage> {};
template<> struct fex_gen_config<vaGetImage> {};
template<> struct fex_gen_config<vaPutImage> {};

// --- the encode submission path ---
template<> struct fex_gen_config<vaBeginPicture> {};
template<> struct fex_gen_config<vaRenderPicture> {};
template<> struct fex_gen_config<vaEndPicture> {};

// --- display acquisition: libva-drm. vaGetDisplayDRM takes a DRM fd (FEX already thunks
//     libdrm, so the fd is valid host-side) and returns the opaque host VADisplay token. ---
template<> struct fex_gen_config<vaGetDisplayDRM> {};
