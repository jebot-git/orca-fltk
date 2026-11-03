// SPDX-License-Identifier: AGPL-3.0-or-later
#include "geometry.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
namespace slicer {
void Bounds::add(Vec3 p) {
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e7f ||
      std::abs(p.y) > 1e7f || std::abs(p.z) > 1e7f)
    throw std::runtime_error("Geometry contains a non-finite coordinate");
  if (!valid) {
    min = max = p;
    valid = true;
  } else {
    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
  }
}
Vec3 Bounds::center() const {
  return {(min.x + max.x) / 2, (min.y + max.y) / 2, (min.z + max.z) / 2};
}
float Bounds::radius() const {
  return valid ? std::max(1.f, std::sqrt((max.x - min.x) * (max.x - min.x) +
                                         (max.y - min.y) * (max.y - min.y) +
                                         (max.z - min.z) * (max.z - min.z)) /
                                   2)
               : 100;
}
static uint32_t u32(const char *p) {
  return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
         (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
}
static float f32(const char *p) {
  auto bits = u32(p);
  float result;
  std::memcpy(&result, &bits, 4);
  return result;
}
Mesh read_stl(const std::filesystem::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    throw std::runtime_error("Cannot open model: " + file.string());
  Mesh mesh;
  char header[84]{};
  in.read(header, 84);
  auto count = in.gcount() == 84 ? u32(header + 80) : 0;
  auto size = std::filesystem::file_size(file);
  if (count && 84ull + 50ull * count == size) {
    if (count > 10000000)
      throw std::runtime_error("STL exceeds ten million triangles");
    mesh.triangles.reserve(count);
    char record[50];
    for (uint32_t i = 0; i < count; ++i) {
      in.read(record, 50);
      if (!in)
        throw std::runtime_error("Truncated STL");
      Triangle t;
      Vec3 *points[] = {&t.a, &t.b, &t.c};
      for (int j = 0; j < 3; ++j) {
        auto p = record + 12 + j * 12;
        *points[j] = {f32(p), f32(p + 4), f32(p + 8)};
        mesh.bounds.add(*points[j]);
      }
      mesh.triangles.push_back(t);
    }
  } else {
    in.clear();
    in.seekg(0);
    std::string line;
    std::vector<Vec3> vertices;
    while (std::getline(in, line)) {
      std::istringstream row(line);
      std::string token;
      row >> token;
      if (token != "vertex")
        continue;
      Vec3 p;
      if (!(row >> p.x >> p.y >> p.z))
        throw std::runtime_error("Invalid ASCII STL vertex");
      mesh.bounds.add(p);
      vertices.push_back(p);
      if (vertices.size() == 3) {
        mesh.triangles.push_back({vertices[0], vertices[1], vertices[2]});
        vertices.clear();
      }
      if (mesh.triangles.size() > 10000000)
        throw std::runtime_error("STL exceeds ten million triangles");
    }
    if (!vertices.empty())
      throw std::runtime_error("Incomplete ASCII STL triangle");
  }
  if (mesh.triangles.empty())
    throw std::runtime_error("Model contains no STL triangles");
  return mesh;
}
Mesh transform_mesh(const Mesh &source, const std::array<double, 3> &degrees, double scale) {
  if (!std::isfinite(scale) || scale <= 0 || scale > 100)
    throw std::runtime_error("Scale must be greater than zero and at most 10000%");
  for (double angle : degrees)
    if (!std::isfinite(angle) || std::abs(angle) > 36000)
      throw std::runtime_error("Invalid model rotation");
  auto result = source;
  result.bounds = {};
  auto centre = source.bounds.center();
  for (auto &triangle : result.triangles)
    for (auto *point : {&triangle.a, &triangle.b, &triangle.c}) {
      double x = (point->x - centre.x) * scale, y = (point->y - centre.y) * scale,
             z = (point->z - centre.z) * scale;
      for (int axis = 0; axis < 3; ++axis) {
        double angle = degrees[axis] * 3.14159265358979323846 / 180, c = std::cos(angle),
               s = std::sin(angle);
        if (axis == 0) {
          double next = y * c - z * s;
          z = y * s + z * c;
          y = next;
        }
        if (axis == 1) {
          double next = x * c + z * s;
          z = -x * s + z * c;
          x = next;
        }
        if (axis == 2) {
          double next = x * c - y * s;
          y = x * s + y * c;
          x = next;
        }
      }
      *point = {float(x + centre.x), float(y + centre.y), float(z + centre.z)};
      result.bounds.add(*point);
    }
  float floor = result.bounds.min.z;
  for (auto &triangle : result.triangles)
    for (auto *point : {&triangle.a, &triangle.b, &triangle.c})
      point->z -= floor;
  result.bounds.min.z -= floor;
  result.bounds.max.z -= floor;
  return result;
}
Toolpath read_gcode(const std::filesystem::path &file, size_t maximum) {
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error("Cannot open G-code: " + file.string());
  Toolpath result;
  Vec3 position, origin;
  bool relative = false, relative_e = false, absolute_arc = false;
  int plane = 17;
  double units = 1;
  unsigned tool = 0, layer = 0;
  std::map<unsigned, double> extruders;
  std::unordered_map<int64_t, unsigned> heights;
  bool explicit_layers = false, next_layer = false, part_role = true;
  std::string line;
  auto append = [&](Vec3 from, Vec3 to, bool extrusion) {
    if (result.segments.size() >= maximum)
      throw std::runtime_error("G-code preview exceeds the segment limit");
    if (extrusion) {
      if (explicit_layers) {
        if (next_layer || result.heights.empty()) {
          layer = unsigned(result.heights.size());
          result.heights.push_back(to.z);
          next_layer = false;
        }
      } else {
        auto key = int64_t(std::llround(double(to.z) * 1000));
        auto found = heights.find(key);
        if (found == heights.end()) {
          layer = unsigned(result.heights.size());
          heights.emplace(key, layer);
          result.heights.push_back(to.z);
        } else
          layer = found->second;
      }
      result.bounds.add(from);
      result.bounds.add(to);
    }
    result.segments.push_back({from, to, layer, tool, extrusion});
  };
  while (std::getline(in, line)) {
    if (line.size() > 1024 * 1024)
      throw std::runtime_error("G-code line exceeds one MiB");
    if (line.find(";LAYER_CHANGE") != std::string::npos ||
        line.find(";LAYER:") != std::string::npos ||
        line.find("; layer num/total_layer_count:") != std::string::npos) {
      explicit_layers = true;
      next_layer = true;
    }
    auto comment = line.find(';');
    if (comment != std::string::npos) {
      auto body = line.substr(comment + 1);
      auto first = body.find_first_not_of(" \t");
      if (first != std::string::npos && body.compare(first, 5, "TYPE:") == 0) {
        auto role = body.substr(first + 5);
        role.erase(0, role.find_first_not_of(" \t"));
        role.erase(role.find_last_not_of(" \t\r") + 1);
        for (char &letter : role)
          letter = char(std::tolower(static_cast<unsigned char>(letter)));
        part_role = role != "custom";
      }
    }
    if (comment != std::string::npos)
      line.resize(comment);
    for (auto open = line.find('('); open != std::string::npos; open = line.find('(')) {
      auto end = line.find(')', open);
      line.erase(open, end == std::string::npos ? std::string::npos : end - open + 1);
    }
    std::map<char, double> args;
    const char *p = line.c_str();
    char command = 0;
    int number = -1;
    double command_value = -1;
    while (*p) {
      if (std::isalpha(static_cast<unsigned char>(*p))) {
        char key = char(std::toupper(static_cast<unsigned char>(*p++)));
        const char *begin = p;
        if (*p == '+' || *p == '-')
          ++p;
        bool digit = false;
        while (std::isdigit(static_cast<unsigned char>(*p))) {
          digit = true;
          ++p;
        }
        if (*p == '.') {
          ++p;
          while (std::isdigit(static_cast<unsigned char>(*p))) {
            digit = true;
            ++p;
          }
        }
        if (!digit) {
          if (p == begin && *p)
            ++p;
          continue;
        }
        double v = 0;
        auto parsed =
            std::from_chars(*begin == '+' ? begin + 1 : begin, p, v, std::chars_format::fixed);
        if (parsed.ec != std::errc() || parsed.ptr != p)
          throw std::runtime_error("Invalid G-code number");
        if (!std::isfinite(v))
          throw std::runtime_error("Non-finite G-code value");
        if ((key == 'G' || key == 'M' || key == 'T') && !command) {
          if (v < 0 || v > 9999)
            throw std::runtime_error("Invalid G-code command");
          command = key;
          command_value = v;
          number = int(v);
        } else
          args[key] = v;
      } else
        ++p;
    }
    if (command == 'T') {
      if (number < 0 || number > 255)
        throw std::runtime_error("Invalid tool number");
      tool = unsigned(number);
      continue;
    }
    if (command == 'M' && number == 82) {
      relative_e = false;
      continue;
    }
    if (command == 'M' && number == 83) {
      relative_e = true;
      continue;
    }
    if (command != 'G')
      continue;
    if (command_value == 90.1 || command_value == 91.1) {
      absolute_arc = command_value == 90.1;
      continue;
    }
    if (number >= 17 && number <= 19) {
      plane = number;
      continue;
    }
    if (number == 20) {
      units = 25.4;
      continue;
    }
    if (number == 21) {
      units = 1;
      continue;
    }
    if (number == 90) {
      relative = false;
      continue;
    }
    if (number == 91) {
      relative = true;
      continue;
    }
    if (number == 92) {
      if (args.count('X'))
        origin.x = position.x - float(args['X'] * units);
      if (args.count('Y'))
        origin.y = position.y - float(args['Y'] * units);
      if (args.count('Z'))
        origin.z = position.z - float(args['Z'] * units);
      if (args.count('E'))
        extruders[tool] = args['E'] * units;
      continue;
    }
    if (number < 0 || number > 3)
      continue;
    auto old = position;
    auto coordinate = [&](char key, float current, float offset) {
      if (!args.count(key))
        return current;
      double value = args[key] * units + (relative ? current : offset);
      if (!std::isfinite(value) || std::abs(value) > 1e7)
        throw std::runtime_error("G-code coordinate exceeds preview limits");
      return float(value);
    };
    position = {coordinate('X', old.x, origin.x), coordinate('Y', old.y, origin.y),
                coordinate('Z', old.z, origin.z)};
    double e = extruders[tool],
           next = args.count('E') ? (args['E'] * units + (relative_e ? e : 0)) : e;
    bool extrusion = next - e > 1e-7 && part_role;
    extruders[tool] = next;
    if (number < 2) {
      if (old.x != position.x || old.y != position.y || old.z != position.z)
        append(old, position, extrusion);
      continue;
    }
    if (plane != 17)
      throw std::runtime_error("Preview supports XY-plane arcs (G17) only");
    constexpr double pi = 3.14159265358979323846;
    auto sweep = [&](double cx, double cy) {
      double angle =
          std::atan2(position.y - cy, position.x - cx) - std::atan2(old.y - cy, old.x - cx);
      if (number == 2) {
        while (angle >= 0)
          angle -= 2 * pi;
      } else {
        while (angle <= 0)
          angle += 2 * pi;
      }
      return angle;
    };
    double cx, cy;
    if (args.count('R')) {
      double dx = position.x - old.x, dy = position.y - old.y, chord = std::hypot(dx, dy);
      double radius = std::abs(args['R'] * units);
      if (chord < 1e-9 || radius < chord / 2)
        throw std::runtime_error("Invalid radius-defined arc");
      double h = std::sqrt(std::max(0., radius * radius - chord * chord / 4));
      cx = (old.x + position.x) / 2 - dy * h / chord;
      cy = (old.y + position.y) / 2 + dx * h / chord;
      bool major = args['R'] < 0;
      if ((std::abs(sweep(cx, cy)) > pi + 1e-7) != major) {
        cx = (old.x + position.x) / 2 + dy * h / chord;
        cy = (old.y + position.y) / 2 - dx * h / chord;
      }
    } else {
      if (!args.count('I') && !args.count('J'))
        throw std::runtime_error("Arc has no centre or radius");
      cx = args.count('I') ? args['I'] * units + (absolute_arc ? origin.x : old.x) : old.x;
      cy = args.count('J') ? args['J'] * units + (absolute_arc ? origin.y : old.y) : old.y;
    }
    double r = std::hypot(old.x - cx, old.y - cy), start = std::atan2(old.y - cy, old.x - cx);
    if (r < 1e-9)
      throw std::runtime_error("Arc has zero radius");
    double angle = sweep(cx, cy);
    double subdivisions = std::max(2., std::ceil(std::abs(angle) * std::max(r, 1.) / .5));
    if (!std::isfinite(subdivisions) || subdivisions > 100000)
      throw std::runtime_error("Arc exceeds preview subdivision limit");
    unsigned steps = unsigned(subdivisions);
    Vec3 from = old;
    for (unsigned i = 1; i <= steps; ++i) {
      double t = double(i) / steps;
      Vec3 to = i == steps ? position
                           : Vec3{float(cx + r * std::cos(start + angle * t)),
                                  float(cy + r * std::sin(start + angle * t)),
                                  float(old.z + (position.z - old.z) * t)};
      append(from, to, extrusion);
      from = to;
    }
  }
  if (!result.bounds.valid)
    throw std::runtime_error("G-code contains no extruding toolpaths");
  return result;
}
} // namespace slicer
