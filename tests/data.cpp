// SPDX-License-Identifier: AGPL-3.0-or-later
#include "geometry.hpp"
#include "profiles.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace slicer;
static void put(const fs::path &p, const std::string &text) {
  std::ofstream(p, std::ios::binary) << text;
}
template <class F> void rejects(F f) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception &) {
    caught = true;
  }
  assert(caught);
}
int main() {
  auto dir = fs::temp_directory_path() / ("orca-fltk-data-" + std::to_string(getpid()));
  fs::create_directories(dir / "profiles/machine");
  try {
    const std::string original =
        "{\n  \"type\":\"machine\",\"name\":\"Česká "
        "tiskárna\",\"inherits\":\"parent\",\"instantiation\":\"true\",\"nozzle_diameter\":[\"0."
        "4\"],\"future_extension\":{\"unknown\":[true,2,\"{macro}\\nG28\"]}}\n";
    put(dir / "profiles/machine/printer.json", original);
    put(dir / "profiles/machine/parent.json",
        R"({"type":"machine","name":"parent","instantiation":"false"})");
    fs::create_directories(dir / "profiles/filament");
    put(dir / "profiles/filament/pla.json",
        R"({"name":"PLA","compatible_printers":["Česká tiskárna"],"temperature":["210"]})");
    put(dir / "profiles/broken.json", "{");
    Profiles profiles;
    profiles.scan({dir / "profiles", dir / "profiles"});
    assert(profiles.entries().size() == 3);
    assert(profiles.choices("machine").size() == 1);
    assert(profiles.choices("filament", "Česká tiskárna").size() == 1);
    assert(profiles.choices("filament", "Other printer").empty());
    assert(profiles.warnings().size() == 1);
    auto profile = profiles.find(dir / "profiles/machine/printer.json");
    assert(profile);
    Profiles::export_original(*profile, dir / "copy.json");
    std::ifstream copy(dir / "copy.json");
    assert(std::string(std::istreambuf_iterator<char>(copy), {}) == original);
    rejects([&] { Profiles::export_original(*profile, dir / "copy.json"); });
    auto gcode = dir / "moves.gcode";
    put(gcode, "G21\nG90\nM82\nG92 E0\nG1 Z0.2\nG1 X10 Y0 E1\nG1 X20 E2\nG1 E1\nG1 X30 E2\nG1 "
               "Z0.4\nM83\nG1 X40 E1\nG1 X45 E-1\nG1 X50 E0\nT1\nM82\nG92 E0\nG1 X60 E1\n");
    auto paths = read_gcode(gcode);
    assert(paths.heights.size() == 2);
    size_t extruding = 0;
    for (const auto &s : paths.segments)
      extruding += s.extrusion;
    assert(extruding == 5);
    assert(paths.segments.back().tool == 1);
    assert(std::abs(paths.bounds.max.x - 60) < .001);
    put(gcode, "G21\nG90\nM83\nG1Z0.2\nG1X10Y20E1\nG1X20Y30E1\n");
    paths = read_gcode(gcode);
    assert(std::abs(paths.bounds.max.x - 20) < .001);
    assert(std::abs(paths.bounds.max.y - 30) < .001);
    put(gcode, "G21\nG90\nM83\nG1 X10 Y0 Z0.2\nG3 X0 Y10 I-10 J0 E1\n");
    paths = read_gcode(gcode);
    assert(paths.segments.size() > 20);
    assert(paths.bounds.max.x <= 10.001 && paths.bounds.max.y <= 10.001);
    assert(paths.bounds.min.x >= -.001 && paths.bounds.min.y >= -.001);
    rejects([&] { read_gcode(gcode, 3); });
    put(gcode, "G21\nG90\nM83\nG1 X10 Y0 Z0.2\nG3 X0 Y10 R10 E1\n");
    paths = read_gcode(gcode);
    assert(paths.segments.size() > 20 && paths.bounds.min.x >= -.001 &&
           paths.bounds.min.y >= -.001);
    put(gcode, "G21\nG90\nM83\nG1 X10 Y0 Z0.2\nG92 X0\nG1 X10 E1\n");
    paths = read_gcode(gcode);
    assert(std::abs(paths.bounds.min.x - 10) < .001 && std::abs(paths.bounds.max.x - 20) < .001);
    put(gcode, "G18\nM83\nG2 X10 Z10 I0 K10 E1\n");
    rejects([&] { read_gcode(gcode); });
    put(gcode, ";TYPE:Custom\nM83\nG1 X100 Y0 E5\n;LAYER_CHANGE\nG1 X10 Y10 Z0.2\n;TYPE:Outer "
               "wall\nG1 X20 E1\n");
    paths = read_gcode(gcode);
    assert(paths.heights.size() == 1 && paths.bounds.max.x == 20 && paths.bounds.min.x == 10);

    put(gcode, "G1 X5 Y3\n");
    rejects([&] { read_gcode(gcode); });
    put(dir / "triangle.stl", "solid sample\n facet normal 0 0 1\n outer loop\n vertex 0 0 0\n "
                              "vertex 10 0 0\n vertex 0 10 0\n endloop\n endfacet\nendsolid\n");
    auto mesh = read_stl(dir / "triangle.stl");
    assert(mesh.triangles.size() == 1);
    assert(mesh.bounds.max.x == 10 && mesh.bounds.max.y == 10);
    auto transformed = transform_mesh(mesh, {90, 0, 0}, 2);
    assert(std::abs(transformed.bounds.max.x - transformed.bounds.min.x - 20) < .001);
    assert(std::abs(transformed.bounds.max.y - transformed.bounds.min.y) < .001);
    assert(std::abs(transformed.bounds.max.z - 20) < .001);
    assert(transformed.bounds.min.z == 0 && mesh.bounds.max.y == 10);
    rejects([&] { transform_mesh(mesh, {0, 0, 0}, 0); });
    std::string binary(134, '\0');
    binary.replace(0, 5, "solid");
    binary[80] = 1;
    float coords[] = {0, 0, 0, 2, 0, 0, 0, 3, 0};
    for (int i = 0; i < 9; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &coords[i], 4);
      for (int b = 0; b < 4; ++b)
        binary[96 + i * 4 + b] = char(bits >> (b * 8));
    }
    put(dir / "binary.stl", binary);
    mesh = read_stl(dir / "binary.stl");
    assert(mesh.triangles.size() == 1 && mesh.bounds.max.y == 3);
    put(dir / "invalid.stl", "solid\nvertex 0 0 0\n");
    rejects([&] { read_stl(dir / "invalid.stl"); });
    fs::remove_all(dir);
    std::cout << "PASS profile byte-for-byte roundtrip, unknown keys, arrays, Unicode, identity, "
                 "template filtering, malformed files; G-code modes, retraction, tools, compact "
                 "tokens, arcs and limits; ASCII/binary STL\n";
  } catch (...) {
    fs::remove_all(dir);
    throw;
  }
}
