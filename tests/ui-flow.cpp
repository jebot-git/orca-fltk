// SPDX-License-Identifier: AGPL-3.0-or-later
#define ORCA_FLTK_NO_MAIN
#include "../src/main.cpp"
#include <cassert>
#include <chrono>
int main(int argc, char **argv) {
  assert(argc == 5);
  fs::path resources = argv[1], worker = argv[2], model = argv[3], out = argv[4];
  fs::create_directories(out);
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
  choose(app.machine, app.machines, "Prusa MK3S 0.4 nozzle");
  app.machine->do_callback();
  complete();
  assert(app.job_result.contains("compatible"));
  assert(!app.processes.empty() && !app.filaments.empty());
  choose(app.process, app.processes, "0.20mm Quality @MK3S");
  choose(app.filament, app.filaments, "Generic PLA @Prusa");
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
  app.viewport->capture(out / "gui-sliced.ppm");
  auto report = out / "report.json";
  fs::remove(report);
  write_json_new(report, {{"passed", true},
                          {"profile_conditions_filtered_by_orca", true},
                          {"native_gui_slice", true},
                          {"orientation_and_scale", true},
                          {"layers", app.viewport->paths->heights.size()},
                          {"gcode", output.string()}});
  app.hide();
  std::cout << "PASS native profile selection, Orca compatibility filtering, slicing, progress and "
               "loaded toolpath preview\n";
}
