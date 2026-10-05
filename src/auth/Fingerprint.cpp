#include "Fingerprint.hpp"
#include "../core/hyprlock.hpp"
#include "../core/Dbus.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include "../config/ConfigManager.hpp"

#include <chrono>
#include <memory>
#include <unistd.h>
#include <pwd.h>

#include <cstring>

static const auto FPRINT        = sdbus::ServiceName{"net.reactivated.Fprint"};
static const auto DEVICE        = sdbus::ServiceName{"net.reactivated.Fprint.Device"};
static const auto MANAGER       = sdbus::ServiceName{"net.reactivated.Fprint.Manager"};
static const auto LOGIN_SERVICE = sdbus::ServiceName{"org.freedesktop.login1"};
static const auto LOGIN_MANAGER = sdbus::ServiceName{"org.freedesktop.login1.Manager"};

static bool       isSignalFromService(const sdbus::ServiceName& service) {
    // Unicast signals bypass bus-side match rules; the proxy's well-known sender match is not authentication.
    try {
        const auto message = g_dbus->m_connection->getCurrentlyProcessedMessage();
        const auto sender  = message.getSender();
        if (!sender || !*sender) {
            Log::logger->log(Log::WARN, "fprint: ignoring signal with no sender");
            return false;
        }

        auto        proxy = sdbus::createProxy(*g_dbus->m_connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"});
        std::string owner;
        proxy->callMethod("GetNameOwner").onInterface("org.freedesktop.DBus").withArguments(service).storeResultsTo(owner);
        if (owner != sender) {
            Log::logger->log(Log::WARN, "fprint: ignoring signal from sender {} which does not own {}", sender, service.c_str());
            return false;
        }

        return true;
    } catch (const sdbus::Error& e) {
        Log::logger->log(Log::WARN, "fprint: ignoring signal, could not resolve owner of {} ({})", service.c_str(), e.what());
        return false;
    }
}

enum MatchResult {
    MATCH_INVALID = 0,
    MATCH_NO_MATCH,
    MATCH_MATCHED,
    MATCH_RETRY,
    MATCH_SWIPE_TOO_SHORT,
    MATCH_FINGER_NOT_CENTERED,
    MATCH_REMOVE_AND_RETRY,
    MATCH_DISCONNECTED,
    MATCH_UNKNOWN_ERROR,
};

static std::map<std::string, MatchResult> s_mapStringToTestType = {{"verify-no-match", MATCH_NO_MATCH},
                                                                   {"verify-match", MATCH_MATCHED},
                                                                   {"verify-retry-scan", MATCH_RETRY},
                                                                   {"verify-swipe-too-short", MATCH_SWIPE_TOO_SHORT},
                                                                   {"verify-finger-not-centered", MATCH_FINGER_NOT_CENTERED},
                                                                   {"verify-remove-and-retry", MATCH_REMOVE_AND_RETRY},
                                                                   {"verify-disconnected", MATCH_DISCONNECTED},
                                                                   {"verify-unknown-error", MATCH_UNKNOWN_ERROR}};

CFingerprint::CFingerprint() {
    static const auto FINGERPRINTREADY   = g_pConfigManager->getValue<Hyprlang::STRING>("auth:fingerprint:ready_message");
    m_sFingerprintReady                  = *FINGERPRINTREADY;
    static const auto FINGERPRINTPRESENT = g_pConfigManager->getValue<Hyprlang::STRING>("auth:fingerprint:present_message");
    m_sFingerprintPresent                = *FINGERPRINTPRESENT;
}

CFingerprint::~CFingerprint() {
    ;
}

void CFingerprint::init() {
    if (!g_dbus->m_connection || !g_dbus->m_freedesktopLogin1) {
        Log::logger->log(Log::ERR, "fprint: Missing dbus proxy?");
        return;
    }

    g_dbus->m_freedesktopLogin1->getPropertyAsync("PreparingForSleep")
        .onInterface(LOGIN_MANAGER)
        .uponReplyInvoke([this](std::optional<sdbus::Error> e, sdbus::Variant preparingForSleep) {
            if (e) {
                Log::logger->log(Log::WARN, "fprint: Failed getting value for PreparingForSleep: {}", e->what());
                return;
            }
            m_sDBUSState.sleeping = preparingForSleep.get<bool>();
            // When entering sleep, the wake signal will trigger startVerify().
            if (m_sDBUSState.sleeping)
                return;

            startVerify();
        });

    g_dbus->m_freedesktopLogin1->uponSignal("PrepareForSleep").onInterface(LOGIN_MANAGER).call([this](bool start) {
        if (!isSignalFromService(LOGIN_SERVICE))
            return;
        Log::logger->log(Log::INFO, "fprint: PrepareForSleep (start: {})", start);
        m_sDBUSState.sleeping = start;
        if (!m_sDBUSState.sleeping && !m_sDBUSState.verifying)
            startVerify();
    });
}

void CFingerprint::handleInput(const std::string& input) {
    // Non-empty input is password auth. Leave the parallel fingerprint session alone.
    if (!input.empty())
        return;

    if (m_sDBUSState.sleeping) {
        Log::logger->log(Log::INFO, "fprint: ignoring restart while preparing for sleep");
        return;
    }

    if (m_sDBUSState.waking) {
        Log::logger->log(Log::INFO, "fprint: wake already in progress");
        return;
    }

    // A live verify is already waiting for a finger. USB-resetting it (the old
    // empty-Enter path) took several seconds and often lost the device.
    // A second Enter within 1.2s soft-rearms without touching USB.
    static auto lastNudge = std::chrono::steady_clock::time_point{};
    const auto  now       = std::chrono::steady_clock::now();
    if (m_sDBUSState.verifying && m_sDBUSState.device) {
        if (lastNudge.time_since_epoch().count() == 0 || now - lastNudge > std::chrono::milliseconds(1200)) {
            lastNudge = now;
            Log::logger->log(Log::INFO, "fprint: already verifying");
            m_sPrompt = "Reader is listening";
            g_pHyprlock->enqueueForceUpdateTimers();
            return;
        }
        Log::logger->log(Log::INFO, "fprint: soft rearm");
    } else {
        Log::logger->log(Log::INFO, "fprint: rearm verify");
    }

    lastNudge                         = now;
    m_sDBUSState.hardWakeAttempts     = 0;
    m_sPrompt                         = "Waking fingerprint…";
    g_pHyprlock->enqueueForceUpdateTimers();
    softRearm();
}

void CFingerprint::softRearm() {
    m_sDBUSState.waking    = true;
    m_sDBUSState.userRearm = true;
    m_sDBUSState.abort     = false;
    m_sDBUSState.done      = false;
    m_sDBUSState.retries   = 0;
    m_sDBUSState.verifying = false;
    m_sFailureReason.clear();
    stopVerify();
    releaseDevice();
    startVerify();
}

void CFingerprint::beginHardWake() {
    if (m_sDBUSState.hardWaking)
        return;

    if (m_sDBUSState.hardWakeAttempts >= 1) {
        m_sDBUSState.waking = false;
        m_sPrompt           = "Fingerprint reader still offline";
        g_pHyprlock->enqueueForceUpdateTimers();
        return;
    }

    m_sDBUSState.hardWakeAttempts++;
    m_sDBUSState.hardWaking = true;
    m_sDBUSState.userRearm  = false;
    m_sDBUSState.waking     = true;
    m_sDBUSState.wakePolls  = 0;
    m_sDBUSState.verifying  = false;
    m_sFailureReason.clear();
    m_sPrompt = "Resetting fingerprint reader…";
    g_pHyprlock->enqueueForceUpdateTimers();

    Log::logger->log(Log::INFO, "fprint: device missing, USB wake");
    stopVerify();
    releaseDevice();

    // Passwordless via polkit rule 50-fingerprint-wake.rules
    spawnAsync("systemctl start --no-block fingerprint-wake.service");
    scheduleWakePoll();
}

void CFingerprint::scheduleWakePoll() {
    g_pHyprlock->addTimer(std::chrono::milliseconds(200), [](ASP<CTimer> self, void* data) { static_cast<CFingerprint*>(data)->pollHardWake(); }, this);
}

void CFingerprint::pollHardWake() {
    if (!m_sDBUSState.hardWaking)
        return;

    if (++m_sDBUSState.wakePolls > 40) {
        Log::logger->log(Log::WARN, "fprint: USB wake timed out");
        m_sDBUSState.hardWaking = false;
        m_sDBUSState.waking     = false;
        m_sPrompt               = "Fingerprint reader still offline";
        g_pHyprlock->enqueueForceUpdateTimers();
        return;
    }

    if (!devicePresent()) {
        scheduleWakePoll();
        return;
    }

    Log::logger->log(Log::INFO, "fprint: device back after {} polls", m_sDBUSState.wakePolls);
    startVerify();
}

std::optional<std::string> CFingerprint::getLastFailText() {
    if (!m_sFailureReason.empty())
        return std::optional(m_sFailureReason);
    return std::nullopt;
}

std::optional<std::string> CFingerprint::getLastPrompt() {
    if (!m_sPrompt.empty())
        return std::optional(m_sPrompt);
    return std::nullopt;
}

bool CFingerprint::checkWaiting() {
    return false;
}

void CFingerprint::terminate() {
    if (!m_sDBUSState.abort)
        releaseDevice();
}

bool CFingerprint::devicePresent() {
    if (!g_dbus->m_connection)
        return false;

    auto              proxy = sdbus::createProxy(*g_dbus->m_connection, FPRINT, sdbus::ObjectPath{"/net/reactivated/Fprint/Manager"});
    sdbus::ObjectPath path;
    try {
        // Short timeout so a restarting daemon cannot freeze the lock UI.
        proxy->callMethod("GetDefaultDevice").onInterface(MANAGER).withTimeout(std::chrono::milliseconds(300)).storeResultsTo(path);
    } catch (sdbus::Error& e) {
        return false;
    }
    return true;
}

bool CFingerprint::createDeviceProxy() {
    auto              proxy = sdbus::createProxy(*g_dbus->m_connection, FPRINT, sdbus::ObjectPath{"/net/reactivated/Fprint/Manager"});

    sdbus::ObjectPath path;
    try {
        proxy->callMethod("GetDefaultDevice").onInterface(MANAGER).withTimeout(std::chrono::milliseconds(800)).storeResultsTo(path);
    } catch (sdbus::Error& e) {
        Log::logger->log(Log::WARN, "fprint: couldn't connect to Fprint service ({})", e.what());
        m_sDBUSState.verifying = false;
        if (m_sDBUSState.hardWaking)
            scheduleWakePoll();
        else if (m_sDBUSState.userRearm)
            beginHardWake();
        else
            m_sDBUSState.waking = false;
        return false;
    }
    Log::logger->log(Log::INFO, "fprint: using device path {}", path.c_str());
    m_sDBUSState.device = sdbus::createProxy(*g_dbus->m_connection, FPRINT, path);

    m_sDBUSState.device->uponSignal("VerifyFingerSelected").onInterface(DEVICE).call([](const std::string& finger) {
        if (!isSignalFromService(FPRINT))
            return;
        Log::logger->log(Log::INFO, "fprint: finger selected: {}", finger);
    });
    m_sDBUSState.device->uponSignal("VerifyStatus").onInterface(DEVICE).call([this](const std::string& result, const bool done) {
        if (!isSignalFromService(FPRINT))
            return;
        handleVerifyStatus(result, done);
    });

    m_sDBUSState.device->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& interface, const std::map<std::string, sdbus::Variant>& properties) {
            if (!isSignalFromService(FPRINT) || interface != DEVICE || m_sDBUSState.done)
                return;

            try {
                const auto presentVariant = properties.at("finger-present");
                bool       isPresent      = presentVariant.get<bool>();
                if (!isPresent)
                    return;
                m_sPrompt = m_sFingerprintPresent;
                g_pHyprlock->enqueueForceUpdateTimers();
            } catch (std::out_of_range& e) {}
        });

    return true;
}

void CFingerprint::handleVerifyStatus(const std::string& result, bool done) {
    Log::logger->log(Log::INFO, "fprint: handling status {}", result);
    auto matchResult   = s_mapStringToTestType[result];
    bool authenticated = false;
    bool retry         = false;
    if (m_sDBUSState.sleeping) {
        stopVerify();
        Log::logger->log(Log::INFO, "fprint: device suspended");
        return;
    }
    switch (matchResult) {
        case MATCH_INVALID:
            Log::logger->log(Log::WARN, "fprint: unknown status: {}", result);
            stopVerify();
            m_sFailureReason = "Fingerprint did not match";
            break;
        case MATCH_NO_MATCH: {
            stopVerify();
            static const auto MAXATTEMPTS = g_pConfigManager->getValue<Hyprlang::INT>("auth:fingerprint:max_attempts");
            // retries counts prior retries; initial attempt is separate. 0 = unlimited.
            if (*MAXATTEMPTS > 0 && m_sDBUSState.retries >= (*MAXATTEMPTS - 1)) {
                m_sFailureReason = "Fingerprint auth disabled (too many failed attempts)";
            } else {
                done                         = false;
                static const auto RETRYDELAY = g_pConfigManager->getValue<Hyprlang::INT>("auth:fingerprint:retry_delay");
                g_pHyprlock->addTimer(std::chrono::milliseconds(*RETRYDELAY), [](ASP<CTimer> self, void* data) { ((CFingerprint*)data)->startVerify(true); }, this);
                m_sFailureReason = "Fingerprint did not match";
            }
            break;
        }
        case MATCH_UNKNOWN_ERROR:
            stopVerify();
            m_sFailureReason = "Fingerprint auth disabled (unknown error)";
            break;
        case MATCH_MATCHED:
            stopVerify();
            authenticated    = true;
            m_sFailureReason.clear();
            g_pAuth->enqueueUnlock();
            break;
        case MATCH_RETRY:
            retry            = true;
            m_sPrompt        = "Please retry fingerprint scan";
            m_sFailureReason = "Fingerprint did not match";
            break;
        case MATCH_SWIPE_TOO_SHORT:
            retry            = true;
            m_sPrompt        = "Swipe too short - try again";
            m_sFailureReason = "Swipe too short - try again";
            break;
        case MATCH_FINGER_NOT_CENTERED:
            retry            = true;
            m_sPrompt        = "Finger not centered - try again";
            m_sFailureReason = "Finger not centered - try again";
            break;
        case MATCH_REMOVE_AND_RETRY:
            retry            = true;
            m_sPrompt        = "Remove your finger and try again";
            m_sFailureReason = "Remove your finger and try again";
            break;
        case MATCH_DISCONNECTED:
            m_sFailureReason   = "Fingerprint device disconnected";
            m_sDBUSState.abort = true;
            break;
    }

    if (!authenticated && !m_sFailureReason.empty())
        g_pAuth->enqueueFail(m_sFailureReason, AUTH_IMPL_FINGERPRINT);
    else if (retry)
        g_pHyprlock->enqueueForceUpdateTimers();

    if (done || m_sDBUSState.abort)
        m_sDBUSState.done = true;
}

void CFingerprint::claimDevice() {
    const auto currentUser = ""; // Empty string means use the caller's id.
    m_sDBUSState.device->callMethodAsync("Claim").onInterface(DEVICE).withArguments(currentUser).uponReplyInvoke([this](std::optional<sdbus::Error> e) {
        if (e) {
            Log::logger->log(Log::WARN, "fprint: could not claim device, {}", e->what());
            m_sDBUSState.verifying = false;
            releaseDevice();
            if (m_sDBUSState.hardWaking)
                scheduleWakePoll();
            else if (m_sDBUSState.userRearm)
                beginHardWake();
            else
                m_sDBUSState.waking = false;
        } else {
            Log::logger->log(Log::INFO, "fprint: claimed device");
            startVerify();
        }
    });
}

void CFingerprint::startVerify(bool isRetry) {
    m_sDBUSState.verifying = true;
    if (!m_sDBUSState.device) {
        if (!createDeviceProxy())
            return;

        claimDevice();
        return;
    }
    auto finger = "any"; // Any finger.
    m_sDBUSState.device->callMethodAsync("VerifyStart").onInterface(DEVICE).withArguments(finger).uponReplyInvoke([this, isRetry](std::optional<sdbus::Error> e) {
        if (e) {
            Log::logger->log(Log::WARN, "fprint: could not start verifying, {}", e->what());
            m_sDBUSState.verifying = false;
            if (isRetry)
                m_sFailureReason = "Fingerprint auth disabled (failed to restart)";
            else if (m_sDBUSState.hardWaking) {
                releaseDevice();
                scheduleWakePoll();
            } else if (m_sDBUSState.userRearm)
                beginHardWake();
            else
                m_sDBUSState.waking = false;
        } else {
            Log::logger->log(Log::INFO, "fprint: started verifying");
            m_sDBUSState.waking     = false;
            m_sDBUSState.hardWaking = false;
            m_sDBUSState.userRearm  = false;
            if (isRetry) {
                m_sDBUSState.retries++;
                m_sPrompt = "Could not match fingerprint. Try again.";
            } else
                m_sPrompt = m_sFingerprintReady;
        }
        g_pHyprlock->enqueueForceUpdateTimers();
    });
}

bool CFingerprint::stopVerify() {
    m_sDBUSState.verifying = false;
    if (!m_sDBUSState.device)
        return false;
    try {
        m_sDBUSState.device->callMethod("VerifyStop").onInterface(DEVICE);
    } catch (sdbus::Error& e) {
        Log::logger->log(Log::WARN, "fprint: could not stop verifying, {}", e.what());
        return false;
    }
    Log::logger->log(Log::INFO, "fprint: stopped verification");
    return true;
}

bool CFingerprint::releaseDevice() {
    if (!m_sDBUSState.device)
        return false;
    try {
        m_sDBUSState.device->callMethod("Release").onInterface(DEVICE);
    } catch (sdbus::Error& e) {
        Log::logger->log(Log::WARN, "fprint: could not release device, {}", e.what());
        m_sDBUSState.device.reset();
        return false;
    }

    m_sDBUSState.device.reset();
    Log::logger->log(Log::INFO, "fprint: released device");
    return true;
}
