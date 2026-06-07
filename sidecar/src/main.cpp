#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "publisher.h"
#include "quote_encode.h"

static kairos::Quote synthetic(std::int64_t tick) {
  std::int64_t last = 58000 + (tick % 20) * 5;
  return kairos::Quote{"2330",
                       kairos::Exchange::Twse,
                       tick * 1000,
                       {{last - 50, 2, 100}},
                       {{last + 50, 2, 80}},
                       last,
                       2,
                       10,
                       false};
}

int main(int argc, char** argv) {
  bool simfeed = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--simfeed") == 0) {
      simfeed = true;
    }
  }
  if (!simfeed) {
    std::cerr << "usage: kairos_sidecar --simfeed   (concords mode comes later)\n";
    return 1;
  }

  kairos::Publisher publisher("", 1001);
  std::cout << "kairos-sidecar: simfeed publishing synthetic 2330 to aeron:ipc stream 1001\n";
  for (std::int64_t tick = 0;; ++tick) {
    publisher.offer(kairos::encode_quote_envelope(synthetic(tick)));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}
