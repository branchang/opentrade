#include "position.h"

#include <postgresql/soci-postgresql.h>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <fstream>
#include <mutex>

#include "connection.h"
#include "database.h"
#include "logger.h"
#include "task_pool.h"

namespace pt = boost::posix_time;

namespace opentrade {

inline void HandlePnl(double qty, double price, double multiplier,
                      Position* p) {
  const auto qty0 = p->qty;
  auto& realized_pnl = p->realized_pnl;
  auto& avg_px = p->avg_px;
  if ((qty0 > 0) && (qty < 0)) {  // sell trade to cover position
    if (qty0 > -qty) {
      realized_pnl += (price - avg_px) * -qty * multiplier;
    } else {
      realized_pnl += (price - avg_px) * qty0 * multiplier;
      avg_px = price;
    }
  } else if ((qty0 < 0) && (qty > 0)) {  // buy trade to cover position
    if (-qty0 > qty) {
      realized_pnl += (avg_px - price) * qty * multiplier;
    } else {
      realized_pnl += (avg_px - price) * -qty0 * multiplier;
      avg_px = price;
    }
  } else {  // open position
    avg_px = (qty0 * avg_px + qty * price) / (qty0 + qty);
  }
  if (qty0 + qty == 0) avg_px = 0;
}

inline void Position::HandleTrade(bool is_buy, double qty, double price,
                                  double price0, double multiplier,
                                  bool is_bust, bool is_otc, bool is_cx) {
  assert(qty > 0);
  PositionValue::HandleTrade(is_buy, qty, price, price0, multiplier, is_bust,
                             is_otc);
  if (!is_buy) qty = -qty;
  if (is_otc) {
    // do nothing
  } else if (!is_bust) {
    if (qty > 0) {
      total_outstanding_buy_qty -= qty;
      total_bought_qty += qty;
    } else {
      total_outstanding_sell_qty -= -qty;
      total_sold_qty += -qty;
    }
  } else {
    if (qty > 0) {
      total_bought_qty -= qty;
    } else {
      total_sold_qty -= -qty;
    }
  }

  if (is_bust) qty = -qty;
  HandlePnl(qty, price, multiplier, this);
  this->qty += qty;
  if (is_cx) this->cx_qty += qty;
}

inline void Position::HandleFinish(bool is_buy, double leaves_qty,
                                   double price0, double multiplier) {
  assert(leaves_qty);
  if (is_buy) {
    total_outstanding_buy_qty -= leaves_qty;
  } else {
    total_outstanding_sell_qty -= leaves_qty;
  }
  PositionValue::HandleFinish(is_buy, leaves_qty, price0, multiplier);
}

inline void Position::HandleNew(bool is_buy, double qty, double price,
                                double multiplier) {
  assert(qty > 0);
  if (is_buy) {
    total_outstanding_buy_qty += qty;
  } else {
    total_outstanding_sell_qty += qty;
  }
  PositionValue::HandleNew(is_buy, qty, price, multiplier);
}

void PositionManager::Initialize() {
  Instance().sql_ = Database::Session();

  auto& self = Instance();
  auto sql = Database::Session();

  auto path = kStorePath / "session";
  std::ifstream ifs(path.c_str());
  char buf[256] = {0};
  if (ifs.good()) {
    ifs.read(buf, sizeof(buf) - 1);
  } else {
    strncpy(buf, GetNowStr<false>(), sizeof(buf));
    std::ofstream ofs(path.c_str(), std::ofstream::trunc);
    if (!ofs.good()) {
      LOG_FATAL("failed to write file '" << path << "' : " << strerror(errno));
    }
    ofs.write(buf, strlen(buf));
    LOG_INFO("Created new session");
    GlobalOrderBook::Instance().ReadPreviousDayExecIds();
  }
  self.session_ = buf;
  std::string tm = buf;
  LOG_INFO("Session time: " << tm << " UTC");
  LOG_INFO("Loading BOD from database");

  auto query = R"(
    select distinct on (sub_account_id, security_id)
      sub_account_id, broker_account_id, security_id,
      qty, cx_qty, avg_px, realized_pnl, tm
    from position
    where tm < :tm
    order by sub_account_id, security_id, tm desc
  )";
  if (Database::is_sqlite()) {
    query = R"(
    select A.sub_account_id, broker_account_id, A.security_id, qty, cx_qty, avg_px, realized_pnl, A.tm
      from position as A inner join
        (select sub_account_id, security_id, max(tm) as tm  from position where tm < :tm group by sub_account_id,security_id) as B
      on A.sub_account_id = B.sub_account_id and A.security_id = B.security_id and A.tm = B.tm
    )";
  }
  soci::rowset<soci::row> st = (sql->prepare << query, soci::use(tm));
  for (auto it = st.begin(); it != st.end(); ++it) {
    Position p{};
    auto i = 0;
    auto sub_account_id = Database::GetValue(*it, i++, 0);
    auto broker_account_id = Database::GetValue(*it, i++, 0);
    auto security_id = Database::GetValue(*it, i++, 0);
    auto sec = SecurityManager::Instance().Get(security_id);
    if (!sec) continue;
    p.qty = Database::GetValue(*it, i++, 0.);
    p.cx_qty = Database::GetValue(*it, i++, 0.);
    p.avg_px = Database::GetValue(*it, i++, 0.);
    p.realized_pnl = Database::GetValue(*it, i++, 0.);
    Bod bod{};
    bod.qty = p.qty;
    bod.cx_qty = p.cx_qty;
    bod.avg_px = p.avg_px;
    bod.realized_pnl = p.realized_pnl;
    bod.broker_account_id = broker_account_id;
    if (Database::is_sqlite()) {
      tm = Database::GetValue(*it, i++, tm);
      static const pt::ptime kEpoch(boost::gregorian::date(1970, 1, 1));
      bod.tm = (pt::time_from_string(tm) - kEpoch).total_seconds();
    } else {
      std::tm std_tm;
      std_tm = Database::GetValue(*it, i++, std_tm);
      bod.tm = mktime(&std_tm);
    }
    self.bods_.emplace(std::make_pair(sub_account_id, security_id), bod);
    self.sub_positions_.emplace(std::make_pair(sub_account_id, security_id), p);
    auto& p2 =
        self.broker_positions_[std::make_pair(broker_account_id, security_id)];
    p2.realized_pnl += p.realized_pnl;
    HandlePnl(p.qty, p.avg_px, sec->multiplier * sec->rate, &p2);
    p2.qty += p.qty;
    p2.cx_qty += p.cx_qty;
    auto& p3 =
        self.user_positions_[std::make_pair(broker_account_id, security_id)];
    p3.realized_pnl += p.realized_pnl;
    HandlePnl(p.qty, p.avg_px, sec->multiplier * sec->rate, &p3);
    p3.qty += p.qty;
    p3.cx_qty += p.cx_qty;
  }

  for (auto& pair : AccountManager::Instance().sub_accounts_) {
    auto acc = pair.second;
    auto path = kStorePath / ("target-" + std::to_string(acc->id) + ".json");
    if (fs::exists(path)) {
      std::ifstream is(path.string());
      std::stringstream buffer;
      buffer << is.rdbuf();
      try {
        auto str = buffer.str();
        if (str.empty()) continue;
        auto j = json::parse(str);
        extern TargetsPtr LoadTargets(const json& j);
        self.SetTargets(*acc, LoadTargets(j));
        LOG_INFO("Target file " << path << " loaded");
      } catch (std::exception& e) {
        LOG_ERROR("Failed to load " << path << ": " << e.what());
      }
    }
  }
}

void PositionManager::Handle(Confirmation::Ptr cm, bool offline) {
  auto ord = cm->order;
  auto sec = ord->sec;
  auto multiplier = sec->rate * sec->multiplier;
  bool is_buy = ord->IsBuy();
  auto is_otc = ord->type == kOTC || ord->type == kCX;
  auto is_cx = ord->type == kCX;
  assert(cm && ord->id > 0);
  static std::mutex kMutex;
  std::lock_guard<std::mutex> lock(kMutex);
  switch (cm->exec_type) {
    case kPartiallyFilled:
    case kFilled: {
      bool is_bust;
      if (cm->exec_trans_type == kTransNew)
        is_bust = false;
      else if (cm->exec_trans_type == kTransCancel)
        is_bust = true;
      else
        return;
      auto qty = cm->last_shares;
      auto px = cm->last_px;
      auto px0 = ord->price;
      auto& pos = sub_positions_[std::make_pair(ord->sub_account->id, sec->id)];
      pos.HandleTrade(is_buy, qty, px, px0, multiplier, is_bust, is_otc, is_cx);
      broker_positions_[std::make_pair(ord->broker_account->id, sec->id)]
          .HandleTrade(is_buy, qty, px, px0, multiplier, is_bust, is_otc,
                       is_cx);
      user_positions_[std::make_pair(ord->user->id, sec->id)].HandleTrade(
          is_buy, qty, px, px0, multiplier, is_bust, is_otc, is_cx);
      const_cast<SubAccount*>(ord->sub_account)
          ->position_value.HandleTrade(is_buy, qty, px, px0, multiplier,
                                       is_bust, is_otc);
      const_cast<BrokerAccount*>(ord->broker_account)
          ->position_value.HandleTrade(is_buy, qty, px, px0, multiplier,
                                       is_bust, is_otc);
      const_cast<User*>(ord->user)->position_value.HandleTrade(
          is_buy, qty, px, px0, multiplier, is_bust, is_otc);
      if (offline) return;
#ifdef BACKTEST
      return;
#endif
      kDatabaseTaskPool.AddTask([this, pos, cm]() {
        try {
          static User::IdType user_id;
          static SubAccount::IdType sub_account_id;
          static Security::IdType security_id;
          static BrokerAccount::IdType broker_account_id;
          static double qty;
          static double cx_qty;
          static double avg_px;
          static double realized_pnl;
          static std::string info;
          static std::string tm;
          static const char* cmd = R"(
            insert into position(user_id, sub_account_id, security_id, 
            broker_account_id, qty, cx_qty, avg_px, realized_pnl, tm, info) 
            values(:user_id, :sub_account_id, :security_id, :broker_account_id,
            :qty, :cx_qty, :avg_px, :realized_pnl, :tm, :info)
        )";
          static soci::statement st =
              (this->sql_->prepare << cmd, soci::use(user_id),
               soci::use(sub_account_id), soci::use(security_id),
               soci::use(broker_account_id), soci::use(qty), soci::use(cx_qty),
               soci::use(avg_px), soci::use(realized_pnl), soci::use(tm),
               soci::use(info));
          auto ord = cm->order;
          user_id = ord->user->id;
          sub_account_id = ord->sub_account->id;
          security_id = ord->sec->id;
          broker_account_id = ord->broker_account->id;
          qty = pos.qty;
          cx_qty = pos.cx_qty;
          avg_px = pos.avg_px;
          realized_pnl = pos.realized_pnl;
          char side[2];
          side[0] = static_cast<char>(ord->side);
          side[1] = 0;
          char type[2];
          type[0] = static_cast<char>(ord->type);
          type[1] = 0;
          json j = {{"tm", cm->transaction_time},
                    {"qty", cm->last_shares},
                    {"px", cm->last_px},
                    {"exec_id", cm->exec_id},
                    {"side", side},
                    {"type", type},
                    {"id", ord->id}};
          if (!ord->destination.empty()) j["destination"] = ord->destination;
          if (ord->optional) {
            for (auto& pair : *ord->optional) {
              j[pair.first] = ToString(pair.second);
            }
          }
          if (cm->exec_trans_type == kTransCancel) j["bust"] = true;
          if (ord->type == kOTC)
            j["otc"] = true;
          else if (ord->type == kCX)
            j["cx"] = true;
          if (cm->misc) {
            for (auto& pair : *cm->misc) j[pair.first] = pair.second;
          }
          info = j.dump();
          tm = GetNowStr<false>();
          st.execute(true);
        } catch (const soci::postgresql_soci_error& e) {
          LOG_FATAL("Trying update position to database: \n"
                    << e.sqlstate() << ' ' << e.what());
        } catch (const soci::soci_error& e) {
          LOG_FATAL("Trying update position to database: \n" << e.what());
        }
      });
    } break;
    case kUnconfirmedNew:
      if (!is_otc) {
        auto qty = ord->qty;
        auto px = ord->price;
        sub_positions_[std::make_pair(ord->sub_account->id, sec->id)].HandleNew(
            is_buy, qty, px, multiplier);
        broker_positions_[std::make_pair(ord->broker_account->id, sec->id)]
            .HandleNew(is_buy, qty, px, multiplier);
        user_positions_[std::make_pair(ord->user->id, sec->id)].HandleNew(
            is_buy, qty, px, multiplier);
        const_cast<SubAccount*>(ord->sub_account)
            ->position_value.HandleNew(is_buy, qty, px, multiplier);
        const_cast<BrokerAccount*>(ord->broker_account)
            ->position_value.HandleNew(is_buy, qty, px, multiplier);
        const_cast<User*>(ord->user)->position_value.HandleNew(is_buy, qty, px,
                                                               multiplier);
      }
      break;
    case kRiskRejected:
    case kCanceled:
    case kRejected:
    case kExpired:
    case kCalculated:
    case kDoneForDay:
      if (!is_otc) {
        auto qty = cm->leaves_qty;
        auto px = ord->price;
        sub_positions_[std::make_pair(ord->sub_account->id, sec->id)]
            .HandleFinish(is_buy, qty, px, multiplier);
        broker_positions_[std::make_pair(ord->broker_account->id, sec->id)]
            .HandleFinish(is_buy, qty, px, multiplier);
        user_positions_[std::make_pair(ord->user->id, sec->id)].HandleFinish(
            is_buy, qty, px, multiplier);
        const_cast<SubAccount*>(ord->sub_account)
            ->position_value.HandleFinish(is_buy, qty, px, multiplier);
        const_cast<BrokerAccount*>(ord->broker_account)
            ->position_value.HandleFinish(is_buy, qty, px, multiplier);
        const_cast<User*>(ord->user)->position_value.HandleFinish(
            is_buy, qty, px, multiplier);
      }
      break;
    default:
      break;
  }
}

template <typename T1, typename T2>
void UpdateBalance(T1* positions, T2* accs) {
  std::map<int64_t, std::pair<double, double>> balances;
  auto& sm = SecurityManager::Instance();
  for (auto& pair : *positions) {
    auto acc = pair.first.first;
    auto sec_id = pair.first.second;
    auto& pos = pair.second;
    if (!pos.qty && !pos.unrealized_pnl) continue;
    auto sec = sm.Get(sec_id);
    if (!sec) continue;
    auto price = sec->CurrentPrice();
    if (!price) continue;
    auto m = sec->rate * sec->multiplier;
    pos.unrealized_pnl = pos.qty * (price - pos.avg_px) * m;
    auto qty = pos.qty + pos.total_outstanding_buy - pos.total_outstanding_sell;
    if (qty > 0)
      balances[acc].first += qty * price * m;
    else
      balances[acc].second -= qty * price * m;
  }
  for (auto& pair : *accs) {
    auto x = balances[pair.first];
    pair.second->position_value.long_value += x.first;
    pair.second->position_value.short_value += x.second;
  }
}

void PositionManager::UpdatePnl() {
  auto& am = AccountManager::Instance();
  UpdateBalance(&sub_positions_, &am.sub_accounts_);
  UpdateBalance(&broker_positions_, &am.broker_accounts_);
  UpdateBalance(&user_positions_, &am.users_);

  std::map<SubAccount::IdType, std::pair<double, double>> pnls;
  for (auto& pair : sub_positions_) {
    auto acc = pair.first.first;
    auto& pos = pair.second;
    auto& pnl = pnls[acc];
    pnl.first += pos.realized_pnl;
    pnl.second += pos.unrealized_pnl;
  }

#ifdef BACKTEST
  return;
#endif

  auto tm = GetTime();
  for (auto& pair : pnls) {
    auto& pnl = pnls_[pair.first];
    if (std::abs(pnl.realized - pair.second.first) < 1 &&
        std::abs(pnl.unrealized - pair.second.second) < 1)
      continue;
    pnl.realized = pair.second.first;
    pnl.unrealized = pair.second.second;
    if (!pnl.of) {
      auto path = kStorePath / ("pnl-" + std::to_string(pair.first));
      pnl.of = new std::ofstream(path.c_str(), std::ofstream::app);
    }
    (*pnl.of) << tm << ' ' << pnl.realized << ' ' << pnl.unrealized
              << std::endl;
  }

  kSharedTaskPool.AddTask([this]() { this->UpdatePnl(); }, pt::seconds(1));
}

}  // namespace opentrade
