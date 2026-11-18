# Change-point sub-blocking

A processor renders whole blocks, but a modulated parameter may move inside one.
Reading every parameter at every sample would cost a call per sample; reading it once
per block would quantise every automation curve to the block size. The modulation
matrix records the samples where a modulated value changes, its *change points*, and
the processor renders the block in pieces that start there.

## How a block is rendered

On the audio thread, per block:

```cpp
// the read scope keeps the matrix's buffers alive until the block is done
auto scope = matrix.read_scope();
matrix.process_with_scope(scope.data(), num_samples);
processor.process_modulated(buffer);  // SmartHandle reads are safe here
```

1. `process_with_scope()` runs every source and routing. A source calls
   `record_change_point(i)` wherever its output moves; a routing carries those points
   to its target parameter.
2. `BaseProcessor::process_modulated(buffer)` asks the processor's
   `get_change_points()` where to cut. A processor that reads several parameters merges
   their lists with `collect_change_points()`, which returns them sorted and without
   duplicates and does not allocate: reserve `samples_per_block` in `prepare()`.
3. `split_and_process()` calls `process(buffer.sub_block(pos, cp - pos), pos)` for
   every change point after the last cut, then once for the tail. A point at sample 0,
   a repeat or a point past the end cuts nothing.
4. Inside `process()`, the processor reads each parameter at `modulation_offset`
   (`SmartHandle::load(offset)`: the parameter's base value plus the modulation at that
   sample) and ramps its smoothers toward it.

The processor itself never sees the matrix: it declares a `Parameter` enum and a
`get_parameter_float(Parameter, modulation_offset)` that the application answers from
`SmartHandle`s, as `TestLimiter` does in `test/modulation/test_Integration.cpp`.

## Try it

A 512-sample block with two parameters. `threshold` follows an LFO that steps every
*decimation* samples. `attack` follows a UI control whose events land at `i · 512 / n`,
which is how `InputEventQueue::drain_spread()` places the *n* events of one stream (see
{doc}`modulation`). The processor reads both, so it cuts at the union of their change
points.

```{raw} html
<div class="tanh-block-split" data-block-size="512"></div>
```

Orange ticks are change points; the dashed tick at sample 0 cuts nothing, because the
first piece starts there anyway. The shaded pieces are the `process()` calls. The
stepped line is `threshold` as read at each piece's `modulation_offset`, and the orange
line is the same value after a 48-sample linear smoother inside `process()`. A cut made
by an `attack` event re-reads `threshold` too, and finds it unchanged.
