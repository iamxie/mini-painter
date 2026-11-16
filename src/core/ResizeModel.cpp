#include "core/ResizeModel.h"

#include <algorithm>
#include <cmath>

namespace mp {

namespace {

constexpr double kCmPerInch = 2.54, kMmPerInch = 25.4, kPointsPerInch = 72.0;

int roundAtLeastOne(double v) { return std::max(1, static_cast<int>(std::lround(v))); }

} // namespace

double pixelsToUnit(double pixels, LengthUnit unit, double dpi, int total) {
    switch (unit) {
    case LengthUnit::Pixels: return pixels;
    case LengthUnit::Percent: return pixels * 100.0 / total;
    case LengthUnit::Inches: return pixels / dpi;
    case LengthUnit::Cm: return pixels / dpi * kCmPerInch;
    case LengthUnit::Mm: return pixels / dpi * kMmPerInch;
    case LengthUnit::Points: return pixels / dpi * kPointsPerInch;
    }
    return pixels;
}

double unitToPixels(double value, LengthUnit unit, double dpi, int total) {
    switch (unit) {
    case LengthUnit::Pixels: return value;
    case LengthUnit::Percent: return value * total / 100.0;
    case LengthUnit::Inches: return value * dpi;
    case LengthUnit::Cm: return value / kCmPerInch * dpi;
    case LengthUnit::Mm: return value / kMmPerInch * dpi;
    case LengthUnit::Points: return value / kPointsPerInch * dpi;
    }
    return value;
}

ResizeModel ResizeModel::forImage(int width, int height, double dpi) {
    ResizeModel m;
    m.origW = width;
    m.origH = height;
    m.w = width;
    m.h = height;
    m.dpi = dpi > 0 ? dpi : 72;
    return m;
}

void ResizeModel::fitInto(double box) {
    const double scale = std::min(box / origW, box / origH);
    w = origW * scale;
    h = origH * scale;
}

void ResizeModel::setWidth(double value) {
    if (value <= 0) return;
    w = unitToPixels(value, unit, dpi, origW);
    if (lock) h = w * origH / origW;
}

void ResizeModel::setHeight(double value) {
    if (value <= 0) return;
    h = unitToPixels(value, unit, dpi, origH);
    if (lock) w = h * origW / origH;
}

void ResizeModel::setDpi(double value) {
    if (value <= 0) return;
    const double newDpi = dpiUnit == DpiUnit::PerInch ? value : value * kCmPerInch;
    const bool physical = unit != LengthUnit::Pixels && unit != LengthUnit::Percent;
    if (resample && physical) {
        w = w * newDpi / dpi;
        h = h * newDpi / dpi;
    }
    dpi = newDpi;
}

void ResizeModel::setResample(bool on) {
    resample = on;
    if (!on) {
        w = origW;
        h = origH;
    }
}

int ResizeModel::previewWidth() const { return roundAtLeastOne(w); }
int ResizeModel::previewHeight() const { return roundAtLeastOne(h); }
int ResizeModel::resultWidth() const { return std::clamp(static_cast<int>(std::lround(w)), 1, kMaxResizeSide); }
int ResizeModel::resultHeight() const { return std::clamp(static_cast<int>(std::lround(h)), 1, kMaxResizeSide); }

} // namespace mp
