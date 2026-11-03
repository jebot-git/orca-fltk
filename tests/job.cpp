// SPDX-License-Identifier: AGPL-3.0-or-later
#include "job.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace slicer;
int main() {
  auto dir = new_job_directory();
  auto worker = dir / "worker with spaces";
  std::ofstream(worker) << "#!/usr/bin/python3\nimport "
                           "sys,time,json\nprint(json.dumps({'argument':sys.argv[1]}),flush=True)"
                           "\nif 'cancel' in sys.argv[1]:time.sleep(60)\n";
  fs::permissions(worker, fs::perms::owner_all);
  auto request = dir / "profile ČR ; literal.json";
  Job job;
  job.start(worker, request);
  std::string received;
  int status = -1;
  for (int i = 0; i < 200 && job.busy(); ++i) {
    job.poll([&](const std::string &line) { received += line; }, [&](int code) { status = code; });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(!job.busy() && status == 0);
  assert(Json::parse(received)["argument"] == request.string());
  job.start(worker, dir / "cancel.json");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  job.cancel();
  for (int i = 0; i < 300 && job.busy(); ++i) {
    job.poll([](const std::string &) {}, [&](int code) { status = code; });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(!job.busy() && status == 130);
  fs::remove_all(dir);
  std::cout << "PASS worker literal arguments, pipe delivery, child reaping and cancellation\n";
}
