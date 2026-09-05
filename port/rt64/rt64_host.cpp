/*
 * RT64::Application lifecycle. Contract is in rt64_host.h.
 *
 * This is the only file in the backend that includes RT64 headers, and it
 * compiles to nothing without PDRT64_WITH_RT64 so the port's MinGW build can
 * glob it without having RT64 on its include path.
 *
 * One constraint that shapes the whole file: RT64's GBI header and the port's
 * PR/gbi.h define overlapping names, so no translation unit may include both.
 * Nothing here may include PR/gbi.h, which is why the interface above is
 * expressed in plain integers rather than in Gfx terms.
 */

#include "rt64_host.h"

#ifndef PDRT64_WITH_RT64

namespace pdrt64 {

/* Built without RT64. The declarations still resolve, so callers link and fail
 * at run time with a reason rather than failing to build. */
const char *hostResultName(HostResult r)
{
    return r == HostResult::NotCompiledIn ? "built without RT64" : "unknown";
}

HostResult hostInit(const HostConfig &) { return HostResult::NotCompiledIn; }
void hostShutdown() {}
bool hostReady() { return false; }
void hostProcessDl(RdramAddr, RdramAddr) {}
void hostUpdateScreen() {}

} // namespace pdrt64

#else

#include <cstdio>
#include <memory>

#include "gbi/rt64_gbi_f3dpd.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_application.h"

namespace pdrt64 {

namespace {

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

const char *hostResultName(HostResult r)
{
    switch (r) {
    case HostResult::Ok:                       return "ok";
    case HostResult::NotCompiledIn:            return "built without RT64";
    case HostResult::AlreadyInitialised:       return "already initialised";
    case HostResult::BadConfig:                return "incomplete host configuration";
    case HostResult::DynamicLibrariesNotFound: return "graphics dynamic libraries not found";
    case HostResult::InvalidGraphicsApi:       return "invalid graphics API";
    case HostResult::GraphicsApiNotFound:      return "graphics API not found";
    case HostResult::GraphicsDeviceNotFound:   return "no usable graphics device";
    case HostResult::UcodeSelectionFailed:     return "could not select the F3DPD microcode";
    case HostResult::Unknown:                  break;
    }
    return "unknown";
}

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

#endif // PDRT64_WITH_RT64
