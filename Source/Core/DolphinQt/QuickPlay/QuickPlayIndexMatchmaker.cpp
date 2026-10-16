// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/QuickPlay/QuickPlayIndexMatchmaker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/NetplaySettings.h"
#include "UICommon/NetPlayIndex.h"

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Readable in the ordinary server browser; the handshake tokens follow PPQP1.
constexpr std::string_view NAME_PREFIX = "P+ Quick Play \xc2\xb7 PPQP1 ";
constexpr std::string_view NAME_FILTER = "PPQP1";
constexpr std::string_view GAME = "Project+";
constexpr int PORT = 2626;
// Not the plain build version: ordinary browsers grey Quick Play listings out.
constexpr std::string_view VERSION_SUFFIX = "+ppqp1";
// The origin digest token. Quick Play hands off to ordinary NetPlay, which has no origin; a
// listing with any other digest is ignored.
constexpr std::string_view DIGEST = "00000000";
// Not secret: the derivation is open source. It keeps ordinary browsers out of the room.
constexpr std::string_view ROOM_KEY = "PPQP1:none";

// Same lifetime as the coordinator's queue ticket.
constexpr auto SEARCH_TIMEOUT = 180s;
constexpr auto SEARCH_LIST_INTERVAL = 3s;
constexpr auto CLAIM_LIST_INTERVAL = 2s;
constexpr auto CLAIM_TIMEOUT = 12s;
// The controller starts the same deadline when it claims an assignment. Recovery must never
// extend it, including while validation is waiting on the worker.
constexpr auto SETUP_TIMEOUT = 25s;
constexpr auto JOIN_PREFERENCE_SEARCH = 15s;
constexpr auto BLOCK_TIME = 60s;
// Consecutive lists without the peer's listing before it counts as gone. A player replacing
// its listing is briefly absent.
constexpr int PEER_MISSES = 3;
// The index times a session out after 15 s without a keepalive, but removes it only during a
// later list, up to 30 s after that.
constexpr double INDEX_SESSION_TIMEOUT = 15;
constexpr int LIST_FAILURES = 3;

struct Listing
{
  char kind = 0;
  // W and C.
  std::uint16_t mask = 0;
  char preference = 'n';
  // W: own nonce in b. C: claimed waiter in a, own nonce in b. A and H: waiter in a, claimer
  // in b.
  std::string a;
  std::string b;
  // A: 'w' when the waiter hosts, 'c' when the claimer hosts.
  char host = 0;
  std::string server_id;
};

bool IsHex(std::string_view value, std::size_t size)
{
  return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

bool IsPreference(std::string_view value)
{
  return value == "h" || value == "n" || value == "j";
}

std::optional<Listing> Parse(const NetPlaySession& session)
{
  if (!session.name.starts_with(NAME_PREFIX) || !session.has_password)
    return std::nullopt;
  const auto tokens = SplitString(session.name.substr(NAME_PREFIX.size()), ' ');
  if (tokens.empty() || tokens[0].size() != 1)
    return std::nullopt;
  Listing listing;
  listing.kind = tokens[0][0];
  listing.server_id = session.server_id;
  std::string_view digest;
  switch (listing.kind)
  {
  case 'W':
    if (tokens.size() != 5 || !IsHex(tokens[1], 3) || !IsPreference(tokens[2]) ||
        !IsHex(tokens[4], 6))
      return std::nullopt;
    listing.mask = static_cast<std::uint16_t>(std::stoul(tokens[1], nullptr, 16));
    listing.preference = tokens[2][0];
    digest = tokens[3];
    listing.b = tokens[4];
    break;
  case 'C':
    if (tokens.size() != 6 || !IsHex(tokens[1], 3) || !IsPreference(tokens[2]) ||
        !IsHex(tokens[4], 6) || !IsHex(tokens[5], 6))
      return std::nullopt;
    listing.mask = static_cast<std::uint16_t>(std::stoul(tokens[1], nullptr, 16));
    listing.preference = tokens[2][0];
    digest = tokens[3];
    listing.a = tokens[4];
    listing.b = tokens[5];
    break;
  case 'A':
    if (tokens.size() != 5 || (tokens[1] != "w" && tokens[1] != "c") || !IsHex(tokens[3], 6) ||
        !IsHex(tokens[4], 6))
      return std::nullopt;
    listing.host = tokens[1][0];
    digest = tokens[2];
    listing.a = tokens[3];
    listing.b = tokens[4];
    break;
  case 'H':
    if (tokens.size() != 4 || !IsHex(tokens[2], 6) || !IsHex(tokens[3], 6))
      return std::nullopt;
    digest = tokens[1];
    listing.a = tokens[2];
    listing.b = tokens[3];
    break;
  default:
    return std::nullopt;
  }
  if (digest != DIGEST)
    return std::nullopt;
  return listing;
}

std::uint16_t RegionMask(const QStringList& regions)
{
  const auto choices = QuickPlayMatchmaker::GetRegions();
  std::uint16_t mask = 0;
  for (std::size_t i = 0; i < choices.size(); ++i)
  {
    if (regions.contains(choices[i].value))
      mask |= static_cast<std::uint16_t>(1u << i);
  }
  return mask;
}

// The first shared region in checklist order, which is also each player's wire order.
QString AgreedRegion(std::uint16_t mask)
{
  const auto choices = QuickPlayMatchmaker::GetRegions();
  for (std::size_t i = 0; i < choices.size(); ++i)
  {
    if (mask & (1u << i))
      return choices[i].value;
  }
  return {};
}

// The index knows 7 regions; ours live in the listing name.
std::string IndexRegion(std::uint16_t mask)
{
  const auto region = AgreedRegion(mask);
  if (region == QStringLiteral("caribbean") || region == QStringLiteral("south-america"))
    return "SA";
  if (region == QStringLiteral("europe"))
    return "EU";
  if (region == QStringLiteral("asia"))
    return "EA";
  if (region == QStringLiteral("australia"))
    return "OC";
  return "NA";
}

QuickPlayMatchmaker::Result Searching(QString notice = {})
{
  QuickPlayMatchmaker::Result result;
  result.state = QuickPlayMatchmaker::State::Searching;
  result.notice = std::move(notice);
  return result;
}

QuickPlayMatchmaker::Result Unreachable()
{
  return QuickPlayMatchmaker::Failure(
      QObject::tr("Cannot reach the Dolphin lobby (NetPlay index), or the request timed out. "
                  "Check the connection, then cancel and try again."),
      true);
}

// Shared public IP (CGNAT, campus): every listing, a claim too, needs one of its 5 slots.
QString CapNotice()
{
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: listing refused, too many sessions from this IP");
  return QObject::tr("Your network already has the most Dolphin lobby rooms allowed. "
                     "Waiting for a free slot.");
}

QuickPlayMatchmaker::Result OpponentLeft()
{
  QuickPlayMatchmaker::Result result;
  result.state = QuickPlayMatchmaker::State::Expired;
  result.error = QObject::tr("The opponent left before the match started. Cancel and try again.");
  return result;
}
}  // namespace

struct QuickPlayIndexMatchmaker::Attempt
{
  enum class Phase
  {
    Searching,
    Waiting,
    Claiming,
    Matched,
    Published,
  };

  explicit Attempt(Now clock_) : clock(std::move(clock_))
  {
    index.SetErrorCallback([this] { listing_lost.store(true); });
  }

  // A step of the search: list when due, then claim, accept, yield or list as waiting.
  // One worker step; a Searching result carries the current stage.
  Result Step();
  // After the handoff: the peer's listing must still be there.
  Result Validate();
  Result PollHost(const QString& id);
  Result Publish(const QString& id, const std::string& host_code);
  void Unlist();

  std::atomic<bool> cancelled{false};
  // Set by the index keepalive thread when our listing expired or was refused.
  std::atomic<bool> listing_lost{false};

  // Immutable after Start.
  const Now clock;
  std::uint16_t mask = 0;
  char preference = 'n';
  std::string version;
  std::string nonce;
  std::string index_region;
  Clock::time_point started;

  // Worker only.
  Phase phase = Phase::Searching;
  std::mt19937 random{std::random_device{}()};
  Clock::time_point next_list{};
  std::optional<Clock::time_point> list_after;
  Clock::time_point claim_deadline{};
  std::string target;
  std::uint16_t target_mask = 0;
  int target_misses = 0;
  std::string waiter;
  std::string claimer;
  bool hosting = false;
  QString match_id;
  QString region;
  int peer_misses = 0;
  int failures = 0;
  bool reached = false;
  QString notice;
  SearchStage stage = SearchStage::Normal;
  int stage_seconds = 0;
  std::map<std::string, Clock::time_point> blocked;
  // Desired listing, retained even while the index temporarily has no live session.
  std::optional<NetPlaySession> listed;
  bool recovering = false;
  int restore_failures = 0;
  Clock::time_point next_restore{};
  Clock::time_point setup_deadline{};
  // Last: its keepalive thread reports into listing_lost.
  NetPlayIndex index;

private:
  Result Advance();
  void UpdateStage(Clock::time_point now);
  bool List(std::vector<Listing>* listings);
  // Replaces our listing. Returns the index status on failure, else an empty string.
  std::string ListAs(const std::string& tokens, bool handshake, const std::string& server_id = {});
  std::optional<Result> RestoreListing();
  Result Search(const std::vector<Listing>& listings, Clock::time_point now);
  Result Wait(const std::vector<Listing>& listings, Clock::time_point now);
  Result AwaitAccept(const std::vector<Listing>& listings, Clock::time_point now);
  Result Claim(const Listing& waiting, Clock::time_point now);
  Result Accept(const Listing& claim);
  Result Matched(std::uint16_t peer_mask);
  Result MatchedResult() const;
  Result AddFailed(const std::string& status);
  bool PeerPresent(const std::vector<Listing>& listings) const;
  const Listing* FindHost(const std::vector<Listing>& listings) const;
  Clock::duration WaitInterval();
};

bool QuickPlayIndexMatchmaker::Attempt::List(std::vector<Listing>* listings)
{
  const auto sessions =
      index.List({{"name", std::string(NAME_FILTER)}, {"password", "1"}, {"version", version}});
  if (!sessions)
  {
    ++failures;
    WARN_LOG_FMT(NETPLAY, "QuickPlay lobby: list failed ({})", index.GetLastError());
    return false;
  }
  failures = 0;
  reached = true;
  // Skip sessions that have already timed out (a crashed player). The freshest listing stands
  // in for the server's clock; the index refreshes every live one every 5 s.
  double newest = 0;
  for (const auto& session : *sessions)
    newest = std::max(newest, session.timestamp);
  for (const auto& session : *sessions)
  {
    if (session.timestamp && session.timestamp < newest - INDEX_SESSION_TIMEOUT)
      continue;
    if (auto listing = Parse(session))
      listings->push_back(std::move(*listing));
  }
  return true;
}

std::string QuickPlayIndexMatchmaker::Attempt::ListAs(const std::string& tokens, bool handshake,
                                                      const std::string& server_id)
{
  Unlist();
  NetPlaySession session;
  session.name = std::string(NAME_PREFIX) + tokens;
  session.region = index_region;
  session.method = "traversal";
  // Only H carries a room code. The others name a nonce, which no browser can decrypt.
  session.server_id = server_id.empty() ? nonce : server_id;
  session.game_id = GAME;
  session.has_password = true;
  // Handshake listings are in-game so that ordinary browsers can hide them.
  session.in_game = handshake;
  session.player_count = handshake ? 2 : 1;
  session.port = PORT;
  session.version = version;
  listing_lost.store(false);
  if (!index.Add(session))
    return index.GetLastError().empty() ? std::string("NO_RESPONSE") : index.GetLastError();
  listed = std::move(session);
  return {};
}

void QuickPlayIndexMatchmaker::Attempt::Unlist()
{
  index.Remove();
  listed.reset();
  recovering = false;
  listing_lost.store(false);
}

std::optional<QuickPlayMatchmaker::Result> QuickPlayIndexMatchmaker::Attempt::RestoreListing()
{
  if (listing_lost.exchange(false) && listed)
  {
    WARN_LOG_FMT(NETPLAY, "QuickPlay lobby: listing lost ({}); listing again",
                 index.GetLastError());
    index.Remove();
    recovering = true;
    restore_failures = 0;
    next_restore = {};
  }
  if (!recovering || cancelled.load() || clock() < next_restore)
    return std::nullopt;
  if (phase == Phase::Claiming && clock() >= claim_deadline)
  {
    Unlist();
    return OpponentLeft();
  }
  if (index.Add(*listed))
  {
    // Cancel may have arrived during the HTTP request. The serialized Cancel also removes,
    // but do not leave a live listing until that queued task runs.
    if (cancelled.load())
      Unlist();
    recovering = false;
    restore_failures = 0;
    notice.clear();
    return std::nullopt;
  }
  const auto status = index.GetLastError();
  next_restore = clock() + SEARCH_LIST_INTERVAL;
  if (status == "TOO_MANY_SESSIONS")
  {
    notice = CapNotice();
    return std::nullopt;
  }
  if ((status == "NO_RESPONSE" || status == "BAD_JSON") && ++restore_failures < LIST_FAILURES)
    return std::nullopt;
  return AddFailed(status);
}

Clock::duration QuickPlayIndexMatchmaker::Attempt::WaitInterval()
{
  return std::chrono::milliseconds{std::uniform_int_distribution<int>(3000, 5000)(random)};
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::AddFailed(const std::string& status)
{
  Unlist();
  if (status == "NO_RESPONSE" || status == "BAD_JSON")
    return Unreachable();
  return Failure(QObject::tr("The Dolphin lobby refused the Quick Play listing (%1). "
                             "Cancel and try again.")
                     .arg(QString::fromStdString(status)));
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Step()
{
  auto result = Advance();
  if (result.state == State::Searching)
  {
    // A claim lasts a few seconds; keep showing the stage it started from.
    if (phase != Phase::Claiming)
      UpdateStage(clock());
    result.stage = stage;
    result.stage_seconds = stage_seconds;
  }
  return result;
}

// Only Prefer to join changes over time: it looks for a waiting host before listing itself.
void QuickPlayIndexMatchmaker::Attempt::UpdateStage(Clock::time_point now)
{
  stage = SearchStage::Normal;
  stage_seconds = 0;
  if (preference != 'j')
    return;
  if (phase == Phase::Searching && (!list_after || now < *list_after))
  {
    const auto left = list_after ? *list_after - now : Clock::duration(JOIN_PREFERENCE_SEARCH);
    stage = SearchStage::JoinOnly;
    stage_seconds =
        std::max(1, static_cast<int>(std::chrono::ceil<std::chrono::seconds>(left).count()));
  }
  else if (phase == Phase::Waiting)
  {
    stage = SearchStage::JoinListed;
  }
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Advance()
{
  const auto now = clock();
  if (now - started >= SEARCH_TIMEOUT)
  {
    Unlist();
    Result result;
    result.state = State::Expired;
    result.error = QObject::tr("No opponent was found in the Dolphin lobby. Cancel and try again.");
    return result;
  }
  if (now < next_list)
    return Searching(notice);
  if (auto error = RestoreListing())
    return *error;
  if (recovering || cancelled.load())
    return Searching(notice);
  std::vector<Listing> listings;
  if (!List(&listings))
  {
    next_list = now + SEARCH_LIST_INTERVAL;
    if (!reached || failures >= LIST_FAILURES)
    {
      Unlist();
      return Unreachable();
    }
    return Searching(notice);
  }
  std::erase_if(blocked, [now](const auto& entry) { return entry.second <= now; });
  switch (phase)
  {
  case Phase::Searching:
    return Search(listings, now);
  case Phase::Waiting:
    return Wait(listings, now);
  case Phase::Claiming:
    return AwaitAccept(listings, now);
  default:
    return Searching(notice);
  }
}

QuickPlayMatchmaker::Result
QuickPlayIndexMatchmaker::Attempt::Search(const std::vector<Listing>& listings,
                                          Clock::time_point now)
{
  std::vector<const Listing*> candidates;
  for (const auto& listing : listings)
  {
    if (listing.kind == 'W' && listing.b != nonce && (listing.mask & mask) &&
        !blocked.contains(listing.b))
      candidates.push_back(&listing);
  }
  // Random among waiting players: the index has no FIFO order, and this spreads contention.
  if (!candidates.empty())
  {
    std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
    return Claim(*candidates[pick(random)], now);
  }

  // Nobody is waiting. Prefer to host: list now. No preference: list again after 0-2 s to damp
  // simultaneous listing. Prefer to join: keep looking for a while first.
  if (!list_after)
  {
    if (preference == 'h')
      list_after = now;
    else if (preference == 'j')
      list_after = now + JOIN_PREFERENCE_SEARCH;
    else
      list_after =
          now + std::chrono::milliseconds{std::uniform_int_distribution<int>(0, 2000)(random)};
  }
  if (now < *list_after)
  {
    next_list = std::min(*list_after, now + SEARCH_LIST_INTERVAL);
    return Searching(notice);
  }
  const auto status =
      ListAs(fmt::format("W {:03x} {} {} {}", mask, preference, DIGEST, nonce), false);
  if (status == "TOO_MANY_SESSIONS")
  {
    notice = CapNotice();
    next_list = now + SEARCH_LIST_INTERVAL;
    return Searching(notice);
  }
  if (!status.empty())
    return AddFailed(status);
  notice.clear();
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: waiting as {}", nonce);
  phase = Phase::Waiting;
  next_list = now + WaitInterval();
  return Searching();
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Claim(const Listing& waiting,
                                                                     Clock::time_point now)
{
  target = waiting.b;
  target_mask = waiting.mask;
  target_misses = 0;
  const auto status =
      ListAs(fmt::format("C {:03x} {} {} {} {}", mask, preference, DIGEST, target, nonce), true);
  if (status == "TOO_MANY_SESSIONS")
  {
    notice = CapNotice();
    phase = Phase::Searching;
    next_list = now + SEARCH_LIST_INTERVAL;
    return Searching(notice);
  }
  if (!status.empty())
    return AddFailed(status);
  notice.clear();
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: {} claims {}", nonce, target);
  phase = Phase::Claiming;
  claim_deadline = now + CLAIM_TIMEOUT;
  next_list = now + CLAIM_LIST_INTERVAL;
  return Searching();
}

QuickPlayMatchmaker::Result
QuickPlayIndexMatchmaker::Attempt::Wait(const std::vector<Listing>& listings, Clock::time_point now)
{
  // Accept one claim: the smallest nonce, so every claimer can predict the outcome.
  const Listing* claim = nullptr;
  for (const auto& listing : listings)
  {
    if (listing.kind == 'C' && listing.a == nonce && listing.b != nonce && (listing.mask & mask) &&
        (!claim || listing.b < claim->b))
      claim = &listing;
  }
  if (claim)
    return Accept(*claim);

  // Two waiters never claim each other otherwise. The larger nonce yields to the smaller.
  const Listing* smaller = nullptr;
  for (const auto& listing : listings)
  {
    if (listing.kind == 'W' && listing.b < nonce && (listing.mask & mask) &&
        !blocked.contains(listing.b) && (!smaller || listing.b < smaller->b))
      smaller = &listing;
  }
  if (smaller)
  {
    INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: {} yields to waiting {}", nonce, smaller->b);
    return Claim(*smaller, now);
  }
  next_list = now + WaitInterval();
  return Searching(notice);
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Accept(const Listing& claim)
{
  // The coordinator's soft rule: the newcomer (claimer) hosts when the waiter prefers to join
  // and the claimer does not, or the waiter has no preference and the claimer prefers to host.
  // Otherwise the player who waited longer hosts.
  const bool claimer_hosts = (preference == 'j' && claim.preference != 'j') ||
                             (preference == 'n' && claim.preference == 'h');
  waiter = nonce;
  claimer = claim.b;
  hosting = !claimer_hosts;
  const auto status = ListAs(
      fmt::format("A {} {} {} {}", claimer_hosts ? 'c' : 'w', DIGEST, waiter, claimer), true);
  if (!status.empty())
  {
    // The claimer gives up after its timeout. Start over.
    WARN_LOG_FMT(NETPLAY, "QuickPlay lobby: accept listing failed ({})", status);
    phase = Phase::Searching;
    list_after.reset();
    next_list = clock();
    return Searching(notice);
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: {} accepted {}, {} hosts", waiter, claimer,
               hosting ? "waiter" : "claimer");
  return Matched(claim.mask);
}

QuickPlayMatchmaker::Result
QuickPlayIndexMatchmaker::Attempt::AwaitAccept(const std::vector<Listing>& listings,
                                               Clock::time_point now)
{
  const Listing* accepted = nullptr;
  bool waiting = false;
  for (const auto& listing : listings)
  {
    // A waiter that hosts can replace A with H before the next list. Only it can have
    // published an H for this pair yet.
    if ((listing.kind == 'A' || listing.kind == 'H') && listing.a == target &&
        (!accepted || listing.b == nonce))
      accepted = &listing;
    else if (listing.kind == 'W' && listing.b == target)
      waiting = true;
  }
  if (accepted && accepted->b == nonce)
  {
    waiter = target;
    claimer = nonce;
    hosting = accepted->kind == 'A' && accepted->host == 'c';
    INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: {} accepted by {}, {} hosts", claimer, waiter,
                 hosting ? "claimer" : "waiter");
    return Matched(target_mask);
  }
  target_misses = waiting ? 0 : target_misses + 1;
  if (accepted || target_misses >= 2 || now >= claim_deadline)
  {
    INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: claim on {} not accepted", target);
    blocked[target] = now + BLOCK_TIME;
    Unlist();
    phase = Phase::Searching;
    list_after.reset();
    return Search(listings, now);
  }
  next_list = now + CLAIM_LIST_INTERVAL;
  return Searching(notice);
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Matched(std::uint16_t peer_mask)
{
  phase = Phase::Matched;
  setup_deadline = clock() + SETUP_TIMEOUT;
  region = AgreedRegion(mask & peer_mask);
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: matched in {}", region.toStdString());
  match_id = QString::fromStdString(waiter + claimer);
  peer_misses = 0;
  next_list = {};
  notice.clear();
  return MatchedResult();
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::MatchedResult() const
{
  Result result;
  result.state = hosting ? State::MatchedHost : State::MatchedClient;
  result.match_id = match_id;
  result.region = region;
  return result;
}

bool QuickPlayIndexMatchmaker::Attempt::PeerPresent(const std::vector<Listing>& listings) const
{
  const bool is_waiter = waiter == nonce;
  return std::any_of(listings.begin(), listings.end(), [&](const Listing& listing) {
    if (listing.a != waiter || listing.b != claimer)
      return false;
    // The waiter's listing is A until it publishes H; the claimer's is C until it does.
    return listing.kind == 'H' || listing.kind == (is_waiter ? 'C' : 'A');
  });
}

const Listing*
QuickPlayIndexMatchmaker::Attempt::FindHost(const std::vector<Listing>& listings) const
{
  const auto it = std::find_if(listings.begin(), listings.end(), [&](const Listing& listing) {
    return listing.kind == 'H' && listing.a == waiter && listing.b == claimer;
  });
  return it == listings.end() ? nullptr : &*it;
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Validate()
{
  int misses = 0;
  while (!cancelled.load())
  {
    if (clock() >= setup_deadline)
    {
      Unlist();
      return Failure(QObject::tr("Quick Play room setup timed out. Cancel and try again."));
    }
    if (auto error = RestoreListing())
      return *error;
    if (!recovering)
    {
      std::vector<Listing> listings;
      if (List(&listings) && PeerPresent(listings))
        return MatchedResult();
      if (failures >= LIST_FAILURES)
      {
        Unlist();
        return Unreachable();
      }
      if (++misses >= PEER_MISSES)
        break;
    }
    // Keep cancellation responsive while waiting for the next recovery/validation request.
    for (int i = 0; i < 10 && !cancelled.load(); ++i)
      std::this_thread::sleep_for(100ms);
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: opponent left before the handoff completed");
  Unlist();
  return OpponentLeft();
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::PollHost(const QString& id)
{
  if (phase != Phase::Matched || hosting || id != match_id)
    return Failure(QObject::tr("Invalid Quick Play client match."));
  Result result;
  result.state = State::WaitingForHost;
  result.match_id = match_id;
  const auto now = clock();
  if (now >= setup_deadline)
  {
    Unlist();
    return Failure(QObject::tr("Quick Play room setup timed out. Cancel and try again."));
  }
  if (now < next_list)
    return result;
  next_list = now + CLAIM_LIST_INTERVAL;
  if (auto error = RestoreListing())
    return *error;
  if (recovering || cancelled.load())
    return result;
  std::vector<Listing> listings;
  if (!List(&listings))
    return failures >= LIST_FAILURES ? Unreachable() : result;
  if (const auto* host = FindHost(listings))
  {
    NetPlaySession session;
    session.server_id = host->server_id;
    const auto code = session.DecryptID(ROOM_KEY);
    if (!code || !IsValidHostCode(QString::fromStdString(*code)))
      return Failure(QObject::tr("The Quick Play host listed an unreadable room code. "
                                 "Cancel and try again."));
    result.state = State::HostReady;
    result.host_code = QString::fromStdString(*code);
    return result;
  }
  if (PeerPresent(listings))
  {
    peer_misses = 0;
    return result;
  }
  if (++peer_misses < PEER_MISSES)
    return result;
  Unlist();
  auto left = OpponentLeft();
  left.match_id = match_id;
  return left;
}

QuickPlayMatchmaker::Result QuickPlayIndexMatchmaker::Attempt::Publish(const QString& id,
                                                                       const std::string& host_code)
{
  if (phase != Phase::Matched || !hosting || id != match_id)
    return Failure(QObject::tr("Invalid Quick Play traversal host code."));
  NetPlaySession code;
  code.server_id = host_code;
  code.EncryptID(ROOM_KEY);
  const auto status =
      ListAs(fmt::format("H {} {} {}", DIGEST, waiter, claimer), true, code.server_id);
  if (!status.empty())
  {
    if (status == "NO_RESPONSE" || status == "BAD_JSON")
      return Unreachable();
    return Failure(QObject::tr("The Dolphin lobby refused the Quick Play room (%1). "
                               "Cancel and try again.")
                       .arg(QString::fromStdString(status)));
  }
  phase = Phase::Published;
  Result result;
  result.state = State::HostPublished;
  return result;
}

QuickPlayIndexMatchmaker::QuickPlayIndexMatchmaker(QObject* parent, Now now)
    : QuickPlayMatchmaker(parent), m_now(std::move(now))
{
}

QuickPlayIndexMatchmaker::~QuickPlayIndexMatchmaker()
{
  Cancel();
  // Join before QObject teardown: the worker may still queue Deliver() on this object, and the
  // final removal of a listing is bounded by the HTTP timeouts.
  m_worker.Shutdown();
}

void QuickPlayIndexMatchmaker::Start(const QStringList& regions, const QString& host_preference,
                                     Callback callback)
{
  Cancel();
  auto attempt = std::make_shared<Attempt>(m_now);
  m_attempt = attempt;
  m_callback = std::move(callback);
  m_pending = true;

  attempt->mask = RegionMask(regions);
  if (!attempt->mask)
  {
    Deliver(attempt, Failure(tr("The Dolphin lobby only knows the listed regions. Tick at least "
                                "one of them in Quick Play Settings.")));
    return;
  }
  const auto normalized = NormalizeHostPreference(host_preference);
  attempt->preference = normalized == QStringLiteral("host") ? 'h' :
                        normalized == QStringLiteral("join") ? 'j' :
                                                               'n';
  attempt->version = Common::GetScmDescStr() + std::string(VERSION_SUFFIX);
  attempt->nonce = fmt::format(
      "{:06x}", std::uniform_int_distribution<std::uint32_t>(0, 0xffffff)(attempt->random));
  attempt->index_region = IndexRegion(attempt->mask);
  attempt->started = m_now();
  INFO_LOG_FMT(NETPLAY, "QuickPlay lobby: search started as {}", attempt->nonce);
  m_worker.Push([this, attempt] {
    if (!attempt->cancelled.load())
      Deliver(attempt, attempt->Step());
  });
}

void QuickPlayIndexMatchmaker::Push(std::function<Result(Attempt&)> step)
{
  if (!m_attempt || m_pending || m_attempt->cancelled.load())
    return;
  m_pending = true;
  m_worker.Push([this, attempt = m_attempt, step = std::move(step)] {
    if (!attempt->cancelled.load())
      Deliver(attempt, step(*attempt));
  });
}

void QuickPlayIndexMatchmaker::Poll()
{
  Push([](Attempt& attempt) {
    return attempt.phase == Attempt::Phase::Matched ? attempt.Validate() : attempt.Step();
  });
}

void QuickPlayIndexMatchmaker::PollMatch(const QString& match_id)
{
  Push([match_id](Attempt& attempt) { return attempt.PollHost(match_id); });
}

void QuickPlayIndexMatchmaker::PublishHostCode(const QString& match_id, const QString& host_code)
{
  if (!IsValidHostCode(host_code))
  {
    if (m_attempt && !m_pending)
    {
      m_pending = true;
      Deliver(m_attempt, Failure(tr("Invalid Quick Play traversal host code.")));
    }
    return;
  }
  Push([match_id, code = host_code.toStdString()](Attempt& attempt) {
    return attempt.Publish(match_id, code);
  });
}

void QuickPlayIndexMatchmaker::Deliver(const std::shared_ptr<Attempt>& attempt, Result result)
{
  QMetaObject::invokeMethod(
      this,
      [this, attempt, result = std::move(result)]() mutable {
        if (attempt != m_attempt || attempt->cancelled.load())
          return;
        m_pending = false;
        // Copy before invoking: callback may call Cancel(), clearing m_callback.
        auto callback = m_callback;
        if (callback)
          callback(std::move(result));
      },
      Qt::QueuedConnection);
}

void QuickPlayIndexMatchmaker::Cancel()
{
  auto attempt = std::exchange(m_attempt, nullptr);
  m_callback = {};
  m_pending = false;
  if (!attempt)
    return;
  attempt->cancelled.store(true);
  // Serialized after any running step, so a listing added by it is removed too.
  m_worker.Push([attempt] { attempt->Unlist(); });
}
