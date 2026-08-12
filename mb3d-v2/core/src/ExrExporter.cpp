// SPDX-License-Identifier: LicenseRef-MB3D-V2-Proprietary
// Copyright (c) 2026 KenTread. All Rights Reserved.
// Proprietary and confidential -- see LICENSE for terms.
//
// ExrExporter.cpp -- multi-layer 32-bit float OpenEXR output via tinyexr.
//
// Channel naming follows the de-facto compositing convention so Nuke,
// Fusion and After Effects pick the layers up without remapping:
//
//   R, G, B, A          beauty (linear, un-tonemapped)
//   Z                   camera-space depth in world units
//   N.X, N.Y, N.Z       world-space normals
//   AO.V                ambient occlusion
//   Shadow.V            raytraced shadow mask
//   MV.X, MV.Y          motion vectors in pixels
//
// EXR wants planar float channels in *reverse alphabetical* order for
// the RGBA group (A, B, G, R) -- getting that wrong is the classic cause
// of channel-swapped reads, so the ordering is built explicitly below.

#include "mb3d/Internal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#if defined(MB3D_WITH_TINYEXR)
#  define TINYEXR_IMPLEMENTATION
#  define TINYEXR_USE_MINIZ 0
#  define TINYEXR_USE_STB_ZLIB 1
#  include <tinyexr.h>
#endif

namespace mb3d {

#if defined(MB3D_WITH_TINYEXR)

namespace {

struct ChannelPlane {
    std::string   name;
    std::vector<float> data;
};

// Deinterleaves one component out of a packed layer into its own plane.
std::vector<float> extract_plane(const Layer& layer, int component, int components) {
    const std::size_t pixels = static_cast<std::size_t>(layer.width) * layer.height;
    std::vector<float> plane(pixels);
    const auto* src = reinterpret_cast<const float*>(layer.bytes.data());
    for (std::size_t i = 0; i < pixels; ++i) {
        plane[i] = src[i * components + component];
    }
    return plane;
}

std::vector<float> constant_plane(int width, int height, float value) {
    return std::vector<float>(static_cast<std::size_t>(width) * height, value);
}

}  // namespace

void write_exr(const FrameBuffer& fb, const std::string& path, bool multilayer) {
    const Layer* beauty = fb.layer(MB3D_LAYER_RGBA32F);
    require(beauty != nullptr, MB3D_ERR_INVALID_ARG,
            "EXR export needs the RGBA32F layer -- add MB3D_LAYER_RGBA32F to layer_mask");

    const int width = fb.width();
    const int height = fb.height();

    std::vector<ChannelPlane> planes;

    // RGBA in reverse-alphabetical order, as EXR readers expect.
    planes.push_back({"A", extract_plane(*beauty, 3, 4)});
    planes.push_back({"B", extract_plane(*beauty, 2, 4)});
    planes.push_back({"G", extract_plane(*beauty, 1, 4)});
    planes.push_back({"R", extract_plane(*beauty, 0, 4)});

    if (multilayer) {
        if (const Layer* depth = fb.layer(MB3D_LAYER_DEPTH)) {
            // Infinity is legal in EXR but several compositors clamp it
            // badly; use a large finite sentinel instead.
            std::vector<float> z = extract_plane(*depth, 0, 1);
            std::replace_if(z.begin(), z.end(),
                            [](float v) { return !std::isfinite(v); }, 1e10f);
            planes.push_back({"Z", std::move(z)});
        }
        if (const Layer* normal = fb.layer(MB3D_LAYER_NORMAL)) {
            planes.push_back({"N.X", extract_plane(*normal, 0, 3)});
            planes.push_back({"N.Y", extract_plane(*normal, 1, 3)});
            planes.push_back({"N.Z", extract_plane(*normal, 2, 3)});
        }
        if (const Layer* ao = fb.layer(MB3D_LAYER_SSAO)) {
            planes.push_back({"AO.V", extract_plane(*ao, 0, 1)});
        }
        if (const Layer* shadow = fb.layer(MB3D_LAYER_SHADOW)) {
            planes.push_back({"Shadow.V", extract_plane(*shadow, 0, 1)});
        }
        if (const Layer* motion = fb.layer(MB3D_LAYER_MOTION)) {
            planes.push_back({"MV.X", extract_plane(*motion, 0, 2)});
            planes.push_back({"MV.Y", extract_plane(*motion, 1, 2)});
        } else {
            // Emit zeroed motion vectors so a comp graph built for
            // animation still connects on a still frame.
            planes.push_back({"MV.X", constant_plane(width, height, 0.0f)});
            planes.push_back({"MV.Y", constant_plane(width, height, 0.0f)});
        }
    }

    EXRHeader header;
    InitEXRHeader(&header);
    EXRImage image;
    InitEXRImage(&image);

    const int channel_count = static_cast<int>(planes.size());
    image.num_channels = channel_count;
    image.width = width;
    image.height = height;

    std::vector<float*> pointers(channel_count);
    for (int i = 0; i < channel_count; ++i) pointers[i] = planes[i].data.data();
    image.images = reinterpret_cast<unsigned char**>(pointers.data());

    std::vector<EXRChannelInfo> channels(channel_count);
    std::vector<int> pixel_types(channel_count, TINYEXR_PIXELTYPE_FLOAT);
    std::vector<int> requested_types(channel_count, TINYEXR_PIXELTYPE_FLOAT);

    for (int i = 0; i < channel_count; ++i) {
        std::memset(channels[i].name, 0, sizeof(channels[i].name));
        const std::string& name = planes[i].name;
        require(name.size() < sizeof(channels[i].name), MB3D_ERR_INVALID_ARG,
                "EXR channel name too long: " + name);
        std::memcpy(channels[i].name, name.c_str(), name.size());

        // Depth and data passes must stay full float; colour can go half
        // without visible loss, but keeping everything float means the
        // file round-trips into the .m3i layer stack losslessly.
        requested_types[i] = TINYEXR_PIXELTYPE_FLOAT;
    }

    header.num_channels = channel_count;
    header.channels = channels.data();
    header.pixel_types = pixel_types.data();
    header.requested_pixel_types = requested_types.data();
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    const char* error = nullptr;
    const int rc = SaveEXRImageToFile(&image, &header, path.c_str(), &error);
    if (rc != TINYEXR_SUCCESS) {
        const std::string message = error ? error : "unknown tinyexr failure";
        FreeEXRErrorMessage(error);
        fail(MB3D_ERR_IO, "EXR write failed: " + message);
    }
}

#else  // !MB3D_WITH_TINYEXR

void write_exr(const FrameBuffer&, const std::string&, bool) {
    fail(MB3D_ERR_UNSUPPORTED,
         "EXR export requires tinyexr; rebuild with -DMB3D_WITH_TINYEXR=ON");
}

#endif

}  // namespace mb3d
