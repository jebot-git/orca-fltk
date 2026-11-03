// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "profiles.hpp"
#include <boost/log/utility/setup/console.hpp>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <pthread.h>
#include <set>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
using namespace Slic3r;
using slicer::Json;
namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void emit(Json value) { std::cout << value.dump() << std::endl; }
constexpr auto rule = ForwardCompatibilitySubstitutionRule::EnableSilent;
void publish(const fs::path &source, const fs::path &destination) {
  std::string pattern = (destination.parent_path() / ".orca-fltk-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  int fd = mkstemp(name.data());
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), "Cannot create output file");
  fs::path temporary(name.data());
  try {
    fs::copy_file(source, temporary, fs::copy_options::overwrite_existing);
    if (fsync(fd))
      throw std::system_error(errno, std::generic_category(), "Cannot flush output");
    close(fd);
    fd = -1;
    if (interrupted)
      throw std::runtime_error("Slicing cancelled");
    if (syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, destination.c_str(),
                RENAME_NOREPLACE))
      throw std::system_error(errno, std::generic_category(), "Cannot publish output");
  } catch (...) {
    if (fd >= 0)
      close(fd);
    fs::remove(temporary);
    throw;
  }
}

fs::path profile_scope(const fs::path &path, const std::string &type) {
  for (auto directory = path.parent_path(); directory != directory.root_path();
       directory = directory.parent_path())
    if (directory.filename() == type)
      return directory.parent_path();
  return path.parent_path();
}
Preset::Type preset_type(const std::string &type) {
  if (type == "machine")
    return Preset::TYPE_PRINTER;
  if (type == "filament")
    return Preset::TYPE_FILAMENT;
  if (type == "process")
    return Preset::TYPE_PRINT;
  throw std::runtime_error("Unsupported profile type");
}
DynamicPrintConfig resolve(PresetBundle &bundle, const fs::path &path, const std::string &type,
                           const slicer::Profiles &catalog, std::set<fs::path> &chain) {
  auto canonical = fs::canonical(path);
  if (!chain.insert(canonical).second)
    throw std::runtime_error("Profile inheritance cycle: " + path.string());
  if (chain.size() > 64)
    throw std::runtime_error("Profile inheritance exceeds 64 levels");
  DynamicPrintConfig config;
  std::string error;
  if (bundle.resolve_preset_config(config, preset_type(type), canonical.string(), rule, error)) {
    chain.erase(canonical);
    return config;
  }
  auto doc = slicer::read_json(canonical);
  // A file inside an upstream vendor manifest must use the canonical vendor loader.
  if (auto p = catalog.find(canonical); p && !p->vendor.empty())
    throw std::runtime_error("Cannot resolve vendor profile: " + error);
  auto parent = doc.value("inherits", std::string());
  if (!parent.empty()) {
    const slicer::Profile *local = nullptr;
    for (const auto &entry : catalog.entries())
      if (entry.type == type && entry.name == parent && entry.vendor.empty() &&
          profile_scope(entry.file, type) == profile_scope(canonical, type)) {
        if (local && local->file != entry.file)
          throw std::runtime_error("Ambiguous inherited user profile: " + parent);
        local = &entry;
      }
    if (local)
      config = resolve(bundle, local->file, type, catalog, chain);
    else if (!bundle.resolve_system_preset(config, preset_type(type), parent, rule, error))
      throw std::runtime_error("Cannot resolve inherited profile " + parent + ": " + error);
  } else {
    if (type == "machine")
      config = bundle.printers.get_presets().front().config;
    if (type == "filament")
      config = bundle.filaments.get_presets().front().config;
    if (type == "process")
      config = bundle.prints.get_presets().front().config;
  }
  if (doc.contains("include") && !doc["include"].empty())
    throw std::runtime_error("User profile template includes require a vendor manifest");
  DynamicPrintConfig leaf;
  std::map<std::string, std::string> values;
  std::string reason;
  ConfigSubstitutionContext substitutions(rule);
  int loaded = leaf.load_from_json(canonical.string(), substitutions, true, values, reason);
  if (loaded != 0)
    throw std::runtime_error("Invalid profile: " + reason);
  config.apply(leaf);
  if (!substitutions.substitutions.empty())
    emit({{"event", "warning"},
          {"message",
           "Orca applied forward-compatibility substitutions while loading " + canonical.string()},
          {"count", substitutions.substitutions.size()}});
  chain.erase(canonical);
  return config;
}
void transform_model(Model &model, const Json &request) {
  auto parameters = request.value("transform", Json::object());
  auto angles = parameters.value("rotation_deg", std::vector<double>{0, 0, 0});
  double scale = parameters.value("scale", 1.0);
  if (angles.size() != 3 || !std::isfinite(scale) || scale <= 0 || scale > 100)
    throw std::runtime_error("Invalid model transform");
  bool changed = std::abs(scale - 1) > 1e-9;
  for (double angle : angles) {
    if (!std::isfinite(angle) || std::abs(angle) > 36000)
      throw std::runtime_error("Invalid model rotation");
    changed |= std::abs(angle) > 1e-9;
  }
  if (!changed)
    return;
  auto extension = fs::path(request.at("input").get<std::string>()).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (extension != ".stl")
    throw std::runtime_error(
        "Orientation and scale edits currently apply to STL imports; 3MF placements are preserved");
  for (auto *object : model.objects) {
    object->scale(scale);
    for (int axis = 0; axis < 3; ++axis)
      if (std::abs(angles[axis]) > 1e-9)
        object->rotate(angles[axis] * 3.14159265358979323846 / 180, Axis(axis));
    object->ensure_on_bed();
  }
}
DynamicPrintConfig assemble(const Json &request, PresetBundle &bundle,
                            const slicer::Profiles &catalog, DynamicPrintConfig project) {
  Preset machine(Preset::TYPE_PRINTER, ""), process(Preset::TYPE_PRINT, "");
  std::vector<Preset> filaments{Preset(Preset::TYPE_FILAMENT, "")};
  for (auto type : {"machine", "process", "filament"}) {
    auto file = fs::path(request.at("profiles").at(type).get<std::string>());
    auto doc = slicer::read_json(file);
    std::set<fs::path> chain;
    auto config = resolve(bundle, file, type, catalog, chain);
    auto name = doc.value("name", file.stem().string());
    if (std::string(type) == "machine") {
      machine.name = name;
      machine.config = std::move(config);
    }
    if (std::string(type) == "process") {
      process.name = name;
      process.config = std::move(config);
    }
    if (std::string(type) == "filament") {
      filaments[0].name = name;
      filaments[0].config = std::move(config);
    }
  }
  if (!is_compatible_with_printer(process.config, Preset::TYPE_PRINT, machine.config, machine.name))
    throw std::runtime_error("Process profile is incompatible with the selected printer");
  if (!is_compatible_with_printer(filaments[0].config, Preset::TYPE_FILAMENT, machine.config,
                                  machine.name))
    throw std::runtime_error("Filament profile is incompatible with the selected printer");
  auto full = PresetBundle::construct_full_config(machine, process, DynamicPrintConfig(), filaments,
                                                  true, std::nullopt);
  if (request.value("use_project_settings", false))
    full.apply(project);
  if (request.contains("overrides"))
    for (const auto &[key, value] : request["overrides"].items()) {
      if (!value.is_string())
        throw std::runtime_error("Override values must use Orca's serialized setting format");
      full.set_deserialize_strict(key, value.get<std::string>());
    }
  if (auto technology = full.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
      technology && technology->value != ptFFF)
    throw std::runtime_error("This frontend supports FFF printer profiles only");
  full.normalize_fdm();
  return full;
}
} // namespace
int main(int argc, char **argv) {
  sigset_t empty;
  sigemptyset(&empty);
  pthread_sigmask(SIG_SETMASK, &empty, nullptr);
  try {
    if (argc == 2 && std::string(argv[1]) == "--version") {
      std::cout << "Orca FLTK worker / Orca " << SoftFever_VERSION << " / " << ORCA_PIN << '\n';
      return 0;
    }
    if (argc != 2)
      throw std::runtime_error("Usage: orca-fltk-worker REQUEST.json");
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    boost::log::add_console_log(std::cerr);
    auto request = slicer::read_json(argv[1]);
    auto action = request.at("action").get<std::string>();
    auto work = fs::absolute(request.at("work_dir").get<std::string>());
    fs::create_directories(work);
    set_resources_dir(fs::absolute(request.at("resources").get<std::string>()).string());
    set_data_dir((work / "orca-data").string());
    fs::create_directories(work / "orca-data");
    if (action == "inspect") {
      DynamicPrintConfig embedded;
      ConfigSubstitutionContext substitutions(rule);
      auto model =
          Model::read_from_file(request.at("input"), &embedded, &substitutions,
                                LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel |
                                    LoadStrategy::LoadConfig | LoadStrategy::CheckVersion);
      transform_model(model, request);
      auto mesh = model.mesh();
      auto output = work / "model.stl";
      if (!mesh.write_binary(output.c_str()))
        throw std::runtime_error("Could not export imported geometry");
      emit({{"event", "result"},
            {"mesh", output.string()},
            {"objects", model.objects.size()},
            {"has_project_settings", !embedded.keys().empty()},
            {"engine", ORCA_PIN}});
      return 0;
    }
    std::vector<fs::path> roots{fs::path(resources_dir()) / "profiles"};
    for (const auto &root : request.value("profile_roots", Json::array()))
      roots.emplace_back(root.get<std::string>());
    slicer::Profiles catalog;
    catalog.scan(roots);
    PresetBundle bundle;
    if (action == "catalog") {
      auto file = fs::path(request.at("machine").get<std::string>());
      auto document = slicer::read_json(file);
      std::set<fs::path> chain;
      auto printer = resolve(bundle, file, "machine", catalog, chain);
      auto name = document.value("name", file.stem().string());
      Json compatible = {{"process", Json::array()}, {"filament", Json::array()}};
      std::string parent;
      if (auto option = printer.option<ConfigOptionString>("inherits"))
        parent = option->value;
      size_t failures = 0;
      for (const auto &entry : catalog.entries()) {
        if ((entry.type != "process" && entry.type != "filament") || !entry.instantiated())
          continue;
        auto listed = entry.document.find("compatible_printers");
        if (listed != entry.document.end() && listed->is_array() && !listed->empty() &&
            std::find(listed->begin(), listed->end(), name) == listed->end() &&
            std::find(listed->begin(), listed->end(), parent) == listed->end())
          continue;
        try {
          std::set<fs::path> lineage;
          auto candidate = resolve(bundle, entry.file, entry.type, catalog, lineage);
          if (is_compatible_with_printer(candidate, preset_type(entry.type), printer, name))
            compatible[entry.type].push_back(entry.file.string());
        } catch (const std::exception &) {
          ++failures;
        }
        if (interrupted)
          throw std::runtime_error("Profile loading cancelled");
      }
      Json defaults = {{"process", ""}, {"filament", Json::array()}};
      if (auto option = printer.option<ConfigOptionString>("default_print_profile"))
        defaults["process"] = option->value;
      if (auto option = printer.option<ConfigOptionStrings>("default_filament_profile"))
        defaults["filament"] = option->values;
      if (failures)
        emit({{"event", "warning"},
              {"message", std::to_string(failures) +
                              " candidate profiles could not be resolved and were excluded"}});
      emit({{"event", "result"},
            {"compatible", compatible},
            {"machine", fs::weakly_canonical(file).string()},
            {"defaults", defaults},
            {"engine", ORCA_PIN}});
      return 0;
    }
    DynamicPrintConfig embedded;
    Model model;
    if (action == "slice") {
      emit({{"event", "progress"},
            {"percent", 0},
            {"message", "Reading model and project settings"}});
      ConfigSubstitutionContext substitutions(rule);
      PlateDataPtrs plates;
      std::vector<Preset *> project_presets;
      model = Model::read_from_file(request.at("input"), &embedded, &substitutions,
                                    LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel |
                                        LoadStrategy::LoadConfig | LoadStrategy::CheckVersion,
                                    &plates, &project_presets);
      auto release = [&] {
        for (auto *p : plates)
          delete p;
        for (auto *p : project_presets)
          delete p;
      };
      auto count = plates.size();
      release();
      if (count > 1)
        throw std::runtime_error("Multi-plate projects require an explicit plate export from "
                                 "OrcaSlicer; this frontend currently slices one plate");
      if (model.objects.empty())
        throw std::runtime_error("Model is empty");
      for (const auto *object : model.objects)
        for (const auto *volume : object->volumes)
          if (volume->is_mm_painted() || volume->extruder_id() > 1)
            throw std::runtime_error("This frontend currently slices one filament; multi-material "
                                     "projects require OrcaSlicer");
    }
    auto config = assemble(request, bundle, catalog, embedded);
    if (auto commands = config.option<ConfigOptionStrings>("post_process");
        commands && std::any_of(commands->values.begin(), commands->values.end(),
                                [](const std::string &command) { return !command.empty(); }))
      emit({{"event", "warning"},
            {"message", "Profile post-processing commands are preserved but are not executed by "
                        "this frontend"}});
    config.save_to_json((work / "effective-config.json").string(),
                        "Orca FLTK effective job configuration", "user", SoftFever_VERSION);
    if (action == "resolve") {
      emit({{"event", "result"},
            {"config", (work / "effective-config.json").string()},
            {"engine", ORCA_PIN}});
      return 0;
    }
    if (action != "slice")
      throw std::runtime_error("Unknown worker action");
    transform_model(model, request);
    if (request.value("center_on_bed", true)) {
      auto points = config.option<ConfigOptionPoints>("printable_area");
      if (!points || points->values.empty())
        throw std::runtime_error("Printer profile has no printable area");
      Vec2d low = points->values.front(), high = low;
      for (const auto &point : points->values) {
        low = low.cwiseMin(point);
        high = high.cwiseMax(point);
      }
      model.center_instances_around_point((low + high) * .5);
      for (auto *object : model.objects)
        object->ensure_on_bed();
    }
    Print print;
    print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
      if (interrupted)
        print.cancel();
      emit({{"event", "progress"}, {"percent", status.percent}, {"message", status.text}});
    });
    print.apply(model, config);
    std::vector<StringObjectException> warnings;
    auto invalid = print.validate(&warnings);
    if (!invalid.string.empty())
      throw std::runtime_error(invalid.string);
    for (const auto &warning : warnings)
      emit({{"event", "warning"}, {"message", warning.string}});
    if (print.empty())
      throw std::runtime_error("No printable objects fit inside the selected printer's volume");
    auto output = fs::absolute(request.at("output").get<std::string>());
    if (fs::exists(output))
      throw std::runtime_error("Output already exists: " + output.string());
    print.process();
    if (interrupted)
      throw std::runtime_error("Slicing cancelled");
    auto temporary = work / "output.gcode";
    GCodeProcessorResult processed;
    auto written = print.export_gcode(temporary.string(), &processed);
    if (interrupted)
      throw std::runtime_error("Slicing cancelled");
    publish(written, output);
    emit({{"event", "result"},
          {"output", output.string()},
          {"config", (work / "effective-config.json").string()},
          {"engine", ORCA_PIN}});
    return 0;
  } catch (const std::exception &ex) {
    emit({{"event", "error"}, {"message", ex.what()}});
    return 1;
  }
}
