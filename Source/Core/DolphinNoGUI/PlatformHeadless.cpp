// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Core/ConfigManager.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayServer.h"
#include "UICommon/GameFile.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/Memmap.h"
#include "Core/Movie.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Disposable headless frontend for the existing NetPlay objects. It owns no transport.
struct ResearchNetPlayUI final : NetPlay::NetPlayUI
{
  bool host = false;
  std::atomic<u32> start_game{0};
  std::atomic<bool> failed{false};
  std::shared_ptr<UICommon::GameFile> game;
  void BootGame(const std::string& filename, std::unique_ptr<BootSessionData> boot_session_data) override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: unexpected BootGame\n"); }
  void StopGame() override { failed = true; Core::QueueHostJob([](Core::System& system) { Core::Stop(system); }); }
  bool IsHosting() const override { return host; }
  void Update() override {  }
  void AppendChat(const std::string& msg) override {  }
  void OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier, const std::string& netplay_name) override {  }
  void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) override {  }
  void OnMsgStartGame(u32 game_id) override { start_game = game_id; }
  void OnMsgStopGame() override {  }
  void OnMsgPowerButton() override {  }
  void OnPlayerConnect(const std::string& player) override {  }
  void OnPlayerDisconnect(const std::string& player) override {  }
  void OnMinimumPadBufferChanged(u32 buffer) override {  }
  void OnPlayerPadBufferChanged(u32 buffer) override {  }
  void OnHostInputAuthorityChanged(bool enabled) override {  }
  void OnDesync(u32 frame, const std::string& player) override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: OnDesync\n"); }
  void OnConnectionLost() override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: OnConnectionLost\n"); }
  void OnConnectionError(const std::string& message) override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: OnConnectionError\n"); }
  void OnTraversalError(Common::TraversalClient::FailureReason error) override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: OnTraversalError\n"); }
  void OnTraversalStateChanged(Common::TraversalClient::State state) override {  }
  void OnGameStartAborted() override { failed = true; std::fprintf(stderr, "PHASE93 ERROR: OnGameStartAborted\n"); }
  void OnGolferChanged(bool is_golfer, const std::string& golfer_name) override {  }
  void OnTtlDetermined(u8 ttl) override {  }
  bool IsRecording() override { return false; }
  bool IsSpectator() override { return false; }
  std::shared_ptr<const UICommon::GameFile> FindGameFile(const NetPlay::SyncIdentifier& sync_identifier, NetPlay::SyncIdentifierComparison* found = nullptr) override { const auto comparison = game->CompareSyncIdentifier(sync_identifier); if (found) *found = comparison; return comparison == NetPlay::SyncIdentifierComparison::SameGame ? game : nullptr; }
  std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title, int device_number) override { return {}; }
  void ShowGameDigestDialog(const std::string& title) override {  }
  void SetGameDigestProgress(int pid, int progress) override {  }
  void SetGameDigestResult(int pid, const std::string& result) override {  }
  void AbortGameDigest() override {  }
  void OnIndexAdded(bool success, std::string error) override {  }
  void OnIndexRefreshFailed(std::string error) override {  }
  void ShowChunkedProgressDialog(const std::string& title, u64 data_size, std::span<const int> players) override {  }
  void HideChunkedProgressDialog() override {  }
  void SetChunkedProgress(int pid, u64 progress) override {  }
  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override {  }
};

// Disposable Phase 9.2 local peer channel. Each process publishes only its owned
// pad. Immutable, atomically renamed files are the confirmed input records.
struct CSSSession
{
  const char* directory = std::getenv("PPLUS_PHASE92_SESSION_DIR");
  const char* role = std::getenv("PPLUS_PHASE92_ROLE");
  bool joined = false;
  bool submitted = false;
  unsigned frame = 0;
  std::array<std::array<int, 3>, 2> pads{};

  std::string Path(unsigned sequence, char owner) const
  {
    return std::string(directory) + "/" + std::to_string(sequence) + "." + owner + ".input";
  }

  static bool Read(std::istream& input, unsigned expected, char owner, std::array<int, 3>& pad)
  {
    unsigned sequence;
    char sender;
    std::string extra;
    return (input >> sequence >> sender >> pad[0] >> pad[1] >> pad[2]) && !(input >> extra) &&
           sequence == expected && sender == owner && pad[0] >= 0 && pad[0] <= 255 && pad[1] >= 0 &&
           pad[1] <= 255 && pad[2] >= 0 && (pad[2] & ~0x300) == 0;
  }

  bool Submit(const std::string& argument)
  {
    if (!joined || submitted)
      return false;
    // The caller supplies no port or peer pad; identity comes from this process.
    std::istringstream input(argument);
    unsigned sequence;
    std::array<int, 3> pad;
    if (!(input >> sequence))
      return false;
    std::ostringstream record;
    record << sequence << ' ' << *role << ' ' << input.rdbuf();
    std::istringstream validation(record.str());
    if (!Read(validation, frame, *role, pad))
      return false;
    const std::string path = Path(frame, *role);
    if (std::ifstream(path).good())  // duplicate/stale publication
      return false;
    std::ofstream output(path + ".tmp");
    output << record.str() << '\n';
    output.close();
    submitted = output.good() && std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
    return submitted;
  }

  bool Confirm(Core::System& system, const std::string& argument)
  {
    if (!joined || !submitted || argument != std::to_string(frame))
      return false;
    // Missing peer input leaves the paused Core untouched. A bounded research
    // timeout reports failure; it never synthesizes input or advances the guest.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!std::ifstream(Path(frame, 'A')).good() || !std::ifstream(Path(frame, 'B')).good())
    {
      if (std::chrono::steady_clock::now() >= deadline)
        return false;
      Core::HostDispatchJobs(system);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (unsigned port = 0; port < 2; ++port)
    {
      std::ifstream record(Path(frame, 'A' + port));
      if (!Read(record, frame, 'A' + port, pads[port]))
        return false;
    }
    return true;
  }

  void Record(Core::System& system) const
  {
    std::ofstream output(std::string(directory) + "/" + std::to_string(frame) + "." + *role +
                         ".consumed");
    output << frame;
    for (const auto& pad : pads)
      output << ' ' << pad[0] << ' ' << pad[1] << ' ' << pad[2];
    output << '\n'
           << system.GetMovie().GetCurrentFrame() << ' ' << system.GetCoreTiming().GetTicks() << ' '
           << system.GetPPCState().pc << '\n';
  }
};

void SetResearchPad(int port, int x, int y, int buttons)
{
  Pad::GetConfig()->GetController(port)->SetInputOverrideFunction(
      [x, y, buttons](std::string_view group, std::string_view control,
                      ControlState) -> std::optional<ControlState> {
        if (group == GCPad::MAIN_STICK_GROUP && (control == "X" || control == "Y"))
        {
          const int value = control == "X" ? x : y;
          return (value - 128.0) / (value >= 128 ? 127.0 : 128.0);
        }
        if (group == GCPad::BUTTONS_GROUP)
          return ((control == GCPad::A_BUTTON && (buttons & 0x100)) ||
                  (control == GCPad::B_BUTTON && (buttons & 0x200)) ||
                  (control == GCPad::START_BUTTON && (buttons & 0x1000))) ?
                     1.0 :
                     0.0;
        return 0.0;
      });
}

class PlatformHeadless final : public Platform
{
public:
  void SetTitle(const std::string& title) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;
};

void PlatformHeadless::SetTitle(const std::string& title)
{
  std::fprintf(stdout, "%s\n", title.c_str());
}

void PlatformHeadless::MainLoop()
{
  // Small opt-in command harness, isolated from the ordinary headless path.
  // Commands are numbered files; the host publishes them by atomic rename.
  const char* control_dir = std::getenv("PPLUS_PHASE91_CONTROL_DIR");
  unsigned command_number = 1;
  bool ready = false;
  CSSSession session;
  const bool phase93 = std::getenv("PPLUS_PHASE93_ATTACH");
  const bool phase94 = std::getenv("PPLUS_PHASE94_SMOKE");
  std::vector<u32> research_stops{0x806876d4};
  ResearchNetPlayUI net_ui;
  std::unique_ptr<NetPlay::NetPlayServer> net_server;
  std::unique_ptr<NetPlay::NetPlayClient> net_client;
  bool net_attached = false;
  auto& system = Core::System::GetInstance();
  while (m_running.IsSet())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    if (control_dir && Core::IsRunning(system))
    {
      if (!ready)
      {
        Core::SetState(system, Core::State::Paused);
        std::ofstream(std::string(control_dir) + "/READY")
            << "game=" << SConfig::GetInstance().GetGameID()
            << " dual_core=" << system.IsDualCoreMode() << '\n';
        ready = true;
      }
      const std::string command_base =
          std::string(control_dir) + "/" + std::to_string(command_number);
      std::ifstream command_file(command_base + ".cmd");
      std::string command, argument;
      if (command_file && std::getline(command_file, command))
      {
        std::getline(command_file, argument);
        bool ok = Core::GetState(system) == Core::State::Paused;
        if (net_attached && (command == "boot" || command == "load" ||
                             command.starts_with("session-")))
          ok = false;
        if (session.joined &&
            (command == "boot" || command == "load" || command == "input" || command == "continue"))
          ok = false;
        if (ok && command == "boot")
        {
          // Pre-measurement only: let the launcher hand off to the Brawl disc.
          Core::SetState(system, Core::State::Running);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
          while (SConfig::GetInstance().GetGameID() != "RSBE01" &&
                 std::chrono::steady_clock::now() < deadline)
          {
            Core::HostDispatchJobs(system);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
          Core::SetState(system, Core::State::Paused);
          ok = SConfig::GetInstance().GetGameID() == "RSBE01";
        }
        else if (ok && phase93 && command == "net-lobby")
        {
          // Only at the already-converged CSS boundary, before any normal pad poll.
          std::string role;
          unsigned port = 0;
          std::istringstream args(argument);
          ok = !session.joined && !net_client && system.GetPPCState().pc == 0x806876d4 &&
               static_cast<bool>(args >> role >> port) && (role == "A" || role == "B") &&
               port > 0 && port <= 65535;
          if (ok)
          {
            net_ui.host = role == "A";
            net_ui.game = std::make_shared<UICommon::GameFile>(
                Config::Get(Config::MAIN_DEFAULT_ISO));
            Config::SetCurrent(Config::NETPLAY_SAVEDATA_LOAD, false);
            Config::SetCurrent(Config::NETPLAY_SYNC_CODES, false);
            Config::SetCurrent(Config::NETPLAY_STRICT_SETTINGS_SYNC, false);
            Config::SetCurrent(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
            if (net_ui.host)
            {
              net_server = std::make_unique<NetPlay::NetPlayServer>(
                  port, false, &net_ui, NetPlay::NetTraversalConfig{});
              ok = net_server->is_connected;
              if (ok)
              {
                net_server->ChangeGame(net_ui.game->GetSyncIdentifier(), "Phase93 CSS fixture");
                net_server->SetHostInputAuthority(false);
                net_server->AdjustMinimumPadBufferSize(2);
              }
            }
            if (ok)
            {
              net_client = std::make_unique<NetPlay::NetPlayClient>(
                  "127.0.0.1", port, &net_ui, role, NetPlay::NetTraversalConfig{});
              ok = net_client->IsConnected() && !net_ui.failed;
            }
          }
        }
        else if (ok && phase93 && command == "net-start")
        {
          ok = net_server && net_client && !net_ui.failed &&
               net_client->GetLobbyState().players.size() == 2;
          if (ok)
          {
            net_server->SetPadMapping({1, 2, 0, 0});
            net_server->SetWiimoteMapping({0, 0, 0, 0});
            ok = net_server->RequestStartGame();
          }
        }
        else if (ok && phase93 && (command == "net-validate" || command == "net-prepare"))
        {
          ok = !session.joined && net_client && !net_ui.failed && net_ui.start_game != 0;
          if (ok)
          {
            net_client->AdjustPlayerPadBufferSize(2);
            const bool validate_only = command == "net-validate";
            ok = net_client->ResearchAttachRunningCore(validate_only);
            net_attached = ok && !validate_only;
            std::ofstream(std::string(control_dir) +
                          (validate_only ? "/net-validated.txt" : "/net-prepared.txt"))
                << net_client->ResearchSessionState();
          }
        }
        else if (ok && phase93 && command == "net-status")
        {
          ok = net_client && !net_ui.failed;
          if (ok)
            std::ofstream(std::string(control_dir) + "/" + argument + ".net.txt")
                << net_client->ResearchSessionState();
        }
        else if (ok && command == "session-join")
        {
          ok = !session.joined && session.directory && session.role &&
               (std::string(session.role) == "A" || std::string(session.role) == "B") &&
               system.GetPPCState().pc == 0x806876d4;
          session.joined = ok;
        }
        else if (ok && command == "session-submit")
        {
          ok = session.Submit(argument);
        }
        else if (ok && (command == "continue" || command == "session-step"))
        {
          if (command == "session-step")
          {
            ok = session.Confirm(system, argument);
            if (ok)
            {
              std::promise<void> completion;
              auto future = completion.get_future();
              Core::RunOnCPUThread(system, [&session, &completion] {
                for (int port = 0; port < 2; ++port)
                  SetResearchPad(port, session.pads[port][0], session.pads[port][1],
                                 session.pads[port][2]);
                completion.set_value();
              });
              while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
                Core::HostDispatchJobs(system);
            }
          }
          if (ok)
          {
            Core::SetState(system, Core::State::Running);
            // Wait for the next real instruction breakpoint; no host-timed stop.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (system.GetCPU().GetState() == CPU::State::Running &&
                   std::chrono::steady_clock::now() < deadline)
            {
              Core::HostDispatchJobs(system);
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Core::SetState(system, Core::State::Paused);
            ok = std::find(research_stops.begin(), research_stops.end(),
                           system.GetPPCState().pc) != research_stops.end();
            if (ok && command == "session-step")
            {
              session.Record(system);
              ++session.frame;
              session.submitted = false;
            }
          }
        }
        else if (ok)
        {
          std::promise<bool> completion;
          auto future = completion.get_future();
          Core::RunOnCPUThread(system, [&system, &completion, &research_stops, phase94, command, argument] {
            bool result = true;
            if (command == "load")
              State::LoadAs(system, argument);
            else if (command == "export")
              result = State::ResearchExport(system, argument);
            else if (command == "screenshot")
              Core::SaveScreenShot(argument);
            else if (phase94 && command == "shader-status")
            {
              std::ofstream(std::string(std::getenv("PPLUS_PHASE91_CONTROL_DIR")) + "/" +
                            argument + ".shader.txt")
                  << "effective=" << static_cast<int>(Config::Get(Config::GFX_SHADER_COMPILATION_MODE))
                  << " configured=" << static_cast<int>(g_Config.iShaderCompilationMode)
                  << " active=" << static_cast<int>(g_ActiveConfig.iShaderCompilationMode)
                  << " wait_effective=" << Config::Get(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING)
                  << " wait_active=" << g_ActiveConfig.bWaitForShadersBeforeStarting << '\n';
            }
            else if (phase94 && command == "boundary")
            {
              // Named native stops only. These change debugger breakpoints, never guest state.
              std::vector<std::pair<u32, u32>> guards;
              if (argument == "css-stage")
                guards = {{0x806876d4, 0x9421ffe0}, {0x806c8d78, 0x9421fff0}};
              else if (argument == "scene-frame")
                guards = {{0x8002d268, 0x807f0004}};
              else
                result = false;
              for (const auto& [pc, instruction] : guards)
                result = result && system.GetMemory().Read_U32(pc) == instruction;
              if (result)
              {
                auto& breakpoints = system.GetPowerPC().GetBreakPoints();
                breakpoints.Clear();
                research_stops.clear();
                for (const auto& [pc, instruction] : guards)
                {
                  breakpoints.Add(pc);
                  research_stops.push_back(pc);
                }
              }
            }
            else if (command == "input")
            {
              // Optional zero-based port is limited to the two participant experiment.
              // Legacy X-only and X/Y/A-B commands remain valid.
              int x = 128, y = 128, buttons = 0, port = 0;
              std::istringstream input(argument);
              result = static_cast<bool>(input >> x);
              if (input >> y)
                result = static_cast<bool>(input >> buttons);
              if (input >> port)
              {
                std::string extra;
                result = result && !(input >> extra);
              }
              result = result && x >= 0 && x <= 255 && y >= 0 && y <= 255 && buttons >= 0 &&
                       (buttons & ~(phase94 ? 0x1300 : 0x300)) == 0 && port >= 0 && port <= 1;
              if (result)
                SetResearchPad(port, x, y, buttons);
              if (!phase94)
              {
                auto& breakpoints = system.GetPowerPC().GetBreakPoints();
                breakpoints.Clear();
                breakpoints.Add(0x806876d4);
              }
            }
            else
              result = false;
            completion.set_value(result);
          });
          while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
            Core::HostDispatchJobs(system);
          ok = future.get();
        }
        std::ofstream(command_base + ".done") << (ok ? "OK" : "ERROR") << '\n';
        ++command_number;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

WindowSystemInfo PlatformHeadless::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  wsi.display_connection = nullptr;
  wsi.render_window = nullptr;
  wsi.render_surface = nullptr;
  return wsi;
}

}  // namespace

std::unique_ptr<Platform> Platform::CreateHeadlessPlatform()
{
  return std::make_unique<PlatformHeadless>();
}
