#pragma once
/// GraphicsRenderer seam (spec §6, §7, §46).
///
/// `WaveformViewEngine` produces a backend-neutral `WaveformFrame`
/// (pixel-space columns, markers, flags, rects, badges). A
/// `FrameRenderer` implementation draws that plan with whatever
/// graphics stack the host provides — JUCE software raster today via
/// `WaveformView`, a batched GPU backend later — without touching
/// the model, the tests, or the marker/index logic.
///
/// The interface is deliberately thin: one entry point that consumes
/// the prepared frame plus capability hints the engine can use for
/// adaptive detail (§45) and refresh targeting (§7). No concrete
/// GPU API is assumed — `ctx` is backend-defined.
#include "ui/WaveformViewEngine.h"

namespace onda::ui {

struct FrameRenderer {
    virtual ~FrameRenderer() = default;
    /// Draw one prepared frame. `ctx` is backend-defined: the JUCE
    /// raster path passes a `juce::Graphics*`, a GPU backend would
    /// pass its command/encoder object. Implementations must not
    /// retain the frame beyond the call.
    virtual void drawFrame(const WaveformFrame& f, void* ctx) = 0;
    /// §46 — true if the backend batches primitives natively. The
    /// engine may emit fuller detail when batching absorbs marker
    /// draw calls, or shed it earlier when the caller pays per call.
    virtual bool nativeBatching() const { return false; }
    /// §7 — refresh ceiling the backend targets. 60 today; a
    /// 120 Hz-capable backend reports 120 so the host can pick its
    /// timer rate without assumptions about every system.
    virtual int maxFps() const { return 60; }
};

/// Headless/diagnostic renderer — counts frames so tests and the
/// shell can drive the pipeline without a graphics context.
struct NullFrameRenderer final : FrameRenderer {
    int drawn = 0;
    int lastColumns = 0;
    void drawFrame(const WaveformFrame& f, void*) override {
        ++drawn;
        lastColumns = (int)f.cols.size();
    }
};

} // namespace onda::ui
