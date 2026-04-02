#if defined(THL_PLATFORM_IOS)

#include <tanh/audio-io/iOSAudioDevices.h>
#include <tanh/audio-io/AudioDeviceManager.h>
#include <tanh/core/Logger.h>
#import <AVFoundation/AVAudioSession.h>

static constexpr AVAudioSessionCategoryOptions kBaseSessionOptions =
    AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowAirPlay |
    AVAudioSessionCategoryOptionMixWithOthers;

namespace thl {

void configureIOSAudioSession() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions options =
        kBaseSessionOptions | AVAudioSessionCategoryOptionAllowBluetoothA2DP;

    // On iOS 26+ enable high-quality BLE recording automatically.
    if (@available(iOS 26.0, *)) {
        options |= AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    }

    NSError* error = nil;
    [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
}

bool isIOSBluetoothRouteActive() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothHFP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
            return true;
        }
    }
    for (AVAudioSessionPortDescription* port in session.currentRoute.inputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothHFP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothLE]) {
            return true;
        }
    }
    return false;
}

bool isIOSClassicBluetoothConnected() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    // Check available inputs for classic BT (HFP) ports.
    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothHFP]) { return true; }
    }
    // Check current route outputs (A2DP devices don't appear in availableInputs).
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
        if ([port.portType isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
            [port.portType isEqualToString:AVAudioSessionPortBluetoothHFP]) {
            return true;
        }
    }
    return false;
}

bool setIOSBluetoothProfile(BluetoothProfile profile, const char** outProfileName) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions options = kBaseSessionOptions;
    const char* profileName = "HFP";

    switch (profile) {
        case BluetoothProfile::HFP:
            options |= AVAudioSessionCategoryOptionAllowBluetoothHFP;
            profileName = "HFP";
            break;
        case BluetoothProfile::A2DP:
            options |= AVAudioSessionCategoryOptionAllowBluetoothA2DP;
            profileName = "A2DP";
            break;
    }

    // On iOS 26+ always enable high-quality BLE recording regardless of profile.
    if (@available(iOS 26.0, *)) {
        options |= AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    }

    if (outProfileName) { *outProfileName = profileName; }

    NSError* error = nil;
    BOOL ok = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                       withOptions:options
                             error:&error];

    if (!ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.ios_devices",
                          "setBluetoothProfile: failed to set session category — %s",
                          error ? [[error localizedDescription] UTF8String] : "unknown error");
        return false;
    }

    ok = [session setActive:YES error:&error];
    if (!ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.ios_devices",
                          "setBluetoothProfile: failed to activate session — %s",
                          error ? [[error localizedDescription] UTF8String] : "unknown error");
        return false;
    }

    thl::Logger::logf(
        thl::Logger::LogLevel::Info,
        "thl.audio_io.ios_devices",
        "Bluetooth profile set to %s (session sampleRate=%.0f, IOBufferDuration=%.4f)",
        profileName,
        session.sampleRate,
        session.IOBufferDuration);
    return true;
}

std::vector<AudioRouteInfo> getIOSAvailableInputRoutes() {
    std::vector<AudioRouteInfo> routes;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        AudioRouteInfo info;
        info.name = [port.portName UTF8String];
        info.portType = [port.portType UTF8String];
        info.uid = [port.UID UTF8String];
        routes.push_back(std::move(info));
    }
    return routes;
}

bool setIOSPreferredInputRoute(const AudioRouteInfo& route) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSString* targetUID = [NSString stringWithUTF8String:route.uid.c_str()];

    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        if ([port.UID isEqualToString:targetUID]) {
            NSError* error = nil;
            BOOL ok = [session setPreferredInput:port error:&error];
            if (!ok) {
                thl::Logger::logf(
                    thl::Logger::LogLevel::Error,
                    "thl.audio_io.ios_devices",
                    "Failed to set preferred input '%s': %s",
                    route.name.c_str(),
                    error ? [[error localizedDescription] UTF8String] : "unknown error");
                return false;
            }
            thl::Logger::logf(thl::Logger::LogLevel::Info,
                              "thl.audio_io.ios_devices",
                              "Preferred input set to '%s' (%s)",
                              route.name.c_str(),
                              route.portType.c_str());
            return true;
        }
    }

    thl::Logger::logf(thl::Logger::LogLevel::Error,
                      "thl.audio_io.ios_devices",
                      "Input route '%s' (uid=%s) not found in available inputs",
                      route.name.c_str(),
                      route.uid.c_str());
    return false;
}

bool overrideIOSOutputToSpeaker(bool toSpeaker) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    AVAudioSessionPortOverride override =
        toSpeaker ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone;
    BOOL ok = [session overrideOutputAudioPort:override error:&error];
    if (!ok) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.ios_devices",
                          "Failed to override output port: %s",
                          error ? [[error localizedDescription] UTF8String] : "unknown error");
        return false;
    }
    thl::Logger::logf(thl::Logger::LogLevel::Info,
                      "thl.audio_io.ios_devices",
                      "Output port override: %s",
                      toSpeaker ? "speaker" : "none (default route)");
    return true;
}

std::string getIOSCurrentInputRouteName() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSArray<AVAudioSessionPortDescription*>* inputs = session.currentRoute.inputs;
    if (inputs.count > 0) { return [inputs[0].portName UTF8String]; }
    return {};
}

std::string getIOSCurrentOutputRouteName() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSArray<AVAudioSessionPortDescription*>* outputs = session.currentRoute.outputs;
    if (outputs.count > 0) { return [outputs[0].portName UTF8String]; }
    return {};
}

}  // namespace thl

#endif  // THL_PLATFORM_IOS
