// SPDX-License-Identifier: AGPL-3.0-or-later
#include "transfer.hpp"
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
int main(int argc, char **argv) {
  if (argc < 5)
    return 2;
  slicer::Transfer transfer;
  try {
    transfer.start({argv[3], argv[1], argv[2], "test-key-do-not-log", "Český cube & space.gcode",
                    "folder with spaces", argc > 5 ? argv[5] : ""});
    const auto start = std::chrono::steady_clock::now();
    while (transfer.status().running) {
      if (std::string(argv[4]) == "cancel" &&
          std::chrono::steady_clock::now() - start > std::chrono::milliseconds(60))
        transfer.cancel();
      if (std::chrono::steady_clock::now() - start > std::chrono::seconds(15))
        return 3;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto s = transfer.status();
    std::cout << nlohmann::json({{"success", s.success},
                                 {"cancelled", s.cancelled},
                                 {"message", s.message},
                                 {"percent", s.percent}})
                     .dump()
              << '\n';
  } catch (const std::exception &e) {
    std::cout
        << nlohmann::json({{"success", false}, {"cancelled", false}, {"message", e.what()}}).dump()
        << '\n';
  }
}
