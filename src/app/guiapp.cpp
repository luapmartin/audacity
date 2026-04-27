/*
 * Audacity: A Digital Audio Editor
 */
#include "guiapp.h"

#include "modularity/ioc.h"
#include "appshell/istartupscenario.h"
#include "appshell/internal/splashscreen/splashscreen.h"
#include "project/types/projecttypes.h"

#include "commandlineparser.h"

#include "muse_framework_config.h"

#include "log.h"

#ifdef AU_FORCE_QML_GC
#include <QTimer>
#include <QQmlApplicationEngine>
#include "ui/iuiengine.h"
#include "ausignposts.h"
#endif

#ifdef AU_MAIN_THREAD_WATCHDOG
#include <QTimer>
#include "global/iglobalconfiguration.h"
#include "mainthreadwatchdog.h"
#endif

using namespace muse;
using namespace au::app;
using namespace au::appshell;
namespace project = au::project;

GuiApp::GuiApp(const std::shared_ptr<AudacityCmdOptions>& options)
    : muse::ui::GuiApplication(options)
{
}

std::shared_ptr<muse::CmdOptions> GuiApp::makeContextOptions(const muse::StringList& args) const
{
    if (args.empty()) {
        return m_appOptions;
    }

    std::vector<std::string> args_ = args.toStdStringList();
    args_.insert(args_.begin(), "dummy/path/to/app.exe"); // argv[0] placeholder
    const int argc = static_cast<int>(args_.size());
    std::vector<char*> argv(argc + 1, nullptr);
    for (int i = 0; i < argc; ++i) {
        argv[i] = args_[i].data();
    }

    CommandLineParser parser;
    parser.init();
    parser.parse(argc, argv.data());
    return parser.options();
}

QString GuiApp::mainWindowQmlPath(const QString& platform) const
{
    return QString(":/qt/qml/Audacity/AppShell/platform/%1/Main.qml").arg(platform);
}

void GuiApp::showContextSplash(const muse::modularity::ContextPtr& ctxId)
{
    if (m_splashScreen) {
        return;
    }

    m_splashScreen = new appshell::SplashScreen(ctxId, appshell::SplashScreen::Default);
    m_splashScreen->show();
}

void GuiApp::doStartupScenario(const muse::modularity::ContextPtr& ctxId)
{
    auto startupScenario = muse::modularity::ioc(ctxId)->resolve<IStartupScenario>("app");
    IF_ASSERT_FAILED(startupScenario) {
        return;
    }

    const std::shared_ptr<AudacityCmdOptions> options
        = std::dynamic_pointer_cast<AudacityCmdOptions>(contextData(ctxId).options);
    IF_ASSERT_FAILED(options) {
        return;
    }

    std::optional<project::ProjectFile> projectFile;
    if (options->startup.projectUrl.has_value()) {
        project::ProjectFile file;
        file.url = options->startup.projectUrl.value();
        if (options->startup.projectDisplayNameOverride.has_value()) {
            file.displayNameOverride = options->startup.projectDisplayNameOverride.value();
        }
        if (options->startup.cloudProjectId.has_value()) {
            file.cloudProjectId = options->startup.cloudProjectId.value();
        }
        projectFile = file;
    }

    startupScenario->setStartupType(options->startup.type);
    startupScenario->setStartupProjectFile(projectFile);
    startupScenario->setStartupMediaFiles(options->startup.mediaFiles);

    startupScenario->runOnSplashScreen();

    QMetaObject::invokeMethod(qApp, [this, startupScenario]() {
        if (m_splashScreen) {
            m_splashScreen->close();
            delete m_splashScreen;
            m_splashScreen = nullptr;
        }
        startupScenario->runAfterSplashScreen();
    }, Qt::QueuedConnection);

#ifdef AU_MAIN_THREAD_WATCHDOG
    // Diagnostic: start a background watchdog that detects main-thread stalls.
    // On detection it emits an os_signpost event and runs `sample(1)` to dump
    // the backtrace of every thread into <userAppData>/stall_samples/.
    {
        auto globalCfg = muse::modularity::globalIoc()->resolve<muse::IGlobalConfiguration>("global");
        const std::string sampleDir = globalCfg
                                      ? (globalCfg->userAppDataPath() + "/stall_samples").toStdString()
                                      : std::string("/tmp/au_stall_samples");

        au::diag::MainThreadWatchdog::instance().start(
            std::chrono::milliseconds { 500 },
            std::chrono::milliseconds { 100 },
            sampleDir);

        // Heartbeat on the main thread: a 100 ms QTimer that records a beat
        // on every fire. If the event loop stalls, beats stop arriving and
        // the watchdog fires. QTimer outlives GuiApp (owned by qApp).
        static QTimer* s_heartbeat = nullptr;
        if (!s_heartbeat) {
            s_heartbeat = new QTimer(qApp);
            s_heartbeat->setInterval(100);
            QObject::connect(s_heartbeat, &QTimer::timeout, qApp, []() {
                au::diag::MainThreadWatchdog::instance().beat();
            });
            s_heartbeat->start();
            LOGI() << "AU_MAIN_THREAD_WATCHDOG: started (500 ms threshold, 100 ms checks)"
                   << ", sample dumps -> " << sampleDir;
        }
    }
#endif

#ifdef AU_FORCE_QML_GC
    // Diagnostic: force QML/JS garbage collection every second to test whether
    // long main-thread freezes are driven by ad-hoc GC pauses.
    auto uiengine = muse::modularity::ioc(ctxId)->resolve<muse::ui::IUiEngine>("app");
    if (uiengine && uiengine->qmlAppEngine()) {
        QQmlApplicationEngine* qmlEngine = uiengine->qmlAppEngine();
        static QTimer* s_gcTimer = nullptr;
        if (!s_gcTimer) {
            s_gcTimer = new QTimer(qApp);
            s_gcTimer->setInterval(1000);
            QObject::connect(s_gcTimer, &QTimer::timeout, qApp, [qmlEngine]() {
                AU_SP_SCOPE("forceQmlGC");
                LOGI() << "AU_FORCE_QML_GC: calling collectGarbage()";
                qmlEngine->collectGarbage();
            });
            s_gcTimer->start();
            LOGI() << "AU_FORCE_QML_GC: started 1 s force-GC timer";
        }
    } else {
        LOGW() << "AU_FORCE_QML_GC: IUiEngine not resolvable; GC timer not started";
    }
#endif
}

void GuiApp::applyCommandLineOptions(const std::shared_ptr<muse::CmdOptions>& opt)
{
    BaseApplication::applyCommandLineOptions(opt);

    std::shared_ptr<AudacityCmdOptions> options = std::dynamic_pointer_cast<AudacityCmdOptions>(opt);
    IF_ASSERT_FAILED(options) {
        return;
    }

    if (options->app.revertToFactorySettings) {
        appshellConfiguration()->revertToFactorySettings();
    }
}
