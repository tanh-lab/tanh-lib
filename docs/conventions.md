# Naming Conventions

## C/C++

- **Classes/structs/enums/unions**: `PascalCase`
- **Methods/functions**: `snake_case`
- **Member variables**: `m_` prefix + `snake_case`, e.g. `m_frequency_range`
- **Local variables/parameters**: `snake_case`
- **Constants** (`constexpr`, `static const`, global `const`): `k_` prefix + `snake_case`, e.g. `k_max_grains`
- **Enum values**: `PascalCase`, e.g. `ResonatorModal`, `LFOWaveform::Sine`
- **Macros**: `ALL_CAPS_WITH_UNDERSCORES`, e.g. `TANH_NONBLOCKING_FUNCTION`
- **Folders**: `kebab-case`, e.g. `audio-io`
- **JSON keys exposed to JS/TS**: `camelCase` — C++ code emitting JSON for the JS bridge must use camelCase keys to match the TypeScript interfaces

## TypeScript / TSX Conventions

- **Variables/functions/parameters**: `camelCase`, e.g. `activeEngine`, `setLoadedSamplePath`
- **React components**: `PascalCase`, functional only, arrow function exports, e.g. `export const EnginePadView = () => {...}`
- **Types/interfaces**: `PascalCase`, no `I` or `T` prefix, e.g. `type ParameterDefinition`, `interface AudioDeviceInfo`
- **Constants**: `UPPER_SNAKE_CASE`, e.g. `NUM_ENGINES`, `SAMPLE_RATE`
- **Theme tokens**: `camelCase` object keys, e.g. `colors`, `spacing`, `radii`
- **Enums**: `PascalCase` declaration, `PascalCase` members, e.g. `LogLevel.Error`
- **Hooks**: `use` prefix, e.g. `useADSRGesture`, `useSampleBrowser`
- **Stores (Zustand)**: `use` prefix, e.g. `useFrontendStore`, `useParameterStore`
- **Files**: `.tsx` for components, `.ts` for utilities/services/stores/hooks
- **Exports**: named exports (`export const`, `export type`), no default exports
- **State management**: Zustand for global state, `useState` for local, SharedValue (reanimated) for animations, refs for per-frame rendering
