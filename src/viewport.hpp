// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "geometry.hpp"
#include <FL/Fl_Gl_Window.H>
#include <memory>
namespace slicer {
class Viewport : public Fl_Gl_Window {
  std::vector<float> vertices_, colours_;
  bool dirty_ = true;
  int last_x_ = 0, last_y_ = 0;
  void scene(int x, int width, int height, float eye);
  void lines();

public:
  std::shared_ptr<Mesh> mesh;
  std::shared_ptr<Toolpath> paths;
  float yaw = 25, pitch = -65, zoom = 3;
  bool stereo = false, travels = false;
  unsigned layer_limit = ~0u;
  Viewport(int x, int y, int w, int h, const char *title = nullptr);
  void changed() {
    dirty_ = true;
    redraw();
  }
  void fit() {
    zoom = 3;
    yaw = 25;
    pitch = -65;
    redraw();
  }
  void draw() override;
  int handle(int event) override;
  void capture(const std::filesystem::path &file);
};
} // namespace slicer
