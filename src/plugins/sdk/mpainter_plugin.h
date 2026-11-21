/*
 * mpainter image reader plugin interface (API version 2).
 *
 * A plugin adds one or more image formats that mpainter can open. It only reads and renders: an image
 * opened through a plugin is read-only in mpainter (no editing, rotating or saving). "Export As" writes a
 * copy in one of mpainter's own formats (PNG, JPEG, BMP, TIFF, WebP).
 *
 * Versions
 *   2: probe() gets the whole file, and is also asked about files that start like a TIFF file (so a plugin
 *      can take formats built on TIFF, such as DNG). mpainter 1.2 and later.
 *   1: probe() gets at most the first MP_PLUGIN_PROBE_BYTES bytes. mpainter 1.1 and later still load
 *      version 1 plugins.
 *
 * Layout
 *   A plugin is a DLL at  <mpainter folder>\plugins\<id>\<id>.dll  . <id> is the folder name; it is how
 *   mpainter.ini refers to the plugin. DLLs the plugin itself depends on can sit in the same folder.
 *   New plugins start disabled: the user turns them on in Settings > Plugins, and changes take effect the
 *   next time mpainter starts.
 *
 * Manifest
 *   The DLL carries a UTF-8 text resource of type RCDATA named MPAINTER_PLUGIN. mpainter reads it without
 *   running any code in the DLL (to list plugins and check conflicts before the user has enabled them):
 *
 *       api=2
 *       name=JPEG XL
 *       version=1.0.0
 *       format=JPEG XL image|.jxl
 *
 *   One "format=" line per format: a display name (English), "|", then the extensions (lowercase, with the
 *   dot, separated by ";"). The format index used below is the order of these lines, starting at 0.
 *   Extensions that mpainter reads by itself (.png, .jpg ...) are never given to a plugin. HEIC / HEIF
 *   (.heic, .heif, .hif) are the exception: mpainter leaves them to Windows (WIC), so a plugin with its own
 *   decoder may claim them, and files that start like a HEIF file are offered to plugins first. Two enabled
 *   plugins may not claim the same extension: mpainter turns both off and asks the user to pick one.
 *
 * Entry point
 *   The DLL exports  const MpPluginFuncs* mpPluginGetFuncs(uint32_t hostApiVersion)  (C linkage, see
 *   MP_PLUGIN_EXPORT). mpainter passes the "api=" value of the manifest. Return NULL if you cannot work with
 *   hostApiVersion; mpainter then reports the plugin as incompatible. The table must stay valid until the DLL
 *   is unloaded, and its apiVersion must equal hostApiVersion.
 *
 * Rules for the three functions
 *   - mpainter may call them at the same time from several threads (the window, background pre-decoding).
 *   - They get the file's bytes; they must not open the file themselves. The bytes are a read-only
 *     memory-mapped view of the whole file: touch only what you need (probe and readSize should not read
 *     the pixels).
 *   - Do not throw C++ exceptions out of them, and do not call exit/abort. A crash inside a plugin crashes
 *     mpainter; on the next start mpainter tells the user which plugin it was and turns it off.
 */
#ifndef MPAINTER_PLUGIN_H
#define MPAINTER_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP_PLUGIN_API_VERSION 2u

/* Name of the RCDATA resource that holds the manifest, and of the exported entry point. */
#define MP_PLUGIN_MANIFEST_NAME "MPAINTER_PLUGIN"
#define MP_PLUGIN_ENTRY_NAME "mpPluginGetFuncs"

/* API version 1 only: probe() sees at most this many bytes from the start of the file. */
#define MP_PLUGIN_PROBE_BYTES 64u

/* Return codes of readSize() and decode(). */
#define MP_OK 0
#define MP_ERR_CORRUPT 1       /* not this format after all, or the data is damaged / truncated */
#define MP_ERR_UNSUPPORTED 2   /* valid file, but it uses something this plugin does not handle */
#define MP_ERR_OUT_OF_MEMORY 3 /* allocImage() returned NULL, or the plugin's own allocation failed */
#define MP_ERR_INTERNAL 4      /* anything else */

/* What mpainter gives decode(). */
typedef struct MpHost {
    void* context; /* pass back to allocImage */
    /* Allocates the output image: width * height pixels, 4 bytes each in the order B, G, R, A, rows top to
       bottom, stride = width * 4, alpha NOT premultiplied. Returns NULL when there is not enough memory
       (return MP_ERR_OUT_OF_MEMORY then). Call it at most once per decode(); mpainter owns the memory. */
    uint8_t* (*allocImage)(void* context, int32_t width, int32_t height);
} MpHost;

/* What decode() reports besides the pixels. mpainter zero-fills it before the call. */
typedef struct MpDecodeInfo {
    double dpiX, dpiY;  /* resolution stored in the file, pixels per inch; leave 0 if the file has none */
    char message[256];  /* on failure: a short English explanation, UTF-8, NUL-terminated. Shown to the user
                           untranslated so that bug reports carry it. */
} MpDecodeInfo;

typedef struct MpPluginFuncs {
    uint32_t apiVersion; /* MP_PLUGIN_API_VERSION the plugin was built with */

    /* Is this one of the plugin's formats? data = the whole file (API version 1: at most the first
       MP_PLUGIN_PROBE_BYTES bytes). Return the format index (see Manifest) or -1. This is called for every
       file that does not start like a PNG, JPEG, WebP or BMP file (TIFF files included), so it must be fast:
       look at the signature and, if you have to, a few header fields. Check every offset against size. */
    int32_t (*probe)(const uint8_t* data, size_t size);

    /* Width and height in pixels, as decode() will produce them (after applying any orientation stored in
       the file). Should read only the header. */
    int32_t (*readSize)(int32_t format, const uint8_t* data, size_t size, int32_t* width, int32_t* height);

    /* Decode the first image in the file into memory from host->allocImage, with any orientation stored
       in the file already applied. */
    int32_t (*decode)(int32_t format, const uint8_t* data, size_t size, const MpHost* host, MpDecodeInfo* info);
} MpPluginFuncs;

typedef const MpPluginFuncs* (*MpPluginGetFuncs)(uint32_t hostApiVersion);

#ifdef __cplusplus
#define MP_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define MP_PLUGIN_EXPORT __declspec(dllexport)
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MPAINTER_PLUGIN_H */
