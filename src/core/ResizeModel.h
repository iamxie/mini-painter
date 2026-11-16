#pragma once

namespace mp {

// 「調整尺寸」對話框的算法：改了一個欄位之後，其他欄位跟著怎麼變。不碰視窗，對話框只負責讀寫控制項。

enum class LengthUnit { Pixels, Percent, Inches, Cm, Mm, Points }; // 順序同對話框的單位下拉
enum class DpiUnit { PerInch, PerCm };                             // 順序同解析度單位下拉

// 像素和某個單位之間換算。total：百分比的 100% 是多少像素；dpi：像素/英寸。
double pixelsToUnit(double pixels, LengthUnit unit, double dpi, int total);
double unitToPixels(double value, LengthUnit unit, double dpi, int total);

// 預設尺寸下拉的選項：圖放進這個邊長的正方形裡。
constexpr int kResizePresets[] = {320, 640, 800, 1024, 1280, 1920};
// 結果的邊長上限。
constexpr int kMaxResizeSide = 30000;

struct ResizeModel {
    int origW = 1, origH = 1;
    double w = 1, h = 1; // 目前的尺寸（像素；算到一半可以有小數）
    double dpi = 72;     // 像素/英寸
    LengthUnit unit = LengthUnit::Pixels;
    DpiUnit dpiUnit = DpiUnit::PerInch;
    bool lock = true;     // 鎖比例
    bool resample = true; // 重新取樣；關掉時只改解析度

    // 一張圖的起始狀態。dpi 不合理（<= 0）時當成 72。
    static ResizeModel forImage(int width, int height, double dpi);

    // 選了預設尺寸：等比例放進 box × box。
    void fitInto(double box);
    // 寬或高的欄位改了，value 是目前單位下的數字（要 > 0，否則不動）。鎖比例時另一邊照原圖的比例跟著。
    void setWidth(double value);
    void setHeight(double value);
    // 解析度的欄位改了，value 是目前解析度單位下的數字（要 > 0，否則不動）。
    // 重新取樣而且尺寸單位是實體長度（英寸、公分、公釐、點）時，實體尺寸不變、像素跟著變。
    void setDpi(double value);
    // 勾或取消「重新取樣」。取消時尺寸回到原圖的。
    void setResample(bool on);

    // 欄位要顯示的數字。
    double widthInUnit() const { return pixelsToUnit(w, unit, dpi, origW); }
    double heightInUnit() const { return pixelsToUnit(h, unit, dpi, origH); }
    double dpiInUnit() const { return dpiUnit == DpiUnit::PerInch ? dpi : dpi / 2.54; }
    // 「結果」那一行顯示的像素尺寸（四捨五入，至少 1）。
    int previewWidth() const;
    int previewHeight() const;
    // 按「好」之後真的要用的像素尺寸（再夾到 kMaxResizeSide）。
    int resultWidth() const;
    int resultHeight() const;
};

} // namespace mp
