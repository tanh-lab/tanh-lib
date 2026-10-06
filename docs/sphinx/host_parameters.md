# Modulation with host parameters (JUCE)

`ModulationMatrix` reads the parameters it modulates through a
`thl::modulation::ParameterBackend`. `thl::State` is one backend
(`ModulationMatrix(thl::State&)`), but a host that already owns its parameters,
such as a JUCE plugin with an `AudioProcessorValueTreeState` (APVTS), hands
them to the matrix with its own backend (`ModulationMatrix(ParameterBackend&)`).
tanh-lib stays JUCE-free: the adapter lives in the plugin. Modulation builds
without the State component for exactly this case (`TANH_BUILD_STATE=OFF`).

There is one parameter store, the host's. The matrix keeps pointers to the
host's base-value atomics and reads them with relaxed loads; it never copies
them and never writes modulated values back. `SmartHandle::load(offset)`
returns *base value + modulation at that sample*, while the host keeps seeing
the unmodulated value, so automation and presets stay clean.

## The backend

A backend answers one question: does a parameter with this key exist, and
where does its base value live?

```cpp
struct ParameterBinding {
    ParameterDefinition m_def;                        // range, type, flags, scope
    ParameterBaseValue m_base;                        // std::atomic<float/double/int/bool>*
    const std::atomic<bool>* m_in_gesture = nullptr;  // nullptr: never in a gesture
};

class ParameterBackend {
public:
    virtual std::optional<ParameterBinding> find(std::string_view key) const = 0;
};
```

- `find()` is called only from non-real-time matrix methods
  (`get_smart_handle()`, `add_routing()`, ...) under the matrix's writer mutex,
  once per key; it may allocate and lock.
- The atomics must keep their address for the lifetime of the matrix (APVTS
  atomics do).
- `m_def` needs a `Range`, the parameter type and `modulatable(true)` for every
  parameter that may be a routing target.
- The `m_base` alternative is the type you request a handle for:
  `SmartHandle<T>` needs `std::atomic<T>*` (`thl::parameter_type_of<T>()` maps
  the C++ type to a `ParameterType`).
- `m_in_gesture` lets routings with `m_skip_during_gesture` pause while the user
  drags the control.

### A JUCE adapter (sketch)

```cpp
std::optional<thl::modulation::ParameterBinding> find(std::string_view key) const override {
    const juce::String id(key.data(), key.size());
    auto* parameter = m_parameters.getParameter(id);
    if (parameter == nullptr) { return std::nullopt; }

    const auto& range = m_parameters.getParameterRange(id);
    thl::modulation::ParameterBinding binding;
    binding.m_def = thl::ParameterDefinition::make_float(
                        parameter->getName(64).toStdString(), to_tanh_range(range),
                        range.convertFrom0to1(parameter->getDefaultValue()))
                        .modulatable(true);
    binding.m_base = m_parameters.getRawParameterValue(id);  // std::atomic<float>*
    binding.m_in_gesture = &m_gestures[static_cast<size_t>(parameter->getParameterIndex())];
    return binding;
}
```

`m_gestures` is one `std::atomic<bool>` per processor parameter, set from a
`juce::AudioProcessorParameter::Listener`'s `parameterGestureChanged()`.

Ranges: JUCE maps a normalised `p` to `start + (end - start) * p^(1/skew)`,
tanh's power law uses `p^skew`, so invert the skew:

```cpp
thl::Range to_tanh_range(const juce::NormalisableRange<float>& range) {
    if (juce::exactlyEqual(range.skew, 1.0f)) {
        return thl::Range::linear(range.start, range.end, range.interval);
    }
    return thl::Range::power_law(range.start, range.end, 1.0f / range.skew, range.interval);
}
```

A `NormalisableRange` with custom conversion lambdas has no tanh equivalent;
keep modulatable parameters linear or skewed. All APVTS values are float
atomics, so bind them as `Float`, request `SmartHandle<float>` and round int
and choice values where they are used.

## Setting up the matrix

Declare the parameters before the backend and the backend before the matrix,
so the atomics outlive every handle:

```cpp
juce::AudioProcessorValueTreeState m_parameters;
ApvtsParameterBackend m_backend{m_parameters};
thl::modulation::ModulationMatrix m_matrix{m_backend};

// once, on the message thread
m_matrix.add_source("lfo", &m_lfo);
auto gain = m_matrix.get_smart_handle<float>("gain");
m_matrix.add_routing(thl::modulation::ModulationRouting(
    "lfo", "gain", /*depth=*/6.0f, /*max_decimation=*/0, thl::modulation::DepthMode::Absolute));
```

`prepare()` the matrix together with the DSP in `prepareToPlay`.

## Per block

Hosts may call the render callback from different threads. Use the matrix's
own audio reader slot, which needs no per-thread registration and never
allocates or locks:

```cpp
void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    const auto scope = m_matrix.audio_read_scope();
    m_matrix.process_with_scope(scope.data(), static_cast<size_t>(buffer.getNumSamples()));
    m_processors.process(buffer);  // reads gain.load(offset) / gain.change_points()
}                                  // scope closes here
```

Keep the scope open while the processors read their `SmartHandle`s: the
modulation buffers are RCU-protected. With [XY controllers](xy_controller.md),
call `set_transport()` on each before the matrix runs.

## Threading contract

- Backend `find()`, `get_smart_handle()`, `add_routing()`, `prepare()`:
  message thread.
- `audio_read_scope()`, `process()` / `process_with_scope()`, `SmartHandle`
  reads: audio thread, one thread at a time (the normal plugin contract).
- `read_scope()` and `ensure_thread_registered()` remain for other readers, for
  example a UI timer showing modulation state.

## Pitfalls

- **Base values.** The matrix only reads the base atomics. Write base values
  through the host's API (`setValueNotifyingHost` on the message thread), never
  by storing into the atomic: that bypasses automation, undo and listeners.
  Recorded gestures and other movement belong in modulation sources.
- **Silencing a routing.** `update_routing_depth(id, 0)` silences an Additive
  routing but not a Replace one (it still writes `src * 0` and wins the target).
  Use `set_routing_enabled(id, false)`: the routing writes nothing, its held
  value is dropped and nothing is rebuilt. `set_routings_enabled(batch)` flips
  several so that no block sees half of the change.
- **Block size.** `process()` takes at most the prepared `samples_per_block`;
  split larger host blocks (see [Transport](transport.md), "Block size").
- **Saving.** With a host backend, `to_json()` / `from_json()` contain the
  routings only (`"enabled": false` for disabled ones). Store that JSON next to
  the APVTS tree in `getStateInformation`.
- **Errors.** `get_smart_handle<T>(key)` throws
  `thl::StateKeyNotFoundException` for an unknown key,
  `thl::ParameterTypeMismatchException` when `T` does not match the bound
  atomic and `std::invalid_argument` when the parameter is not modulatable, on
  every backend. `tanh/state/Exceptions.h` is installed without State too.
- **No State.** `ModulationMatrix(thl::State&)` and `state()` exist only when
  tanh is built with State; `state()` throws `std::logic_error` on a matrix
  built from a backend. A `SmartHandle`'s metadata accessors (`def()`,
  `range()`, `key()`) and `load_normalized()` need a handle from
  `get_smart_handle()`.
