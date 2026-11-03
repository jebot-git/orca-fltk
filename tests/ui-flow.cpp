// SPDX-License-Identifier: AGPL-3.0-or-later
#define ORCA_FLTK_NO_MAIN
#include "../src/main.cpp"
#include <cassert>
#include <chrono>
int main(int argc, char **argv) {
  assert(argc == 5);
  fs::path resources = argv[1], worker = argv[2], model = argv[3], out = argv[4];
  fs::create_directories(out);
  auto config = fs::absolute(out / "config");
  fs::create_directories(config);
  fs::remove(config / "orca-fltk/settings.json");
  setenv("XDG_CONFIG_HOME", config.c_str(), 1);
  dx::init();
  Application app(resources, worker, {resources / "profiles"});
  app.show();
  auto choose = [&](Fl_Choice *widget, const std::vector<const Profile *> &entries,
                    const std::string &name) {
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i]->name == name) {
        widget->value(int(i));
        return;
      }
    throw std::runtime_error("Missing profile in native selector: " + name);
  };
  auto complete = [&] {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (app.job.busy() && std::chrono::steady_clock::now() < until)
      Fl::wait(.03);
    assert(!app.job.busy() && !app.failed && !app.job_result.is_null());
  };
  // Literal flat menus must retain duplicate names and special characters.
  Profile one{out / "account-one/same.json", "Same | PET/PA & path\\font", "process", "",
              Json::object()};
  Profile two{out / "account-two/same.json", one.name, "process", "", Json::object()};
  std::vector<const Profile *> duplicates{&one, &two};
  app.fill(app.process, duplicates, two.file);
  assert(app.process->size() == 3 && app.process->value() == 1);
  assert(app.selected(app.process, duplicates)->file == two.file);
  app.clear_materials();
  assert(app.machine->size() == int(app.machines.size() + 1));
  choose(app.machine, app.machines, "Prusa MK3S 0.4 nozzle");
  app.machine->do_callback();
  complete();
  assert(app.job_result.contains("compatible"));
  assert(!app.processes.empty() && !app.filaments.empty());
  choose(app.process, app.processes, "0.20mm Quality @MK3S");
  choose(app.filament, app.filaments, "Generic PLA @Prusa");
  app.process->do_callback();
  app.filament->do_callback();
  auto printer_path = app.active_printer;
  auto process_path = app.selected(app.process, app.processes)->file;
  auto filament_path = app.selected(app.filament, app.filaments)->file;
  app.filter->value("nothing should match this query");
  app.filter->do_callback();
  assert(app.machines.size() == 1 && app.active_printer == printer_path);
  assert(app.selected(app.machine, app.machines)->file == printer_path);
  assert(app.selected(app.process, app.processes)->file == process_path);
  app.filter->value("");
  app.filter->do_callback();
  choose(app.machine, app.machines, "Bambu Lab X1 Carbon 0.4 nozzle");
  app.machine->do_callback();
  complete();
  assert(app.compatible_printer == app.active_printer && app.active_printer != printer_path);
  choose(app.machine, app.machines, "Prusa MK3S 0.4 nozzle");
  app.machine->do_callback();
  assert(!app.job.busy()); // cached compatibility, per-printer remembered choices
  assert(app.selected(app.process, app.processes)->file == process_path);
  assert(app.selected(app.filament, app.filaments)->file == filament_path);
  auto stale = app.compatibility_cache.at(printer_path.string());
  stale["machine"] = "wrong-printer";
  app.compatible_profiles(stale);
  assert(app.selected(app.process, app.processes)->file == process_path);
  app.scan();
  complete();
  assert(app.selected(app.process, app.processes)->file == process_path);
  assert(app.selected(app.filament, app.filaments)->file == filament_path);
  {
    Application restored(resources, worker, {resources / "profiles"});
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (restored.job.busy() && std::chrono::steady_clock::now() < until)
      Fl::wait(.03);
    assert(!restored.job.busy() && !restored.failed);
    assert(restored.active_printer == printer_path);
    assert(restored.selected(restored.process, restored.processes)->file == process_path);
    assert(restored.selected(restored.filament, restored.filaments)->file == filament_path);
  }
  assert((fs::status(app.settings_file).permissions() & fs::perms::others_all) == fs::perms::none);
  app.open_file(model);
  app.rotation = {90, 0, 0};
  app.scale = 1.5;
  app.viewport->mesh =
      std::make_shared<Mesh>(transform_mesh(*app.original_mesh, app.rotation, app.scale));
  auto output = out / "gui-sliced.gcode";
  fs::remove(output);
  app.output->value(output.c_str());
  app.slice->do_callback();
  complete();
  assert(fs::is_regular_file(output));
  assert(app.viewport->paths && app.viewport->paths->heights.size() == 60);
  assert(app.xr->active());
  if (auto server = getenv("ORCA_TEST_UPLOAD_URL")) {
    for (const char *protocol : {"octoprint", "moonraker"}) {
      app.start_transfer(
          {output, protocol, server, "test-key-do-not-log", "gui-sliced.gcode", "", ""});
      assert(!app.send->active() && app.abort->active());
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (app.transfer_active && std::chrono::steady_clock::now() < until)
        Fl::wait(.03);
      assert(!app.transfer_active && app.transfer.status().success);
      assert(app.send->active() && !app.abort->active());
      assert(read_json(app.settings_file).dump().find("test-key-do-not-log") == std::string::npos);
      assert(app.selected(app.process, app.processes)->file == process_path);
    }
  }
  app.viewport->capture(out / "gui-sliced.ppm");
  auto report = out / "report.json";
  fs::remove(report);
  write_json_new(report,
                 {{"passed", true},
                  {"profile_conditions_filtered_by_orca", true},
                  {"native_gui_slice", true},
                  {"native_gui_network_transfer", getenv("ORCA_TEST_UPLOAD_URL") != nullptr},
                  {"orientation_and_scale", true},
                  {"stable_duplicate_names_filter_rescan_restore", true},
                  {"layers", app.viewport->paths->heights.size()},
                  {"gcode", output.string()}});
  assert(app.generated_slice);
  app.process->do_callback();
  assert(app.gcode.empty() && !app.send->active() && !app.xr->active());
  app.open_file(output); // explicitly opened G-code remains transferable
  assert(!app.generated_slice && app.send->active());
  app.process->do_callback();
  assert(app.send->active() && app.gcode == fs::absolute(output));
  app.hide();
  std::cout << "PASS native profile selection, Orca compatibility filtering, slicing, progress and "
               "loaded toolpath preview\n";
}
