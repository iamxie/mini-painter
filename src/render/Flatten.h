#pragma once
#include "core/Document.h"
#include "core/Image.h"
#include "render/RenderDevice.h"

namespace mp {

// flatten 畫得出正確像素的最大邊長（2^19 − 1）。再大的話 Direct2D 的軟體算繪每個呼叫都成功，
// 但畫出來的位置會錯（2026-11-16 在 Win10 VM 量的：524287 正確，524288 起有錯的像素）。
constexpr int kFlattenMaxSide = 524287;

// flatten 的結果。失敗時帶著能查的原因，放進存檔失敗的訊息給回報問題用。
struct FlattenResult {
    bool ok = false;
    const wchar_t* kind = nullptr; // 畫不出來的那個標註的種類（kindName，英文）；不是某個標註的問題時 nullptr
    HRESULT hr = S_OK;             // Direct2D / DirectWrite / WIC 回的失敗；沒有（例如邊長超過上限）時 S_OK
    explicit operator bool() const { return ok; }
};

// 把標註畫到底圖上，壓成一張圖（存檔用）。標註以外的像素和底圖一模一樣。
// 沒有標註時 out 是底圖的複本；呼叫端有底圖可以直接用的話不必呼叫。
// 畫不出正確結果（邊長超過 kFlattenMaxSide、記憶體不夠、任何一個標註畫不完整、繪製失敗）回失敗，這時 out 不能用。
// 記憶體：過程中除了底圖，另有一張畫布與 out（各一張圖的大小）；有放大鏡時再多一張底圖的點陣圖。
FlattenResult flatten(RenderDevice& device, const DocState& state, Image& out);

} // namespace mp
