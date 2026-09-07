/*
 * RT64::Application lifecycle. Contract is in rt64_host.h.
 *
 * This is the only file in the backend that includes RT64 headers, and it
 * compiles to nothing without PDRT64_WITH_RT64 so the port's MinGW build can
 * glob it without having RT64 on its include path.
 *
 * Three build configurations select between three implementations of the same
 * declarations, which is the whole point of the split:
 *   PDRT64_WITH_RT64      - this file, calling RT64 directly. MSVC only, and
 *                           what the replay harness uses.
 *   PDRT64_WITH_RT64_DLL  - rt64_hostdll.cpp, calling rt64shim.dll. What the
 *                           MinGW port uses; this file is then empty.
 *   neither               - the refusal stub below, so the port links and
 *                           fails at run time with a reason.
 * hostResultName is deliberately not here: it lives in rt64_hostdll.cpp,
 * which is the one translation unit present in all three.
 *
 * One constraint that shapes the whole file: RT64's GBI header and the port's
 * PR/gbi.h define overlapping names, so no translation unit may include both.
 * Nothing here may include PR/gbi.h, which is why the interface above is
 * expressed in plain integers rather than in Gfx terms.
 */

#include "rt64_host.h"

#if defined(PDRT64_WITH_RT64_DLL)

/* rt64_hostdll.cpp provides the implementation by loading rt64shim.dll. */

#elif !defined(PDRT64_WITH_RT64)

namespace pdrt64 {

/* Built without RT64. The declarations still resolve, so callers link and fail
 * at run time with a reason rather than failing to build. */
HostResult hostInit(const HostConfig &) { return HostResult::NotCompiledIn; }
void hostShutdown() {}
bool hostReady() { return false; }
void hostProcessDl(RdramAddr, RdramAddr) {}
void hostUpdateScreen() {}

} // namespace pdrt64

#else

#include <cstdio>
#include <cstdlib>
#include <memory>

#if RT_ENABLED && defined(_WIN32)
/* For DRED. RT64 links d3d12 already; this only needs the declarations. */
#   include <d3d12.h>
#endif

#include "gbi/rt64_gbi_f3dpd.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_application.h"

namespace pdrt64 {

namespace {

/*
 * Texture filtering, port value -> RT64 configuration.
 *
 * rt64_host.h leaves the mapping to the implementation, so it is spelled out
 * here. The wire values are the port's own `enum FilteringMode`
 * (gfx_rendering_api.h:15), passed as int because the shim ABI carries POD
 * only: 0 FILTER_NONE, 1 FILTER_LINEAR, 2 FILTER_THREE_POINT.
 *
 * **Only `threePointFiltering` is touched.** The obvious-looking mapping - onto
 * `UserConfiguration::filtering`, which even has Nearest and Linear members -
 * is wrong, and measurably so. That enum is consumed in exactly one place,
 * `rt64_vi_renderer.cpp:48-56`: it is the filter the VI renderer uses when
 * scaling the finished image to the output, not how texels are sampled. Set it
 * and nothing about texture sampling changes; the first version of this
 * function did, and nearest, linear and RT64's own default all rendered
 * byte-identical through dlreplay, which renders to RAM at native resolution
 * where the VI scaler never runs.
 *
 * Texture sampling is `threePointFiltering`, read at `rt64_state.cpp:823` as
 * `const bool linearFiltering = !ext.userConfig->threePointFiltering`. So:
 *
 *   FILTER_THREE_POINT -> threePointFiltering = true   (the RDP's own filter)
 *   FILTER_LINEAR      -> threePointFiltering = false  (bilinear)
 *   FILTER_NONE        -> threePointFiltering = false, and see below
 *
 * FILTER_NONE cannot be honoured. RT64 has no "sample textures nearest"
 * switch: point sampling is what the display list asks for through
 * G_MDSFT_TEXTFILT, which already reaches RT64 in the stream, so the renderer
 * follows the game rather than a global override. Mapping it to linear is
 * therefore the closest honest answer - it leaves the per-draw G_TF_POINT
 * intact and only declines the global forcing that RT64 does not implement.
 *
 * `userConfig.filtering` is deliberately left alone. It is presentation
 * scaling, the port does not expose a setting for it through this call, and
 * overriding it here would silently change how the frame is scaled to the
 * window on the strength of a texture-filter menu item.
 *
 * `mipmapMode` and `anisotropy` are accepted and dropped. RT64's
 * UserConfiguration has no mipmap-filter field and no anisotropy field at all,
 * so there is nothing to forward them to; the backend already reports
 * rt64GetMaxAnisotropyLevel() == 1, which says the same thing to the options
 * menu. They stay in the signature because removing them would be an ABI
 * change for no gain if RT64 ever grows the settings.
 */
constexpr int kFilterNone = 0;
constexpr int kFilterLinear = 1;
constexpr int kFilterThreePoint = 2;

/*
 * The live host. RT64's Application keeps the pointers it is given rather than
 * copying them, so the register block and the RDRAM buffer have to outlive it.
 * Those belong to the caller; this file stores only the Core that points at
 * them, which is why Core is a member here and not a local.
 */
struct Host {
    std::unique_ptr<RT64::Application> app;
    RT64::Application::Core core = {};
    Registers *regs = nullptr;

    /* The port sets the texture filter from videoInit (video.c:175), which can
     * run before the backend is up, so the request is remembered and applied
     * again once Application exists. Default matches the port's own default,
     * texFilter = FILTER_LINEAR (video.c:59), so that a build which never
     * calls the setter still agrees with the reference path rather than
     * silently keeping RT64's defaults. */
    int filterMode = kFilterLinear;
};

Host g_host;

/* RT64 calls this when it would raise an interrupt. Nothing on the HLE path
 * needs one, but the pointer must be valid - RT64 calls it unconditionally. */
void checkInterrupts()
{
}

HostResult fromSetupResult(RT64::Application::SetupResult r)
{
    using S = RT64::Application::SetupResult;
    switch (r) {
    case S::Success:                   return HostResult::Ok;
    case S::DynamicLibrariesNotFound:  return HostResult::DynamicLibrariesNotFound;
    case S::InvalidGraphicsAPI:        return HostResult::InvalidGraphicsApi;
    case S::GraphicsAPINotFound:       return HostResult::GraphicsApiNotFound;
    case S::GraphicsDeviceNotFound:    return HostResult::GraphicsDeviceNotFound;
    default:                           return HostResult::Unknown;
    }
}

/* Pushes g_host.filterMode into RT64's live configuration. Safe to call with
 * no Application: the value is kept and applied by hostInit. */
void applyFiltering()
{
    if (!g_host.app) {
        return;
    }

    g_host.app->userConfig.threePointFiltering =
        (g_host.filterMode == kFilterThreePoint);

    /* discardFBs=false: filtering changes how existing targets are sampled,
     * not their size or format, so throwing the framebuffers away would cost a
     * visible reallocation for nothing. */
    g_host.app->updateUserConfig(false);
}

/*
 * Turns the path tracer on from the environment, for development runs.
 *
 * RT64 already has a toggle - F2, via DeveloperShortcut::RayTracing
 * (rt64_application.cpp:580,658) - but it arrives through RT64's own window
 * message and SDL event filters, and this port never installs either: the port
 * owns the window and RT64 only ever receives display lists. Rather than route
 * input across the shim boundary for a development switch, the two values a
 * test run needs are read once at init.
 *
 *   PDRT64_RT=1        enable the path tracer
 *   PDRT64_RT_VIZ=<n>  VisualizationMode, per the enum's declaration order in
 *                      rt64_raytracing_params.h. 5 is InstanceId, which is the
 *                      one that shows whether the TLAS is built correctly.
 *
 * Deliberately not part of the shim ABI. When path tracing becomes something a
 * player turns on it belongs in the port's own options and the ABI, and adding
 * an export now would freeze a shape that has not been designed yet.
 *
 * Guarded on RT_ENABLED because rtConfig, VisualizationMode and
 * WorkloadQueue::rtEnabled only exist in a raytracing build of RT64. A shim
 * built against a raster-only RT64 compiles this away entirely.
 */
/*
 * Turns on Device Removed Extended Data, which has to happen before the device is
 * created to be of any use.
 *
 * A GPU fault shows up as DXGI_ERROR_DEVICE_REMOVED on some later, unrelated call -
 * the first symptom here was a buffer allocation failing three calls downstream of
 * the actual cause. DRED records which GPU operations completed, so the breadcrumbs
 * name the one that faulted instead.
 *
 * Off unless asked for. It costs performance on every submission, and the raster path
 * has no need of it.
 */
/*
 * Turns on the D3D12 debug layer, which also has to happen before the device is
 * created. DRED has reported nothing useful here - no breadcrumbs, no page fault -
 * and the debug layer answers a different question: not which GPU operation died,
 * but which API call was invalid. A removed device is nearly always preceded by one.
 *
 * Needs the Graphics Tools optional feature installed, and says so if it is missing
 * rather than failing quietly. Costs a great deal of performance, so it is opt-in.
 */
void applyRaytracingDebugLayer()
{
#if RT_ENABLED && defined(_WIN32)
    const char *want = getenv("PDRT64_RT_DEBUGLAYER");
    if ((want == nullptr) || (want[0] == '0') || (want[0] == '\0')) {
        return;
    }

    ID3D12Debug *debug = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        printf("rt64: the debug layer was requested but is not available; install the Graphics Tools optional feature\n");
        fflush(stdout);
        return;
    }

    debug->EnableDebugLayer();
    debug->Release();
    printf("rt64: D3D12 debug layer enabled - validation errors will be reported as they happen\n");
    fflush(stdout);
#endif
}

void applyRaytracingDred()
{
#if RT_ENABLED && defined(_WIN32)
    const char *dred = getenv("PDRT64_RT_DRED");
    if ((dred == nullptr) || (dred[0] == '0') || (dred[0] == '\0')) {
        return;
    }

    ID3D12DeviceRemovedExtendedDataSettings1 *settings = nullptr;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)))) {
        printf("rt64: DRED was requested but D3D12GetDebugInterface refused; is the Graphics Tools feature installed?\n");
        fflush(stdout);
        return;
    }

    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->Release();

    printf("rt64: DRED enabled - a device removal will name the GPU operation that caused it\n");
    fflush(stdout);
#endif
}

void applyRaytracingEnvironment()
{
#if RT_ENABLED
    if (!g_host.app) {
        return;
    }

    const char *enable = getenv("PDRT64_RT");
    if ((enable == nullptr) || (enable[0] == '0') || (enable[0] == '\0')) {
        return;
    }

    if (g_host.app->device && !g_host.app->device->getCapabilities().raytracing) {
        printf("rt64: PDRT64_RT is set but the device reports no raytracing support; staying on the raster path\n");
        fflush(stdout);
        return;
    }

    RT64::RaytracingConfiguration rtConfig = g_host.app->rtConfig;

    /* Forces the render resolution, which is what the path tracer sizes every one of
     * its buffers from. Without this the scale comes from the swap chain, so the
     * replay harness - which renders into a hidden window - always traces at the
     * N64 resolution and cannot reproduce anything that only breaks at the size a
     * real window asks for. Two bugs in a row were exactly that, so the harness
     * needs to be able to ask for the same size. */
    const char *resScale = getenv("PDRT64_RT_RESSCALE");
    if (resScale != nullptr) {
        const double scale = atof(resScale);
        if (scale > 0.0) {
            g_host.app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            g_host.app->userConfig.resolutionMultiplier = scale;
            g_host.app->updateUserConfig(true);
            printf("rt64: render resolution forced to %gx native\n", scale);
            fflush(stdout);
        }
    }

    const char *viz = getenv("PDRT64_RT_VIZ");
    if (viz != nullptr) {
        const int mode = atoi(viz);
        if ((mode >= 0) && (mode < (int)interop::VisualizationMode::Count)) {
            rtConfig.visualizationMode = (interop::VisualizationMode)mode;
        }
    }

    /* Fraction of the display resolution to trace at. RT64 renders at the window's
     * size, and every ray generation dispatch and RT buffer is sized from it - 110 ms
     * of GPU time per frame at 2880x1980, measured against 21 ms with the path tracer
     * off, which is what trips the driver's two second timeout. Tracing below the
     * display resolution and composing back up is the normal arrangement. */
    const char *rtScale = getenv("PDRT64_RT_SCALE");
    if (rtScale != nullptr) {
        const float scale = float(atof(rtScale));
        if (scale > 0.0f) {
            rtConfig.resolutionScale = scale;
            printf("rt64: tracing at %g of the display resolution\n", scale);
            fflush(stdout);
        }
    }

    g_host.app->rtConfig = rtConfig;

    /* setRtConfig rather than assigning sharedQueueResources->rtConfig: it
     * takes the configuration mutex and raises rtConfigChanged, and that flag
     * is what makes the workload queue compile the RT pipeline and push the
     * configuration into the renderer (rt64_workload_queue.cpp:242-249). Set
     * before rtEnabled so the first frame that traces already has both. */
    g_host.app->sharedQueueResources->setRtConfig(rtConfig);
    g_host.app->workloadQueue->rtEnabled = true;

    printf("rt64: path tracing enabled from the environment, visualizationMode=%d\n",
           (int)rtConfig.visualizationMode);
    fflush(stdout);
#endif
}

/* Wires the register block into Core. Application::Core::decodeVI reads
 * through these pointers every frame (rt64_application.cpp:45-62), so they
 * must point at storage that stays put - hence Registers being the caller's. */
void wireRegisters(RT64::Application::Core &core, Registers *regs)
{
    core.MI_INTR_REG = &regs->miIntr;
    core.DPC_START_REG = &regs->dpc[0];
    core.DPC_END_REG = &regs->dpc[1];
    core.DPC_CURRENT_REG = &regs->dpc[2];
    core.DPC_STATUS_REG = &regs->dpc[3];
    core.DPC_CLOCK_REG = &regs->dpc[4];
    core.DPC_BUFBUSY_REG = &regs->dpc[5];
    core.DPC_PIPEBUSY_REG = &regs->dpc[6];
    core.DPC_TMEM_REG = &regs->dpc[7];

    core.VI_STATUS_REG = &regs->viStatus;
    core.VI_ORIGIN_REG = &regs->viOrigin;
    core.VI_WIDTH_REG = &regs->viWidth;
    core.VI_INTR_REG = &regs->viIntr;
    core.VI_V_CURRENT_LINE_REG = &regs->viVCurrentLine;
    core.VI_TIMING_REG = &regs->viBurst;
    core.VI_V_SYNC_REG = &regs->viVSync;
    core.VI_H_SYNC_REG = &regs->viHSync;
    core.VI_LEAP_REG = &regs->viLeap;
    core.VI_H_START_REG = &regs->viHStart;
    core.VI_V_START_REG = &regs->viVStart;
    core.VI_V_BURST_REG = &regs->viVBurst;
    core.VI_X_SCALE_REG = &regs->viXScale;
    core.VI_Y_SCALE_REG = &regs->viYScale;
}

/*
 * Tells the interpreter which microcode it is executing.
 *
 * RT64 normally works this out by hashing the microcode's text and data
 * segments out of RDRAM (Interpreter::loadUCodeGBI, rt64_interpreter.cpp:26-
 * 50). There is no microcode here to hash: the display lists are synthesized,
 * and the RSP never ran. So the GBI is selected directly.
 *
 * Without this, hleGBI stays null and the interpreter dereferences it on the
 * first command (rt64_interpreter.cpp:179). A release build does not even
 * assert first - the assert above that line is compiled out - so the symptom
 * is an access violation inside processDisplayLists with nothing to read.
 *
 * The body mirrors the ucode branch of GBIManager::getGBIForUCode
 * (rt64_gbi.cpp:462-478): RDP first for the shared commands, then F3DPD over
 * it. If RT64 changes how a GBI is assembled, this has to follow.
 */
bool selectPerfectDarkGbi(RT64::Application &app)
{
    RT64::Interpreter *interpreter = app.interpreter.get();
    if (interpreter == nullptr) {
        return false;
    }

    RT64::GBI &gbi =
        interpreter->gbiManager.gbiCache[uint32_t(RT64::GBIUCode::F3DPD)];
    if (gbi.ucode == RT64::GBIUCode::Unknown) {
        gbi.ucode = RT64::GBIUCode::F3DPD;
        RT64::GBI_RDP::setup(&gbi, true);
        RT64::GBI_F3DPD::setup(&gbi);
    }

    interpreter->hleGBI = &gbi;
    app.state->rsp->setGBI(&gbi);

    /* loadUCodeGBI runs this after selecting a GBI; it seeds the lookat
     * vectors and fog that F3D expects (rt64_gbi_f3d.cpp:197-200). */
    if (gbi.resetFromTask != nullptr) {
        gbi.resetFromTask(app.state.get());
    }
    return true;
}

} // namespace

HostResult hostInit(const HostConfig &cfg)
{
    if (g_host.app) {
        return HostResult::AlreadyInitialised;
    }
    if (!cfg.nativeWindow || !cfg.rdram || !cfg.rdramSize || !cfg.regs) {
        return HostResult::BadConfig;
    }

    g_host.regs = cfg.regs;
    g_host.core = {};
    /* Core::window is plume's RenderWindow, which is an HWND on Windows
     * (plume_render_interface_types.h:39) and a different type elsewhere.
     * Taking the member's own type keeps this honest rather than naming a
     * typedef that only exists on one platform. */
    g_host.core.window = reinterpret_cast<decltype(g_host.core.window)>(cfg.nativeWindow);
    g_host.core.RDRAM = cfg.rdram;

    /* HEADER, DMEM and IMEM are the low-level paths' business. Nothing on the
     * HLE path reads them, but they must not be null. Pointing them at the
     * start of our own buffer keeps them in mapped memory without giving RT64
     * anything else to own. */
    g_host.core.HEADER = cfg.rdram;
    g_host.core.DMEM = cfg.rdram;
    g_host.core.IMEM = cfg.rdram;

    wireRegisters(g_host.core, cfg.regs);
    g_host.core.checkInterrupts = &checkInterrupts;

    applyRaytracingDebugLayer();
    applyRaytracingDred();

    RT64::ApplicationConfiguration appConfig;
    if (cfg.dataPath) {
        appConfig.dataPath = cfg.dataPath;
        appConfig.detectDataPath = false;
    }

    g_host.app = std::make_unique<RT64::Application>(g_host.core, appConfig);

    const RT64::Application::SetupResult result = g_host.app->setup(0);
    if (result != RT64::Application::SetupResult::Success) {
        g_host.app.reset();
        return fromSetupResult(result);
    }

    if (!selectPerfectDarkGbi(*g_host.app)) {
        g_host.app.reset();
        return HostResult::UcodeSelectionFailed;
    }

    /* Whatever the port asked for before the backend existed. Without this,
     * RT64 keeps threePointFiltering on (rt64_user_configuration.cpp:79) while
     * the port believes it selected FILTER_LINEAR (video.c:59). */
    applyFiltering();

    /*
     * Report what RT64's own device says about raytracing, rather than what
     * the GPU's model number implies. RT-PLAN's RT0 asks for the dev GPU's DXR
     * support to be confirmed, and the capability RT64 will actually consult
     * is this one - it is what gates the acceleration-structure buffer flags
     * that already exist in the raster path (rt64_workload.cpp:197,241) and
     * what any future RT work would build on. One line at init, because the
     * answer is a property of the machine and belongs in every run's log.
     */
    if (g_host.app->device) {
        const auto &caps = g_host.app->device->getCapabilities();
        printf("rt64: device raytracing=%d raytracingStateUpdate=%d\n",
               (int)caps.raytracing, (int)caps.raytracingStateUpdate);
        fflush(stdout);
    }

    applyRaytracingEnvironment();

    return HostResult::Ok;
}

void hostShutdown()
{
    if (!g_host.app) {
        return;
    }
    g_host.app->end();
    g_host.app.reset();
    g_host.regs = nullptr;
}

bool hostReady()
{
    return g_host.app != nullptr;
}

void hostSetTextureFiltering(int filterMode, int mipmapMode, uint32_t anisotropy)
{
    (void)mipmapMode;
    (void)anisotropy;
    if (filterMode < kFilterNone || filterMode > kFilterThreePoint) {
        filterMode = kFilterLinear;
    }
    g_host.filterMode = filterMode;
    applyFiltering();
}

void hostProcessDl(RdramAddr dlStart, RdramAddr dlEnd)
{
    if (!g_host.app) {
        return;
    }

    /* Strip the tag before handing the addresses over. RT64 resolves tagged
     * addresses inside a stream once G_EX_SETRDRAMEXTENDED is on, but these
     * two are the bounds of the stream itself, taken as plain offsets from the
     * RDRAM base it was given. */
    const uint32_t start = dlStart & ~kExtendedAddrBit;
    const uint32_t end = dlEnd & ~kExtendedAddrBit;
    g_host.app->processDisplayLists(g_host.core.RDRAM, start, end, true);
}

void hostUpdateScreen()
{
    if (!g_host.app) {
        return;
    }
    g_host.app->updateScreen();
}

} // namespace pdrt64

#endif // implementation selection
