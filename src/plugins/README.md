# Writing an image plugin for mpainter

A plugin teaches mpainter to open another image format. It only reads and renders. An image opened
through a plugin is read-only in mpainter: the user can view it and use "Export As", which writes
one of mpainter's own formats (PNG, JPEG, BMP, TIFF, WebP). Editing, rotating and saving are off.

- Interface: [`sdk/mpainter_plugin.h`](sdk/mpainter_plugin.h). It is plain C. Everything below is
  a summary of that header, and the header is the one to follow.
- Example: [`jxl/`](jxl), the JPEG XL plugin that ships with mpainter (libjxl).
- [`dng/`](dng) is the other bundled plugin: DNG through the Adobe DNG SDK (source in
  `../third_party/dng_sdk`). Its `probe` reads the TIFF header and the first IFD, which is why
  version 2 of the interface hands `probe` the whole file. This product includes DNG technology
  under license by Adobe.

## What a plugin is

- A 64-bit Windows DLL at `<mpainter folder>\plugins\<id>\<id>.dll`. `<id>` is the folder name.
  `mpainter.ini` refers to the plugin by it (`plugin=<id>|on`).
- DLLs that the plugin depends on can go in the same folder. mpainter loads the plugin with that
  folder in the DLL search path.
- An RCDATA resource named `MPAINTER_PLUGIN` holds the manifest (UTF-8 text). mpainter reads it
  without running the DLL, so it can list the plugin and check for conflicts before the user turns
  it on:

  ```
  api=2
  name=My Format
  version=1.0.0
  format=My Format image|.myf;.myf2
  ```

  Add one `format=` line per format: a display name, `|`, then the extensions, separated by `;`.
  A format's index is the position of its line, counting from 0.
- One exported function, `mpPluginGetFuncs`, returns a table of three functions:
  - `probe`: is this file mine? It gets the whole file (with `api=1`, only the first 64 bytes).
  - `readSize`: width and height.
  - `decode`: the pixels.

## Rules that matter

- Output pixels are 8-bit B, G, R, A, with straight alpha (not premultiplied). Rows run top to
  bottom, and the stride is `width * 4`. Get the memory from `host->allocImage`, once per decode.
- Apply the orientation stored in the file. `readSize` must report the size after rotation.
- mpainter does no color management. If the format stores colors in another space, convert them
  to sRGB if you can. libjxl does this for lossy JPEG XL, for example.
- Only the first image (or frame) is shown.
- The functions may be called at the same time from several threads.
- The functions get a read-only view of the whole file. Do not open the file yourself.
- `probe` runs for every file that does not start like a PNG, JPEG, WebP or BMP file, TIFF files
  included (DNG, for example, is a TIFF file inside). Keep it to a signature check and, if you need
  to, a few header fields. Check every offset against the size.
- Never let a C++ exception or `abort()` leave the plugin. On failure, return an `MP_ERR_*` code
  and put a short English reason in `info->message`. The user sees that text as it is.
- Extensions that mpainter opens by itself (`.png`, `.jpg`, `.zip`, ...) are never given to a
  plugin. HEIC / HEIF (`.heic`, `.heif`, `.hif`) are the exception: mpainter leaves them to Windows,
  which needs extensions from Microsoft Store. A plugin with its own decoder may claim them, and
  files that start like a HEIF file are offered to plugins before Windows.
- Two enabled plugins may not claim the same extension. The Plugins page asks the user to pick one.
  If the ini turns both on anyway, mpainter turns both off at the next start.

## Building

Any compiler that makes an x64 DLL with C exports will do. The bundled plugin uses MSVC with the
static CRT (`/MT`), so that it needs only `KERNEL32.dll`. Its CMake target is `mp_plugin_jxl` in
the top-level `CMakeLists.txt`. To add the manifest, put a `.rc` file like
[`jxl/jxl.rc`](jxl/jxl.rc) next to your sources:

```
LANGUAGE 0, 0
MPAINTER_PLUGIN RCDATA "manifest.txt"
```

## Trying it

1. Copy the DLL to `plugins\<id>\<id>.dll` next to `mpainter.exe`.
2. Open Edit > Settings > Plugins. The new plugin is listed and starts disabled. If the manifest is
   wrong, the text under the list says why.
3. Tick it and restart mpainter. Its extensions now appear in the Open dialog. They are also added
   to "Open with mpainter" in the Explorer context menu, if that option is on.

If the plugin crashes mpainter, mpainter starts again with the plugin turned off and names the
plugin, the function and the file. Fix the crash, then tick the plugin again.
