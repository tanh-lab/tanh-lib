// sampler::SamplePlayer and its building blocks (SampleView reads,
// LoopRegion, zero crossings, LoopMarkers), driven like the granular voice
// drives them (see GranularTestHelpers.h).

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/sampler/LoopMarkers.h>
#include <tanh/dsp/sampler/LoopRegion.h>
#include <tanh/dsp/sampler/SamplePlayer.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/sampler/ZeroCrossing.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "GranularTestHelpers.h"

using namespace granular_test;
using thl::dsp::sampler::LoopRegion;

TEST(SamplePlayer, LoopWrapIsExactEqualPowerCrossfade) {
    // Region [0, 4800), Loop at 1000: over the last 480 frames before End
    // the head blends equal-power into the audio just before Loop (the copy
    // reads 3800 frames behind), so at End it already reads what follows
    // Loop and the jump is silent. Nothing past End is ever read.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.render(6016);

    for (size_t n = 0; n < 4320; ++n) { ASSERT_NEAR(rig.m_out[n], static_cast<float>(n), 1e-3f); }
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(4320 + k) + gain_in(k) * static_cast<float>(520 + k);
        ASSERT_NEAR(rig.m_out[4320 + k], expected, 0.05f) << "k=" << k;
    }
    // Past End only the head remains, continuing from Loop.
    for (size_t k = 0; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], static_cast<float>(1000 + k), 1e-3f) << "k=" << k;
    }
}

TEST(SamplePlayer, RetriggerInsideLoopFadeParksTheBlend) {
    // Same wrap, then a note-on 224 frames into the loop fade. The tail
    // keeps the blend it had (head and copy at their frozen gains) while it
    // fades out; the live head restarts at Start.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.render(4320 + 224);
    rig.m_player.note_on();
    rig.render(1024);

    constexpr size_t k_retrigger = 4320 + 224;
    // The blend of the last frame rendered before the note-on (k = 223).
    float const head_gain = gain_out(223);
    float const copy_gain = gain_in(223);
    for (size_t j = 0; j < k_fade; ++j) {
        float const tail =
            head_gain * static_cast<float>(4544 + j) + copy_gain * static_cast<float>(744 + j);
        float const expected = gain_in(j) * static_cast<float>(j) + gain_out(j) * tail;
        ASSERT_NEAR(rig.m_out[k_retrigger + j], expected, 0.1f) << "j=" << j;
    }
    ASSERT_NEAR(rig.m_out[k_retrigger + k_fade], static_cast<float>(k_fade), 1e-3f);
}

TEST(SamplePlayer, MarkerMoveInsideLoopFadeWaitsForTheWrap) {
    // End moves from 4800 to 4600 while the fade toward 4800 runs: the fade
    // finishes against the old End, the next pass loops at the new one.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.render(4416);
    rig.m_params.m_sample_end = 0.46f;
    rig.render(4096);

    for (size_t k = 96; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(4320 + k) + gain_in(k) * static_cast<float>(520 + k);
        ASSERT_NEAR(rig.m_out[4320 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = 0; k < 3120; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], static_cast<float>(1000 + k), 1e-3f) << "k=" << k;
    }
    // 3600 frames later the head reaches the new End (4600) and is back at Loop.
    ASSERT_NEAR(rig.m_out[4800 + 3600], 1000.0f, 1e-3f);
}

TEST(SamplePlayer, BankSwitchCrossfadesAndKeepsHeadPosition) {
    // Bank 1 is bank 0 + 1000. Switching mid-play keeps the head position:
    // the old bank fades out at the head, the new one fades in at the head.
    std::vector<thl::core::BufferF> banks;
    banks.push_back(make_ramp(1, 10000));
    thl::core::BufferF plus(1, 10000, k_sample_rate);
    float* d = plus.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f; }
    banks.push_back(std::move(plus));
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;
    rig.render(1024);

    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(2048 + k) + gain_in(k) * static_cast<float>(3048 + k);
        ASSERT_NEAR(rig.m_out[2048 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = k_fade; k < 1024; ++k) {
        ASSERT_NEAR(rig.m_out[2048 + k], static_cast<float>(3048 + k), 1e-3f) << "k=" << k;
    }
}

TEST(SamplePlayer, FourDiscontinuitiesInOneFadeStealTheOldestTail) {
    // Bank k = ramp + 1000k. Three bank switches 64 frames apart, then a
    // retrigger: the fourth discontinuity finds no free tail slot and steals
    // the one closest to done (the first). Every tail is parked with the
    // live head's fade-in gain at that moment: sin(64 theta) for all three
    // later ones.
    std::vector<thl::core::BufferF> banks;
    for (int k = 0; k < 4; ++k) {
        thl::core::BufferF b(1, 10000, k_sample_rate);
        float* d = b.get_write_pointer(0);
        for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f * k; }
        banks.push_back(std::move(b));
    }
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;
    rig.render(64);
    rig.m_params.m_sample_index = 2;
    rig.render(64);
    rig.m_params.m_sample_index = 3;
    rig.render(64);
    rig.m_player.note_on();
    rig.render(512);

    float const parked = gain_in(64);
    for (size_t j = 0; j < k_fade; ++j) {
        float expected = gain_in(j) * static_cast<float>(3000 + j);       // live: bank 3 from Start
        expected += parked * gain_out(j) * static_cast<float>(5240 + j);  // bank 3 at the head
        if (64 + j < k_fade) {
            expected += parked * gain_out(64 + j) * static_cast<float>(4240 + j);
        }
        if (128 + j < k_fade) {
            expected += parked * gain_out(128 + j) * static_cast<float>(3240 + j);
        }
        // bank 0's tail (parked at 2048, gain 1) was stolen: no term.
        ASSERT_NEAR(rig.m_out[2240 + j], expected, 0.1f) << "j=" << j;
    }
}

TEST(SamplePlayer, TailWhoseBankWasUnloadedReadsSilence) {
    std::vector<thl::core::BufferF> banks;
    banks.push_back(make_ramp(1, 10000));
    thl::core::BufferF plus(1, 10000, k_sample_rate);
    float* d = plus.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f; }
    banks.push_back(std::move(plus));
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;  // bank 0 becomes the tail
    rig.render(64);

    // Reload with bank 0 emptied; the parked tail must go silent, the live
    // head on bank 1 continues its fade-in.
    std::vector<thl::core::BufferF> reloaded;
    reloaded.emplace_back();
    thl::core::BufferF plus2(1, 10000, k_sample_rate);
    float* d2 = plus2.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d2[i] = static_cast<float>(i) + 1000.0f; }
    reloaded.push_back(std::move(plus2));
    load(rig.m_store, std::move(reloaded));
    rig.render(512);
    for (size_t j = 0; j < k_fade - 64; ++j) {
        float const expected = gain_in(64 + j) * static_cast<float>(3112 + j);
        ASSERT_NEAR(rig.m_out[2112 + j], expected, 0.05f) << "j=" << j;
    }

    // Empty the live bank too: silence, head reported finished.
    std::vector<thl::core::BufferF> gone(2);
    load(rig.m_store, std::move(gone));
    rig.render(64);
    EXPECT_FALSE(rig.m_player.started());
    EXPECT_EQ(rig.m_listener.m_dropped, 0u);
    EXPECT_EQ(rig.m_listener.m_finished.size(), 1u);
    for (size_t j = 0; j < 64; ++j) { ASSERT_FLOAT_EQ(rig.m_out[2624 + j], 0.0f); }
}

TEST(SamplePlayer, LoopBodyFloorAndFadeFitTheLoop) {
    // Region [0, 500) with Loop at 490: the loop body is floored to 2 ms
    // (96 frames, Loop 404) and the fade shrinks to half the body (48).
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.05f;
    rig.m_params.m_sample_loop_point = 0.049f;
    rig.render(1024);
    for (size_t k = 0; k < 48; ++k) {
        float const t = static_cast<float>(k) / 48.0f * std::numbers::pi_v<float> * 0.5f;
        float const expected =
            std::cos(t) * static_cast<float>(452 + k) + std::sin(t) * static_cast<float>(356 + k);
        ASSERT_NEAR(rig.m_out[452 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = 0; k < 48; ++k) {
        ASSERT_NEAR(rig.m_out[500 + k], static_cast<float>(404 + k), 1e-3f) << "k=" << k;
    }
    ASSERT_NEAR(rig.m_out[596], 404.0f, 1e-3f);
}

TEST(SamplePlayer, LoopSnapWithoutCrossingsKeepsEqualPower) {
    // Loop Snap on, but a ramp has no zero crossing to snap to: the join is
    // not in phase, so the loop fade stays equal-power.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.m_params.m_loop_snap = true;
    rig.render(4864);
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(4320 + k) + gain_in(k) * static_cast<float>(520 + k);
        ASSERT_NEAR(rig.m_out[4320 + k], expected, 0.05f) << "k=" << k;
    }
}

TEST(SamplePlayer, LoopSnapJudgesThePhaseAtTheRealReentry) {
    // Crossing only at 6000 (End); Loop parked on End loops the region whole
    // from Start (1000), which has no crossing: not in phase, equal-power.
    thl::core::BufferF signed_ramp(1, 10000, k_sample_rate);
    float* d = signed_ramp.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) {
        bool const negative = i >= 5000 && i < 6000;
        d[i] = (negative ? -1.0f : 1.0f) * static_cast<float>(i + 1);
    }
    std::vector<thl::core::BufferF> banks;
    banks.push_back(std::move(signed_ramp));
    PlayerRig rig(std::move(banks));
    rig.m_params.m_sample_start = 0.1f;
    rig.m_params.m_sample_end = 0.6f;
    rig.m_params.m_sample_loop_point = 0.6f;
    rig.m_params.m_loop_snap = true;
    rig.render(5056);
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * -static_cast<float>(5521 + k) + gain_in(k) * static_cast<float>(521 + k);
        ASSERT_NEAR(rig.m_out[4520 + k], expected, 0.1f) << "k=" << k;
    }
}

TEST(SamplePlayer, LoopAtSampleStartFadesAfterTheWrap) {
    // Region [0, 96) with Loop at 0: there is no audio before Loop, so the
    // fade runs after the wrap instead — the head restarts at 0 under a copy
    // carrying on past End (96 + k) that fades out over half the body (48).
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.0096f;
    rig.render(256);
    for (size_t n = 0; n < 96; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(n), 1e-3f) << "n=" << n;
    }
    for (size_t k = 0; k < 48; ++k) {
        float const t = static_cast<float>(k) / 48.0f * std::numbers::pi_v<float> * 0.5f;
        float const expected =
            std::cos(t) * static_cast<float>(96 + k) + std::sin(t) * static_cast<float>(k);
        ASSERT_NEAR(rig.m_out[96 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = 48; k < 96; ++k) {
        ASSERT_NEAR(rig.m_out[96 + k], static_cast<float>(k), 1e-3f) << "k=" << k;
    }
    // Every wrap does the same; no tail is parked.
    ASSERT_NEAR(rig.m_out[192], 96.0f, 1e-3f);
}

TEST(SamplePlayer, StartOnEndStillPlaysTheMinimumSpan) {
    // Start == End would be an empty region; it plays 2 ms (96 frames)
    // forwards from Start instead.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_start = 0.5f;
    rig.m_params.m_sample_end = 0.5f;
    rig.render(256);
    for (size_t n = 0; n < 48; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(5000 + n), 1e-3f) << "n=" << n;
    }
    ASSERT_NEAR(rig.m_out[96], 5000.0f, 1e-3f);
}

TEST(SamplePlayer, LoopSnapMovesMarkersToUpwardCrossings) {
    // x[i] = +-(i + 1), negative in [1000, 2000) and [5000, 6000): upward
    // zero crossings sit at 2000 and 6000 only, and |x| is the position.
    // End near 5980 snaps to 6000 and Loop near 1990 to 2000; Start (no
    // crossing within 5 ms) stays.
    thl::core::BufferF signed_ramp(1, 10000, k_sample_rate);
    float* d = signed_ramp.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) {
        bool const negative = (i >= 1000 && i < 2000) || (i >= 5000 && i < 6000);
        d[i] = (negative ? -1.0f : 1.0f) * static_cast<float>(i + 1);
    }
    auto run = [&](bool snap) {
        std::vector<thl::core::BufferF> banks;
        banks.push_back(signed_ramp);
        PlayerRig rig(std::move(banks));
        rig.m_params.m_sample_end = 0.598f;
        rig.m_params.m_sample_loop_point = 0.199f;
        rig.m_params.m_loop_snap = snap;
        rig.render(6400);
        return rig.m_out;
    };
    auto const snapped = run(true);
    ASSERT_NEAR(snapped[0], 1.0f, 1e-3f);
    ASSERT_NEAR(snapped[6000], 2001.0f, 1e-3f);
    ASSERT_NEAR(snapped[6001], 2002.0f, 1e-3f);
    auto const raw = run(false);
    EXPECT_GT(std::abs(raw[6000] - 2001.0f), 1.0f);
}

TEST(SamplePlayer, LoopSnapNeverReversesTheRegion) {
    // Start 1900 snaps up to the crossing at 2000, past End 1950: the region
    // must stay forward (End pushed out to the 2 ms span), not reverse.
    thl::core::BufferF signed_ramp(1, 10000, k_sample_rate);
    float* d = signed_ramp.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) {
        bool const negative = i >= 1000 && i < 2000;
        d[i] = (negative ? -1.0f : 1.0f) * static_cast<float>(i + 1);
    }
    std::vector<thl::core::BufferF> banks;
    banks.push_back(std::move(signed_ramp));
    PlayerRig rig(std::move(banks));
    rig.m_params.m_sample_start = 0.19f;
    rig.m_params.m_sample_end = 0.195f;
    rig.m_params.m_loop_snap = true;
    rig.render(64);
    for (size_t n = 0; n < 48; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(2001 + n), 1e-3f) << "n=" << n;
    }
}

TEST(SamplePlayer, SnappedTinyLoopOnASineHasNoJumps) {
    // Start == End on a 220 Hz sine: the 2 ms span snaps out to one period,
    // so the loop fade blends in-phase audio (equal-gain, no +3 dB bulge)
    // and the output stays the sine: x[n-1] + x[n+1] == 2 cos(w) x[n].
    constexpr size_t k_frames = 48000;
    thl::core::BufferF sine(1, k_frames, k_sample_rate);
    float* d = sine.get_write_pointer(0);
    for (size_t i = 0; i < k_frames; ++i) {
        d[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / k_sample_rate));
    }
    std::vector<thl::core::BufferF> banks;
    banks.push_back(std::move(sine));
    auto run = [&](bool snap) {
        PlayerRig rig(banks);
        rig.m_params.m_sample_start = 0.5f;
        rig.m_params.m_sample_end = 0.5f;
        rig.m_params.m_loop_snap = snap;
        rig.render(9600);
        float peak = 0.0f;
        float residual = 0.0f;  // 0 for a pure 220 Hz sine
        auto const two_cos_w =
            static_cast<float>(2.0 * std::cos(2.0 * std::numbers::pi * 220.0 / k_sample_rate));
        for (size_t n = 1; n + 1 < rig.m_out.size(); ++n) {
            peak = std::max(peak, std::abs(rig.m_out[n]));
            residual =
                std::max(residual,
                         std::abs(rig.m_out[n - 1] + rig.m_out[n + 1] - two_cos_w * rig.m_out[n]));
        }
        return std::pair{peak, residual};
    };
    auto const [snapped_peak, snapped_residual] = run(true);
    EXPECT_NEAR(snapped_peak, 1.0f, 0.02f);
    EXPECT_LT(snapped_residual, 0.005f);
    // Unsnapped, the 96-frame loop cuts mid-period: the blend is audible.
    auto const [raw_peak, raw_residual] = run(false);
    EXPECT_GT(raw_residual, 0.02f);
}

TEST(SamplePlayer, ReverseRegionPlaysBackwardsAndLoopsAtTheMirroredPoint) {
    // Start 0.48 (4800), End 0 → the head enters at 4799 and runs down to 0,
    // then re-enters at Loop = 0.38 (physical 3800), running down again. The
    // loop fade mirrors too: the copy reads the audio just above Loop.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_start = 0.48f;
    rig.m_params.m_sample_end = 0.0f;
    rig.m_params.m_sample_loop_point = 0.38f;
    rig.render(6016);

    for (size_t n = 0; n < 4320; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(4799 - n), 1e-3f) << "n=" << n;
    }
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(479 - k) + gain_in(k) * static_cast<float>(4280 - k);
        ASSERT_NEAR(rig.m_out[4320 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = 0; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], static_cast<float>(3800 - k), 1e-3f) << "k=" << k;
    }
}

TEST(SamplePlayer, ReverseRegionMarkerMovesKeepTheHeadInPlace) {
    // Reversed region: moving Start or End every block must not step the read
    // position (the mirror axis is Start + End). Like forward, the head keeps
    // running down one frame per frame; only reaching End wraps it.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_start = 0.9f;
    rig.m_params.m_sample_end = 0.1f;
    rig.render(k_block);
    for (int b = 0; b < 40; ++b) {
        rig.m_params.m_sample_start -= 0.001f;  // 10 frames per block
        rig.m_params.m_sample_end += 0.0013f;   // 13 frames per block
        rig.render(k_block);
    }
    for (size_t n = 1; n < rig.m_out.size(); ++n) {
        ASSERT_NEAR(rig.m_out[n - 1] - rig.m_out[n], 1.0f, 1e-3f) << "n=" << n;
    }
}

TEST(SamplePlayer, OneShotStopsAtEndWithTailFade) {
    // Region [0, 4800), Loop off: the head plays to End once, the outgoing
    // tail rides out the 10 ms fade past End and then everything is silent.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_loop = false;
    rig.render(6016);
    for (size_t n = 0; n < 4800; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(n), 1e-3f) << "n=" << n;
    }
    EXPECT_TRUE(rig.m_player.finished());
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected = gain_out(k) * static_cast<float>(4800 + k);
        ASSERT_NEAR(rig.m_out[4800 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = k_fade; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], 0.0f, 1e-5f) << "k=" << k;
    }
    // note_on brings the head back from the region start, fading in (the
    // ADSR may still be releasing) with nothing to park.
    rig.m_player.note_on();
    rig.render(k_block);
    EXPECT_FALSE(rig.m_player.finished());
    for (size_t n = 8; n < k_block; ++n) {
        ASSERT_NEAR(rig.m_out[6016 + n], static_cast<float>(n) * gain_in(n), 0.5f) << "n=" << n;
    }
}

TEST(LoopRegion, FromNormalizedClampsEveryEdge) {
    auto r = LoopRegion::from_normalized(0.5f, 0.25f, 0.0f, 1000);  // end < start: reverse
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.size(), 250u);
    r = LoopRegion::from_normalized(0.0f, 0.5f, 0.9f, 1000);  // loop past End: loops whole
    EXPECT_EQ(r.m_loop_point, 0u);
    r = LoopRegion::from_normalized(-2.0f, 3.0f, -1.0f, 1000);  // out of range
    EXPECT_EQ(r.m_start, 0u);
    EXPECT_EQ(r.m_end, 1000u);
    EXPECT_EQ(r.m_loop_point, 0u);
    r = LoopRegion::from_normalized(1.0f, 1.0f, 1.0f, 1000);  // empty at the end
    EXPECT_EQ(r.size(), 0u);
}

TEST(LoopRegion, EndBeforeStartReversesAndMirrorsTheLoop) {
    auto r = LoopRegion::from_normalized(0.8f, 0.2f, 0.7f, 1000);
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.m_start, 200u);
    EXPECT_EQ(r.m_end, 800u);
    // Loop at 700 physical (near Start = 800) re-enters near the virtual
    // start: 200 + 800 - 700 - 1 = 299.
    EXPECT_EQ(r.m_loop_point, 299u);
    EXPECT_DOUBLE_EQ(r.physical(200.0), 799.0);
    EXPECT_DOUBLE_EQ(r.physical(799.0), 200.0);
    auto f = LoopRegion::from_normalized(0.2f, 0.8f, 0.7f, 1000);
    EXPECT_FALSE(f.m_reverse);
    EXPECT_EQ(f.m_loop_point, 700u);
    // Loop on the exit (End, where the UI parks it when the markers are
    // squeezed) or outside the region loops the whole region from its
    // entry, in both directions: reversed, End is the lowest frame.
    EXPECT_EQ(LoopRegion::from_normalized(0.8f, 0.2f, 0.2f, 1000).m_loop_point, 200u);
    EXPECT_EQ(LoopRegion::from_normalized(0.8f, 0.2f, 0.1f, 1000).m_loop_point, 200u);
    EXPECT_EQ(LoopRegion::from_normalized(0.8f, 0.2f, 0.9f, 1000).m_loop_point, 200u);
    EXPECT_EQ(LoopRegion::from_normalized(0.2f, 0.8f, 0.8f, 1000).m_loop_point, 200u);
    // Reversed Loop on Start is the entry itself.
    EXPECT_EQ(LoopRegion::from_normalized(0.8f, 0.2f, 0.7999f, 1000).m_loop_point, 200u);
}

TEST(SampleView, WrapsGrainsAndClampsHeads) {
    auto const ramp = make_ramp(2, 100, 1000.0f);
    auto const view = thl::dsp::sampler::SampleView::of(ramp);
    auto const empty = thl::dsp::sampler::SampleView::of(thl::core::BufferF{});
    using thl::dsp::sampler::read_clamped;
    using thl::dsp::sampler::read_wrapped;

    EXPECT_TRUE(view.valid());
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(empty.m_num_frames, 0u);
    EXPECT_FLOAT_EQ(read_wrapped(view, 0, 99.5f), 0.5f * 99.0f + 0.5f * 0.0f);  // wraps to 0
    EXPECT_FLOAT_EQ(read_clamped(view, 0, 99.5), 99.0f);                        // holds last
    EXPECT_FLOAT_EQ(read_clamped(view, 0, 5000.0), 99.0f);
    EXPECT_FLOAT_EQ(read_clamped(view, 1, 10.25), 1010.25f);  // channel 1 = ramp + 1000
    EXPECT_FLOAT_EQ(read_wrapped(view, 5, 10.0f), 0.0f);      // no such channel
    EXPECT_FLOAT_EQ(read_clamped(empty, 0, 10.0), 0.0f);      // empty source
}

TEST(ZeroCrossing, FindsTheNearestUpwardCrossingWithinReach) {
    // Sign flips upward at 50 and 80 (the frame before is negative), and
    // downward at 30 and 60.
    thl::core::BufferF buffer(1, 100, k_sample_rate);
    float* d = buffer.get_write_pointer(0);
    for (size_t i = 0; i < 100; ++i) {
        bool const negative = (i >= 30 && i < 50) || (i >= 60 && i < 80);
        d[i] = negative ? -1.0f : 1.0f;
    }
    auto const view = thl::dsp::sampler::SampleView::of(buffer);
    using thl::dsp::sampler::nearest_upward_crossing;
    using thl::dsp::sampler::upward_crossing_at;
    EXPECT_TRUE(upward_crossing_at(view, 50));
    EXPECT_TRUE(upward_crossing_at(view, 80));
    EXPECT_FALSE(upward_crossing_at(view, 30));
    EXPECT_FALSE(upward_crossing_at(view, 0));
    EXPECT_EQ(nearest_upward_crossing(view, 52, 0, 100, 10), 50u);
    EXPECT_EQ(nearest_upward_crossing(view, 76, 0, 100, 10), 80u);
    // Out of reach, or outside [lo, hi]: the target, clamped.
    EXPECT_EQ(nearest_upward_crossing(view, 10, 0, 100, 5), 10u);
    EXPECT_EQ(nearest_upward_crossing(view, 52, 51, 70, 10), 52u);
    EXPECT_EQ(nearest_upward_crossing(view, 200, 0, 90, 5), 90u);
}

TEST(LoopMarkers, MinimumSpanKeepsTheDirectionAndCachesUnmovedMarkers) {
    auto const ramp = make_ramp(1, 10000);
    auto const view = thl::dsp::sampler::SampleView::of(ramp);
    thl::dsp::sampler::LoopMarkers markers;
    markers.set_spans(96, 240);
    bool joined = true;
    // Start == End plays the minimum span forwards.
    auto r = markers.resolve(view, 0.5f, 0.5f, 0.0f, false, joined);
    EXPECT_FALSE(joined);
    EXPECT_FALSE(r.m_reverse);
    EXPECT_EQ(r.m_start, 5000u);
    EXPECT_EQ(r.m_end, 5096u);
    // End just below Start keeps reversing, 96 frames down.
    r = markers.resolve(view, 0.5f, 0.4999f, 0.0f, false, joined);
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.m_start, 4904u);
    EXPECT_EQ(r.m_end, 5000u);
    // At the sample's end Start gives way.
    r = markers.resolve(view, 1.0f, 1.0f, 0.0f, false, joined);
    EXPECT_EQ(r.m_start, 9904u);
    EXPECT_EQ(r.m_end, 10000u);
}

TEST(SamplePlayer, PlaysAPlainBufferWithoutAnyVoice) {
    // Standalone use: one buffer, no store, no visualiser, no channel mixer.
    // A stereo ramp comes out unmixed, channel for channel.
    auto const ramp = make_ramp(2, 1000, 1000.0f);
    auto const source = thl::dsp::sampler::SampleView::of(ramp);
    thl::dsp::sampler::SamplePlayer player;
    player.prepare(k_sample_rate);
    player.note_on();
    player.set_markers(0.1f, 0.9f, 0.1f);
    Block block(2, k_block);
    ASSERT_TRUE(player.render({&source, 1}, 0, block.out(), 2, k_block));
    EXPECT_TRUE(player.started());
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_FLOAT_EQ(block.m_data[0][i], static_cast<float>(100 + i));
        ASSERT_FLOAT_EQ(block.m_data[1][i], static_cast<float>(1100 + i));
    }
    // No sources: a silent early-out that resets the player.
    EXPECT_FALSE(player.render({}, 0, block.out(), 2, k_block));
    EXPECT_FALSE(player.started());
}

TEST(SamplePlayer, SourcesChangedDropsMarkersSnappedToTheOldAudio) {
    // Same buffer (address and length), new audio: without sources_changed()
    // the snapped End would stay on the old crossing.
    thl::core::BufferF buffer(1, 10000, k_sample_rate);
    auto fill = [&](size_t crossing) {
        float* d = buffer.get_write_pointer(0);
        for (size_t i = 0; i < 10000; ++i) {
            d[i] = (i >= crossing - 100 && i < crossing) ? -1.0f : 1.0f;
        }
    };
    fill(5010);  // upward crossing at 5010
    auto const view = thl::dsp::sampler::SampleView::of(buffer);
    thl::dsp::sampler::SamplePlayer player;
    player.prepare(k_sample_rate);
    player.note_on();
    player.set_markers(0.0f, 0.5f, 0.0f);
    player.set_snap(true);
    Block block(2, k_block);
    ASSERT_TRUE(player.render({&view, 1}, 0, block.out(), 2, k_block));
    EXPECT_EQ(player.region().m_end, 5010u);

    fill(4990);  // the crossing moved; the buffer did not
    player.sources_changed();
    ASSERT_TRUE(player.render({&view, 1}, 0, block.out(), 2, k_block));
    EXPECT_EQ(player.region().m_end, 4990u);
}
