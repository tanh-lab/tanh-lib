# Modulation: offsets and chains

A routing moves its target by what its source outputs times its depth. Two
fields of `ModulationRouting` extend that, for Additive routings and the
Multiply combine mode.

## Offset

`m_offset` is added to the routing's contribution, in the depth's units (a
fraction of the target's range for `DepthMode::Normalized`):

```
contribution = source * depth + offset
```

A source in -1 ... 1 with `offset = depth` contributes 0 ... 2 depth:
modulation that only goes one way (a "unipolar" assignment), without a second
routing. `ModulationMatrix::update_routing_offset()` changes it without a
rebuild, like `update_routing_depth()`. Replace and ReplaceHold ignore it.

## Chains

Additive routings on a target are summed, in any order. A target with at
least one `CombineMode::Multiply` routing is a chain instead: its Additive and
Multiply routings are folded sample by sample, in `m_chain_position` order
(ties by routing ID), in normalised units:

```
value = contribution of the first enabled routing
for each further routing:
    Additive: value += contribution
    Multiply: value *= contribution
the target moves by value (of its range)
```

So `a + b * c` and `(a + b) * c` are different chains of the same routings.
A routing whose source is inactive at a sample contributes the operation's
identity there (0 when adding, 1 when multiplying); a disabled routing, or one
skipped during a gesture, leaves the chain for that block.

How it runs: a chain target's routings write their contributions to buffers of
their own while the schedule runs; after the schedule the fold pass combines
them into the target's additive buffer, before the host hand-over and the
change-point pass. Targets without a Multiply routing take the usual path.

Limits: a chain is mono. A Multiply routing on a polyphonic target (a scoped
source, or a global source into a target with scoped routings) adds instead,
with a logged warning. Chains are folded in normalised units, so a routing in
`DepthMode::Absolute` is converted by the target's range first.

Serialisation: `"combine_mode": "multiply"`, `"offset"` and
`"chain_position"` (the last two only when not 0, so older readers and presets
are unaffected).
