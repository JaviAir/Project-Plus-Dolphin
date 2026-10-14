// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QIcon>
#include <QMimeData>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QMessageBox>
#include <QByteArray>
#include <QJsonDocument>

#include <fmt/format.h>

#include <future>
#include <optional>
#include <utility>
#include <variant>

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
#include <signal.h>

#include "QtUtils/SignalDaemon.h"
#endif

#ifndef _WIN32
#include <qpa/qplatformnativeinterface.h>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Common/WindowSystemInfo.h"
#include "Common/HttpRequest.h"
#include "Common/scmrev.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigLoaders/NetPlayConfigLoader.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/ProjectPlusOnline/LobbyPresentation.h"
#include "Core/ProjectPlusOnline/LocalCSS.h"
#include "Core/ProjectPlusOnline/LocalInput.h"
#include "Core/ProjectPlusOnline/Presentation.h"
#include "Core/ProjectPlusOnline/Status.h"
#include "Core/QuickPlayRecipe.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "VideoCommon/Present.h"

#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/RiivolutionPatcher.h"

#include "DolphinQt/AboutDialog.h"
#include "DolphinQt/Achievements/AchievementsWindow.h"
#include "DolphinQt/CheatsManager.h"
#include "DolphinQt/Config/FreeLookWindow.h"
#include "DolphinQt/Config/LogConfigWidget.h"
#include "DolphinQt/Config/LogWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Config/SettingsWindow.h"
#include "DolphinQt/Debugger/AssemblerWidget.h"
#include "DolphinQt/Debugger/BreakpointWidget.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Debugger/JITWidget.h"
#include "DolphinQt/Debugger/MemoryWidget.h"
#include "DolphinQt/Debugger/NetworkWidget.h"
#include "DolphinQt/Debugger/RegisterWidget.h"
#include "DolphinQt/Debugger/ThreadWidget.h"
#include "DolphinQt/Debugger/WatchWidget.h"
#include "DolphinQt/DiscordHandler.h"
#include "DolphinQt/EmulatedUSB/LogitechMicWindow.h"
#include "DolphinQt/EmulatedUSB/WiiSpeakWindow.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/GameCount.h"
#include "DolphinQt/GameList/GameList.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/InfinityBase/InfinityBaseWindow.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NKitWarningDialog.h"
#include "DolphinQt/NetPlay/NetPlayBrowser.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/NetPlay/NetPlaySetupDialog.h"
#include "DolphinQt/ProjectPlus/InstallUpdateDialog.h"
#include "DolphinQt/ProjectPlus/UpdateDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/FileOpenEventFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt/QuickPlay/PersistentSession.h"
#include "DolphinQt/QuickPlay/QuickPlayClient.h"
#include "DolphinQt/QuickPlay/QuickPlayController.h"
#include "DolphinQt/QuickPlay/QuickPlayDialog.h"
#include "DolphinQt/QuickPlay/QuickPlayLauncher.h"
#include "DolphinQt/QuickPlay/QuickPlaySettingsDialog.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/ResourcePackManager.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/RiivolutionBootWidget.h"
#include "DolphinQt/SearchBar.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/SkylanderPortal/SkylanderPortalWindow.h"
#include "DolphinQt/TAS/GBATASInputWindow.h"
#include "DolphinQt/TAS/GCTASInputWindow.h"
#include "DolphinQt/TAS/WiiTASInputWindow.h"
#include "DolphinQt/ToolBar.h"
#include "DolphinQt/Updater.h"
#include "DolphinQt/WiiUpdate.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/ResourcePack.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"

#ifdef HAVE_XRANDR
#include "UICommon/X11Utils.h"
// This #define within X11/X.h conflicts with our WiimoteSource enum.
#undef None
#endif

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
void MainWindow::OnSignal()
{
  close();
}

static void InstallSignalHandler()
{
  struct sigaction sa;
  sa.sa_handler = &SignalDaemon::HandleInterrupt;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}
#endif

static WindowSystemType GetWindowSystemType()
{
  // Determine WSI type based on Qt platform.
  QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("windows"))
    return WindowSystemType::Windows;
  else if (platform_name == QStringLiteral("cocoa"))
    return WindowSystemType::MacOS;
  else if (platform_name == QStringLiteral("xcb"))
    return WindowSystemType::X11;
  else if (platform_name == QStringLiteral("wayland"))
    return WindowSystemType::Wayland;
  else if (platform_name == QStringLiteral("haiku"))
    return WindowSystemType::Haiku;

  ModalMessageBox::critical(
      nullptr, QStringLiteral("Error"),
      QString::asprintf("Unknown Qt platform: %s", platform_name.toStdString().c_str()));
  return WindowSystemType::Headless;
}

// Persistent Quick Play (same-Core handoff) is supported on X11, including XWayland,
// and on Windows. The single-core and separate render window requirements
// are checked separately.
static bool IsQuickPlayWindowSystem()
{
  const QString platform_name = QGuiApplication::platformName();
  return platform_name == QStringLiteral("xcb") || platform_name == QStringLiteral("windows");
}

static WindowSystemInfo GetWindowSystemInfo(QWindow* window)
{
  WindowSystemInfo wsi;
  wsi.type = GetWindowSystemType();

  // Our Win32 Qt external doesn't have the private API.
#if defined(WIN32) || defined(__APPLE__) || defined(__HAIKU__)
  wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  wsi.display_connection = pni->nativeResourceForWindow("display", window);
  if (wsi.type == WindowSystemType::Wayland)
    wsi.render_window = window ? pni->nativeResourceForWindow("surface", window) : nullptr;
  else
    wsi.render_window = window ? reinterpret_cast<void*>(window->winId()) : nullptr;
  wsi.render_surface = wsi.render_window;
#endif
  wsi.render_surface_scale = window ? static_cast<float>(window->devicePixelRatio()) : 1.0f;

  return wsi;
}

static std::vector<std::string> StringListToStdVector(const QStringList& list)
{
  std::vector<std::string> result;
  result.reserve(list.size());

  for (const QString& s : list)
    result.push_back(s.toStdString());

  return result;
}

MainWindow::MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                       const std::string& movie_path)
    : QMainWindow(nullptr), m_system(system)
{
  setWindowTitle(QString::fromStdString(Common::GetScmRevStr()));
  setWindowIcon(Resources::GetAppIcon());
  setUnifiedTitleAndToolBarOnMac(true);
  setAcceptDrops(true);
  setAttribute(Qt::WA_NativeWindow);

  CreateComponents();

  ConnectGameList();
  ConnectHost();
  ConnectToolBar();
  ConnectRenderWidget();
  ConnectStack();
  ConnectMenuBar();

  QSettings& settings = Settings::GetQSettings();
  restoreState(settings.value(QStringLiteral("mainwindow/state")).toByteArray());
  restoreGeometry(settings.value(QStringLiteral("mainwindow/geometry")).toByteArray());
  if (!Settings::Instance().IsBatchModeEnabled())
  {
    show();
  }

  InitControllers();
  ConnectHotkeys();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme colorScheme) {
            Settings::Instance().ApplyStyle();
            if (m_skylander_window)
              m_skylander_window->RefreshList();
          });
#endif

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_game_list, &GameList::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  InitCoreCallbacks();

  NetPlayInit();
  // Offline Launcher boots from this frontend prepare the in-game Quick Play origin.
  QuickPlayRecipe::SetFrontendSupported(true);
  QuickPlayRecipe::SetLocalSnapshotHandler([this] { TakeLocalQuickPlaySnapshot(); });

  auto* status_timer = new QTimer(this);
  connect(status_timer, &QTimer::timeout, this, &MainWindow::PublishQuickPlayStatus);
  status_timer->start(100);
  PublishQuickPlayStatus();

#ifdef SHOW_UPDATER
  // The Quick Play trial launcher sets this; the modal check would block its -e boot.
  if (!qEnvironmentVariableIsSet("QUICKPLAY_DISABLE_UPDATE_CHECK"))
    CheckForUpdatesAuto();
#endif  // SHOW_UPDATER

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().Init(reinterpret_cast<void*>(winId()));
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
    Settings::Instance().SetDebugModeEnabled(false);
  // This needs to trigger on both RA_HARDCORE_ENABLED and RA_ENABLED
  m_config_changed_callback_id = Config::AddConfigChangedCallback(
      [this] { QueueOnObject(this, [this] { this->OnHardcoreChanged(); }); });
  // If hardcore is enabled when the emulator starts, make sure it turns off what it needs to
  if (Config::Get(Config::RA_HARDCORE_ENABLED))
    OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  auto* daemon = new SignalDaemon(this);

  connect(daemon, &SignalDaemon::InterruptReceived, this, &MainWindow::OnSignal);

  InstallSignalHandler();
#endif

  if (boot_parameters)
  {
    m_pending_boot = std::move(boot_parameters);

    if (!movie_path.empty())
    {
      std::optional<std::string> savestate_path;
      if (m_system.GetMovie().PlayInput(movie_path, &savestate_path))
      {
        m_pending_boot->boot_session_data.SetSavestateData(std::move(savestate_path),
                                                           DeleteSavestateAfterBoot::No);
        emit RecordingStatusChanged(true);
      }
    }
  }

  m_state_slot =
      std::clamp(Settings::Instance().GetStateSlot(), 1, static_cast<int>(State::NUM_STATES));

  m_render_widget_geometry = settings.value(QStringLiteral("renderwidget/geometry")).toByteArray();

  // Restoring of window states can sometimes go wrong, resulting in widgets being visible when they
  // shouldn't be so we have to reapply all our rules afterwards.
  Settings::Instance().RefreshWidgetVisibility();

  if (!ResourcePack::Init())
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Error occurred while loading some texture packs"));
  }

  for (auto& pack : ResourcePack::GetPacks())
  {
    if (!pack.IsValid())
    {
      ModalMessageBox::critical(this, tr("Error"),
                                tr("Invalid Pack %1 provided: %2")
                                    .arg(QString::fromStdString(pack.GetPath()))
                                    .arg(QString::fromStdString(pack.GetError())));
      return;
    }
  }

  Host::GetInstance()->SetMainWindowHandle(reinterpret_cast<void*>(winId()));

  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

MainWindow::~MainWindow()
{
  m_exit_requested = true;
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay)
    RetirePersistentQuickPlay(false);
  // Destroy orchestration while its MainWindow callbacks and NetPlay dialog are still alive.
  delete m_quickplay_dialog;
  m_quickplay_dialog = nullptr;
  delete m_quickplay_controller;
  m_quickplay_controller = nullptr;

  // Shut down NetPlay first to avoid race condition segfault
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();

#ifdef USE_RETRO_ACHIEVEMENTS
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
  AchievementManager::GetInstance().Shutdown();
#endif  // USE_RETRO_ACHIEVEMENTS

  delete m_render_widget;
  delete m_netplay_dialog;

  for (int i = 0; i < 4; i++)
  {
    delete m_gc_tas_input_windows[i];
    delete m_gba_tas_input_windows[i];
    delete m_wii_tas_input_windows[i];
  }

  ShutdownControllers();

  QSettings& settings = Settings::GetQSettings();

  if (!Settings::Instance().IsBatchModeEnabled())
  {
    settings.setValue(QStringLiteral("mainwindow/state"), saveState());
    settings.setValue(QStringLiteral("mainwindow/geometry"), saveGeometry());
  }

  settings.setValue(QStringLiteral("renderwidget/geometry"), m_render_widget_geometry);

  Config::Save();
}

WindowSystemInfo MainWindow::GetWindowSystemInfo() const
{
  return ::GetWindowSystemInfo(m_render_widget->windowHandle());
}

void MainWindow::InitControllers()
{
  if (g_controller_interface.IsInit())
    return;

  UICommon::InitControllers(::GetWindowSystemInfo(windowHandle()));

  m_hotkey_scheduler = new HotkeyScheduler();
  m_hotkey_scheduler->Start();

  // Defaults won't work reliably without loading and saving the config first

  Wiimote::LoadConfig();
  Wiimote::GetConfig()->SaveConfig();

  Pad::LoadConfig();
  Pad::GetConfig()->SaveConfig();

  Pad::LoadGBAConfig();
  Pad::GetGBAConfig()->SaveConfig();

  Keyboard::LoadConfig();
  Keyboard::GetConfig()->SaveConfig();

  FreeLook::LoadInputConfig();
  FreeLook::GetInputConfig()->SaveConfig();
}

void MainWindow::ShutdownControllers()
{
  m_hotkey_scheduler->Stop();

  Settings::Instance().UnregisterDevicesChangedCallback();

  UICommon::ShutdownControllers();

  m_hotkey_scheduler->deleteLater();
}

void MainWindow::InitCoreCallbacks()
{
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Stopping && m_persistent_quickplay)
      RetirePersistentQuickPlay(false);
    if (state == Core::State::Stopping && m_quickplay_session)
      EndQuickPlaySession(true);
    if (state == Core::State::Uninitialized)
      OnStopComplete();
    if (state == Core::State::Running && m_quickplay_recovery == QuickPlayRecovery::Booting)
    {
      m_quickplay_recovery = QuickPlayRecovery::None;
      m_quickplay_controller->Cancel();
      INFO_LOG_FMT(NETPLAY, "QuickPlay: local recovery running");
    }

    if (state == Core::State::Running && m_fullscreen_requested)
    {
      FullScreen();
      m_fullscreen_requested = false;
    }
  });
  installEventFilter(this);
  m_render_widget->installEventFilter(this);

  // Handle file open events
  auto* filter = new FileOpenEventFilter(QGuiApplication::instance());
  connect(filter, &FileOpenEventFilter::fileOpened, this, [this](const QString& file_name) {
    StartGame(BootParameters::GenerateFromFile(file_name.toStdString()));
  });
}

static void InstallHotkeyFilter(QWidget* dialog)
{
  auto* filter = new WindowActivationEventFilter(dialog);
  dialog->installEventFilter(filter);

  filter->connect(filter, &WindowActivationEventFilter::windowDeactivated,
                  [] { HotkeyManagerEmu::Enable(true); });
  filter->connect(filter, &WindowActivationEventFilter::windowActivated,
                  [] { HotkeyManagerEmu::Enable(false); });
}

void MainWindow::CreateComponents()
{
  m_menu_bar = new MenuBar(this);
  m_tool_bar = new ToolBar(this);
  m_search_bar = new SearchBar(this);
  m_game_count = new GameCount(this);
  m_game_list = new GameList(this);
  m_render_widget = new RenderWidget;
  m_stack = new QStackedWidget(this);

  for (int i = 0; i < 4; i++)
  {
    m_gc_tas_input_windows[i] = new GCTASInputWindow(nullptr, i);
    m_gba_tas_input_windows[i] = new GBATASInputWindow(nullptr, i);
    m_wii_tas_input_windows[i] = new WiiTASInputWindow(nullptr, i);
  }

  m_jit_widget = new JITWidget(m_system, this);
  m_log_widget = new LogWidget(this);
  m_log_config_widget = new LogConfigWidget(this);
  m_memory_widget = new MemoryWidget(m_system, this);
  m_network_widget = new NetworkWidget(this);
  m_register_widget = new RegisterWidget(this);
  m_thread_widget = new ThreadWidget(this);
  m_watch_widget = new WatchWidget(this);
  m_breakpoint_widget = new BreakpointWidget(this);
  m_code_widget = new CodeWidget(this);
  m_assembler_widget = new AssemblerWidget(this);

  const auto request_watch = [this](const QString& name, u32 addr) {
    m_watch_widget->AddWatch(name, addr);
  };
  const auto request_breakpoint = [this](u32 addr) { m_breakpoint_widget->AddBP(addr); };
  const auto request_memory_breakpoint = [this](u32 addr) {
    m_breakpoint_widget->AddAddressMBP(addr);
  };
  const auto request_view_in_memory = [this](u32 addr) { m_memory_widget->SetAddress(addr); };
  const auto request_view_in_code = [this](u32 addr) {
    m_code_widget->SetAddress(addr, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  };

  connect(m_jit_widget, &JITWidget::SetCodeAddress, m_code_widget, &CodeWidget::OnSetCodeAddress);
  connect(m_watch_widget, &WatchWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_watch_widget, &WatchWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_register_widget, &RegisterWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_register_widget, &RegisterWidget::RequestWatch, request_watch);
  connect(m_register_widget, &RegisterWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_register_widget, &RegisterWidget::RequestViewInCode, request_view_in_code);
  connect(m_thread_widget, &ThreadWidget::RequestBreakpoint, request_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestMemoryBreakpoint, request_memory_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestWatch, request_watch);
  connect(m_thread_widget, &ThreadWidget::RequestViewInMemory, request_view_in_memory);
  connect(m_thread_widget, &ThreadWidget::RequestViewInCode, request_view_in_code);

  connect(m_code_widget, &CodeWidget::RequestPPCComparison, m_jit_widget,
          &JITWidget::OnRequestPPCComparison);
  connect(m_code_widget, &CodeWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);
  connect(m_memory_widget, &MemoryWidget::ShowCode, m_code_widget, [this](u32 address) {
    m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_memory_widget, &MemoryWidget::RequestWatch, request_watch);

  connect(m_breakpoint_widget, &BreakpointWidget::ShowCode, [this](u32 address) {
    if (Core::GetState(m_system) == Core::State::Paused)
      m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  });
  connect(m_breakpoint_widget, &BreakpointWidget::ShowMemory, m_memory_widget,
          &MemoryWidget::SetAddress);
}

void MainWindow::ConnectMenuBar()
{
  setMenuBar(m_menu_bar);
  // File
  connect(m_menu_bar, &MenuBar::Open, this, &MainWindow::Open);
  connect(m_menu_bar, &MenuBar::Exit, this, &MainWindow::close);
  connect(m_menu_bar, &MenuBar::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_menu_bar, &MenuBar::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_menu_bar, &MenuBar::OpenUserFolder, this, &MainWindow::OpenUserFolder);
  connect(m_menu_bar, &MenuBar::OpenConfigFolder, this, &MainWindow::OpenConfigFolder);
  connect(m_menu_bar, &MenuBar::OpenCacheFolder, this, &MainWindow::OpenCacheFolder);

  // Emulation
  connect(m_menu_bar, &MenuBar::Pause, this, &MainWindow::Pause);
  connect(m_menu_bar, &MenuBar::Play, this, [this] { Play(); });
  connect(m_menu_bar, &MenuBar::Stop, this, &MainWindow::RequestStop);
  connect(m_menu_bar, &MenuBar::Reset, this, &MainWindow::Reset);
  connect(m_menu_bar, &MenuBar::Fullscreen, this, &MainWindow::FullScreen);
  connect(m_menu_bar, &MenuBar::FrameAdvance, this, &MainWindow::FrameAdvance);
  connect(m_menu_bar, &MenuBar::Screenshot, this, &MainWindow::ScreenShot);
  connect(m_menu_bar, &MenuBar::StateLoad, this, &MainWindow::StateLoad);
  connect(m_menu_bar, &MenuBar::StateSave, this, &MainWindow::StateSave);
  connect(m_menu_bar, &MenuBar::StateLoadSlot, this, &MainWindow::StateLoadSlot);
  connect(m_menu_bar, &MenuBar::StateSaveSlot, this, &MainWindow::StateSaveSlot);
  connect(m_menu_bar, &MenuBar::StateLoadSlotAt, this, &MainWindow::StateLoadSlotAt);
  connect(m_menu_bar, &MenuBar::StateSaveSlotAt, this, &MainWindow::StateSaveSlotAt);
  connect(m_menu_bar, &MenuBar::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_menu_bar, &MenuBar::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_menu_bar, &MenuBar::StateSaveOldest, this, &MainWindow::StateSaveOldest);
  connect(m_menu_bar, &MenuBar::SetStateSlot, this, &MainWindow::SetStateSlot);

  // Options
  connect(m_menu_bar, &MenuBar::Configure, this, &MainWindow::ShowSettingsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureGraphics, this, &MainWindow::ShowGraphicsWindow);
  connect(m_menu_bar, &MenuBar::ConfigureAudio, this, &MainWindow::ShowAudioWindow);
  connect(m_menu_bar, &MenuBar::ConfigureControllers, this, &MainWindow::ShowControllersWindow);
  connect(m_menu_bar, &MenuBar::ConfigureHotkeys, this, &MainWindow::ShowHotkeyDialog);
  connect(m_menu_bar, &MenuBar::ConfigureFreelook, this, &MainWindow::ShowFreeLookWindow);

  // Tools
  connect(m_menu_bar, &MenuBar::ShowMemcardManager, this, &MainWindow::ShowMemcardManager);
  connect(m_menu_bar, &MenuBar::ShowResourcePackManager, this,
          &MainWindow::ShowResourcePackManager);
  connect(m_menu_bar, &MenuBar::ShowCheatsManager, this, &MainWindow::ShowCheatsManager);
  connect(m_menu_bar, &MenuBar::BootGameCubeIPL, this, &MainWindow::OnBootGameCubeIPL);
  connect(m_menu_bar, &MenuBar::ImportNANDBackup, this, &MainWindow::OnImportNANDBackup);
  connect(m_menu_bar, &MenuBar::PerformOnlineUpdate, this, &MainWindow::PerformOnlineUpdate);
  connect(m_menu_bar, &MenuBar::BootWiiSystemMenu, this, &MainWindow::BootWiiSystemMenu);
  connect(m_menu_bar, &MenuBar::StartNetPlay, this, &MainWindow::ShowNetPlaySetupDialog);
  connect(m_menu_bar, &MenuBar::ShowQuickPlayDiagnostics, this,
          &MainWindow::ShowQuickPlayDiagnostics);
  connect(m_menu_bar, &MenuBar::BrowseNetPlay, this, &MainWindow::ShowNetPlayBrowser);
  connect(m_menu_bar, &MenuBar::ShowFIFOPlayer, this, &MainWindow::ShowFIFOPlayer);
  connect(m_menu_bar, &MenuBar::ShowSkylanderPortal, this, &MainWindow::ShowSkylanderPortal);
  connect(m_menu_bar, &MenuBar::ShowInfinityBase, this, &MainWindow::ShowInfinityBase);
  connect(m_menu_bar, &MenuBar::ShowWiiSpeakWindow, this, &MainWindow::ShowWiiSpeakWindow);
  connect(m_menu_bar, &MenuBar::ShowLogitechMicWindow, this, &MainWindow::ShowLogitechMicWindow);
  connect(m_menu_bar, &MenuBar::ConnectWiiRemote, this, &MainWindow::OnConnectWiiRemote);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_menu_bar, &MenuBar::ShowAchievementsWindow, this, &MainWindow::ShowAchievementsWindow);
#endif  // USE_RETRO_ACHIEVEMENTS

  // Movie
  connect(m_menu_bar, &MenuBar::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_menu_bar, &MenuBar::StartRecording, this, &MainWindow::OnStartRecording);
  connect(m_menu_bar, &MenuBar::StopRecording, this, &MainWindow::OnStopRecording);
  connect(m_menu_bar, &MenuBar::ExportRecording, this, &MainWindow::OnExportRecording);
  connect(m_menu_bar, &MenuBar::ShowTASInput, this, &MainWindow::ShowTASInput);
  connect(m_menu_bar, &MenuBar::ConfigureOSD, this, &MainWindow::ShowOSDWindow);

  // View
  connect(m_menu_bar, &MenuBar::ShowList, m_game_list, &GameList::SetListView);
  connect(m_menu_bar, &MenuBar::ShowGrid, m_game_list, &GameList::SetGridView);
  connect(m_menu_bar, &MenuBar::PurgeGameListCache, m_game_list, &GameList::PurgeCache);
  connect(m_menu_bar, &MenuBar::ShowSearch, m_search_bar, &SearchBar::Show);

  connect(m_menu_bar, &MenuBar::ColumnVisibilityToggled, m_game_list,
          &GameList::OnColumnVisibilityToggled);

  connect(m_menu_bar, &MenuBar::GameListPlatformVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);
  connect(m_menu_bar, &MenuBar::GameListRegionVisibilityToggled, m_game_list,
          &GameList::OnGameListVisibilityChanged);

  connect(m_menu_bar, &MenuBar::ShowAboutDialog, this, &MainWindow::ShowAboutDialog);
#ifdef SHOW_UPDATER
  connect(m_menu_bar, &MenuBar::ShowUpdateDialog, this, &MainWindow::ShowUpdateDialog);
#endif  // SHOW_UPDATER

  connect(m_game_list, &GameList::SelectionChanged, m_menu_bar, &MenuBar::SelectionChanged);
  connect(this, &MainWindow::ReadOnlyModeChanged, m_menu_bar, &MenuBar::ReadOnlyModeChanged);
  connect(this, &MainWindow::RecordingStatusChanged, m_menu_bar, &MenuBar::RecordingStatusChanged);
}

void MainWindow::ConnectHotkeys()
{
  qRegisterMetaType<ProjectPlusOnline::SetupCancelRequest>();
  connect(
      m_hotkey_scheduler, &HotkeyScheduler::CancelPausedQuickPlay, this,
      [this](ProjectPlusOnline::SetupCancelRequest request) {
        // Local controller 1, mapped or a Wii U adapter port; unsupported types are disconnected.
        const bool device_connected = ProjectPlusOnline::LocalCommandDeviceConnected();
        // Complete after releasing the input lock: a sampling gap while waiting revokes the ticket.
        const bool current = ProjectPlusOnline::setup_action.Complete(request);
        if (!current || !OwnsInGameQuickPlay() || !IsPersistentQuickPlayBoundary() ||
            request.context.identity != QuickPlayIdentity() || m_quickplay_validating ||
            m_quickplay_blocking_connection || m_quickplay_paused_css != request.context.identity ||
            !m_quickplay_setup || m_quickplay_setup->attempt != request.context.identity.attempt ||
            m_quickplay_setup->window != m_render_widget ||
            m_quickplay_setup->native_window != static_cast<u64>(m_render_widget->effectiveWinId()) ||
            m_quickplay_setup->client != Settings::Instance().GetNetPlayClient().get() ||
            m_quickplay_setup->server != Settings::Instance().GetNetPlayServer().get() ||
            QApplication::activeModalWidget() || m_stop_confirm_showing ||
            !Host::GetInstance()->GetRenderFocus() ||
            ProjectPlusOnline::host_ui_input_blocked.load() || !device_connected ||
            !m_quickplay_controller ||
            m_quickplay_controller->GetGuestCancelAttempt() != request.context.identity.attempt)
          return;
        m_quickplay_cancel_consumed = true;
        ProjectPlusOnline::setup_action.Invalidate();
        m_quickplay_controller->Cancel();
        PublishQuickPlayStatus();
      },
      Qt::QueuedConnection);
  connect(
      m_hotkey_scheduler, &HotkeyScheduler::LeaveQuickPlaySession, this,
      [this](ProjectPlusOnline::SetupCancelRequest request) {
        // Local controller 1 held L + R + D-pad Right; revalidate the owned session first.
        const bool device_connected = ProjectPlusOnline::LocalCommandDeviceConnected();
        const bool current = ProjectPlusOnline::leave_action.Complete(request);
        INFO_LOG_FMT(NETPLAY,
                     "Persistent QuickPlay: leave chord received (current {} owns {} identity {})",
                     current, OwnsInGameQuickPlay(),
                     request.context.identity == QuickPlayIdentity());
        if (!current || !OwnsInGameQuickPlay() || request.context.identity != QuickPlayIdentity() ||
            m_quickplay_validating || m_quickplay_blocking_connection ||
            m_quickplay_recovery != QuickPlayRecovery::None || !Core::IsRunning(m_system) ||
            QApplication::activeModalWidget() || m_stop_confirm_showing ||
            (!Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) &&
             !Host::GetInstance()->GetRenderFocus()) ||
            ProjectPlusOnline::host_ui_input_blocked.load() || !device_connected)
          return;
        ProjectPlusOnline::leave_action.Invalidate();
        INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: local leave chord, game {}",
                     m_persistent_game);
        m_quickplay_left_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        NetPlayQuit();
        PublishQuickPlayStatus();
      },
      Qt::QueuedConnection);
  connect(m_hotkey_scheduler, &HotkeyScheduler::Open, this, &MainWindow::Open);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ChangeDisc, this, &MainWindow::ChangeDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::EjectDisc, this, &MainWindow::EjectDisc);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExitHotkey, this, &MainWindow::close);
  connect(m_hotkey_scheduler, &HotkeyScheduler::UnlockCursor, this, &MainWindow::UnlockCursor);
  connect(m_hotkey_scheduler, &HotkeyScheduler::TogglePauseHotkey, this, &MainWindow::TogglePause);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ActivateChat, this, &MainWindow::OnActivateChat);
  connect(m_hotkey_scheduler, &HotkeyScheduler::CollapseChat, this, &MainWindow::OnCollapseChat);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExpandChat, this, &MainWindow::OnExpandChat);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RequestGolfControl, this,
          &MainWindow::OnRequestGolfControl);
  connect(m_hotkey_scheduler, &HotkeyScheduler::RefreshGameListHotkey, this,
          &MainWindow::RefreshGameList);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StopHotkey, this, &MainWindow::RequestStop);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ResetHotkey, this, &MainWindow::Reset);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ScreenShotHotkey, this, &MainWindow::ScreenShot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::FullScreenHotkey, this, &MainWindow::FullScreen);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlot, this, &MainWindow::StateLoadSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlot, this, &MainWindow::StateSaveSlotAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadLastSaved, this,
          &MainWindow::StateLoadLastSavedAt);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadUndo, this, &MainWindow::StateLoadUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveUndo, this, &MainWindow::StateSaveUndo);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveOldest, this,
          &MainWindow::StateSaveOldest);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveFile, this, &MainWindow::StateSave);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadFile, this, &MainWindow::StateLoad);

  connect(m_hotkey_scheduler, &HotkeyScheduler::StateLoadSlotHotkey, this,
          &MainWindow::StateLoadSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StateSaveSlotHotkey, this,
          &MainWindow::StateSaveSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetStateSlotHotkey, this,
          &MainWindow::SetStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::IncrementSelectedStateSlotHotkey, this,
          &MainWindow::IncrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::DecrementSelectedStateSlotHotkey, this,
          &MainWindow::DecrementSelectedStateSlot);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StartRecording, this,
          &MainWindow::OnStartRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::PlayRecording, this, &MainWindow::OnPlayRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ExportRecording, this,
          &MainWindow::OnExportRecording);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ConnectWiiRemote, this,
          &MainWindow::OnConnectWiiRemote);
  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleReadOnlyMode, [this] {
    auto& movie = m_system.GetMovie();
    bool read_only = !movie.IsReadOnly();
    movie.SetReadOnly(read_only);
    emit ReadOnlyModeChanged(read_only);
  });
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hotkey_scheduler, &HotkeyScheduler::OpenAchievements, this,
          &MainWindow::ShowAchievementsWindow, Qt::QueuedConnection);
#endif  // USE_RETRO_ACHIEVEMENTS

  connect(m_hotkey_scheduler, &HotkeyScheduler::Step, m_code_widget, &CodeWidget::Step);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOver, m_code_widget, &CodeWidget::StepOver);
  connect(m_hotkey_scheduler, &HotkeyScheduler::StepOut, m_code_widget, &CodeWidget::StepOut);
  connect(m_hotkey_scheduler, &HotkeyScheduler::Skip, m_code_widget, &CodeWidget::Skip);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ShowPC, m_code_widget, &CodeWidget::ShowPC);
  connect(m_hotkey_scheduler, &HotkeyScheduler::SetPC, m_code_widget, &CodeWidget::SetPC);

  connect(m_hotkey_scheduler, &HotkeyScheduler::ToggleBreakpoint, m_code_widget,
          &CodeWidget::ToggleBreakpoint);
  connect(m_hotkey_scheduler, &HotkeyScheduler::AddBreakpoint, m_code_widget,
          &CodeWidget::AddBreakpoint);

  connect(m_hotkey_scheduler, &HotkeyScheduler::SkylandersPortalHotkey, this,
          &MainWindow::ShowSkylanderPortal);
  connect(m_hotkey_scheduler, &HotkeyScheduler::InfinityBaseHotkey, this,
          &MainWindow::ShowInfinityBase);
}

void MainWindow::ConnectToolBar()
{
  addToolBar(m_tool_bar);

  connect(m_tool_bar, &ToolBar::OpenPressed, this, &MainWindow::Open);
  connect(m_tool_bar, &ToolBar::RefreshPressed, this, &MainWindow::RefreshGameList);

  connect(m_tool_bar, &ToolBar::PlayPressed, this, [this] { Play(); });
  connect(m_tool_bar, &ToolBar::PausePressed, this, &MainWindow::Pause);
  connect(m_tool_bar, &ToolBar::StopPressed, this, &MainWindow::RequestStop);
  connect(m_tool_bar, &ToolBar::FullScreenPressed, this, &MainWindow::FullScreen);
  connect(m_tool_bar, &ToolBar::ScreenShotPressed, this, &MainWindow::ScreenShot);
  connect(m_tool_bar, &ToolBar::NetPlaySetupDialogPressed, this,
          &MainWindow::ShowNetPlaySetupDialog);
  connect(m_tool_bar, &ToolBar::QuickPlayPressed, this, &MainWindow::StartQuickPlay);
  connect(m_tool_bar, &ToolBar::QuickPlaySettingsPressed, this, &MainWindow::ShowQuickPlaySettings);
  connect(m_tool_bar, &ToolBar::SettingsPressed, this, &MainWindow::ShowSettingsWindow);
  connect(m_tool_bar, &ToolBar::ControllersPressed, this, &MainWindow::ShowControllersWindow);
  connect(m_tool_bar, &ToolBar::GraphicsPressed, this, &MainWindow::ShowGraphicsWindow);
  #ifdef SHOW_UPDATER
  connect(m_tool_bar, &ToolBar::InstallUpdateManuallyPressed, this, &MainWindow::ShowUpdateDialog);
#endif  // SHOW_UPDATER

  connect(m_tool_bar, &ToolBar::StepPressed, m_code_widget, &CodeWidget::Step);
  connect(m_tool_bar, &ToolBar::StepOverPressed, m_code_widget, &CodeWidget::StepOver);
  connect(m_tool_bar, &ToolBar::StepOutPressed, m_code_widget, &CodeWidget::StepOut);
  connect(m_tool_bar, &ToolBar::SkipPressed, m_code_widget, &CodeWidget::Skip);
  connect(m_tool_bar, &ToolBar::ShowPCPressed, m_code_widget, &CodeWidget::ShowPC);
  connect(m_tool_bar, &ToolBar::SetPCPressed, m_code_widget, &CodeWidget::SetPC);
}

void MainWindow::ConnectGameList()
{
  connect(m_game_list, &GameList::GameSelected, this, [this] { Play(); });
  connect(m_game_list, &GameList::NetPlayHost, this, &MainWindow::NetPlayHost);
  connect(m_game_list, &GameList::OnStartWithRiivolution, this,
          &MainWindow::ShowRiivolutionBootWidget);

  connect(m_game_list, &GameList::OpenGeneralSettings, this, &MainWindow::ShowGeneralWindow);
  connect(m_game_list, &GameList::OpenGraphicsSettings, this, &MainWindow::ShowGraphicsWindow);
}

void MainWindow::ConnectRenderWidget()
{
  m_rendering_to_main = false;
  m_render_widget->hide();
  connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
  connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
    if (m_render_widget->isFullScreen())
      SetFullScreenResolution(focus);
  });
}

void MainWindow::ConnectHost()
{
  connect(Host::GetInstance(), &Host::RequestStop, this, &MainWindow::RequestStop);
  // The Core host job already marshals to this thread and validates its lifetime.
  // Do not queue again: a queued signal could outlive reset/load invalidation.
  connect(Host::GetInstance(), &Host::RequestQuickPlay, this,
          [this] { StartQuickPlayFrom(ProjectPlusOnline::EntrySource::InGame); });
  connect(
      Host::GetInstance(), &Host::RequestQuickPlayCancel, this,
      [this] {
        // Synchronous GUI delivery from the guarded Core host job. No event-loop
        // boundary between its identity check and the canonical desktop Cancel.
        const auto attempt = ProjectPlusOnline::guest_cancel_attempt.load();
        if (attempt && m_quickplay_controller &&
            m_quickplay_controller->GetGuestCancelAttempt() == attempt)
        {
          m_quickplay_controller->Cancel();
          PublishQuickPlayStatus();
        }
      },
      Qt::DirectConnection);
}

void MainWindow::ConnectStack()
{
  auto* widget = new QWidget;
  auto* layout = new QVBoxLayout;
  widget->setLayout(layout);

  layout->addWidget(m_game_list);
  layout->addWidget(m_search_bar);
  layout->addWidget(m_game_count);
  layout->setSpacing(0);
  layout->setContentsMargins(0, 0, 0, 0);

  connect(m_search_bar, &SearchBar::Search, m_game_list, &GameList::SetSearchTerm);
  connect(m_game_list, &GameList::GameCountUpdated, m_game_count, &GameCount::OnGameCountUpdated);

  m_game_list->UpdateGameCount();

  const auto update_spacing = [this](const bool game_count_is_visible) {
    // The bottom margin of the search bar and the top margin of the game count are both suitable
    // when the other widget is hidden, but when both are visible the gap created by the combination
    // is too large. To fix this we set the bottom margin of the search bar to 0 when the game count
    // is visible and set it to the top margin when the game count is hidden.
    m_game_count->setVisible(game_count_is_visible);
    auto* const search_layout = m_search_bar->layout();
    QMargins search_margins = search_layout->contentsMargins();
    const int new_bottom_margin = game_count_is_visible ? 0 : search_margins.top();
    search_margins.setBottom(new_bottom_margin);
    search_layout->setContentsMargins(search_margins);
  };
  update_spacing(Settings::Instance().IsGameCountVisible());

  connect(&Settings::Instance(), &Settings::GameCountVisibilityChanged, update_spacing);

  m_stack->addWidget(widget);

  setCentralWidget(m_stack);

  setDockOptions(DockOption::AllowNestedDocks | DockOption::AllowTabbedDocks);
  setTabPosition(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea, QTabWidget::North);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_log_config_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_code_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_register_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_thread_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_watch_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_breakpoint_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_memory_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_network_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_jit_widget);
  addDockWidget(Qt::LeftDockWidgetArea, m_assembler_widget);

  tabifyDockWidget(m_log_widget, m_log_config_widget);
  tabifyDockWidget(m_log_widget, m_code_widget);
  tabifyDockWidget(m_log_widget, m_register_widget);
  tabifyDockWidget(m_log_widget, m_thread_widget);
  tabifyDockWidget(m_log_widget, m_watch_widget);
  tabifyDockWidget(m_log_widget, m_breakpoint_widget);
  tabifyDockWidget(m_log_widget, m_memory_widget);
  tabifyDockWidget(m_log_widget, m_network_widget);
  tabifyDockWidget(m_log_widget, m_jit_widget);
  tabifyDockWidget(m_log_widget, m_assembler_widget);
}

void MainWindow::RefreshGameList()
{
  Settings::Instance().ReloadTitleDB();
  Settings::Instance().RefreshGameList();
}

QStringList MainWindow::PromptFileNames()
{
  auto& settings = Settings::Instance().GetQSettings();
  QStringList paths = DolphinFileDialog::getOpenFileNames(
      this, tr("Select a File"),
      settings.value(QStringLiteral("mainwindow/lastdir"), QString{}).toString(),
      QStringLiteral("%1 (*.elf *.dol *.gcm *.bin *.iso *.tgc *.wbfs *.ciso *.gcz *.wia *.rvz "
                     "hif_000000.nfs *.wad *.dff *.m3u *.json);;%2 (*)")
          .arg(tr("All GC/Wii files"))
          .arg(tr("All Files")));

  if (!paths.isEmpty())
  {
    settings.setValue(QStringLiteral("mainwindow/lastdir"),
                      QFileInfo(paths.front()).absoluteDir().absolutePath());
  }

  return paths;
}

void MainWindow::ChangeDisc()
{
  std::vector<std::string> paths = StringListToStdVector(PromptFileNames());

  if (paths.empty())
    return;

  m_system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{m_system}, paths);
}

void MainWindow::EjectDisc()
{
  m_system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{m_system}, DVD::EjectCause::User);
}

void MainWindow::OpenUserFolder()
{
  std::string path = File::GetUserPath(D_USER_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenConfigFolder()
{
  std::string path = File::GetUserPath(D_CONFIG_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::OpenCacheFolder()
{
  std::string path = File::GetUserPath(D_CACHE_IDX);

  QUrl url = QUrl::fromLocalFile(QString::fromStdString(path));
  QDesktopServices::openUrl(url);
}

void MainWindow::Open()
{
  QStringList files = PromptFileNames();
  if (!files.isEmpty())
    StartGame(StringListToStdVector(files));
}

void MainWindow::Play(const std::optional<std::string>& savestate_path)
{
  if (m_quickplay_validating || m_persistent_retiring ||
      (m_persistent_quickplay && m_quickplay_setup))
    return;
  // If we're in a paused game, start it up again.
  // Otherwise, play the selected game, if there is one.
  // Otherwise, play the default game.
  // Otherwise, play the last played game, if there is one.
  // Otherwise, prompt for a new game.
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Core::SetState(m_system, Core::State::Running);
  }
  else
  {
    std::shared_ptr<const UICommon::GameFile> selection = m_game_list->GetSelectedGame();
    if (selection)
    {
      StartGame(selection->GetFilePath(), ScanForSecondDisc::Yes,
                std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
    }
    else
    {
      const QString default_path = QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
      if (!default_path.isEmpty() && QFile::exists(default_path))
      {
        StartGame(default_path, ScanForSecondDisc::Yes,
                  std::make_unique<BootSessionData>(savestate_path, DeleteSavestateAfterBoot::No));
      }
      else
      {
        Open();
      }
    }
  }
}

void MainWindow::Pause()
{
  Core::SetState(m_system, Core::State::Paused);
}

void MainWindow::TogglePause()
{
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Play();
  }
  else
  {
    Pause();
  }
}

void MainWindow::OnStopComplete()
{
  if (!Core::IsUninitialized(m_system))
    return;
  m_stop_requested = false;
  if (m_quickplay_recovery == QuickPlayRecovery::WaitingForStop)
  {
    m_quickplay_recovery = QuickPlayRecovery::Queued;
    // Let the old Core's observers drain before destroying the old lobby or booting.
    QTimer::singleShot(0, this, &MainWindow::FinishQuickPlayRecovery);
    return;
  }
  if (m_quickplay_recovery == QuickPlayRecovery::Queued ||
      m_quickplay_recovery == QuickPlayRecovery::Releasing)
    return;
  if (m_quickplay_recovery == QuickPlayRecovery::Booting)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
  }
  if (m_quickplay_retain_window && !m_exit_requested &&
      !Settings::Instance().IsBatchModeEnabled() && !m_pending_boot)
  {
    const auto attempt = std::exchange(m_quickplay_stop_attempt, 0);
    if (attempt)
    {
      // Drain every observer of the old Uninitialized notification before creating a
      // lobby: NetPlayDialog also handles that notification and must see no new session.
      QTimer::singleShot(0, this, [this, attempt] {
        if (m_quickplay_retain_window && !m_exit_requested && !m_pending_boot &&
            Core::IsUninitialized(m_system) && m_quickplay_controller->OwnsAttempt(attempt))
          m_quickplay_controller->OnCoreStopped(attempt);
      });
    }
    return;
  }
  HideRenderWidget(!m_exit_requested, m_exit_requested);
#ifdef USE_DISCORD_PRESENCE
  if (!m_netplay_dialog->HasRoom())
    Discord::UpdateDiscordPresence();
#endif

  SetFullScreenResolution(false);

  if (m_exit_requested || Settings::Instance().IsBatchModeEnabled())
  {
    if (m_assembler_widget->ApplicationCloseRequest())
    {
      QGuiApplication::exit(0);
    }
    else
    {
      m_exit_requested = false;
    }
  }

  // If the current emulation prevented the booting of another, do that now
  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

bool MainWindow::RequestStop()
{
  // Revoke the deferred admission before a confirmation dialog can run its
  // nested event loop. Restore only the pause owned by this pending request.
  if (m_quickplay_validating)
  {
    CancelQuickPlayValidation();
    InvalidateQuickPlayPresentation();
  }
  ProjectPlusOnline::presentation.Invalidate();
  // A close during the deferred stop continuation must cancel the reboot, while
  // leaving its sole teardown owner alive to release the old session.
  if (m_quickplay_recovery != QuickPlayRecovery::None)
    m_quickplay_recovery_restart = false;
  if (m_quickplay_retain_window || m_quickplay_stop_attempt)
  {
    if (m_quickplay_controller &&
        m_quickplay_controller->GetState() != QuickPlayController::State::NetPlayOwned)
      m_quickplay_controller->Cancel();
    m_quickplay_stop_attempt = 0;
    m_quickplay_retain_window = false;
  }
  if (Core::IsUninitialized(m_system))
  {
    Core::QueueHostJob([this](Core::System&) { OnStopComplete(); }, true);
    return true;
  }

  const bool rendered_widget_was_active =
      Settings::Instance().IsKeepWindowOnTopEnabled() ||
      (m_render_widget->isActiveWindow() && !m_render_widget->isFullScreen());
  QWidget* confirm_parent = (!m_rendering_to_main && rendered_widget_was_active) ?
                                m_render_widget :
                                static_cast<QWidget*>(this);
  const bool was_cursor_locked = m_render_widget->IsCursorLocked();

  if (!m_render_widget->isFullScreen())
    m_render_widget_geometry = m_render_widget->saveGeometry();
  else
    FullScreen();

  bool confirm_on_stop = Config::Get(Config::MAIN_CONFIRM_ON_STOP);
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  confirm_on_stop = confirm_on_stop || AchievementManager::GetInstance().CheckForModifications();
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (confirm_on_stop)
  {
    if (std::exchange(m_stop_confirm_showing, true))
      return true;

    Common::ScopeGuard confirm_lock([this] { m_stop_confirm_showing = false; });

    const Core::State state = Core::GetState(m_system);

    // Only pause the game, if NetPlay is not running
    bool pause = !Settings::Instance().GetNetPlayClient();

    if (pause)
      Core::SetState(m_system, Core::State::Paused);

    if (rendered_widget_was_active)
    {
      // We have to do this before creating the message box, otherwise we might receive the window
      // activation event before we know we need to lock the cursor again.
      m_render_widget->SetCursorLockedOnNextActivation(was_cursor_locked);
    }

    // This is to avoid any "race conditions" between the "Window Activate" message and the
    // message box returning, which could break cursor locking depending on the order
    m_render_widget->SetWaitingForMessageBox(true);
    QString message;
    if (m_stop_requested)
    {
      message = tr("A shutdown is already in progress. Unsaved data "
                   "may be lost if you stop the current emulation "
                   "before it completes. Force stop?");
    }
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    else if (AchievementManager::GetInstance().CheckForModifications())
    {
      message = tr(
          "Do you want to stop the current emulation? Unsaved achievement modifications detected.");
    }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    else
    {
      message = tr("Do you want to stop the current emulation?");
    }
    auto confirm = ModalMessageBox::question(confirm_parent, tr("Confirm"), message,
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::NoButton, Qt::ApplicationModal);

    // If a user confirmed stopping the emulation, we do not capture the cursor again,
    // even if the render widget will stay alive for a while.
    // If a used rejected stopping the emulation, we instead capture the cursor again,
    // and let them continue playing as if nothing had happened
    // (assuming cursor locking is on).
    if (confirm != QMessageBox::Yes)
    {
      m_render_widget->SetWaitingForMessageBox(false);

      if (pause)
        Core::SetState(m_system, state);

      return false;
    }
    else
    {
      m_render_widget->SetCursorLockedOnNextActivation(false);
      // This needs to be after SetCursorLockedOnNextActivation(false) as it depends on it
      m_render_widget->SetWaitingForMessageBox(false);
    }
  }

  if (m_persistent_quickplay || m_persistent_retiring)
  {
    // Explicit application Stop/Close retires the session and stops this Core;
    // only a room/session leave performs clean local CSS recovery.
    ForceStop();
    return true;
  }
  if (m_quickplay_session || m_quickplay_recovery != QuickPlayRecovery::None)
  {
    // Guest power events are asynchronous to synchronized pad input. End this
    // session using the normal NetPlay stop notification instead of guest shutdown.
    EndQuickPlaySession(false);
    ForceStop();
    return true;
  }

  OnStopRecording();
  // TODO: Add Debugger shutdown

  if (!m_stop_requested && UICommon::TriggerSTMPowerEvent())
  {
    m_stop_requested = true;

    // Unpause because gracefully shutting down needs the game to actually request a shutdown.
    // TODO: Do not unpause in debug mode to allow debugging until the complete shutdown.
    if (Core::GetState(m_system) == Core::State::Paused)
      Core::SetState(m_system, Core::State::Running);

    // Tell NetPlay about the power event
    if (NetPlay::IsNetPlayRunning())
      NetPlay::SendPowerButtonEvent();

    return true;
  }

  ForceStop();
#ifdef Q_OS_WIN
  // Allow windows to idle or turn off display again
  SetThreadExecutionState(ES_CONTINUOUS);
#endif
  return true;
}

void MainWindow::ForceStop()
{
  InvalidateQuickPlayPresentation();
  if (m_persistent_quickplay || m_persistent_retiring)
    RetirePersistentQuickPlay(false);
  Core::Stop(m_system);
}

void MainWindow::Reset()
{
  CancelQuickPlayValidation();
  InvalidateQuickPlayPresentation();
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    movie.SetReset(true);
  m_system.GetProcessorInterface().ResetButton_Tap();
}

void MainWindow::FrameAdvance()
{
  if (m_quickplay_validating || m_persistent_retiring ||
      (m_persistent_quickplay && m_quickplay_setup))
    return;
  Core::DoFrameStep(m_system);
}

void MainWindow::FullScreen()
{
  // If the render widget is fullscreen we want to reset it to whatever is in
  // settings. If it's set to be fullscreen then it just remakes the window,
  // which probably isn't ideal.
  bool was_fullscreen = m_render_widget->isFullScreen();

  if (!was_fullscreen)
    m_render_widget_geometry = m_render_widget->saveGeometry();

  HideRenderWidget(false);
  SetFullScreenResolution(!was_fullscreen);

  if (was_fullscreen)
  {
    ShowRenderWidget();
  }
  else
  {
    m_render_widget->showFullScreen();
  }
}

void MainWindow::UnlockCursor()
{
  if (!m_render_widget->isFullScreen())
    m_render_widget->SetCursorLocked(false);
}

void MainWindow::ScreenShot()
{
  Core::SaveScreenShot();
}

void MainWindow::ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                               std::unique_ptr<BootSessionData> boot_session_data)
{
  auto second_game = m_game_list->FindSecondDisc(game);

  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());

  StartGame(paths, std::move(boot_session_data));
}

void MainWindow::StartGame(const QString& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(path.toStdString(), scan, std::move(boot_session_data));
}

void MainWindow::StartGame(const std::string& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  if (scan == ScanForSecondDisc::Yes)
  {
    std::shared_ptr<const UICommon::GameFile> game = m_game_list->FindGame(path);
    if (game != nullptr)
    {
      ScanForSecondDiscAndStartGame(*game, std::move(boot_session_data));
      return;
    }
  }

  StartGame(BootParameters::GenerateFromFile(
      path, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(const std::vector<std::string>& paths,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(BootParameters::GenerateFromFile(
      paths, boot_session_data ? std::move(*boot_session_data) : BootSessionData()));
}

void MainWindow::StartGame(std::unique_ptr<BootParameters>&& parameters)
{
  if (parameters && std::holds_alternative<BootParameters::Disc>(parameters->parameters))
  {
    if (std::get<BootParameters::Disc>(parameters->parameters).volume->IsNKit())
    {
      if (!NKitWarningDialog::ShowUnlessDisabled())
        return;
    }

    const auto volume_type =
        std::get<BootParameters::Disc>(parameters->parameters).volume->GetVolumeType();
    if (volume_type != DiscIO::Platform::Triforce)
    {
      const bool triforce_hardware_sp1 =
          Config::Get(Config::MAIN_SERIAL_PORT_1) == ExpansionInterface::EXIDeviceType::Baseboard;
      const bool triforce_hardware_port_1 = Config::Get(Config::GetInfoForSIDevice(0)) ==
                                            SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD;

      // Some Triforce tools don't include a boot.id file, but they can still be launched.
      if (triforce_hardware_sp1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from SP1."),
                                 QMessageBox::Ok);
      }
      if (triforce_hardware_port_1)
      {
        ModalMessageBox::warning(this, tr("Warning"),
                                 tr("Non-Triforce games cannot be booted with Triforce hardware "
                                    "attached.\nPlease remove the Triforce Baseboard from Port 1."),
                                 QMessageBox::Ok);
      }
    }
  }

  // Only the guarded NetPlay BootGame callback may consume an owned handoff.
  if (m_quickplay_setup || m_quickplay_recovery == QuickPlayRecovery::WaitingForStop ||
      m_quickplay_recovery == QuickPlayRecovery::Queued ||
      m_quickplay_recovery == QuickPlayRecovery::Releasing)
    return;

  // If we're running, only start a new game once we've stopped the last.
  if (!Core::IsUninitialized(m_system))
  {
    if (!RequestStop())
      return;

    // As long as the shutdown isn't complete, we can't boot, so let's boot later
    m_pending_boot = std::move(parameters);
    return;
  }

  // The retained-window lease ends when an explicit later boot consumes the window.
  m_quickplay_retain_window = false;
  // We need the render widget before booting.
  ShowRenderWidget();

  // Boot up, show an error if it fails to load the game.
  if (!BootManager::BootCore(m_system, std::move(parameters),
                             ::GetWindowSystemInfo(m_render_widget->windowHandle())))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to init core"), QMessageBox::Ok);
    HideRenderWidget();
    return;
  }

#ifdef USE_DISCORD_PRESENCE
  if (!NetPlay::IsNetPlayRunning())
    Discord::UpdateDiscordPresence();
#endif

  if (Config::Get(Config::MAIN_FULLSCREEN))
    m_fullscreen_requested = true;
}

void MainWindow::SetFullScreenResolution(bool fullscreen)
{
  if (Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES) == "Auto")
    return;
#ifdef _WIN32

  if (!fullscreen)
  {
    ChangeDisplaySettings(nullptr, CDS_FULLSCREEN);
    return;
  }

  DEVMODE screen_settings;
  memset(&screen_settings, 0, sizeof(screen_settings));
  screen_settings.dmSize = sizeof(screen_settings);
  sscanf(Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES).c_str(), "%lux%lu",
         &screen_settings.dmPelsWidth, &screen_settings.dmPelsHeight);
  screen_settings.dmBitsPerPel = 32;
  screen_settings.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

  // Try To Set Selected Mode And Get Results.  NOTE: CDS_FULLSCREEN Gets Rid Of Start Bar.
  ChangeDisplaySettings(&screen_settings, CDS_FULLSCREEN);
#elif defined(HAVE_XRANDR) && HAVE_XRANDR
  if (m_xrr_config)
    m_xrr_config->ToggleDisplayMode(fullscreen);
#endif
}

void MainWindow::ShowRenderWidget()
{
  SetFullScreenResolution(false);
  Host::GetInstance()->SetRenderFullscreen(false);

  if (Config::Get(Config::MAIN_RENDER_TO_MAIN))
  {
    // If we're rendering to main, add it to the stack and update our title when necessary.
    m_rendering_to_main = true;

    m_stack->setCurrentIndex(m_stack->addWidget(m_render_widget));
    connect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->repaint();

    Host::GetInstance()->SetRenderFocus(isActiveWindow());
  }
  else
  {
    // Otherwise, just show it.
    m_rendering_to_main = false;

    m_render_widget->showNormal();
    m_render_widget->restoreGeometry(m_render_widget_geometry);
  }
}

void MainWindow::HideRenderWidget(bool reinit, bool is_exit)
{
  if (m_rendering_to_main)
  {
    // Remove the widget from the stack and reparent it to nullptr, so that it can draw
    // itself in a new window if it wants. Disconnect the title updates.
    m_stack->removeWidget(m_render_widget);
    m_render_widget->setParent(nullptr);
    m_rendering_to_main = false;
    m_stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    disconnect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
    setWindowTitle(QString::fromStdString(Common::GetScmRevStr()));
  }

  // The following code works around a driver bug that would lead to Dolphin crashing when changing
  // graphics backends (e.g. OpenGL to Vulkan). To avoid this the render widget is (safely)
  // recreated
  if (reinit)
  {
    m_render_widget->hide();
    disconnect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);

    m_render_widget->removeEventFilter(this);
    m_render_widget->deleteLater();

    m_render_widget = new RenderWidget;

    m_render_widget->installEventFilter(this);
    connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
    connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool focus) {
      if (m_render_widget->isFullScreen())
        SetFullScreenResolution(focus);
    });

    // The controller interface will still be registered to the old render widget, if the core
    // has booted. Therefore, we should re-bind it to the main window for now. When the core
    // is next started, it will be swapped back to the new render widget.
    g_controller_interface.ChangeWindow(::GetWindowSystemInfo(windowHandle()).render_window,
                                        is_exit ? ControllerInterface::WindowChangeReason::Exit :
                                                  ControllerInterface::WindowChangeReason::Other);
  }
}

void MainWindow::ShowControllersWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Controllers);
}

void MainWindow::ShowTriforceWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Triforce);
}

void MainWindow::ShowFreeLookWindow()
{
  if (!m_freelook_window)
  {
    m_freelook_window = new FreeLookWindow(this);
    InstallHotkeyFilter(m_freelook_window);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_freelook_window, &FreeLookWindow::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_freelook_window->show();
  m_freelook_window->raise();
  m_freelook_window->activateWindow();
}

void MainWindow::ShowSettingsWindow()
{
  if (!m_settings_window)
  {
#ifdef HAVE_XRANDR
    if (GetWindowSystemType() == WindowSystemType::X11)
    {
      m_xrr_config = std::make_unique<X11Utils::XRRConfiguration>(
          static_cast<Display*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
              "display", windowHandle())),
          winId());
    }
#endif
    m_settings_window = new SettingsWindow(this);
    InstallHotkeyFilter(m_settings_window);
  }

  m_settings_window->show();
  m_settings_window->raise();
  m_settings_window->activateWindow();
}

void MainWindow::ShowAudioWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Audio);
}

void MainWindow::ShowGeneralWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::General);
}

void MainWindow::ShowOSDWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::OnScreenDisplay);
}

void MainWindow::ShowAboutDialog()
{
  AboutDialog about{this};
  about.exec();
}

// P+ change: New updater; credit to RainbowTabitha and the Mario Party Netplay team for the base code!

#ifdef SHOW_UPDATER
void MainWindow::ShowUpdateDialog()
{
    Common::HttpRequest httpRequest;

    // Make the GET request
    auto response = httpRequest.Get("https://api.github.com/repos/Project-Plus-Development-Team/Project-Plus-Dolphin/releases/latest");

    if (response)
    {
        // Access the underlying vector and convert it to QByteArray
        QByteArray responseData(reinterpret_cast<const char*>(response->data()), response->size());

        // Parse the JSON response
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonObject jsonObject = jsonDoc.object();
      
        QString currentVersion = QString::fromStdString(SCM_DESC_STR);
        QString latestVersion = jsonObject.value(QStringLiteral("tag_name")).toString();

        if (currentVersion != latestVersion)
        {
          // Create and show the UpdateDialog with the fetched data
          bool forced = false; // Set this based on your logic
          UserInterface::Dialog::UpdateDialog updater(this, jsonObject, forced);
          updater.exec();
        } else {
          QMessageBox::information(this, tr("Info"), tr("You are already up to date."));
        }
    }
    else
    {
        // Handle error
        QMessageBox::critical(this, tr("Error"), tr("Failed to fetch update information."));
    }
}

void MainWindow::CheckForUpdatesAuto()
{
    Common::HttpRequest httpRequest;

    // Make the GET request
    auto response = httpRequest.Get("https://api.github.com/repos/Project-Plus-Development-Team/Project-Plus-Dolphin/releases/latest");

    if (response)
    {
        // Access the underlying vector and convert it to QByteArray
        QByteArray responseData(reinterpret_cast<const char*>(response->data()), response->size());

        // Parse the JSON response
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonObject jsonObject = jsonDoc.object();
      
        QString currentVersion = QString::fromStdString(SCM_DESC_STR);
        QString latestVersion = jsonObject.value(QStringLiteral("tag_name")).toString();

        if (currentVersion != latestVersion)
        {
          // Create and show the UpdateDialog with the fetched data
          bool forced = false; // Set this based on your logic
          UserInterface::Dialog::UpdateDialog updater(this, jsonObject, forced);
          updater.exec();
        }
    }
    else
    {
        // Handle error
        QMessageBox::critical(this, tr("Error"), tr("Failed to fetch update information."));
    }
}
#endif  // SHOW_UPDATER

void MainWindow::ShowHotkeyDialog()
{
  if (!m_hotkey_window)
  {
    m_hotkey_window = new MappingWindow(this, MappingWindow::Type::MAPPING_HOTKEYS, 0);
    InstallHotkeyFilter(m_hotkey_window);
  }

  m_hotkey_window->show();
  m_hotkey_window->raise();
  m_hotkey_window->activateWindow();
}

void MainWindow::ShowGraphicsWindow()
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(SettingsWindowPaneIndex::Graphics);
}

void MainWindow::ShowQuickPlaySettings()
{
  if (m_quickplay_controller &&
      m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
    return;

  // Settings access must not construct a controller or start a matchmaking attempt.
  m_tool_bar->SetQuickPlaySettingsOpen(true);
  if (!m_quickplay_settings_dialog)
  {
    m_quickplay_settings_dialog = new QuickPlaySettingsDialog(this);
    connect(m_quickplay_settings_dialog, &QDialog::finished, this,
            [this] { m_tool_bar->SetQuickPlaySettingsOpen(false); });
  }
  m_quickplay_settings_dialog->Open();
}

void MainWindow::PublishQuickPlayStatus()
{
  ProjectPlusOnline::persistent_setup_pause = m_quickplay_validating || m_persistent_retiring ||
      (m_persistent_quickplay && m_quickplay_setup);
  Common::ScopeGuard publish_presentation([this] { PublishQuickPlayPresentation(); });
  using ProjectPlusOnline::GuestStatus;
  using ProjectPlusOnline::Reason;
  using State = QuickPlayController::State;
  ProjectPlusOnline::persistent_local_input = PersistentQuickPlaySession::IsConfigured() &&
                                              !m_persistent_quickplay && !m_persistent_retiring &&
                                              !NetPlay::IsNetPlayRunning() && !m_exit_requested &&
                                              !m_quickplay_validating;
  const auto publish = [this](GuestStatus status, Reason reason = Reason::None) {
    const auto attempt =
        m_quickplay_controller && m_quickplay_recovery == QuickPlayRecovery::None &&
                !m_quickplay_validating && !m_quickplay_blocking_connection &&
                !m_persistent_retiring && ProjectPlusOnline::IsCancellable(status) ?
            m_quickplay_controller->GetGuestCancelAttempt() :
            0;
    if (ProjectPlusOnline::guest_cancel_attempt.load() != attempt)
      ProjectPlusOnline::guest_cancel_attempt = 0;
    if (ProjectPlusOnline::IsCancellable(status) && !attempt)
      reason = Reason::Lifecycle;
    ProjectPlusOnline::guest_status.Publish(status, reason);
    ProjectPlusOnline::guest_cancel_attempt = attempt;
  };
  if (m_quickplay_recovery != QuickPlayRecovery::None)
    return publish(GuestStatus::Unavailable, Reason::Lifecycle);
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
  {
    const auto lobby = client->GetLobbyState();
    if (lobby.connected && lobby.players.size() >= 2)
      return publish(GuestStatus::Connected);
  }
  if (client || Settings::Instance().GetNetPlayServer() || m_quickplay_session)
    return publish(GuestStatus::Connecting);
  if (m_quickplay_stop_attempt && m_quickplay_setup &&
      m_quickplay_controller->OwnsAttempt(m_quickplay_stop_attempt))
  {
    if (m_quickplay_presentation == QuickPlayPresentation::Matched)
      return publish(GuestStatus::Matched);
    if (m_quickplay_presentation == QuickPlayPresentation::Connecting)
      return publish(GuestStatus::Connecting);
  }
  const auto state = m_quickplay_controller ? m_quickplay_controller->GetState() : State::Idle;
  switch (state)
  {
  case State::Idle:
    if (Core::GetState(m_system) == Core::State::Stopping)
      return publish(GuestStatus::Unavailable, Reason::Lifecycle);
    return publish(m_quickplay_setup ? GuestStatus::Connecting : GuestStatus::Idle);
  case State::Searching:
    return publish(GuestStatus::Searching);
  case State::MatchedHost:
  case State::MatchedClient:
    return publish(GuestStatus::Matched);
  case State::Error:
    return publish(GuestStatus::Error, Reason::AttemptFailed);
  case State::NetPlayConnected:
  case State::NetPlayOwned:
    return publish(GuestStatus::Unavailable, Reason::Lifecycle);
  default:
    return publish(GuestStatus::Connecting);
  }
}

void MainWindow::InvalidateQuickPlayPresentation()
{
  ProjectPlusOnline::presentation.Invalidate();
  ProjectPlusOnline::setup_action.Invalidate();
  ProjectPlusOnline::leave_action.Invalidate();
  m_quickplay_paused_css.reset();
  ++m_quickplay_room_generation;
  ++m_quickplay_validation_request;
  m_quickplay_validating = false;
  m_quickplay_blocking_connection = false;
  m_quickplay_validation_fence.reset();
  m_quickplay_admission_error.clear();
  m_quickplay_cancelled_until = {};
}

void MainWindow::CancelQuickPlayValidation()
{
  if (!m_quickplay_validating || !m_quickplay_validation_fence)
    return;
  const auto fence = *m_quickplay_validation_fence;
  ++m_quickplay_validation_request;
  m_quickplay_validating = false;
  m_quickplay_validation_fence.reset();
  ProjectPlusOnline::presentation.Invalidate();
  if (m_quickplay_validation_was_running &&
      fence.identity.core == ProjectPlusOnline::presentation_core_generation.load() &&
      fence.identity.input == ProjectPlusOnline::persistent_input_epoch.load() &&
      fence.window == static_cast<std::uintptr_t>(m_render_widget->effectiveWinId()) &&
      Core::GetState(m_system) == Core::State::Paused)
    Core::SetState(m_system, Core::State::Running);
}

ProjectPlusOnline::PresentationIdentity MainWindow::QuickPlayIdentity() const
{
  return {ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load(), m_quickplay_owned_attempt,
          m_quickplay_room_generation, m_persistent_game};
}

bool MainWindow::OwnsInGameQuickPlay() const
{
  return m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
         m_quickplay_owner_core == ProjectPlusOnline::presentation_core_generation.load() &&
         m_persistent_quickplay && !m_persistent_retiring && m_quickplay_owned_attempt &&
         m_persistent_client == Settings::Instance().GetNetPlayClient().get() &&
         m_persistent_server == Settings::Instance().GetNetPlayServer().get();
}

void MainWindow::ReportQuickPlayError(const QString& error)
{
  INFO_LOG_FMT(NETPLAY, "QuickPlay: error shown: {}", error.toStdString());
  m_quickplay_admission_error = error;
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  m_netplay_dialog->SetDiagnosticError(error);
  PublishQuickPlayStatus();
  const bool renderer_route = Core::IsRunning(m_system) && !m_system.IsDualCoreMode() &&
                              PersistentQuickPlaySession::IsConfigured() &&
                              IsQuickPlayWindowSystem() &&
                              !m_rendering_to_main && m_render_widget->isVisible();
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame && !error.isEmpty() &&
      !renderer_route && !m_exit_requested && !m_pending_boot &&
      Core::GetState(m_system) != Core::State::Stopping)
    m_netplay_dialog->ShowDiagnostics();
}

void MainWindow::ShowQuickPlayDiagnostics()
{
  ProjectPlusOnline::setup_action.Invalidate();
  if (!m_quickplay_admission_error.isEmpty())
    m_netplay_dialog->SetDiagnosticError(m_quickplay_admission_error);
  m_netplay_dialog->ShowDiagnostics();
}

void MainWindow::ObservePausedQuickPlayCSS()
{
  m_quickplay_paused_css.reset();
  ProjectPlusOnline::setup_action.Invalidate();
  const auto identity = QuickPlayIdentity();
  const auto window = QPointer<MainWindow>(this);
  Core::RunOnCPUThread(m_system, [window, identity, &system = m_system] {
    if (identity.core != ProjectPlusOnline::presentation_core_generation.load() ||
        identity.input != ProjectPlusOnline::persistent_input_epoch.load() ||
        Core::GetState(system) != Core::State::Paused)
      return;
    const bool css =
        ProjectPlusOnline::ObserveLocalCSS(system) && !system.GetMovie().IsMovieActive();
    QueueOnObject(QApplication::instance(), [window, identity, css] {
      if (!window || identity != window->QuickPlayIdentity() ||
          !window->IsPersistentQuickPlayBoundary())
        return;
      if (css)
        window->m_quickplay_paused_css = identity;
      window->PublishQuickPlayStatus();
    });
  });
}

void MainWindow::PrepareQuickPlayConnection(u64 attempt, bool host)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt))
    return;
  m_quickplay_blocking_connection = true;
  ProjectPlusOnline::setup_action.Invalidate();
  PublishQuickPlayStatus();
  const auto fence = ProjectPlusOnline::PrepaintFence{
      QuickPlayIdentity(), ++m_quickplay_validation_request,
      static_cast<std::uintptr_t>(m_render_widget->effectiveWinId())};
  const auto connect_room = [this, attempt, host, fence](bool painted) {
    const auto current = ProjectPlusOnline::PrepaintFence{
        QuickPlayIdentity(), m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_render_widget->effectiveWinId())};
    if (fence != current || !m_quickplay_controller->OwnsAttempt(attempt) || !m_quickplay_setup ||
        m_quickplay_setup->attempt != attempt)
      return;
    const bool dispatchable = !m_exit_requested && !m_pending_boot && !m_stop_confirm_showing &&
                              !QApplication::activeModalWidget();
    const bool supported =
        !m_persistent_quickplay ||
        (IsPersistentQuickPlayBoundary() && !m_system.IsDualCoreMode() &&
         IsQuickPlayWindowSystem() && !m_rendering_to_main &&
         m_render_widget->isVisible());
    const QString error = painted && dispatchable && supported ?
                              QString{} :
                              tr("Quick Play could not display its paused connection status. Open "
                                 "Tools > Quick Play Diagnostics.");
    m_quickplay_controller->ContinueConnection(attempt, host, error);
    m_quickplay_blocking_connection = false;
    ProjectPlusOnline::setup_action.Invalidate();  // A fresh B release is required after blocking.
    PublishQuickPlayStatus();
  };
  if (m_persistent_quickplay)
    RedrawQuickPlayPresentation(connect_room);
  else
    connect_room(true);  // Ordinary Desktop setup retains its synchronous behavior.
}

// The single remote player of a two-player room, as untrusted plain text bounded
// for the compact in-game strip. Never the coordinator label, host role or slot.
static QString QuickPlayOpponentName(
    const std::optional<NetPlay::NetPlayClient::LobbyState>& lobby)
{
  if (!lobby || !lobby->connected || lobby->players.size() != 2)
    return {};
  const NetPlay::Player* opponent = nullptr;
  for (const auto& player : lobby->players)
  {
    if (player.pid == lobby->local_player)
      continue;
    if (opponent)
      return {};
    opponent = &player;
  }
  if (!opponent)
    return {};
  constexpr int MAX_DISPLAY_CHARACTERS = 20;
  QString name;
  int count = 0;
  for (const char32_t cp : QString::fromStdString(opponent->name).trimmed().toUcs4())
  {
    const auto category = QChar::category(cp);
    if (category == QChar::Other_Control || category == QChar::Other_Format ||
        category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
        category == QChar::Separator_Paragraph)
      continue;
    if (count == MAX_DISPLAY_CHARACTERS)
    {
      name += QStringLiteral("…");
      break;
    }
    name += QString::fromUcs4(&cp, 1);
    ++count;
  }
  return name;
}

void MainWindow::PublishQuickPlayPresentation()
{
  using namespace ProjectPlusOnline;
  using State = QuickPlayController::State;
  PresentationSnapshot value;
  const auto observed_revision = presentation.Revision();
  const auto core_state = Core::GetState(m_system);
  const auto core = presentation_core_generation.load();
  const auto input = persistent_input_epoch.load();
  value.identity = QuickPlayIdentity();
  value.entry = m_quickplay_entry;
  const auto client = Settings::Instance().GetNetPlayClient();
  const auto lobby = client ? std::optional{client->GetLobbyState()} : std::nullopt;
  const bool disconnected = lobby && !lobby->connected;
  const bool supported = Core::IsRunning(m_system) && PersistentQuickPlaySession::IsConfigured();
  const bool eligible = local_css.Read().IsFresh(core, input, std::chrono::steady_clock::now()) &&
                        IsQuickPlayWindowSystem() &&
                        !m_rendering_to_main && m_render_widget->isVisible() && !client &&
                        !Settings::Instance().GetNetPlayServer();
  const auto state = m_quickplay_controller ? m_quickplay_controller->GetState() : State::Idle;
  const bool paused_action =
      OwnsInGameQuickPlay() && IsPersistentQuickPlayBoundary() &&
      m_quickplay_paused_css == value.identity && m_quickplay_setup &&
      m_quickplay_setup->attempt == value.identity.attempt &&
      m_quickplay_setup->window == m_render_widget &&
      m_quickplay_setup->native_window == static_cast<u64>(m_render_widget->effectiveWinId()) &&
      m_quickplay_controller &&
      m_quickplay_controller->OwnsAttempt(value.identity.attempt) &&
      m_quickplay_controller->GetGuestCancelAttempt() == value.identity.attempt &&
      guest_cancel_attempt.load() == value.identity.attempt &&
      (!lobby || (lobby->connected && lobby->current_game == value.identity.game)) && supported &&
      IsQuickPlayWindowSystem() && !m_rendering_to_main &&
      m_render_widget->isVisible() && !m_exit_requested && !m_pending_boot &&
      !m_stop_confirm_showing && !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection && m_quickplay_recovery == QuickPlayRecovery::None;
  const auto action = setup_action.Publish(value.identity, paused_action);
  // Leave is available for the whole owned in-game session once the opponent has
  // joined: paused setup, shared CSS, stage select, gameplay and results.
  const bool leave_action_enabled =
      OwnsInGameQuickPlay() && lobby && lobby->connected && lobby->players.size() >= 2 &&
      supported && IsQuickPlayWindowSystem() && !m_rendering_to_main &&
      m_render_widget->isVisible() && !m_exit_requested && !m_pending_boot &&
      !m_stop_confirm_showing && !QApplication::activeModalWidget() && !m_quickplay_validating &&
      !m_quickplay_blocking_connection && m_quickplay_recovery == QuickPlayRecovery::None &&
      (core_state == Core::State::Running || core_state == Core::State::Paused);
  leave_action.Publish(value.identity, leave_action_enabled);
  // Diagnostic: one line per change while a persistent session exists.
  if (leave_action_enabled != m_quickplay_leave_enabled_logged &&
      (m_persistent_quickplay || m_quickplay_leave_enabled_logged))
  {
    INFO_LOG_FMT(NETPLAY,
                 "Persistent QuickPlay: leave enabled {} (owns {} lobby {} players {} "
                 "supported {} modal {} validating {} recovery {})",
                 leave_action_enabled, OwnsInGameQuickPlay(), lobby && lobby->connected,
                 lobby ? lobby->players.size() : 0, supported,
                 QApplication::activeModalWidget() != nullptr, m_quickplay_validating,
                 static_cast<int>(m_quickplay_recovery));
    m_quickplay_leave_enabled_logged = leave_action_enabled;
  }
  const QString opponent = QuickPlayOpponentName(lobby);
  const bool left = std::chrono::steady_clock::now() < m_quickplay_left_until;
  const bool opponent_left = std::chrono::steady_clock::now() < m_quickplay_opponent_left_until;
  QString text;
  const auto recipe = QuickPlayRecipe::GetPhase();
  if (recipe != QuickPlayRecipe::Phase::Failed)
    m_quickplay_recipe_failure_until.reset();
  else if (!m_quickplay_recipe_failure_until)
    m_quickplay_recipe_failure_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  if (!supported && Core::IsRunning(m_system) && !m_exit_requested &&
      recipe == QuickPlayRecipe::Phase::Preparing)
  {
    // Search stays unavailable until this Core has captured its origin.
    value.phase = PresentationPhase::Checking;
    text = tr("Preparing Quick Play…");
  }
  else if (!supported && Core::IsRunning(m_system) && !m_exit_requested &&
           recipe == QuickPlayRecipe::Phase::Failed &&
           std::chrono::steady_clock::now() < *m_quickplay_recipe_failure_until)
  {
    value.phase = PresentationPhase::Error;
    text = tr("Quick Play unavailable: %1. Local play continues.")
               .arg(QString::fromStdString(QuickPlayRecipe::FailureReason()));
  }
  else if (supported && !m_exit_requested && m_persistent_retiring &&
           (left || m_quickplay_opponent_left))
  {
    value.phase = PresentationPhase::Setup;
    text = left ? tr("Leaving…") : tr("Opponent left the session");
  }
  else if (supported && !m_exit_requested && !m_persistent_retiring &&
           m_quickplay_recovery == QuickPlayRecovery::None)
  {
    if (m_quickplay_validating)
    {
      value.phase = PresentationPhase::Checking;
      text = QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::Strict ?
                 tr("Checking game files…") : tr("Preparing session…");
    }
    else if (!m_quickplay_admission_error.isEmpty() && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_quickplay_admission_error + tr("  Tools > Quick Play Diagnostics");
    }
    else if (state == State::Error && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Error;
      text = m_quickplay_controller->GetErrorMessage();
    }
    else if (!disconnected && state != State::Idle && value.entry == EntrySource::InGame &&
             m_quickplay_owner_core == core)
    {
      value.phase = PresentationPhase::Setup;
      switch (state)
      {
      case State::Searching:
      {
        value.phase = PresentationPhase::Searching;
        // The regions captured when this attempt started, not the current setting.
        const auto& regions = m_quickplay_controller->GetRegions();
        const auto backend = m_quickplay_controller->GetBackendLabel();
        // Kept short: the overlay has two lines, and the cancel hint follows the text.
        const auto stage = m_quickplay_controller->GetSearchStage();
        const QString where = regions.size() == 1 ?
                                  QuickPlayClient::GetRegionLabel(regions.front()) :
                                  tr("%n regions", "", regions.size());
        if (!m_quickplay_controller->GetSearchNotice().isEmpty())
          text = m_quickplay_controller->GetSearchNotice();
        else if (regions.isEmpty())
          text = tr("Searching (%1)…").arg(backend);
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinOnly)
          text = tr("Looking for a host in %1 (%2)… %3 s")
                     .arg(where, backend)
                     .arg(m_quickplay_controller->GetSearchStageSeconds());
        else if (stage == QuickPlayMatchmaker::SearchStage::JoinListed)
          text = tr("Searching in %1 (%2), now listed…").arg(where, backend);
        else
          text = tr("Searching in %1 (%2)…").arg(where, backend);
        break;
      }
      case State::MatchedHost:
      case State::MatchedClient:
      case State::PreparingHandoff:
      case State::WaitingForCoreStop:
      case State::WaitingForHandoff:
        text = tr("Opponent found. Preparing session…");
        break;
      case State::ValidatingMatch:
        text = tr("Checking match…");
        break;
      case State::CreatingHost:
        text = tr("Creating room…");
        break;
      case State::WaitingForTraversalCode:
      case State::PublishingHostCode:
        text = tr("Waiting for connection…");
        break;
      case State::WaitingForOpponent:
        text = tr("Waiting for opponent…");
        break;
      case State::WaitingForHost:
        text = tr("Waiting for host…");
        break;
      case State::Connecting:
        text = tr("Connecting…");
        break;
      case State::NetPlayConnected:
        text = tr("Preparing shared session…");
        break;
      case State::NetPlayOwned:
        if (m_persistent_quickplay && m_persistent_game && lobby &&
            lobby->current_game == m_persistent_game && NetPlay::IsNetPlayRunning())
        {
          value.phase = PresentationPhase::Active;
          text = opponent.isEmpty() ? tr("Online session") : tr("vs %1").arg(opponent);
        }
        else
          value.phase = PresentationPhase::Hidden;
        break;
      default:
        value.phase = PresentationPhase::Hidden;
        break;
      }
      value.cancel = CanPresentCancel(core_state == Core::State::Running, eligible,
                                      !m_quickplay_blocking_connection && !m_quickplay_setup &&
                                          guest_cancel_attempt.load() == value.identity.attempt,
                                      m_quickplay_controller->GetGuestCancelAttempt()) ||
                     setup_action.Available(action);
      if (m_quickplay_blocking_connection)
        text = tr("Connecting to room…");
      // Once the opponent has joined, setup status names them as well.
      if (value.phase == PresentationPhase::Setup && !opponent.isEmpty())
        text = tr("vs %1").arg(opponent) + QStringLiteral("   ") + text;
      value.leave = leave_action_enabled && value.phase != PresentationPhase::Hidden;
    }
    else if (eligible && core_state == Core::State::Running)
    {
      value.phase = PresentationPhase::LocalCSS;
      text = tr("L + R + D-pad Up — Search");
      if (left || opponent_left)
      {
        value.phase = PresentationPhase::Cancelled;
        text = left ? tr("Left session") : tr("Opponent left the session");
      }
      else if (std::chrono::steady_clock::now() < m_quickplay_cancelled_until)
      {
        value.phase = PresentationPhase::Cancelled;
        text = tr("Cancelled");
      }
    }
  }
  // Bound plain UTF-8 on complete code points; strip control/format characters.
  const auto copy_text = [](const QString& source, auto& target) {
    std::size_t offset = 0;
    for (const auto cp : source.toUcs4())
    {
      const auto category = QChar::category(cp);
      if (category == QChar::Other_Control || category == QChar::Other_Format ||
          category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
          category == QChar::Separator_Paragraph)
        continue;
      const char32_t scalar = cp;
      const auto bytes = QString::fromUcs4(&scalar, 1).toUtf8();
      if (offset + bytes.size() >= target.size())
        break;
      std::copy(bytes.begin(), bytes.end(), target.begin() + offset);
      offset += bytes.size();
    }
  };
  copy_text(text, value.text);
  if (value.cancel)
    copy_text(tr("B — Cancel"), value.cancel_text);
  if (value.leave)
    copy_text(tr("Hold L + R + D-pad Right — Leave"), value.leave_text);
  if (presentation.Publish(value, observed_revision) && core_state == Core::State::Paused &&
      supported)
    RedrawQuickPlayPresentation();
}

void MainWindow::RedrawQuickPlayPresentation(std::function<void(bool)> finished)
{
  if (finished)
    m_quickplay_redraw_callbacks.push_back(std::move(finished));
  if (m_quickplay_redraw_pending)
    return;
  m_quickplay_redraw_pending = true;
  const auto core = ProjectPlusOnline::presentation_core_generation.load();
  const auto input = ProjectPlusOnline::persistent_input_epoch.load();
  const auto revision = ProjectPlusOnline::presentation.Revision();
  const auto identity = QuickPlayIdentity();
  const auto native = m_render_widget->effectiveWinId();
  const auto widget = QPointer<RenderWidget>(m_render_widget);
  const auto window = QPointer<MainWindow>(this);
  Core::RunOnCPUThread(m_system, [window, widget, native, core, input, revision, identity] {
    const bool ok = ProjectPlusOnline::presentation_core_generation.load() == core &&
                    ProjectPlusOnline::persistent_input_epoch.load() == input &&
                    ProjectPlusOnline::presentation.Revision() == revision && g_presenter &&
                    g_presenter->RedrawRetainedFrame();
    QueueOnObject(
        QApplication::instance(), [window, widget, native, core, input, revision, identity, ok] {
          if (!window)
            return;
          window->m_quickplay_redraw_pending = false;
          auto callbacks = std::move(window->m_quickplay_redraw_callbacks);
          window->m_quickplay_redraw_callbacks.clear();
          const bool current = ok && widget && window->m_render_widget == widget &&
                               widget->effectiveWinId() == native &&
                               ProjectPlusOnline::presentation_core_generation.load() == core &&
                               ProjectPlusOnline::persistent_input_epoch.load() == input &&
                               ProjectPlusOnline::presentation.Revision() == revision &&
                               window->QuickPlayIdentity() == identity;
          // Coalesce changed presentation while keeping every caller's own request fences.
          if (!current && widget && window->m_render_widget == widget &&
              widget->effectiveWinId() == native && window->QuickPlayIdentity() == identity &&
              ProjectPlusOnline::presentation.Revision() != revision)
          {
            window->m_quickplay_redraw_callbacks = std::move(callbacks);
            window->RedrawQuickPlayPresentation();
            return;
          }
          for (auto& callback : callbacks)
            callback(current);
        });
  });
}

void MainWindow::ValidateQuickPlayContent(u64 attempt, std::function<void(bool, QString)> finished)
{
  if (m_system.IsDualCoreMode() || !IsQuickPlayWindowSystem() ||
      m_rendering_to_main || !m_render_widget->isVisible())
  {
    finished(false, tr("Quick Play requires single-core mode and a separate render window."));
    return;
  }
  const auto previous = Core::GetState(m_system);
  const auto request = ++m_quickplay_validation_request;
  const auto core = ProjectPlusOnline::presentation_core_generation.load();
  const auto input = ProjectPlusOnline::persistent_input_epoch.load();
  const auto room = m_quickplay_room_generation;
  const auto widget = QPointer<RenderWidget>(m_render_widget);
  const auto native = widget->effectiveWinId();
  const ProjectPlusOnline::PrepaintFence fence{{core, input, attempt, room, m_persistent_game},
                                               request,
                                               static_cast<std::uintptr_t>(native)};
  m_quickplay_validation_was_running = previous == Core::State::Running;
  m_quickplay_validation_fence = fence;
  m_quickplay_validating = true;
  const bool trial = QuickPlayContent::SelectedPolicy() ==
                     QuickPlayContent::Policy::DeclaredBundleTrialV1;
  if (!trial)
    PublishQuickPlayStatus();
  Core::SetState(m_system, Core::State::Paused);
  auto admit = [this, previous, request, fence, widget, attempt,
                               finished = std::move(finished)](bool painted) {
    if (request != m_quickplay_validation_request)
      return;
    const ProjectPlusOnline::PrepaintFence current_fence{
        {ProjectPlusOnline::presentation_core_generation.load(),
         ProjectPlusOnline::persistent_input_epoch.load(), m_quickplay_owned_attempt,
         m_quickplay_room_generation, m_persistent_game},
        m_quickplay_validation_request,
        static_cast<std::uintptr_t>(m_render_widget->effectiveWinId())};
    const bool current = fence == current_fence && widget && widget == m_render_widget &&
                         !m_exit_requested && !m_pending_boot &&
                         Core::GetState(m_system) == Core::State::Paused &&
                         (!attempt || (m_quickplay_setup && m_quickplay_setup->attempt == attempt &&
                                       m_quickplay_controller->OwnsAttempt(attempt)));
    m_quickplay_validating = false;
    m_quickplay_validation_fence.reset();
    if (!current)
    {
      // Never resume or admit a different Core/window/input lifetime.
      PublishQuickPlayStatus();
      return;
    }
    QString error;
    auto candidate = std::move(m_persistent_candidate);
    if (!candidate)
      candidate = std::make_unique<PersistentQuickPlaySession>();
    const bool dispatch_ready = !m_stop_confirm_showing && !QApplication::activeModalWidget();
    const auto admitted_identity = candidate->Identity();
    bool valid = painted && dispatch_ready && candidate->Validate(m_system, &error);
    // The trial does not rescan content; it does refuse a declaration that
    // changed between the initial admission and this matched revalidation.
    if (valid && candidate->IsTrial() && attempt && !admitted_identity.empty() &&
        candidate->Identity() != admitted_identity)
    {
      valid = false;
      error = tr("The Quick Play bundle changed during this search. Cancel and try again.");
    }
    if (!dispatch_ready)
      error = tr("Close the current desktop dialog before starting Quick Play.");
    if (!painted)
      error = tr("Quick Play could not display the paused content check. Use desktop diagnostics.");
    if (valid)
      m_persistent_candidate = std::move(candidate);
    // A successful matched handoff retains its paused boundary; all other
    // outcomes restore exactly the state before validation.
    if (!valid || !attempt)
      Core::SetState(m_system, previous);
    finished(valid, std::move(error));
    PublishQuickPlayStatus();
  };
  // Trial admission has bounded IO only. Keep the same pause/ownership fences,
  // but do not enqueue a verification prepaint or expose Checking game files.
  // Complete on a later event-loop turn, like the strict prepaint callback.
  if (trial)
    QTimer::singleShot(0, this, [admit = std::move(admit)] { admit(true); });
  else
    RedrawQuickPlayPresentation(std::move(admit));
}

void MainWindow::StartQuickPlay()
{
  StartQuickPlayFrom(ProjectPlusOnline::EntrySource::Desktop);
}

void MainWindow::StartQuickPlayFrom(ProjectPlusOnline::EntrySource entry)
{
  if (m_quickplay_validating || m_quickplay_recovery != QuickPlayRecovery::None ||
      m_persistent_retiring || m_exit_requested || m_pending_boot)
    return;
  if (entry == ProjectPlusOnline::EntrySource::Desktop &&
      (Settings::Instance().GetNetPlayClient() || m_netplay_dialog->HasRoom()))
  {
    ShowQuickPlayDiagnostics();
    return;
  }
  if (Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return;
  if (m_quickplay_settings_dialog && m_quickplay_settings_dialog->isVisible())
    return;
  if (Core::GetState(m_system) == Core::State::Stopping || m_quickplay_stop_attempt)
    return;
  if (entry == ProjectPlusOnline::EntrySource::InGame && m_quickplay_controller &&
      m_quickplay_controller->GetState() == QuickPlayController::State::Error)
    m_quickplay_controller->Cancel();
  if (m_quickplay_controller &&
      m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
  {
    // Explicit desktop diagnostics may reveal an existing attempt without
    // replacing its provenance or starting another coordinator request.
    if (entry == ProjectPlusOnline::EntrySource::Desktop)
    {
      m_quickplay_dialog->show();
      m_quickplay_dialog->raise();
      m_quickplay_dialog->activateWindow();
    }
    return;
  }
  m_quickplay_entry = entry;
  m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
  m_quickplay_admission_error.clear();
  m_netplay_dialog->SetDiagnosticError({});
  m_quickplay_cancelled_until = {};
  m_quickplay_left_until = {};
  m_quickplay_opponent_left_until = {};
  ++m_quickplay_room_generation;
  if (Core::IsRunning(m_system) &&
      (PersistentQuickPlaySession::IsConfigured() ||
       QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::DeclaredBundleTrialV1))
  {
    ValidateQuickPlayContent(0, [this](bool valid, QString error) {
      if (!valid)
      {
        ReportQuickPlayError(error);
        PublishQuickPlayStatus();
        if (m_quickplay_entry == ProjectPlusOnline::EntrySource::Desktop)
          ModalMessageBox::critical(this, tr("Quick Play"), m_quickplay_admission_error);
        return;
      }
      BeginQuickPlaySearch();
    });
    return;
  }
  BeginQuickPlaySearch();
}

void MainWindow::BeginQuickPlaySearch()
{
  m_tool_bar->SetQuickPlayActive(true);
  if (!m_quickplay_controller)
  {
    m_quickplay_controller = new QuickPlayController(
        [this](u64 attempt) { return StartQuickPlayHost(attempt); },
        [this](u64 attempt) { CancelQuickPlayHost(attempt); },
        [this](const QString& code) {
          if (JoinQuickPlayHost(code))
            return QString{};
          return m_quickplay_admission_error.isEmpty() ?
                     tr("Could not join the Quick Play host. Cancel and try again.") :
                     m_quickplay_admission_error;
        },
        this);
    connect(m_quickplay_controller, &QuickPlayController::PrepareConnection, this,
            &MainWindow::PrepareQuickPlayConnection);
    connect(m_quickplay_controller, &QuickPlayController::PrepareHandoff, this,
            &MainWindow::PrepareQuickPlayHandoff);
    connect(m_netplay_dialog, &NetPlayDialog::HostTraversalChanged, this,
            [this](quint64 attempt, const QString& code, bool failed) {
              if (failed && !m_quickplay_admission_error.isEmpty())
                m_quickplay_controller->OnLaunchFailed(attempt, m_quickplay_admission_error);
              else
                m_quickplay_controller->OnHostTraversalChanged(attempt, code, failed);
            });
    connect(m_netplay_dialog, &NetPlayDialog::QuickPlayOpponentConnected, m_quickplay_controller,
            &QuickPlayController::OnHostOpponentConnected);
    connect(m_quickplay_controller, &QuickPlayController::StateChanged, this,
            [this, previous =
                       QuickPlayController::State::Idle](QuickPlayController::State state) mutable {
              const auto old_state = std::exchange(previous, state);
              if (state == QuickPlayController::State::Error)
              {
                ReportQuickPlayError(m_quickplay_controller->GetErrorMessage());
              }
              if (state == QuickPlayController::State::Searching)
              {
                m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
                m_quickplay_owned_attempt = m_quickplay_controller->GetGuestCancelAttempt();
              }
              if (state == QuickPlayController::State::Idle)
              {
                m_quickplay_owned_attempt = 0;
                m_quickplay_cancelled_until =
                    old_state != QuickPlayController::State::NetPlayOwned &&
                            old_state != QuickPlayController::State::Error ?
                        std::chrono::steady_clock::now() + std::chrono::seconds(2) :
                        std::chrono::steady_clock::time_point{};
                if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame &&
                    m_quickplay_dialog)
                  m_quickplay_dialog->hide();
              }
              PublishQuickPlayStatus();
              if (state == QuickPlayController::State::NetPlayConnected && m_quickplay_setup)
              {
                const auto attempt = m_quickplay_setup->attempt;
                if (m_quickplay_retain_window)
                {
                  QTimer::singleShot(0, this, [this, attempt] { CheckQuickPlayLaunch(attempt); });
                }
                else
                {
                  // Desktop Quick Play still hands a normal lobby to the user.
                  m_quickplay_host_attempt = 0;
                  m_quickplay_join_attempt = 0;
                  m_quickplay_setup.reset();
                  m_quickplay_controller->OnLaunchOwned(attempt);
                }
              }
              m_tool_bar->SetQuickPlayActive(state != QuickPlayController::State::Idle &&
                                             state != QuickPlayController::State::NetPlayOwned);
            });
    m_quickplay_dialog = new QuickPlayDialog(*m_quickplay_controller, this);
  }

  // Start remains canonical; entry provenance controls only presentation.
  m_quickplay_controller->Start();
  PublishQuickPlayStatus();
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::Desktop)
  {
    m_quickplay_dialog->show();
    m_quickplay_dialog->raise();
    m_quickplay_dialog->activateWindow();
  }
  else
  {
    m_quickplay_dialog->hide();
  }
}

void MainWindow::PrepareQuickPlayHandoff(u64 attempt)
{
  if (!m_quickplay_controller->OwnsAttempt(attempt))
    return;
  const auto state = Core::GetState(m_system);
  if (m_exit_requested || m_pending_boot || Settings::Instance().IsBatchModeEnabled() ||
      Settings::Instance().GetNetPlayClient() || Settings::Instance().GetNetPlayServer() ||
      m_netplay_dialog->HasRoom() ||
      (state != Core::State::Uninitialized && state != Core::State::Running &&
       state != Core::State::Paused))
  {
    m_quickplay_controller->OnCoreStopped(attempt, tr("The local game cannot hand off right now."));
    return;
  }
  if (state != Core::State::Uninitialized &&
      (!IsQuickPlayWindowSystem() || m_rendering_to_main ||
       !m_render_widget->isVisible()))
  {
    m_quickplay_controller->OnCoreStopped(
        attempt, tr("In-game Quick Play requires a separate render window."));
    return;
  }
  ProjectPlusOnline::presentation.Invalidate();
  ProjectPlusOnline::setup_action.Invalidate();
  ProjectPlusOnline::leave_action.Invalidate();
  m_quickplay_paused_css.reset();
  ++m_quickplay_room_generation;
  m_quickplay_setup = QuickPlaySetup{attempt,
                                     Config::Get(Config::NETPLAY_TRAVERSAL_SERVER),
                                     Config::Get(Config::NETPLAY_TRAVERSAL_PORT),
                                     Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT),
                                     Config::Get(Config::NETPLAY_LISTEN_PORT),
                                     Config::Get(Config::NETPLAY_NICKNAME),
                                     Config::Get(Config::NETPLAY_NETWORK_MODE),
                                     Config::Get(Config::NETPLAY_MINIMUM_BUFFER_SIZE),
                                     Config::Get(Config::NETPLAY_USE_UPNP)};
  if (state == Core::State::Uninitialized)
  {
    QTimer::singleShot(0, this, [this, attempt] {
      if (Core::IsUninitialized(m_system) && !m_exit_requested)
        m_quickplay_controller->OnCoreStopped(attempt);
    });
    return;
  }
  if (m_persistent_candidate || PersistentQuickPlaySession::IsConfigured() ||
      QuickPlayContent::SelectedPolicy() == QuickPlayContent::Policy::DeclaredBundleTrialV1)
  {
    ValidateQuickPlayContent(attempt, [this, attempt](bool valid, QString error) {
      if (!m_quickplay_controller->OwnsAttempt(attempt))
        return;
      if (!valid)
      {
        m_quickplay_controller->OnHandoffReady(attempt, error);
        return;
      }
      m_persistent_quickplay = std::move(m_persistent_candidate);
      m_quickplay_retain_window = true;
      m_quickplay_setup->window = m_render_widget;
      m_quickplay_setup->native_window = static_cast<u64>(m_render_widget->winId());
      ProjectPlusOnline::persistent_local_input = false;
      ++ProjectPlusOnline::persistent_input_epoch;
      const auto* session_owner = m_persistent_quickplay.get();
      const auto request = m_quickplay_validation_request;
      const auto room = m_quickplay_room_generation;
      m_persistent_quickplay->Restore(m_system, [window = QPointer<MainWindow>(this), attempt,
                                                 session_owner, request, room](bool ok) {
        if (!window || window->m_persistent_quickplay.get() != session_owner ||
            window->m_persistent_retiring || request != window->m_quickplay_validation_request ||
            room != window->m_quickplay_room_generation ||
            !window->m_quickplay_controller->OwnsAttempt(attempt))
          return;
        window->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
        INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: paused handoff attempt {} restored {}",
                     attempt, ok);
        if (ok)
          window->ObservePausedQuickPlayCSS();
        window->m_quickplay_controller->OnHandoffReady(
            attempt, ok ? QString{} : tr("The common CSS could not be restored."));
      });
    });
    return;
  }
  m_quickplay_stop_attempt = attempt;
  m_quickplay_retain_window = true;
  m_quickplay_setup->window = m_render_widget;
  m_quickplay_setup->native_window = static_cast<u64>(m_render_widget->winId());
  // Bounded local presentation stages owned by the assigned handoff. The guest
  // renders snapshots only; no ACK, guest timer or renderer can hold up shutdown.
  m_quickplay_presentation = QuickPlayPresentation::Matched;
  PublishQuickPlayStatus();
  QTimer::singleShot(500, this, [this, attempt] { AdvanceQuickPlayPresentation(attempt); });
}

void MainWindow::AdvanceQuickPlayPresentation(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_stop_attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt) ||
      m_quickplay_controller->GetState() != QuickPlayController::State::WaitingForHandoff)
    return;
  const auto state = Core::GetState(m_system);
  if (m_exit_requested || m_pending_boot ||
      (state != Core::State::Running && state != Core::State::Paused))
    return;
  if (m_quickplay_presentation == QuickPlayPresentation::Matched)
  {
    m_quickplay_presentation = QuickPlayPresentation::Connecting;
    PublishQuickPlayStatus();
    QTimer::singleShot(500, this, [this, attempt] { AdvanceQuickPlayPresentation(attempt); });
    return;
  }
  if (m_quickplay_presentation != QuickPlayPresentation::Connecting)
    return;
  m_quickplay_presentation = QuickPlayPresentation::None;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: stopping local Core, attempt {}", attempt);
  Core::Stop(m_system);
}

bool MainWindow::IsQuickPlayLaunchReady(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt) ||
      m_quickplay_controller->GetState() != QuickPlayController::State::NetPlayConnected ||
      m_exit_requested || m_pending_boot ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      m_rendering_to_main || Config::Get(Config::MAIN_RENDER_TO_MAIN) ||
      !m_quickplay_retain_window ||
      m_quickplay_setup->window != m_render_widget || !m_render_widget->isVisible() ||
      m_quickplay_setup->native_window != static_cast<u64>(m_render_widget->effectiveWinId()))
    return false;
  const auto server = Settings::Instance().GetNetPlayServer();
  if (m_quickplay_setup->client != Settings::Instance().GetNetPlayClient().get() ||
      m_quickplay_setup->server != server.get())
    return false;
  if (m_quickplay_host_attempt == attempt)
  {
    if (!server || server->GetQuickPlayAttempt() != attempt)
      return false;
  }
  else if (m_quickplay_join_attempt != attempt || server)
  {
    return false;
  }
  return m_netplay_dialog->IsQuickPlayReady();
}

void MainWindow::CheckQuickPlayLaunch(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt ||
      !m_quickplay_controller->OwnsAttempt(attempt))
    return;
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !client->IsConnected())
  {
    m_quickplay_controller->OnLaunchFailed(attempt, tr("The Quick Play peer disconnected."));
    return;
  }
  if (!m_quickplay_setup->start_requested && IsQuickPlayLaunchReady(attempt) &&
      !QApplication::activeModalWidget() && m_quickplay_host_attempt == attempt)
  {
    m_quickplay_setup->start_requested = true;
    INFO_LOG_FMT(NETPLAY, "QuickPlay: readiness satisfied; normal host Start Game, attempt {}",
                 attempt);
    if (!m_netplay_dialog->StartQuickPlayGame(attempt))
    {
      m_quickplay_controller->OnLaunchFailed(attempt, m_quickplay_admission_error.isEmpty() ?
                                                          tr("NetPlay could not start the game.") :
                                                          m_quickplay_admission_error);
      return;
    }
  }
  QTimer::singleShot(100, this, [this, attempt] { CheckQuickPlayLaunch(attempt); });
}

bool MainWindow::IsPersistentQuickPlayBoundary() const
{
  return m_persistent_quickplay && !m_persistent_retiring &&
         Core::GetState(m_system) == Core::State::Paused;
}

void MainWindow::PreparePersistentQuickPlay(u32 game)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_quickplay_setup || !IsPersistentQuickPlayBoundary() ||
      !IsQuickPlayLaunchReady(m_quickplay_setup->attempt))
  {
    ReportQuickPlayError(tr("The persistent session start lost its owned readiness boundary."));
    return RetirePersistentQuickPlay(true);
  }
  ProjectPlusOnline::presentation.Invalidate();
  m_persistent_game = game;
  ObservePausedQuickPlayCSS();
  PublishQuickPlayPresentation();
  const Core::CPUThreadGuard guard(m_system);
  if (!client->PreparePersistentSession(game, m_persistent_quickplay->Identity(),
                                        m_persistent_quickplay->OriginDigest(),
                                        m_persistent_quickplay->ContractText()))
    QTimer::singleShot(0, this, [this, game,
                                 mismatch = QString::fromStdString(
                                     client->PersistentConfigMismatch())] {
      if (m_persistent_game == game)
      {
        ReportQuickPlayError(
            mismatch.isEmpty() ?
                tr("The persistent session contract could not be prepared.") :
                tr("Quick Play refused the join: this setting differs from the host's: %1. "
                   "Change it back, or restart the Offline Launcher.")
                    .arg(mismatch));
        RetirePersistentQuickPlay(true);
      }
    });
}

void MainWindow::HandlePersistentBarrier(u32 game, bool release)
{
  const auto client = Settings::Instance().GetNetPlayClient();
  if (!client || !m_quickplay_setup || game != m_persistent_game ||
      !IsPersistentQuickPlayBoundary() || client.get() != m_quickplay_setup->client ||
      !client->IsConnected())
    return;
  bool ok;
  {
    const Core::CPUThreadGuard guard(m_system);
    ok = release ? client->ReleasePersistentSession(game) : client->AttachPersistentSession(game);
  }
  if (!ok)
  {
    ReportQuickPlayError(tr("The persistent session attach/release barrier failed."));
    return RetirePersistentQuickPlay(true);
  }
  if (!release)
    return;
  const auto attempt = m_quickplay_setup->attempt;
  m_quickplay_setup.reset();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_netplay_dialog->ReleaseQuickPlayStart();
  m_quickplay_controller->OnLaunchOwned(attempt);
  Core::SetState(m_system, Core::State::Running);
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: active game {} same Core", game);
}

void MainWindow::RetirePersistentQuickPlay(bool restore_css)
{
  if (m_persistent_retiring)
  {
    if (!restore_css)
      m_persistent_restore_css = false;
    return;
  }
  if (!m_persistent_quickplay)
    return;
  const auto delivered_cause = m_netplay_dialog->SessionError();
  const auto cause = delivered_cause.isEmpty() ? m_quickplay_admission_error : delivered_cause;
  InvalidateQuickPlayPresentation();
  m_quickplay_admission_error = cause;
  m_persistent_retiring = true;
  ProjectPlusOnline::persistent_setup_pause = true;
  m_persistent_restore_css = restore_css && !m_exit_requested && !m_pending_boot;
  ProjectPlusOnline::persistent_local_input = false;
  ++ProjectPlusOnline::persistent_input_epoch;
  const auto client = Settings::Instance().GetNetPlayClient();
  if (client)
  {
    client->RequestStopGame();
    // Do this BEFORE pause/CPU guard/global NetPlay lock: GetNetPads can hold
    // that lock while waiting for a remote packet on a stalled session.
    client->InvokeStop();
  }
  if (Core::IsRunning(m_system))
  {
    Core::SetState(m_system, Core::State::Paused);
    const Core::CPUThreadGuard guard(m_system);
    if (client)
      client->DetachPersistentSession();
    Config::RemoveLayer(Config::LayerType::Netplay);
    // Restore local desired-device ownership as well as the live checkpoint's
    // device instances. A late mapping/disconnect notification must not leave
    // a pending NetPlay device request behind for the next local SI poll. A
    // successful CSS restore then pins them to the checkpoint's standard devices.
    for (int port = 0; port < 4; ++port)
      m_system.GetSerialInterface().ChangeDevice(Config::Get(Config::GetInfoForSIDevice(port)), port);
    Core::UpdateWantDeterminism(m_system);
  }
  else
  {
    // Stop/Close may already have put the Core in Stopping. InvokeStop above
    // releases its final pad wait; disable the global pointer before destroying
    // the client even though its running flag is now clear. Leave its FIFOs
    // intact until CPU teardown and object destruction have finished.
    NetPlay::NetPlay_Disable();
    Config::RemoveLayer(Config::LayerType::Netplay);
  }
  m_persistent_game = 0;
  m_persistent_client = nullptr;
  m_persistent_server = nullptr;
  m_quickplay_setup.reset();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_quickplay_retain_window = false;
  m_netplay_dialog->hide();
  m_netplay_dialog->ResetSession();
  m_quickplay_retiring_client = client;
  m_quickplay_retiring_server = Settings::Instance().GetNetPlayServer();
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
  INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: retiring session; Core retained restore {}",
               m_persistent_restore_css);
  QTimer::singleShot(0, this, &MainWindow::FinishPersistentQuickPlayRetirement);
}

void MainWindow::FinishPersistentQuickPlayRetirement()
{
  if (!m_persistent_retiring)
    return;
  if (!m_quickplay_retiring_client.expired() || !m_quickplay_retiring_server.expired())
  {
    QTimer::singleShot(25, this, &MainWindow::FinishPersistentQuickPlayRetirement);
    return;
  }
  // Fence notifications queued by the last network-thread destructor too.
  m_netplay_dialog->ResetSession();
  const bool restore = m_persistent_restore_css && Core::IsRunning(m_system) && !m_exit_requested;
  const auto finish = [window = QPointer<MainWindow>(this), restore](bool ok) {
    if (!window || !window->m_persistent_retiring)
      return;
    const bool resume = restore && ok && window->m_persistent_restore_css &&
                        Core::IsRunning(window->m_system) && !window->m_exit_requested;
    window->m_persistent_quickplay.reset();
    window->m_persistent_retiring = false;
    window->m_persistent_restore_css = false;
    if (window->m_quickplay_controller)
      window->m_quickplay_controller->Cancel();
    if (resume && window->m_quickplay_cancel_consumed)
      ProjectPlusOnline::cancel_release_handoff.Arm(
          ProjectPlusOnline::presentation_core_generation.load(),
          ProjectPlusOnline::persistent_input_epoch.load());
    window->m_quickplay_cancel_consumed = false;
    if (std::exchange(window->m_quickplay_opponent_left, false))
      window->m_quickplay_opponent_left_until =
          std::chrono::steady_clock::now() + std::chrono::seconds(4);
    if (resume)
    {
      {
        const Core::CPUThreadGuard guard(window->m_system);
        QuickPlayRecipe::ReleaseExtraPorts(window->m_system);
      }
      Core::SetState(window->m_system, Core::State::Running);
    }
    window->m_quickplay_owner_core = ProjectPlusOnline::presentation_core_generation.load();
    if (!ok)
      window->ReportQuickPlayError(window->m_quickplay_admission_error +
                                   tr(" The local CSS could not be restored."));
    if (!ok)
      ERROR_LOG_FMT(NETPLAY, "Persistent QuickPlay: local CSS restore failed; keeping Core paused");
    INFO_LOG_FMT(NETPLAY, "Persistent QuickPlay: session retired local CSS {}", resume);
    window->PublishQuickPlayStatus();
  };
  if (!restore)
    return finish(true);
  // Leave returns to this run's hand-free local CSS; the origin (with its port 2 controller,
  // covered until the CSS drops that hand) only if the local snapshot is missing.
  PersistentQuickPlaySession::RestoreLocal(
      m_system, [window = QPointer<MainWindow>(this), finish](bool ok) {
        if (ok || !window || !window->m_persistent_quickplay || !window->m_persistent_retiring)
          return finish(ok);
        window->m_persistent_quickplay->Restore(window->m_system, finish);
      });
}

void MainWindow::TakeLocalQuickPlaySnapshot()
{
  // Only from the idle local CSS: never during a search, handoff, session or retirement.
  if (!Core::IsRunning(m_system) || Core::GetState(m_system) != Core::State::Running ||
      m_quickplay_setup || m_persistent_quickplay || m_persistent_retiring || m_quickplay_session ||
      m_quickplay_validating || Settings::Instance().GetNetPlayClient() ||
      Settings::Instance().GetNetPlayServer())
  {
    INFO_LOG_FMT(NETPLAY, "Quick Play recipe: local CSS snapshot skipped (not idle)");
    QuickPlayRecipe::FinishLocalSnapshot();
    return;
  }
  Core::SetState(m_system, Core::State::Paused);
  Core::RunOnCPUThread(m_system, [this] {
    QuickPlayRecipe::SaveLocalSnapshot(m_system);
    QueueOnObject(this, [this] {
      QuickPlayRecipe::FinishLocalSnapshot();
      if (Core::GetState(m_system) == Core::State::Paused && !m_quickplay_setup &&
          !m_persistent_quickplay && !m_persistent_retiring)
        Core::SetState(m_system, Core::State::Running);
    });
  });
}

void MainWindow::ReleaseQuickPlayWindow()
{
  m_quickplay_presentation = QuickPlayPresentation::None;
  m_quickplay_stop_attempt = 0;
  if (std::exchange(m_quickplay_retain_window, false) && Core::IsUninitialized(m_system))
  {
    HideRenderWidget();
    SetFullScreenResolution(false);
  }
  // During Stopping the normal OnStopComplete performs the same cleanup.
}

void MainWindow::EndQuickPlaySession(bool restart)
{
  if (m_quickplay_recovery != QuickPlayRecovery::None)
  {
    if (!restart)
      m_quickplay_recovery_restart = false;
    return;
  }
  if (!std::exchange(m_quickplay_session, false))
    return;

  m_quickplay_recovery = QuickPlayRecovery::WaitingForStop;
  m_quickplay_recovery_restart = restart && !m_exit_requested && !m_pending_boot &&
                                 m_render_widget && m_render_widget->isVisible();
  m_quickplay_retain_window = m_quickplay_recovery_restart;
  if (!restart)
  {
    if (const auto client = Settings::Instance().GetNetPlayClient())
      client->RequestStopGame();
  }
  INFO_LOG_FMT(NETPLAY, "QuickPlay: ending session, local restart {}",
               m_quickplay_recovery_restart);
}

void MainWindow::FinishQuickPlayRecovery()
{
  if ((m_quickplay_recovery != QuickPlayRecovery::Queued &&
       m_quickplay_recovery != QuickPlayRecovery::Releasing) ||
      !Core::IsUninitialized(m_system))
    return;

  if (m_quickplay_recovery == QuickPlayRecovery::Queued)
  {
    // Core and boot configuration cleanup has finished. Release presentation
    // ownership too; the golf overlay can hold a client reference.
    m_quickplay_recovery = QuickPlayRecovery::Releasing;
    m_quickplay_retiring_client = Settings::Instance().GetNetPlayClient();
    m_quickplay_retiring_server = Settings::Instance().GetNetPlayServer();
    m_netplay_dialog->hide();
    m_netplay_dialog->ResetSession();
    Settings::Instance().ResetNetPlayClient();
    Settings::Instance().ResetNetPlayServer();
  }
  // A nested modal callback can still hold a session reference. Never boot a new
  // Core until those references unwind and both network-thread destructors finish.
  if (!m_quickplay_retiring_client.expired() || !m_quickplay_retiring_server.expired() ||
      QApplication::activeModalWidget())
  {
    QTimer::singleShot(25, this, &MainWindow::FinishQuickPlayRecovery);
    return;
  }
  // Invalidate even notifications produced while the last reference was retiring.
  m_netplay_dialog->ResetSession();
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;

  if (!m_quickplay_recovery_restart || m_exit_requested || m_pending_boot ||
      !m_quickplay_retain_window || !m_render_widget->isVisible())
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_retain_window = false;
    m_quickplay_controller->Cancel();
    OnStopComplete();
    return;
  }

  const auto launcher = FindPPlusNetplayLauncher(m_game_list->GetGameListModel());
  if (!launcher.game)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
    ReleaseQuickPlayWindow();
    ModalMessageBox::critical(this, tr("Quick Play"), launcher.error);
    return;
  }

  m_quickplay_recovery = QuickPlayRecovery::Booting;
  INFO_LOG_FMT(NETPLAY, "QuickPlay: booting local recovery");
  StartGame(launcher.game->GetFilePath(), ScanForSecondDisc::Yes);
  if (Core::IsUninitialized(m_system) && m_quickplay_recovery == QuickPlayRecovery::Booting)
  {
    m_quickplay_recovery = QuickPlayRecovery::None;
    m_quickplay_controller->Cancel();
  }
}

void MainWindow::ShowNetPlaySetupDialog()
{
  if (Settings::Instance().GetNetPlayClient() || m_netplay_dialog->HasRoom())
  {
    ShowQuickPlayDiagnostics();
    return;
  }
  m_netplay_setup_dialog->show();
  m_netplay_setup_dialog->raise();
  m_netplay_setup_dialog->activateWindow();
}

void MainWindow::ShowNetPlayBrowser()
{
  auto* browser = new NetPlayBrowser(this);
  browser->setAttribute(Qt::WA_DeleteOnClose, true);
  connect(browser, &NetPlayBrowser::Join, this, &MainWindow::NetPlayJoin);
  browser->exec();
}

void MainWindow::ShowFIFOPlayer()
{
  if (!m_fifo_window)
  {
    m_fifo_window.reset(new FIFOPlayerWindow(m_system.GetFifoPlayer(), m_system.GetFifoRecorder()));
    connect(m_fifo_window.get(), &FIFOPlayerWindow::LoadFIFORequested, this,
            [this](const QString& path) { StartGame(path, ScanForSecondDisc::No); });
  }

  m_fifo_window->show();
  m_fifo_window->raise();
  m_fifo_window->activateWindow();
}

void MainWindow::ShowSkylanderPortal()
{
  if (!m_skylander_window)
  {
    m_skylander_window = new SkylanderPortalWindow();
  }

  m_skylander_window->show();
  m_skylander_window->raise();
  m_skylander_window->activateWindow();
}

void MainWindow::ShowInfinityBase()
{
  if (!m_infinity_window)
  {
    m_infinity_window = new InfinityBaseWindow();
  }

  m_infinity_window->show();
  m_infinity_window->raise();
  m_infinity_window->activateWindow();
}

void MainWindow::ShowWiiSpeakWindow()
{
  if (!m_wii_speak_window)
  {
    m_wii_speak_window = new WiiSpeakWindow();
  }

  m_wii_speak_window->show();
  m_wii_speak_window->raise();
  m_wii_speak_window->activateWindow();
}

void MainWindow::ShowLogitechMicWindow()
{
  if (!m_logitech_mic_window)
  {
    m_logitech_mic_window = new LogitechMicWindow();
  }

  m_logitech_mic_window->show();
  m_logitech_mic_window->raise();
  m_logitech_mic_window->activateWindow();
}

void MainWindow::StateLoad()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::LoadAs(m_system, path.toStdString());
}

void MainWindow::StateSave()
{
  QString dialog_path = (Config::Get(Config::MAIN_CURRENT_STATE_PATH).empty()) ?
                            QDir::currentPath() :
                            QString::fromStdString(Config::Get(Config::MAIN_CURRENT_STATE_PATH));
  QString path = DolphinFileDialog::getSaveFileName(
      this, tr("Select a File"), dialog_path, tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(path).dir().path().toStdString());
  if (!path.isEmpty())
    State::SaveAs(m_system, path.toStdString());
}

void MainWindow::StateLoadSlot()
{
  State::Load(m_system, m_state_slot);
}

void MainWindow::StateSaveSlot()
{
  State::Save(m_system, m_state_slot);
}

void MainWindow::StateLoadSlotAt(int slot)
{
  State::Load(m_system, slot);
}

void MainWindow::StateLoadLastSavedAt(int slot)
{
  State::LoadLastSaved(m_system, slot);
}

void MainWindow::StateSaveSlotAt(int slot)
{
  State::Save(m_system, slot);
}

void MainWindow::StateLoadUndo()
{
  State::UndoLoadState(m_system);
}

void MainWindow::StateSaveUndo()
{
  State::UndoSaveState(m_system);
}

void MainWindow::StateSaveOldest()
{
  State::SaveFirstSaved(m_system);
}

void MainWindow::SetStateSlot(int slot)
{
  Settings::Instance().SetStateSlot(slot);
  m_state_slot = slot;

  Core::DisplayMessage(fmt::format("Selected slot {} - {}", m_state_slot,
                                   State::GetInfoStringOfSlot(m_state_slot, false)),
                       2500);
}

void MainWindow::IncrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot + 1;
  if (state_slot > State::NUM_STATES)
    state_slot = 1;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::DecrementSelectedStateSlot()
{
  u32 state_slot = m_state_slot - 1;
  if (state_slot < 1)
    state_slot = State::NUM_STATES;
  m_menu_bar->SetStateSlot(state_slot);
}

void MainWindow::PerformOnlineUpdate(const std::string& region)
{
  WiiUpdate::PerformOnlineUpdate(region, this);
  // Since the update may have installed a newer system menu, trigger a refresh.
  Settings::Instance().NANDRefresh();
}

void MainWindow::BootWiiSystemMenu()
{
  StartGame(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}));
}

void MainWindow::NetPlayInit()
{
  const auto& game_list_model = m_game_list->GetGameListModel();
  m_netplay_setup_dialog = new NetPlaySetupDialog(game_list_model, this);
  m_netplay_dialog = new NetPlayDialog(
      game_list_model,
      [this](const std::string& path, std::unique_ptr<BootSessionData> boot_session_data) {
        if (m_quickplay_setup)
        {
          const auto attempt = m_quickplay_setup->attempt;
          if (!NetPlay::IsNetPlayRunning() || !IsQuickPlayLaunchReady(attempt))
          {
            QTimer::singleShot(0, this, [this, attempt] {
              m_quickplay_controller->OnLaunchFailed(attempt,
                                                     tr("Quick Play launch was invalidated."));
            });
            return;
          }
          INFO_LOG_FMT(NETPLAY, "QuickPlay: NetPlay owns boot, attempt {}", attempt);
          const auto* retained_window = m_render_widget;
          const auto native_window = m_quickplay_setup->native_window;
          m_quickplay_host_attempt = 0;
          m_quickplay_join_attempt = 0;
          m_quickplay_setup.reset();
          m_netplay_dialog->ReleaseQuickPlayStart();
          m_quickplay_controller->OnLaunchOwned(attempt);
          // Ownership observers may close the window before the fresh boot is entered.
          if (!m_quickplay_retain_window || m_exit_requested ||
              m_render_widget != retained_window || !m_render_widget->isVisible() ||
              static_cast<u64>(m_render_widget->effectiveWinId()) != native_window)
          {
            if (const auto client = Settings::Instance().GetNetPlayClient())
              client->RequestStopGame();
            return;
          }
          m_quickplay_session = true;
        }
        const auto client = Settings::Instance().GetNetPlayClient();
        StartGame(path, ScanForSecondDisc::Yes, std::move(boot_session_data));
        if (Core::IsUninitialized(m_system) && client &&
            client == Settings::Instance().GetNetPlayClient())
          client->RequestStopGame();
      });
#ifdef USE_DISCORD_PRESENCE
  m_netplay_discord = new DiscordHandler(this);
#endif

  connect(m_netplay_dialog, &NetPlayDialog::InGameError, this, [this](const QString& error) {
    ReportQuickPlayError(error);
    const auto identity = QuickPlayIdentity();
    if (m_quickplay_blocking_connection)
      return;  // Synchronous caller returns the retained cause after construction.
    QTimer::singleShot(0, this, [this, identity, error] {
      if (identity != QuickPlayIdentity() || !OwnsInGameQuickPlay())
        return;
      if (m_quickplay_controller && m_quickplay_controller->OwnsAttempt(identity.attempt))
        m_quickplay_controller->OnLaunchFailed(identity.attempt, error);
      else if (m_persistent_quickplay)
        RetirePersistentQuickPlay(true);
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::QuickPlayStartAborted, this, [this](quint64 attempt) {
    // Defer destruction until the normal NetPlay callback has returned.
    QTimer::singleShot(0, this, [this, attempt] {
      if (m_quickplay_controller)
        m_quickplay_controller->OnLaunchFailed(attempt, tr("NetPlay game start was aborted."));
    });
  });
  connect(m_netplay_dialog, &NetPlayDialog::PersistentStart, this,
          &MainWindow::PreparePersistentQuickPlay);
  connect(m_netplay_dialog, &NetPlayDialog::PersistentBarrier, this,
          &MainWindow::HandlePersistentBarrier);
  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, [this] {
    if (m_persistent_quickplay || m_persistent_retiring)
    {
      // A peer stop with no captured failure: the opponent left (local leave and
      // Stop retire first, so their own stop echo arrives while retiring).
      if (m_persistent_quickplay && !m_persistent_retiring &&
          m_netplay_dialog->SessionError().isEmpty())
        m_quickplay_opponent_left = true;
      RetirePersistentQuickPlay(true);
      return;
    }
    EndQuickPlaySession(true);
    ForceStop();
    if (m_quickplay_recovery == QuickPlayRecovery::WaitingForStop &&
        Core::IsUninitialized(m_system))
      OnStopComplete();
  });
  connect(m_netplay_dialog, &NetPlayDialog::rejected, this, &MainWindow::NetPlayQuit);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Join, this, &MainWindow::NetPlayJoin);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Host, this, &MainWindow::NetPlayHost);
#ifdef USE_DISCORD_PRESENCE
  connect(m_netplay_discord, &DiscordHandler::Join, this, &MainWindow::NetPlayJoin);

  Discord::InitNetPlayFunctionality(*m_netplay_discord);
  m_netplay_discord->Start();
#endif
  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
}

bool MainWindow::NetPlayJoin()
{
  if (m_quickplay_setup || m_quickplay_host_attempt || m_quickplay_joining ||
      m_persistent_quickplay || m_persistent_retiring || m_quickplay_session ||
      m_quickplay_validating || m_quickplay_recovery != QuickPlayRecovery::None ||
      (m_quickplay_controller &&
       m_quickplay_controller->GetState() != QuickPlayController::State::Idle &&
       m_quickplay_controller->GetState() != QuickPlayController::State::Error))
    return false;
  return NetPlayJoinInternal(false);
}

// The trial declaration names the game list's own launcher entry, so the
// network-thread lookup compares the exact path string it will select.
static std::optional<QuickPlayContent::TrialLauncher>
QuickPlayTrialLauncher(const PersistentQuickPlaySession* session, const GameListModel& model)
{
  if (!session || !session->TrialLauncher())
    return std::nullopt;
  auto trial = *session->TrialLauncher();
  if (const auto launcher = FindPPlusNetplayLauncher(model).game)
    trial.path = launcher->GetFilePath();
  return trial;
}

bool MainWindow::NetPlayJoinInternal(bool force_traversal, const NetPlay::NetPlayServer* self_join)
{
  if (!Core::IsUninitialized(m_system) &&
      !(IsPersistentQuickPlayBoundary() && (m_quickplay_joining || m_quickplay_host_attempt)))
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  const auto existing_server = Settings::Instance().GetNetPlayServer();
  const bool local_join = self_join && existing_server.get() == self_join &&
                          (!m_quickplay_setup ||
                           (m_quickplay_controller->OwnsAttempt(m_quickplay_setup->attempt) &&
                            existing_server->GetQuickPlayAttempt() == m_quickplay_setup->attempt));
  if (!ProjectPlusOnline::CanJoinRoom(Settings::Instance().GetNetPlayClient() != nullptr,
                                      existing_server != nullptr, m_netplay_dialog->HasRoom(),
                                      local_join))
  {
    m_netplay_dialog->ReportError(tr("A NetPlay Session is already in progress!"));
    return false;
  }

  const auto setup =
      (m_quickplay_joining || m_quickplay_host_attempt) ? m_quickplay_setup : std::nullopt;
  auto server = Settings::Instance().GetNetPlayServer();

  // Settings
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = force_traversal || traversal_choice == "traversal";

  std::string host_ip;
  u16 host_port;
  if (server)
  {
    host_ip = "127.0.0.1";
    host_port = server->GetPort();
  }
  else
  {
    host_ip = is_traversal ? Config::Get(Config::NETPLAY_HOST_CODE) :
                             Config::Get(Config::NETPLAY_ADDRESS);
    host_port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  const std::string traversal_host =
      (setup ? setup->traversal_host : Config::Get(Config::NETPLAY_TRAVERSAL_SERVER));
  const u16 traversal_port =
      (setup ? setup->traversal_port : Config::Get(Config::NETPLAY_TRAVERSAL_PORT));
  const std::string nickname = (setup ? setup->nickname : Config::Get(Config::NETPLAY_NICKNAME));
  const std::string network_mode =
      (setup ? setup->network_mode : Config::Get(Config::NETPLAY_NETWORK_MODE));
  const bool host_input_authority = network_mode == "hostinputauthority" || network_mode == "golf";

  if (server)
  {
    server->SetHostInputAuthority(host_input_authority);
    server->AdjustMinimumPadBufferSize(
        (setup ? setup->minimum_buffer : Config::Get(Config::NETPLAY_MINIMUM_BUFFER_SIZE)));
  }

  if (!local_join)
    m_netplay_dialog->BeginSession(
        m_quickplay_setup ? m_quickplay_entry : ProjectPlusOnline::EntrySource::Desktop,
        m_persistent_quickplay != nullptr, m_quickplay_setup ? m_quickplay_setup->attempt : 0,
        QuickPlayTrialLauncher(m_persistent_quickplay.get(), m_game_list->GetGameListModel()));
  if (m_quickplay_setup && m_quickplay_retain_window)
  {
    const auto attempt = m_quickplay_setup->attempt;
    m_netplay_dialog->ArmQuickPlayStart(
        attempt, [this, attempt] { return IsQuickPlayLaunchReady(attempt); });
  }
  // Create Client
  const bool is_hosting_netplay = server != nullptr;
  Settings::Instance().ResetNetPlayClient(new NetPlay::NetPlayClient(
      host_ip, host_port, m_netplay_dialog, nickname,
      NetPlay::NetTraversalConfig{is_hosting_netplay ? false : is_traversal, traversal_host,
                                  traversal_port},
      m_persistent_quickplay != nullptr));

  if (m_quickplay_setup)
  {
    m_quickplay_setup->client = Settings::Instance().GetNetPlayClient().get();
    m_quickplay_setup->server = Settings::Instance().GetNetPlayServer().get();
    if (m_persistent_quickplay)
    {
      m_persistent_client = m_quickplay_setup->client;
      m_persistent_server = m_quickplay_setup->server;
    }
  }

  if (!Settings::Instance().GetNetPlayClient()->IsConnected())
  {
    NetPlayQuit();
    return false;
  }

  m_netplay_setup_dialog->close();
  m_netplay_dialog->InitializeRoom(nickname, is_traversal);
  m_netplay_dialog->ShowAutomatic();

  return true;
}

bool MainWindow::JoinQuickPlayHost(const QString& host_code)
{
  if (!m_quickplay_setup || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(m_quickplay_setup->attempt) ||
      m_quickplay_setup->attempt != m_quickplay_owned_attempt || m_quickplay_joining ||
      m_quickplay_host_attempt || !QuickPlayClient::IsValidHostCode(host_code) ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return false;

  if (!FindPPlusNetplayLauncher(m_game_list->GetGameListModel()).game)
    return false;
  m_quickplay_joining = true;
  m_quickplay_join_attempt = m_quickplay_setup ? m_quickplay_setup->attempt : 0;
  const auto previous_method = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const auto previous_code = Config::Get(Config::NETPLAY_HOST_CODE);
  Common::ScopeGuard restore([&] {
    Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, previous_method);
    Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, previous_code);
    m_quickplay_joining = false;
  });
  // Same effective settings as a traversal browser session. The connect port is unused for
  // traversal, and the configured traversal server/port stay in effect. Restore manual values.
  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, std::string("traversal"));
  Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, host_code.toStdString());
  return NetPlayJoinInternal(false);
}

QString MainWindow::StartQuickPlayHost(u64 attempt)
{
  if (!m_quickplay_setup || m_quickplay_setup->attempt != attempt || !m_quickplay_controller ||
      !m_quickplay_controller->OwnsAttempt(attempt) || m_quickplay_owned_attempt != attempt ||
      (!Core::IsUninitialized(m_system) && !IsPersistentQuickPlayBoundary()) ||
      Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom())
    return tr("Close the current game or NetPlay session before hosting Quick Play.");
  const auto launcher = FindPPlusNetplayLauncher(m_game_list->GetGameListModel());
  if (!launcher.game)
    return launcher.error;
  m_quickplay_host_attempt = attempt;
  if (!NetPlayHostInternal(*launcher.game, attempt))
    return m_quickplay_admission_error.isEmpty() ?
               tr("Could not create the Quick Play NetPlay host. Cancel and try again.") :
               m_quickplay_admission_error;
  return {};
}

void MainWindow::CancelQuickPlayHost(u64 attempt)
{
  if (!attempt)
    return;
  if (m_quickplay_validating && m_quickplay_setup && m_quickplay_setup->attempt == attempt)
    CancelQuickPlayValidation();
  if (m_persistent_quickplay && m_quickplay_setup && m_quickplay_setup->attempt == attempt)
  {
    RetirePersistentQuickPlay(true);
    return;
  }
  const bool owns_session =
      m_quickplay_host_attempt == attempt || m_quickplay_join_attempt == attempt;
  if (m_quickplay_setup && m_quickplay_setup->attempt == attempt)
  {
    m_quickplay_setup.reset();
    ReleaseQuickPlayWindow();
  }
  if (!owns_session)
    return;
  m_quickplay_host_attempt = 0;
  m_quickplay_join_attempt = 0;
  m_netplay_dialog->hide();
  NetPlayQuit();
}

bool MainWindow::NetPlayHost(const UICommon::GameFile& game)
{
  if (m_quickplay_setup || m_quickplay_host_attempt || m_quickplay_joining ||
      m_persistent_quickplay || m_persistent_retiring || m_quickplay_session ||
      m_quickplay_validating || m_quickplay_recovery != QuickPlayRecovery::None ||
      (m_quickplay_controller &&
       m_quickplay_controller->GetState() != QuickPlayController::State::Idle &&
       m_quickplay_controller->GetState() != QuickPlayController::State::Error))
    return false;
  return NetPlayHostInternal(game, 0);
}

bool MainWindow::NetPlayHostInternal(const UICommon::GameFile& game, u64 quickplay_attempt)
{
  if (!Core::IsUninitialized(m_system) &&
      !(quickplay_attempt && m_quickplay_setup && m_quickplay_setup->attempt == quickplay_attempt &&
        IsPersistentQuickPlayBoundary()))
  {
    ModalMessageBox::critical(nullptr, tr("Error"),
                              tr("Can't start a NetPlay Session while a game is still running!"));
    return false;
  }

  if (Settings::Instance().GetNetPlayServer() || Settings::Instance().GetNetPlayClient() ||
      m_netplay_dialog->HasRoom() || m_persistent_retiring ||
      m_quickplay_recovery != QuickPlayRecovery::None)
  {
    m_netplay_dialog->ReportError(tr("A NetPlay Session is already in progress!"));
    return false;
  }

  const auto setup = quickplay_attempt ? m_quickplay_setup : std::nullopt;
  // Settings
  u16 host_port = Config::Get(Config::NETPLAY_HOST_PORT);
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const bool is_traversal = quickplay_attempt != 0 || traversal_choice == "traversal";
  const bool use_upnp = (setup ? setup->use_upnp : Config::Get(Config::NETPLAY_USE_UPNP));

  const std::string traversal_host =
      (setup ? setup->traversal_host : Config::Get(Config::NETPLAY_TRAVERSAL_SERVER));
  const u16 traversal_port =
      (setup ? setup->traversal_port : Config::Get(Config::NETPLAY_TRAVERSAL_PORT));
  const u16 traversal_port_alt =
      (setup ? setup->traversal_port_alt : Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT));

  if (is_traversal)
    host_port = (setup ? setup->listen_port : Config::Get(Config::NETPLAY_LISTEN_PORT));

  // Establish presentation before the server or local client can report synchronous errors.
  m_netplay_dialog->BeginSession(quickplay_attempt ? m_quickplay_entry :
                                                     ProjectPlusOnline::EntrySource::Desktop,
                                 m_persistent_quickplay != nullptr, quickplay_attempt,
                                 QuickPlayTrialLauncher(m_persistent_quickplay.get(),
                                                        m_game_list->GetGameListModel()));
  // Create Server
  Settings::Instance().ResetNetPlayServer(new NetPlay::NetPlayServer(
      host_port, use_upnp, m_netplay_dialog,
      NetPlay::NetTraversalConfig{is_traversal, traversal_host, traversal_port, traversal_port_alt},
      quickplay_attempt));

  if (!Settings::Instance().GetNetPlayServer()->is_connected)
  {
    m_netplay_dialog->ReportError(
        tr("Failed to listen on port %1. Is another instance of the NetPlay server running?")
            .arg(host_port));
    NetPlayQuit();
    return false;
  }

  Settings::Instance().GetNetPlayServer()->SetPersistentSession(m_persistent_quickplay != nullptr);
  Settings::Instance().GetNetPlayServer()->ChangeGame(
      m_persistent_quickplay && m_persistent_quickplay->TrialLauncher() ?
          m_persistent_quickplay->TrialLauncher()->identity : game.GetSyncIdentifier(),
                                                      m_game_list->GetNetPlayName(game));

  // Join our local server
  return NetPlayJoinInternal(quickplay_attempt != 0, Settings::Instance().GetNetPlayServer().get());
}

void MainWindow::NetPlayQuit()
{
  if (m_quickplay_entry == ProjectPlusOnline::EntrySource::InGame)
  {
    const auto cause = m_netplay_dialog->SessionError();
    if (!cause.isEmpty())
      ReportQuickPlayError(cause);
  }
  if (m_persistent_quickplay || m_persistent_retiring)
  {
    RetirePersistentQuickPlay(true);
    return;
  }
  InvalidateQuickPlayPresentation();
  if (m_quickplay_session || m_quickplay_recovery != QuickPlayRecovery::None)
  {
    EndQuickPlaySession(false);
    ForceStop();
    if (Core::IsUninitialized(m_system))
      OnStopComplete();
    return;
  }
  const auto attempt = m_quickplay_setup ? m_quickplay_setup->attempt : 0;
  ReleaseQuickPlayWindow();
  m_quickplay_setup.reset();
  m_quickplay_join_attempt = 0;
  m_quickplay_host_attempt = 0;
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
  m_netplay_dialog->ResetSession();
  if (m_quickplay_controller)
  {
    if (attempt)
      m_quickplay_controller->OnHostClosed(attempt);
    else if (m_quickplay_controller->GetState() != QuickPlayController::State::Idle)
      m_quickplay_controller->Cancel();
  }
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void MainWindow::UpdateScreenSaverInhibition()
{
  const bool inhibit = Config::Get(Config::MAIN_DISABLE_SCREENSAVER) &&
                       (Core::GetState(m_system) == Core::State::Running);

  if (inhibit == m_is_screensaver_inhibited)
    return;

  m_is_screensaver_inhibited = inhibit;

  UICommon::InhibitScreenSaver(inhibit);
}

bool MainWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Close)
  {
    if (RequestStop() && object == this)
      m_exit_requested = true;

    static_cast<QCloseEvent*>(event)->ignore();
    return true;
  }

  return false;
}

QMenu* MainWindow::createPopupMenu()
{
  // Disable the default popup menu as it exposes the debugger UI even when the debugger UI is
  // disabled, which can lead to user confusion (see e.g. https://bugs.dolphin-emu.org/issues/13306)
  return nullptr;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QList<QUrl>& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  QStringList files;
  QStringList folders;

  for (const QUrl& url : urls)
  {
    QFileInfo file_info(url.toLocalFile());
    QString path = file_info.filePath();

    if (!file_info.exists() || !file_info.isReadable())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
      return;
    }

    (file_info.isFile() ? files : folders).append(path);
  }

  if (!files.isEmpty())
  {
    StartGame(StringListToStdVector(files));
  }
  else
  {
    Settings& settings = Settings::Instance();
    const bool show_confirm = !settings.GetPaths().empty();

    for (const QString& folder : folders)
    {
      if (show_confirm)
      {
        if (ModalMessageBox::question(
                this, tr("Confirm"),
                tr("Do you want to add \"%1\" to the list of Game Paths?").arg(folder)) !=
            QMessageBox::Yes)
          return;
      }
      settings.AddPath(folder);
    }
  }
}

QSize MainWindow::sizeHint() const
{
  return QSize(800, 600);
}

void MainWindow::OnBootGameCubeIPL(DiscIO::Region region)
{
  StartGame(std::make_unique<BootParameters>(BootParameters::IPL{region}));
}

void MainWindow::OnImportNANDBackup()
{
  auto response = ModalMessageBox::question(
      this, tr("Question"),
      tr("Merging a new NAND over your currently selected NAND will overwrite any channels "
         "and savegames that already exist. This process is not reversible, so it is "
         "recommended that you keep backups of both NANDs. Are you sure you want to "
         "continue?"));

  if (response == QMessageBox::No)
    return;

  QString file =
      DolphinFileDialog::getOpenFileName(this, tr("Select NAND Backup"), QDir::currentPath(),
                                         tr("BootMii NAND backup file (*.bin);;"
                                            "All Files (*)"));

  if (file.isEmpty())
    return;

  ParallelProgressDialog dialog(this);
  dialog.GetRaw()->setWindowTitle(tr("Importing NAND backup"));

  std::future<void> result = std::async(std::launch::async, [&] {
    DiscIO::NANDImporter().ImportNANDBin(
        file.toStdString(),
        [&dialog](DiscIO::NANDImporter::Step step, u32 cur, u32 max) {
          switch (step)
          {
          case DiscIO::NANDImporter::Step::Loading:
            dialog.SetLabelText(tr("Loading NAND..."));
            break;
          case DiscIO::NANDImporter::Step::Extracting:
            dialog.SetLabelText(tr("Extracting NAND..."));
            break;
          }
          dialog.SetValue(cur);
          dialog.SetMaximum(max);
          return dialog.WasCanceled();
        },
        [this] {
          std::optional<std::string> keys_file = RunOnObject(this, [this] {
            return DolphinFileDialog::getOpenFileName(
                       this, tr("Select Keys File (OTP/SEEPROM Dump)"), QDir::currentPath(),
                       tr("BootMii keys file (*.bin);;"
                          "All Files (*)"))
                .toStdString();
          });
          if (keys_file)
            return *keys_file;
          return std::string("");
        });
    dialog.Reset();
  });

  dialog.GetRaw()->exec();

  result.wait();

  m_menu_bar->UpdateToolsMenu(Core::State::Uninitialized);
}

void MainWindow::OnPlayRecording()
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Playback of input recordings is disabled in RetroAchievements hardcore mode."));
    return;
  }

  QString dtm_file = DolphinFileDialog::getOpenFileName(
      this, tr("Select the Recording File to Play"), QString(), tr("Dolphin TAS Movies (*.dtm)"));

  if (dtm_file.isEmpty())
    return;

  auto& movie = m_system.GetMovie();
  if (!movie.IsReadOnly())
  {
    // let's make the read-only flag consistent at the start of a movie.
    movie.SetReadOnly(true);
    emit ReadOnlyModeChanged(true);
  }

  std::optional<std::string> savestate_path;
  if (movie.PlayInput(dtm_file.toStdString(), &savestate_path))
  {
    emit RecordingStatusChanged(true);

    Play(savestate_path);
  }
}

void MainWindow::OnStartRecording()
{
  auto& movie = m_system.GetMovie();
  if (Core::GetState(m_system) == Core::State::Starting ||
      Core::GetState(m_system) == Core::State::Stopping || movie.IsRecordingInput() ||
      movie.IsPlayingInput())
  {
    return;
  }

  if (movie.IsReadOnly())
  {
    // The user just chose to record a movie, so that should take precedence
    movie.SetReadOnly(false);
    emit ReadOnlyModeChanged(true);
  }

  Movie::ControllerTypeArray controllers{};
  Movie::WiimoteEnabledArray wiimotes{};

  for (int i = 0; i < 4; i++)
  {
    const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(si_device))
      controllers[i] = Movie::ControllerType::GC;
    else
      controllers[i] = Movie::ControllerType::None;
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  if (movie.BeginRecordingInput(controllers, wiimotes))
  {
    emit RecordingStatusChanged(true);

    if (Core::IsUninitialized(m_system))
      Play();
  }
}

void MainWindow::OnStopRecording()
{
  auto& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    OnExportRecording();
  if (movie.IsMovieActive())
    movie.EndPlayInput(false);
  emit RecordingStatusChanged(false);
}

void MainWindow::OnExportRecording()
{
  const Core::CPUThreadGuard guard(m_system);

  QString dtm_file = DolphinFileDialog::getSaveFileName(
      this, tr("Save Recording File As"), QString(), tr("Dolphin TAS Movies (*.dtm)"));
  if (!dtm_file.isEmpty())
    m_system.GetMovie().SaveRecording(dtm_file.toStdString());
}

void MainWindow::OnActivateChat()
{
  if (const auto overlay = g_netplay_chat_ui.load())
    overlay->Activate();
}

void MainWindow::OnCollapseChat()
{
  if (const auto overlay = g_netplay_chat_ui.load())
    overlay->Collapse();
}

void MainWindow::OnExpandChat()
{
  if (const auto overlay = g_netplay_chat_ui.load())
    overlay->Expand();
}

void MainWindow::OnRequestGolfControl()
{
  auto client = Settings::Instance().GetNetPlayClient();
  if (client)
    client->RequestGolfControl();
}

void MainWindow::ShowTASInput()
{
  for (int i = 0; i < num_gc_controllers; i++)
  {
    const auto si_device = Config::Get(Config::GetInfoForSIDevice(i));
    if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
    {
      m_gba_tas_input_windows[i]->show();
      m_gba_tas_input_windows[i]->raise();
      m_gba_tas_input_windows[i]->activateWindow();
    }
    else if (si_device != SerialInterface::SIDEVICE_NONE &&
             si_device != SerialInterface::SIDEVICE_GC_GBA)
    {
      m_gc_tas_input_windows[i]->show();
      m_gc_tas_input_windows[i]->raise();
      m_gc_tas_input_windows[i]->activateWindow();
    }
  }

  for (int i = 0; i < num_wii_controllers; i++)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(i)) == WiimoteSource::Emulated &&
        (!Core::IsRunning(m_system) || m_system.IsWii()))
    {
      m_wii_tas_input_windows[i]->show();
      m_wii_tas_input_windows[i]->raise();
      m_wii_tas_input_windows[i]->activateWindow();
    }
  }
}

void MainWindow::OnConnectWiiRemote(int id)
{
  const Core::CPUThreadGuard guard(m_system);
  if (const auto bt = WiiUtils::GetBluetoothEmuDevice())
  {
    const auto wm = bt->AccessWiimoteByIndex(id);
    wm->Activate(!wm->IsConnected());
  }
}

#ifdef USE_RETRO_ACHIEVEMENTS
void MainWindow::ShowAchievementsWindow()
{
  if (!m_achievements_window)
  {
    m_achievements_window = new AchievementsWindow(this);
  }

  m_achievements_window->show();
  m_achievements_window->raise();
  m_achievements_window->activateWindow();
  m_achievements_window->UpdateData(AchievementManager::UpdatedItems{.all = true});
}

void MainWindow::ShowAchievementSettings()
{
  ShowAchievementsWindow();
  m_achievements_window->ForceSettingsTab();
}

void MainWindow::OnHardcoreChanged()
{
  bool hardcore_active = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore_active)
    Settings::Instance().SetDebugModeEnabled(false);
  // EmulationStateChanged causes several dialogs to redraw, including anything affected by hardcore
  // mode. Every dialog that depends on hardcore mode is redrawn by EmulationStateChanged.
  if (hardcore_active != m_former_hardcore_setting)
    emit Settings::Instance().EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  m_former_hardcore_setting = hardcore_active;
}
#endif  // USE_RETRO_ACHIEVEMENTS

void MainWindow::ShowMemcardManager()
{
  GCMemcardManager manager(this);

  manager.exec();
}

void MainWindow::ShowResourcePackManager()
{
  ResourcePackManager manager(this);

  manager.exec();
}

void MainWindow::ShowCheatsManager()
{
  if (!m_cheats_manager)
  {
    m_cheats_manager = new CheatsManager(m_system, this);

    connect(m_cheats_manager, &CheatsManager::ShowMemory, m_memory_widget,
            &MemoryWidget::SetAddress);
    connect(m_cheats_manager, &CheatsManager::RequestWatch, m_watch_widget, &WatchWidget::AddWatch);
    connect(m_cheats_manager, &CheatsManager::OpenGeneralSettings, this,
            &MainWindow::ShowGeneralWindow);

#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_cheats_manager, &CheatsManager::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }

  m_cheats_manager->show();
}

void MainWindow::ShowRiivolutionBootWidget(const UICommon::GameFile& game)
{
  auto second_game = m_game_list->FindSecondDisc(game);
  std::vector<std::string> paths = {game.GetFilePath()};
  if (second_game != nullptr)
    paths.push_back(second_game->GetFilePath());
  std::unique_ptr<BootParameters> boot_params = BootParameters::GenerateFromFile(paths);
  if (!boot_params)
    return;
  if (!std::holds_alternative<BootParameters::Disc>(boot_params->parameters))
    return;

  auto& disc = std::get<BootParameters::Disc>(boot_params->parameters);
  RiivolutionBootWidget w(disc.volume->GetGameID(), disc.volume->GetRevision(),
                          disc.volume->GetDiscNumber(), game.GetFilePath(), this);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(&w, &RiivolutionBootWidget::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  w.exec();
  if (!w.ShouldBoot())
    return;

  AddRiivolutionPatches(boot_params.get(), std::move(w.GetPatches()));
  StartGame(std::move(boot_params));
}
