#pragma once
// Port addition: the world lighting (engine/world/lighting.hpp) shaded on the
// GPU with OpenGL 3.3 (or OpenGL ES 3.0), in a hidden window of its own: the
// frame is computed by a fragment shader into an off-screen picture read back
// for the presenter, which stays as it is. create() returns null when no such
// context can be made (the lighting then runs on the processor).

#include "engine/world/lighting.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace crusader {

class GlLighting : public LightingGpu {
public:
    static std::unique_ptr<GlLighting> create(std::string* error = nullptr);
    ~GlLighting() override;
    GlLighting(const GlLighting&) = delete;
    GlLighting& operator=(const GlLighting&) = delete;

    bool shade(const LitFrame& frame, std::uint32_t* argb) override;
    // "OpenGL 3.3" / "OpenGL ES 3.0" and the renderer's name.
    const std::string& description() const { return description_; }

private:
    struct Impl;
    explicit GlLighting(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    std::string description_;
};

}  // namespace crusader
