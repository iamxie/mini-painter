// DNG decoding for the mpainter DNG plugin, on top of the Adobe DNG SDK (src/third_party/dng_sdk).
// This product includes DNG technology under license by Adobe.
//
// Kept apart from the plugin exports (dng.cpp) so that mp_tests can call it directly, for example with a
// different thread count. Return values are the MP_* codes of src/plugins/sdk/mpainter_plugin.h.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mpdng {

// Does the file start like a TIFF file whose first IFD has a DNGVersion tag (50706)? Reads only the
// header and the first IFD's entries, with every offset checked against size.
bool looksLikeDng(const uint8_t* data, size_t size);

// Width and height of the rendered image, after the orientation stored in the file. Parses the tags only;
// the image data is not read.
int32_t dngReadSize(const uint8_t* data, size_t size, int32_t* width, int32_t* height, std::string* message);

// Gets the memory for the output: width * height pixels, 4 bytes each (B, G, R, A), stride width * 4.
// Returns nullptr when there is not enough memory.
using AllocFn = uint8_t* (*)(void* context, int32_t width, int32_t height);

// Full render of the main image (or the enhanced image, when the file has one) to 8-bit sRGB, the same way
// the SDK's dng_validate -tif does: default crop, camera profile and tone curve from the file, lens
// corrections stored as opcodes, orientation. Alpha is 255. threads: worker threads for the SDK's area tasks
// (1 = everything on the calling thread).
int32_t dngDecode(const uint8_t* data, size_t size, unsigned threads, AllocFn alloc, void* allocContext, std::string* message);

// Threads the plugin uses: the logical processors, at most 8.
unsigned defaultThreads();

} // namespace mpdng
