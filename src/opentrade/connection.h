#ifndef OPENTRADE_CONNECTION_H_
#define OPENTRADE_CONNECTION_H_

#include <boost/asio.hpp>
#include <boost/unordered_map.hpp>
#include <map>
#include <memory>
#include <unordered_map>

#include "3rd/json.hpp"
#include "account.h"
#include "algo.h"
#include "market_data.h"
#include "order.h"
#include "security.h"

using json = nlohmann::json;

namespace opentrade {

struct Transport {
  typedef std::shared_ptr<Transport> Ptr;
  virtual void Send(const std::string& msg) = 0;
  virtual std::string GetAddress() const = 0;
  bool stateless = false;
};

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  typedef std::shared_ptr<Connection> Ptr;
  Connection(Transport::Ptr transport,
             std::shared_ptr<boost::asio::io_service> service);
  ~Connection();
  void OnMessageAsync(const std::string&);
  void OnMessageSync(const std::string&, const std::string& token = "");
  void OnAlgo(const json& j, const std::string& msg);
  void OnOrder(const json& j, const std::string& msg);
  void OnSecurities(const json& j);
  void OnAdmin(const json& j);
  void OnAdminUsers(const json& j, const std::string& name,
                    const std::string& action);
  void OnAdminBrokerAccounts(const json& j, const std::string& name,
                             const std::string& action);
  void OnAdminSubAccounts(const json& j, const std::string& name,
                          const std::string& action);
  void OnAdminExchanges(const json& j, const std::string& name,
                        const std::string& action);
  void OnPosition(const json& j, const std::string& msg);
  void OnTarget(const json& j, const std::string& msg);
  void OnLogin(const std::string& action, const json& j);
  void Send(Confirmation::Ptr cm);
  void Send(const SubAccount& acc, const std::string& msg);
  void Send(const Algo& algo, const std::string& status,
            const std::string& body, uint32_t seq);
  void Close() { closed_ = true; }
  void SendTestMsg(const std::string& token, const std::string& msg,
                   bool stopped);

 protected:
  void HandleMessageSync(const std::string&, const std::string& token);
  void PublishMarketdata();
  void PublishMarketStatus();
  void Send(const std::string& msg) {
    sent_ = true;
    if (!closed_) transport_->Send(msg);
  }
  void Send(const json& msg) { Send(msg.dump()); }
  void Send(const Confirmation& cm, bool offline);
  void Send(Algo::IdType id, time_t tm, const std::string& token,
            const std::string& name, const std::string& status,
            const std::string& body, uint32_t seq, bool offline);
  std::string GetAddress() const { return transport_->GetAddress(); }
  bool Disable(const json& j, AccountBase* acc);
  static std::string GetDisabledSubAccounts();

 private:
  Transport::Ptr transport_;
  const User* user_ = nullptr;
  boost::unordered_map<std::pair<Security::IdType, DataSrc::IdType>,
                       std::pair<MarketData, uint32_t>>
      subs_;
#if BOOST_VERSION < 106600
  boost::asio::strand strand_;
#else
  boost::asio::io_context::strand strand_;
#endif
  boost::asio::deadline_timer timer_;
  std::map<std::string, bool> ecs_;
  std::map<std::string, bool> mds_;
  std::map<SubAccount::IdType, std::pair<double, double>> pnls_;
  boost::unordered_map<std::pair<SubAccount::IdType, Security::IdType>,
                       std::pair<double, double>>
      single_pnls_;
  tbb::concurrent_unordered_set<std::string> test_algo_tokens_;
  bool sub_pnl_ = false;
  bool closed_ = false;
  bool sent_ = false;
  friend class Server;
  friend class AlgoManager;
  friend class GlobalOrderBook;
};

}  // namespace opentrade

#endif  // OPENTRADE_CONNECTION_H_
