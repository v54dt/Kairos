#include <iostream>

#include "feed.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: kairos_sidecar <config.toml>\n";
    return 1;
  }
  return kairos::concords::RunFeed(argv[1]);
}
