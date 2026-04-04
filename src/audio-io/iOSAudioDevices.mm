#if defined(THL_PLATFORM_IOS)

#include <tanh/audio-io/iOSAudioDevices.h>
#include <tanh/audio-io/AudioDeviceManager.h>
#include <tanh/core/Logger.h>
#import <AVFoundation/AVAudioSession.h>

static constexpr AVAudioSessionCategoryOptions k_base_session_options =
    AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowAirPlay |
    AVAudioSessionCategoryOptionMixWithOthers;

namespace thl {

void configure_ios_audio_session() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions options =
        k_base_session_options | AVAudioSessionCategoryOptionAllowBluetoothA2DP;

    // On iOS 26+ enable high-quality BLE recording automatically.
    if (@available(iOS 26.0, *)) {
        options |= AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    }

    NSError* error = nil;
    [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
}

bool is_ios_bluetooth_route_active() {
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

bool is_ios_classic_bluetooth_connected() {
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

bool set_ios_bluetooth_profile(BluetoothProfile profile, const char** out_profile_name) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions options = k_base_session_options;
    const char* profile_name = "HFP";

    switch (profile) {
        case BluetoothProfile::HFP:
            options |= AVAudioSessionCategoryOptionAllowBluetoothHFP;
            profile_name = "HFP";
            break;
        case BluetoothProfile::A2DP:
            options |= AVAudioSessionCategoryOptionAllowBluetoothA2DP;
            profile_name = "A2DP";
            break;
    }

    // On iOS 26+ always enable high-quality BLE recording regardless of profile.
    if (@available(iOS 26.0, *)) {
        options |= AVAudioSessionCategoryOptionBluetoothHighQualityRecording;
    }

    if (out_profile_name) { *out_profile_name = profile_name; }

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
        profile_name,
        session.sampleRate,
        session.IOBufferDuration);
    return true;
}

std::vector<AudioRouteInfo> get_ios_available_input_routes() {
    std::vector<AudioRouteInfo> routes;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        AudioRouteInfo info;
        info.m_name = [port.portName UTF8String];
        info.m_port_type = [port.portType UTF8String];
        info.m_uid = [port.UID UTF8String];
        routes.push_back(std::move(info));
    }
    return routes;
}

bool set_ios_preferred_input_route(const AudioRouteInfo& route) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSString* targetUID = [NSString stringWithUTF8String:route.m_uid.c_str()];

    for (AVAudioSessionPortDescription* port in session.availableInputs) {
        if ([port.UID isEqualToString:targetUID]) {
            NSError* error = nil;
            BOOL ok = [session setPreferredInput:port error:&error];
            if (!ok) {
                thl::Logger::logf(
                    thl::Logger::LogLevel::Error,
                    "thl.audio_io.ios_devices",
                    "Failed to set preferred input '%s': %s",
                    route.m_name.c_str(),
                    error ? [[error localizedDescription] UTF8String] : "unknown error");
                return false;
            }
            thl::Logger::logf(thl::Logger::LogLevel::Info,
                              "thl.audio_io.ios_devices",
                              "Preferred input set to '%s' (%s)",
                              route.m_name.c_str(),
                              route.m_port_type.c_str());
            return true;
        }
    }

    thl::Logger::logf(thl::Logger::LogLevel::Error,
                      "thl.audio_io.ios_devices",
                      "Input route '%s' (uid=%s) not found in available inputs",
                      route.m_name.c_str(),
                      route.m_uid.c_str());
    return false;
}

bool override_ios_output_to_speaker(bool to_speaker) {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    AVAudioSessionPortOverride override =
        to_speaker ? AVAudioSessionPortOverrideSpeaker : AVAudioSessionPortOverrideNone;
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
                      to_speaker ? "speaker" : "none (default route)");
    return true;
}

std::string get_ios_current_input_route_name() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSArray<AVAudioSessionPortDescription*>* inputs = session.currentRoute.inputs;
    if (inputs.count > 0) { return [inputs[0].portName UTF8String]; }
    return {};
}

std::string get_ios_current_output_route_name() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSArray<AVAudioSessionPortDescription*>* outputs = session.currentRoute.outputs;
    if (outputs.count > 0) { return [outputs[0].portName UTF8String]; }
    return {};
}

}  // namespace thl

#endif  // THL_PLATFORM_IOS
