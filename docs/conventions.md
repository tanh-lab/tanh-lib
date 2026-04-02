# C++ Naming Conventions

- **Classes/structs/enums/unions**: `PascalCase`
- **Methods/functions**: `snake_case`
- **Member variables**: `m_` prefix + `snake_case`, e.g. `m_frequency_range`
- **Local variables/parameters**: `snake_case`
- **Constants** (`constexpr`, `static const`, global `const`): `k_` prefix + `snake_case`, e.g. `k_max_grains`
- **Enum values**: `PascalCase`, e.g. `ResonatorModal`, `LFOWaveform::Sine`
- **Macros**: `ALL_CAPS_WITH_UNDERSCORES`, e.g. `TANH_NONBLOCKING_FUNCTION`
- **Folders**: `kebab-case`, e.g. `audio-io`
