#ifndef OPENTRADE_SERVER_H_
#define OPENTRADE_SERVER_H_

#include "algo.h"

namespace opentrade {

class Server {
 public:
  static void Start(int port, int nthreads = 1);
  static void Publish(Confirmation::Ptr cm);
  static void Publish(const Algo& algo, const std::string& status,
                      const std::string& body, uint32_t seq);
  static void Publish(const SubAccount& acc, const std::string& msg);
  static void Publish(const std::string& msg);
  static void PublishTestMsg(const std::string& token, const std::string& msg,
                             bool stopped = false);
  static void Stop();
};

}  // namespace opentrade

#endif  // OPENTRADE_SERVER_H_
