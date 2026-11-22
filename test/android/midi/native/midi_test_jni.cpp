// JNI surface of the Android MIDI test app: thl::midi::DeviceManager as the Java test
// (io.github.tanhlab.miditest.MidiTest) sees it.
#include <jni.h>
#include <tanh/core/Clock.h>
#include <tanh/midi/AndroidMidi.h>
#include <tanh/midi/DeviceInfo.h>
#include <tanh/midi/DeviceManager.h>
#include <tanh/midi/Ump.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<thl::midi::DeviceManager> g_manager;
std::atomic<int> g_changes{0};

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    // What every app using tanh::Midi on Android does.
    thl::midi::set_android_java_vm(vm);
    return JNI_VERSION_1_6;
}

JNIEXPORT jboolean JNICALL Java_io_github_tanhlab_miditest_Native_start(JNIEnv* /*env*/,
                                                                        jclass /*clazz*/,
                                                                        jboolean midi2,
                                                                        jboolean auto_open) {
    thl::midi::DeviceManagerConfig config;
    config.m_protocol = midi2 == JNI_TRUE ? thl::midi::Protocol::Midi2 : thl::midi::Protocol::Midi1;
    config.m_auto_open = auto_open == JNI_TRUE;
    g_manager = std::make_unique<thl::midi::DeviceManager>(config);
    g_manager->set_device_change_callback([] { g_changes.fetch_add(1); });
    return g_manager->running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_io_github_tanhlab_miditest_Native_stop(JNIEnv* /*env*/,
                                                                   jclass /*clazz*/) {
    g_manager.reset();
}

JNIEXPORT jstring JNICALL Java_io_github_tanhlab_miditest_Native_devices(JNIEnv* env,
                                                                         jclass /*clazz*/) {
    std::string text;
    if (g_manager) {
        for (const thl::midi::DeviceInfo& device : g_manager->input_devices()) {
            text += std::to_string(device.m_id) + '\t' + device.m_name + '\t' +
                    device.m_manufacturer + '\t' +
                    std::to_string(static_cast<int>(device.m_transport)) + '\t' +
                    std::to_string(static_cast<int>(device.m_protocol)) + '\t' +
                    (device.m_open ? "1" : "0") + '\t' + std::to_string(device.m_source) + '\t' +
                    device.m_persistent_key + '\n';
        }
    }
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT jboolean JNICALL Java_io_github_tanhlab_miditest_Native_open(JNIEnv* /*env*/,
                                                                       jclass /*clazz*/,
                                                                       jint id) {
    return g_manager && g_manager->open_input(static_cast<std::uint32_t>(id)) ? JNI_TRUE
                                                                              : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_io_github_tanhlab_miditest_Native_close(JNIEnv* /*env*/,
                                                                        jclass /*clazz*/,
                                                                        jint id) {
    return g_manager && g_manager->close_input(static_cast<std::uint32_t>(id)) ? JNI_TRUE
                                                                               : JNI_FALSE;
}

JNIEXPORT jlongArray JNICALL Java_io_github_tanhlab_miditest_Native_drain(JNIEnv* env,
                                                                          jclass /*clazz*/) {
    std::vector<jlong> values;
    if (g_manager) {
        g_manager->input_queue().drain([&](const thl::midi::TimedUmp& packet) {
            values.push_back(packet.m_time_ns);
            values.push_back(packet.m_source);
            values.push_back(packet.m_ump.m_size);
            for (const std::uint32_t word : packet.m_ump.m_words) { values.push_back(word); }
        });
    }
    const jlongArray array = env->NewLongArray(static_cast<jsize>(values.size()));
    env->SetLongArrayRegion(array, 0, static_cast<jsize>(values.size()), values.data());
    return array;
}

JNIEXPORT jint JNICALL Java_io_github_tanhlab_miditest_Native_changes(JNIEnv* /*env*/,
                                                                      jclass /*clazz*/) {
    return g_changes.load();
}

JNIEXPORT jlong JNICALL Java_io_github_tanhlab_miditest_Native_dropped(JNIEnv* /*env*/,
                                                                       jclass /*clazz*/) {
    return g_manager ? static_cast<jlong>(g_manager->input_queue().dropped()) : 0;
}

JNIEXPORT jlong JNICALL Java_io_github_tanhlab_miditest_Native_uptimeNs(JNIEnv* /*env*/,
                                                                        jclass /*clazz*/) {
    return thl::core::uptime_ns();
}

}  // extern "C"
