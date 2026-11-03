// SPDX-License-Identifier: AGPL-3.0-or-later
#include "viewport.hpp"
#include <FL/Fl.H>
#include <FL/gl.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace slicer {
Viewport::Viewport(int x, int y, int w, int h, const char *title)
    : Fl_Gl_Window(x, y, w, h, title) {
  mode(FL_RGB | FL_DOUBLE | FL_DEPTH | FL_MULTISAMPLE);
  end();
}
void Viewport::lines() {
  if (!dirty_)
    return;
  dirty_ = false;
  vertices_.clear();
  colours_.clear();
  if (!paths)
    return;
  constexpr float palette[][3] = {{.22f, .8f, .98f}, {.98f, .55f, .16f}, {.55f, .9f, .35f},
                                  {.9f, .35f, .7f},  {.9f, .9f, .3f},    {.7f, .5f, .98f}};
  for (const auto &s : paths->segments) {
    if (s.layer > layer_limit || (!s.extrusion && !travels))
      continue;
    for (auto p : {s.a, s.b}) {
      vertices_.insert(vertices_.end(), {p.x, p.y, p.z});
      auto colour = palette[s.tool % 6];
      if (s.extrusion)
        colours_.insert(colours_.end(), colour, colour + 3);
      else
        colours_.insert(colours_.end(), {.25f, .29f, .34f});
    }
  }
}
void Viewport::scene(int x, int width, int height, float eye) {
  glViewport(x, 0, width, height);
  glEnable(GL_SCISSOR_TEST);
  glScissor(x, 0, width, height);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  auto bounds = paths ? paths->bounds : (mesh ? mesh->bounds : Bounds{});
  float radius = bounds.radius(), distance = radius * zoom, near = std::max(.01f, radius * .01f),
        far = radius * 30;
  float top = near * .41421356f, right = top * float(width) / std::max(1, height),
        offset = eye * radius * .1f, shift = -offset * near / distance;
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-right + shift, right + shift, -top, top, near, far);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(-offset, 0, -distance);
  glRotatef(pitch, 1, 0, 0);
  glRotatef(yaw, 0, 0, 1);
  auto centre = bounds.center();
  glTranslatef(-centre.x, -centre.y, -centre.z);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glDisable(GL_TEXTURE_2D);
  if (!paths && mesh) {
    static_assert(sizeof(Triangle) == 9 * sizeof(float));
    glColor3f(.2f, .63f, .82f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, mesh->triangles.data());
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1, 1);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(mesh->triangles.size() * 3));
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColor3f(.08f, .2f, .27f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(mesh->triangles.size() * 3));
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisableClientState(GL_VERTEX_ARRAY);
  }
  if (paths) {
    lines();
    glLineWidth(1.5f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices_.data());
    glColorPointer(3, GL_FLOAT, 0, colours_.data());
    glDrawArrays(GL_LINES, 0, GLsizei(vertices_.size() / 3));
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
  }
  float size = radius * 1.2f, z = bounds.valid ? bounds.min.z - .05f : 0;
  glColor3f(.22f, .25f, .29f);
  glLineWidth(1);
  glBegin(GL_LINES);
  for (int i = -5; i <= 5; ++i) {
    float p = size * i / 5;
    glVertex3f(centre.x - size, centre.y + p, z);
    glVertex3f(centre.x + size, centre.y + p, z);
    glVertex3f(centre.x + p, centre.y - size, z);
    glVertex3f(centre.x + p, centre.y + size, z);
  }
  glEnd();
}
void Viewport::draw() {
  glClearColor(.055f, .07f, .09f, 1);
  int width = pixel_w(), height = pixel_h();
  if (stereo) {
    int half = width / 2;
    scene(0, half, height, -1);
    scene(half, width - half, height, 1);
  } else
    scene(0, width, height, 0);
}
int Viewport::handle(int event) {
  switch (event) {
  case FL_PUSH:
    last_x_ = Fl::event_x();
    last_y_ = Fl::event_y();
    take_focus();
    return 1;
  case FL_DRAG:
    yaw += (Fl::event_x() - last_x_) * .4f;
    pitch = std::clamp(pitch + (Fl::event_y() - last_y_) * .4f, -175.f, -5.f);
    last_x_ = Fl::event_x();
    last_y_ = Fl::event_y();
    redraw();
    return 1;
  case FL_MOUSEWHEEL:
    zoom = std::clamp(zoom * std::pow(1.12f, float(Fl::event_dy())), 1.1f, 20.f);
    redraw();
    return 1;
  case FL_KEYDOWN:
    if (Fl::event_key() == ' ') {
      fit();
      return 1;
    }
    if (Fl::event_key() == FL_Escape && stereo) {
      hide();
      return 1;
    }
    break;
  case FL_FOCUS:
  case FL_UNFOCUS:
    return 1;
  }
  return Fl_Gl_Window::handle(event);
}
void Viewport::capture(const std::filesystem::path &file) {
  make_current();
  draw();
  glFinish();
  int w = pixel_w(), h = pixel_h();
  std::vector<unsigned char> pixels(size_t(w) * h * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
  std::ofstream out(file, std::ios::binary);
  if (!out)
    throw std::runtime_error("Cannot create preview capture");
  out << "P6\n" << w << ' ' << h << "\n255\n";
  for (int y = h - 1; y >= 0; --y)
    out.write(reinterpret_cast<const char *>(pixels.data() + size_t(y) * w * 3), w * 3);
}
} // namespace slicer
