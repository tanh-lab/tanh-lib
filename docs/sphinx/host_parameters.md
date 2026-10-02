# Modulation with host parameters (JUCE)

`ModulationMatrix` reads the parameters it modulates through a
`thl::modulation::ParameterBackend`. `thl::State` is one backend
(`ModulationMatrix(thl::State&)`), but a host that already owns its
parameters — a JUCE plugin with an `AudioProcessorValueTreeState` (APVTS) —
can hand them to the matrix directly with its own backend
(`ModulationMatrix(ParameterBackend&)`). tanh-lib stays JUCE-free: the adapter
lives in the plugin.

## No mirroring

There is still exactly one parameter store: the APVTS. The matrix keeps only
pointers to the APVTS atomics and reads them with relaxed loads on the audio
thread; it never copies them into a `State` and never writes modulated values
back.

| Owned by the host (APVTS) | Owned by the matrix |
|---|---|
| base value of every parameter (`std::atomic<float>`) | routings, sources, modulation buffers, change points |
| host automation, undo, saved state | RCU-published processing config |

Per block, `SmartHandle::load(offset)` returns *APVTS base value + modulation
at that sample*. The host keeps seeing the unmodulated base value, so
automation and presets stay clean.

## The backend

A backend answers one question: does a parameter with this key exist, and
where does its base value live?

```cpp
struct ParameterBinding {
    ParameterDefinition m_def;                        // range, type, flags, scope
    ParameterBaseValue m_base;                        // std::atomic<float/double/int/bool>*
    const std::atomic<bool>* m_in_gesture = nullptr;  // nullptr: never in a gesture
    void* m_state_record = nullptr;                   // set by the State backend only
};

class ParameterBackend {
public:
    virtual std::optional<ParameterBinding> find(std::string_view key) const = 0;
};
```

Rules for a host backend:

- The atomics must keep their address for the lifetime of every matrix built
  on the backend (APVTS atomics do), and the host must allow writes through
  them (see `store_base()` below).
- `m_def` needs a `Range`, the parameter type and `modulatable(true)` for
  every parameter that may be a routing target.
- `m_in_gesture` lets routings with `m_skip_during_gesture` pause while the
  user drags the control.

### A JUCE adapter

```cpp
class ApvtsParameterBackend final : public thl::modulation::ParameterBackend,
                                    private juce::AudioProcessorParameter::Listener {
public:
    explicit ApvtsParameterBackend(juce::AudioProcessorValueTreeState& parameters)
        : m_parameters(parameters)
        , m_gestures(std::make_unique<std::atomic<bool>[]>(
              static_cast<size_t>(parameters.processor.getParameters().size()))) {
        for (auto* p : m_parameters.processor.getParameters()) { p->addListener(this); }
    }

    ~ApvtsParameterBackend() override {
        for (auto* p : m_parameters.processor.getParameters()) { p->removeListener(this); }
    }

    std::optional<thl::modulation::ParameterBinding> find(std::string_view key) const override {
        const juce::String id(key.data(), key.size());
        auto* parameter = m_parameters.getParameter(id);
        if (parameter == nullptr) { return std::nullopt; }

        const auto& range = m_parameters.getParameterRange(id);
        return thl::modulation::ParameterBinding{
            .m_def = thl::ParameterDefinition::make_float(
                         parameter->getName(64).toStdString(),
                         to_tanh_range(range),
                         range.convertFrom0to1(parameter->getDefaultValue()))
                         .modulatable(true),
            .m_base = m_parameters.getRawParameterValue(id),
            .m_in_gesture = &m_gestures[static_cast<size_t>(parameter->getParameterIndex())],
        };
    }

private:
    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int index, bool starting) override {
        m_gestures[static_cast<size_t>(index)].store(starting, std::memory_order_relaxed);
    }

    juce::AudioProcessorValueTreeState& m_parameters;
    std::unique_ptr<std::atomic<bool>[]> m_gestures;  // one per processor parameter
};
```

Ranges: JUCE maps a normalised value `p` to `start + (end - start) * p^(1/skew)`,
tanh's power law uses `p^skew`, so the skew is inverted:

```cpp
thl::Range to_tanh_range(const juce::NormalisableRange<float>& range) {
    if (juce::exactlyEqual(range.skew, 1.0f)) {
        return thl::Range::linear(range.start, range.end, range.interval);
    }
    return thl::Range::power_law(range.start, range.end, 1.0f / range.skew, range.interval);
}
```

A `NormalisableRange` with custom conversion lambdas has no tanh equivalent
(`Range::custom` takes plain function pointers); keep modulatable parameters
linear or skewed. All APVTS parameters are float atomics, so bind them as
`Float` and request `SmartHandle<float>`; round int and choice values where
they are used.

## Setting up the matrix

Declare the APVTS before the backend and the backend before the matrix, so the
atomics outlive every handle:

```cpp
juce::AudioProcessorValueTreeState m_parameters;
ApvtsParameterBackend m_backend{m_parameters};
thl::modulation::ModulationMatrix m_matrix{m_backend};

// once, on the message thread
m_matrix.add_source("lfo", &m_lfo);
auto gain = m_matrix.get_smart_handle<float>("gain");
m_matrix.add_routing({.m_source_id = "lfo", .m_target_id = "gain", .m_depth = 6.0f,
                      .m_depth_mode = thl::modulation::DepthMode::Absolute});
```

`prepare()` the matrix together with the DSP in `prepareToPlay`.

## Per block

Hosts may call the render callback from a different thread every block
(Logic, AUv3 hosts). Use the matrix's own audio reader slot, which needs no
per-thread registration and never allocates or locks:

```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    const auto scope = m_matrix.audio_read_scope();
    m_matrix.process_with_scope(scope.data(), static_cast<size_t>(buffer.getNumSamples()));
    m_processors.process(buffer);  // reads gain.load(offset) / gain.change_points()
}                                  // scope closes here
```

With XY controllers ([XY controller](xy_controller.md)), give every controller
the block's transport before the matrix runs; `m_num_samples` is the block
length its driver step renders:

```cpp
const auto t = m_clock.block_info();           // HostTransportClock fed from the playhead
for (auto& pad : m_pads) { pad->set_transport(t); }
m_matrix.process_with_scope(scope.data(), t.m_num_samples);
```

Keep the scope open while the processors read their `SmartHandle`s: the
modulation buffers they point into are RCU-protected. The audio path must not
run on two threads at the same time — the normal plugin contract.
`ensure_thread_registered()` and `read_scope()` remain for other threads
(e.g. a UI timer reading modulation state).

## Writing base values

`SmartHandle::store_base(value)` writes the base value directly (real-time
safe, relaxed, no notification) — the counterpart of
`thl::ParameterHandle::store()`. For APVTS host parameters, write through the
APVTS instead (`setValueNotifyingHost` on the message thread): a direct store
bypasses host automation, undo and listeners. Recorded gestures and other
movement belong in modulation sources, not in base-value writes.

## Switching routings on and off

`update_routing_depth(id, 0)` silences an Additive routing but not a Replace
one (it still writes `src * 0` and wins the target). Use
`set_routing_enabled(id, false)` instead: the routing writes nothing, its held
value is dropped, and nothing is rebuilt. `set_routings_enabled(batch)` flips
several at once so that no block sees half of the change — e.g. a pad mode
switch (`XYModeRouter`).

## Saving

With a host backend, `ModulationMatrix::to_json()` / `from_json()` contain the
routings only (including `"enabled": false` for disabled ones); the parameters
are saved by the host (APVTS state). Store the
routing JSON next to the APVTS tree in `getStateInformation`.

## Errors

Both constructors share one error contract: `get_smart_handle<T>(key)` throws
`thl::StateKeyNotFoundException` for an unknown key and
`thl::ParameterTypeMismatchException` when `T` does not match the bound
atomic. `Exceptions.h` is available without the State component.

`SmartHandle::raw_handle()` is deprecated: it returns the `thl::ParameterHandle`
for State-backed handles and `std::nullopt` otherwise, and is only compiled
when the State component is built. Use `store_base()` / `load_base()`, or
`State::get_handle<T>(key)`.
