// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.

#pragma once

#include "mb3d/Internal.hpp"

namespace mb3d {

// Embedded LuaJIT state. Held behind a pimpl so sol2 headers -- which
// are heavy and pull in the whole Lua C API -- stay out of every other
// translation unit.
class LuaEngine {
public:
    explicit LuaEngine(FormulaRegistry& registry);
    ~LuaEngine();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // Binds `settings` as the global `scene`. The pointer must outlive
    // every subsequent script call -- scripts mutate it in place.
    void bind_settings(mb3d_render_settings_t* settings);

    void run_string(std::string_view source);
    void run_file(const std::string& path);
    // Calls a global function with (frame, time). The animation hook.
    void call_frame(const std::string& fn_name, int frame, double time);
    // Latest FFT magnitude spectrum for audio-reactive scripts.
    void push_fft(const float* bins, int count);

private:
    void bind_types();

    struct Impl;
    FormulaRegistry&      registry_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mb3d
