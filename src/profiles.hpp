// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace slicer {
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Profile {
  fs::path file;
  std::string name, type, vendor;
  Json document;
  bool instantiated() const;
};
class Profiles {
  std::vector<Profile> entries_;
  std::vector<std::string> warnings_;

public:
  void scan(const std::vector<fs::path> &roots);
  const std::vector<Profile> &entries() const { return entries_; }
  const std::vector<std::string> &warnings() const { return warnings_; }
  std::vector<const Profile *> choices(const std::string &type,
                                       const std::string &printer = "") const;
  const Profile *find(const fs::path &file) const;
  // An exact byte copy retains unknown keys, array types, macros and inheritance.
  static void export_original(const Profile &, const fs::path &destination);
};
Json read_json(const fs::path &file);
void write_json_new(const fs::path &file, const Json &document);
} // namespace slicer
