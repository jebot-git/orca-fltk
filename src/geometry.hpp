// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <filesystem>
#include <vector>
namespace slicer {
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Bounds {
  Vec3 min{}, max{};
  bool valid = false;
  void add(Vec3 p);
  Vec3 center() const;
  float radius() const;
};
struct Triangle {
  Vec3 a, b, c;
};
struct Mesh {
  std::vector<Triangle> triangles;
  Bounds bounds;
};
Mesh read_stl(const std::filesystem::path &file);
Mesh transform_mesh(const Mesh &mesh, const std::array<double, 3> &degrees, double scale);
struct Segment {
  Vec3 a, b;
  unsigned layer = 0, tool = 0;
  bool extrusion = false;
};
struct Toolpath {
  std::vector<Segment> segments;
  Bounds bounds;
  std::vector<float> heights;
};
Toolpath read_gcode(const std::filesystem::path &file, size_t maximum_segments = 5000000);
} // namespace slicer
