// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Main processing class.

#include "supercell/dsp/granular_processor.h"

#include <cstring>

#include "supercell/drivers/debug_pin.h"

#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/utils/buffer_allocator.h"

#include "supercell/resources.h"

namespace clouds {

using namespace std;
using namespace stmlib;

void GranularProcessor::Init(
    void* large_buffer, size_t large_buffer_size,
    void* small_buffer, size_t small_buffer_size) {
  buffer_[0] = large_buffer;
  buffer_[1] = small_buffer;
  buffer_size_[0] = large_buffer_size;
  buffer_size_[1] = small_buffer_size;

  num_channels_ = 2;
  low_fidelity_ = false;
  bypass_ = false;

  src_down_.Init();
  src_up_.Init();

  phase_vocoder_.Init();

  ResetFilters();

  previous_playback_mode_ = PLAYBACK_MODE_LAST;
  reset_buffers_ = true;
  mute_in_ = false;
  mute_out_ = false;
  mute_in_fade_ = 0.0f;
  mute_out_fade_ = 0.0f;
  dry_wet_ = 0.0f;
  reverb_dry_signal_ = true;
}

void GranularProcessor::ResetFilters() {
  for (int32_t i = 0; i < 2; ++i) {
    fb_filter_[i].Init();
    lp_filter_[i].Init();
    hp_filter_[i].Init();
  }
}

void GranularProcessor::ProcessGranular(
    FloatFrame* input,
    FloatFrame* output,
    size_t size) {
  // At the exception of the spectral mode, all modes require the incoming
  // audio signal to be written to the recording buffer.
  if (playback_mode_ != PLAYBACK_MODE_SPECTRAL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR) {
    const float* input_samples = &input[0].l;
    const bool play = !parameters_.freeze ||
      playback_mode_ == PLAYBACK_MODE_OLIVERB ||
      playback_mode_ == PLAYBACK_MODE_KAMMERL;
    for (int32_t i = 0; i < num_channels_; ++i) {
      if (resolution() == 8) {
        buffer_8_[i].WriteFade(&input_samples[i], size, 2, play);
      } else {
        buffer_16_[i].WriteFade(&input_samples[i], size, 2, play);
      }
    }
  }

  switch (playback_mode_) {
    case PLAYBACK_MODE_GRANULAR:
      // In Granular mode, DENSITY is a meta parameter.
      parameters_.granular.use_deterministic_seed = parameters_.density < 0.5f;
      if (parameters_.density >= 0.53f) {
        parameters_.granular.overlap = (parameters_.density - 0.53f) * 2.12f;
      } else if (parameters_.density <= 0.47f) {
        parameters_.granular.overlap = (0.47f - parameters_.density) * 2.12f;
      } else {
        parameters_.granular.overlap = 0.0f;
      }

#ifdef CLOUDS_QUANTIZE_SEMITONES
      // Quantize pitch to closest semitone
      if (parameters_.pitch < 0.5f) parameters_.pitch -= 0.5f;
      parameters_.pitch = static_cast<int>(parameters_.pitch + 0.5f);
#endif

      // And TEXTURE too.
      parameters_.granular.window_shape = parameters_.texture < 0.75f
          ? parameters_.texture * 1.333f : 1.0f;

      if (resolution() == 8) {
        player_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        player_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_STRETCH:
      if (resolution() == 8) {
        ws_player_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        ws_player_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_LOOPING_DELAY:
      if (resolution() == 8) {
        looper_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        looper_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_SPECTRAL:
      {
        parameters_.spectral.quantization = parameters_.texture;
        parameters_.spectral.refresh_rate = 0.01f + 0.99f * parameters_.density;
        float warp = parameters_.size - 0.5f;
        parameters_.spectral.warp = 4.0f * warp * warp * warp + 0.5f;

        float randomization = parameters_.density - 0.5f;
        randomization *= randomization * 4.2f;
        randomization -= 0.05f;
        CONSTRAIN(randomization, 0.0f, 1.0f);
        parameters_.spectral.phase_randomization = randomization;
        phase_vocoder_.Process(parameters_, input, output, size);
      }
      break;

    case PLAYBACK_MODE_SPECTRAL_CLOUD:
      {
        phase_vocoder_.Process(parameters_, input, output, size);

        if (num_channels_ == 1) {
          for (size_t i = 0; i < size; ++i) {
            output[i].r = output[i].l;
          }
        }
      }
      break;

    case PLAYBACK_MODE_OLIVERB:
      {

        // Pre-delay, controlled by position or tap tempo sync
        Parameters p = {
          ws_player_.synchronized() ?
          parameters_.position :
          parameters_.position * 0.25f, // position;
          0.1f, // size;
          0.0f, // pitch;
          0.0f, // density;
          0.5f, // texture;
          1.0f, // dry_wet;
          0.0f, // stereo_spread;
          0.0f, // feedback;
          0.0f, // reverb;
          0.0f, // freeze;
          parameters_.trigger, // trigger;
          0.0f // gate;
        };

        if (resolution() == 8) {
          ws_player_.Play(buffer_8_, p, &output[0].l, size);
        } else {
          ws_player_.Play(buffer_16_, p, &output[0].l, size);
        }

        // Settings of the reverb
        oliverb_.set_diffusion(0.3f + 0.5f * parameters_.stereo_spread);
        oliverb_.set_size(0.05f + 0.94f * parameters_.size);
        oliverb_.set_mod_rate(parameters_.feedback);
        oliverb_.set_mod_amount(parameters_.reverb * 300.0f);
        oliverb_.set_ratio(SemitonesToRatio(parameters_.pitch));

        float x = parameters_.pitch;
        const float limit = 0.7f;
        const float slew = 0.4f;

        float wet =
          x < -limit ? 1.0f :
          x < -limit + slew ? 1.0f - (x + limit) / slew:
          x < limit - slew ? 0.0f :
          x < limit ? 1.0f + (x - limit) / slew:
          1.0f;
        oliverb_.set_pitch_shift_amount(wet);

        if (parameters_.freeze) {
          oliverb_.set_input_gain(0.0f);
          oliverb_.set_decay(1.0f);
          oliverb_.set_lp(1.0f);
          oliverb_.set_hp(0.0f);
        } else {
          oliverb_.set_decay(parameters_.density * 1.3f
                           + 0.15f * abs(parameters_.pitch) / 24.0f);
          oliverb_.set_input_gain(0.5f);
          float lp = parameters_.texture < 0.5f ?
            parameters_.texture * 2.0f : 1.0f;
          float hp = parameters_.texture > 0.5f ?
            (parameters_.texture - 0.5f) * 2.0f : 0.0f;
          oliverb_.set_lp(0.03f + 0.9f * lp);
          oliverb_.set_hp(0.01f + 0.2f * hp); // the small offset
                                                  // gets rid of
                                                  // feedback of large
                                                  // DC offset.
        }
        oliverb_.Process(output, size);
      }
      break;

  case PLAYBACK_MODE_RESONESTOR:
    {
      copy(&input[0], &input[size], &output[0]);

      resonestor_.set_pitch(parameters_.pitch);
      resonestor_.set_chord(parameters_.size);
      resonestor_.set_trigger(parameters_.trigger);
      resonestor_.set_burst_damp(parameters_.position);
      resonestor_.set_burst_comb((1.0f - parameters_.position));
      resonestor_.set_burst_duration((1.0f - parameters_.position));
      resonestor_.set_spread_amount(parameters_.reverb);
      resonestor_.set_stereo(parameters_.stereo_spread < 0.5f ? 0.0f :
        (parameters_.stereo_spread - 0.5f) * 2.0f);
      resonestor_.set_separation(parameters_.stereo_spread > 0.5f ? 0.0f :
                                (0.5f - parameters_.stereo_spread) * 2.0f);
      resonestor_.set_freeze(parameters_.freeze);
      resonestor_.set_harmonicity(1.0f - (parameters_.feedback * 0.5f));
      resonestor_.set_distortion(parameters_.dry_wet);

      float t = parameters_.texture;
      if (t < 0.5f) {
        resonestor_.set_narrow(0.001f);
        float l = 1.0f - (0.5f - t) / 0.5f;
        l = l * (1.0f - 0.08f) + 0.08f;
        resonestor_.set_damp(l * l);
      } else {
        resonestor_.set_damp(1.0f);
        float n = (t - 0.5f) / 0.5f * 1.35f;
        n *= n * n * n;
        resonestor_.set_narrow(0.001f + n * n * 0.6f);
      }

      float d = (parameters_.density - 0.05f) / 0.9f;
      if (d < 0.0f) d = 0.0f;
      d *= d * d;
      d *= d * d;
      d *= d * d;
      resonestor_.set_feedback(d * 20.0f);

      resonestor_.Process(output, size);
    }
    break;

  case PLAYBACK_MODE_KAMMERL:
    if (resolution() == 8) {
      kammerl_.Play(buffer_8_, parameters_, &output[0].l, size);
    } else {
      kammerl_.Play(buffer_16_, parameters_, &output[0].l, size);
    }
    break;

    default:
      break;
  }
}

void GranularProcessor::WarmDistortion(float* in, float parameter) {
	if (parameter < 0.1) {
		return;
	}
	static const float kMaxDistf = 2.0f;
	const float fac = kMaxDistf * parameter;
	const float amp = 1.0f - parameter * 0.45f;

	float smp = *in;
	smp = (1.0f + fac) * smp - fac * smp * smp * smp;

	float sign = 1.0f;
	if (smp < 0) {
		sign = -1.0;
	}
	float tanh_loopup =  std::max(0.0f, std::min(1.0f, (smp / 2.0f) * sign));
	float inv_tanh_smp = Interpolate(lut_inv_tanh, tanh_loopup,
			static_cast<float>(LUT_INV_TANH_SIZE-1)) * sign;

	smp = smp + (inv_tanh_smp - smp) * fac;
	smp *= amp;
	smp = std::max(-1.0f, std::min(1.0f, smp));
	*in = smp;
}


void GranularProcessor::Process(
    ShortFrame* input,
    ShortFrame* output,
    size_t size) {
  // TIC
  if (bypass_) {
    copy(&input[0], &input[size], &output[0]);
    return;
  }

  if (silence_ || reset_buffers_ ||
      previous_playback_mode_ != playback_mode_) {
    short* output_samples = &output[0].l;
    fill(&output_samples[0], &output_samples[size * 2], 0);
    return;
  }

  // Convert input buffers to float
  for (size_t i = 0; i < size; ++i) {
    in_[i].l = static_cast<float>(input[i].l) / 32768.0f;
    in_[i].r = static_cast<float>(input[i].r) / 32768.0f;
  }

  // SUPERCELL Handle Mute In separately
  float mute_level_in = mute_in_ ? 0.0f : 1.0f;
  float original_mute_in_fade = mute_in_fade_;
  for (size_t i = 0; i < size; i++) {
      ONE_POLE(mute_in_fade_, mute_level_in, 0.01f);
      in_[i].l = in_[i].l * mute_in_fade_;
      in_[i].r = in_[i].r * mute_in_fade_;
  }

  // mixdown for mono processing.
  if (num_channels_ == 1) {
    for (size_t i = 0; i < size; ++i) {
      float xfade = 0.5f;
      // in mono delay modes, stereo spread controls input crossfade
      if (playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY ||
          playback_mode_ == PLAYBACK_MODE_STRETCH)
        xfade = parameters_.stereo_spread;

      in_[i].l = in_[i].l * (1.0f - xfade) + in_[i].r * xfade;
      in_[i].r = in_[i].l;
    }
  }

  // Apply feedback, with high-pass filtering to prevent build-ups at very
  // low frequencies (causing large DC swings).
  float feedback =
		  (playback_mode_ == PLAYBACK_MODE_KAMMERL
				  && kammerl_.isSlicePlaybackActive()) ?
				  parameters_.reverb : 0.0f; // Map reverb parameter to feedback in PLAYBACK_MODE_KAMMERL.
  if (playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD) {
	ONE_POLE(freeze_lp_, parameters_.freeze ? 1.0f : 0.0f, 0.0005f)
	feedback = parameters_.feedback;
	float cutoff = (20.0f + 100.0f * feedback * feedback) / sample_rate();
	fb_filter_[0].set_f_q<FREQUENCY_FAST>(cutoff, 1.0f);
	fb_filter_[1].set(fb_filter_[0]);
	fb_filter_[0].Process<FILTER_MODE_HIGH_PASS>(&fb_[0].l, &fb_[0].l, size, 2);
	fb_filter_[1].Process<FILTER_MODE_HIGH_PASS>(&fb_[0].r, &fb_[0].r, size, 2);
  }
  float fb_gain = feedback * (1.0f - freeze_lp_);
  for (size_t i = 0; i < size; ++i) {
	in_[i].l += fb_gain * (
		SoftLimit(fb_gain * 1.4f * fb_[i].l + in_[i].l) - in_[i].l);
	in_[i].r += fb_gain * (
		SoftLimit(fb_gain * 1.4f * fb_[i].r + in_[i].r) - in_[i].r);
  }

  if (low_fidelity_) {
    size_t downsampled_size = size / kDownsamplingFactor;
    src_down_.Process(in_, in_downsampled_,size);
    ProcessGranular(in_downsampled_, out_downsampled_, downsampled_size);
    src_up_.Process(out_downsampled_, out_, downsampled_size);
  } else {
    ProcessGranular(in_, out_, size);
  }

  // Diffusion and pitch-shifting post-processings.
  if (playback_mode_ != PLAYBACK_MODE_SPECTRAL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD &&
      playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL) {
    float texture = parameters_.texture;
    float diffusion = playback_mode_ == PLAYBACK_MODE_GRANULAR
        ? texture > 0.75f ? (texture - 0.75f) * 4.0f : 0.0f
        : parameters_.density;
    diffuser_.set_amount(diffusion);
    diffuser_.Process(out_, size);
  }

  if (((playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY)
      && (!parameters_.freeze || looper_.synchronized()))
      || (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD)) {
    pitch_shifter_.set_ratio(SemitonesToRatio(parameters_.pitch));
    pitch_shifter_.set_size(parameters_.size);
    if (PLAYBACK_MODE_SPECTRAL_CLOUD != playback_mode_) {
      // parasites
      float x = parameters_.pitch;
      const float limit = 0.7f;
      const float slew = 0.4f;
      float wet =
        x < -limit ? 1.0f :
        x < -limit + slew ? 1.0f - (x + limit) / slew:
        x < limit - slew ? 0.0f :
        x < limit ? 1.0f + (x - limit) / slew:
        1.0f;
      pitch_shifter_.set_dry_wet(wet);
    } else {
      // beat repeat
      pitch_shifter_.set_dry_wet(1.f);
    }
    pitch_shifter_.Process(out_, size);
  }

  // Apply filters.
  if (playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY ||
      playback_mode_ == PLAYBACK_MODE_STRETCH) {
    float cutoff = parameters_.texture;
    float lp_cutoff = 0.5f * SemitonesToRatio(
        (cutoff < 0.5f ? cutoff - 0.5f : 0.0f) * 216.0f);
    float hp_cutoff = 0.25f * SemitonesToRatio(
        (cutoff < 0.5f ? -0.5f : cutoff - 1.0f) * 216.0f);
    CONSTRAIN(lp_cutoff, 0.0f, 0.499f);
    CONSTRAIN(hp_cutoff, 0.0f, 0.499f);

    lp_filter_[0].set_f_q<FREQUENCY_FAST>(lp_cutoff, 0.9f);
    lp_filter_[0].Process<FILTER_MODE_LOW_PASS>(
        &out_[0].l, &out_[0].l, size, 2);

    lp_filter_[1].set(lp_filter_[0]);
    lp_filter_[1].Process<FILTER_MODE_LOW_PASS>(
        &out_[0].r, &out_[0].r, size, 2);

    hp_filter_[0].set_f_q<FREQUENCY_FAST>(hp_cutoff, 0.9f);
    hp_filter_[0].Process<FILTER_MODE_HIGH_PASS>(
        &out_[0].l, &out_[0].l, size, 2);

    hp_filter_[1].set(hp_filter_[0]);
    hp_filter_[1].Process<FILTER_MODE_HIGH_PASS>(
        &out_[0].r, &out_[0].r, size, 2);
  }

  // This is what is fed back. Reverb is not fed back.
  copy(&out_[0], &out_[size], &fb_[0]);

  // SUPERCELL Added Pre-Reverb Muting.
  float mute_level_out = mute_out_ ? 0.0f : 1.0f;
  float original_mute_out_fade = mute_out_fade_;
  for (size_t i = 0; i < size; i++) {
      ONE_POLE(mute_out_fade_, mute_level_out, 0.01f);
      out_[i].l *= mute_out_fade_;
      out_[i].r *= mute_out_fade_;
  }

  if (!reverb_dry_signal_ &&
      playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL) {
    // Apply reverb.
    float reverb_amount = parameters_.reverb;

    reverb_.set_amount(reverb_amount * 0.54f);
    reverb_.set_diffusion(0.7f);
    reverb_.set_time(0.35f + 0.63f * reverb_amount);
    reverb_.set_input_gain(0.2f);
    reverb_.set_lp(0.6f + 0.37f * feedback);
    reverb_.Process(out_, size);
  }

  const float post_gain = 1.2f;

  if (playback_mode_ != PLAYBACK_MODE_RESONESTOR) {

    ParameterInterpolator dry_wet_mod(&dry_wet_, parameters_.dry_wet, size);
    float mute_out_fade = original_mute_out_fade;
    float mute_in_fade = original_mute_in_fade;

    for (size_t i = 0; i < size; ++i) {
      float dry_wet = dry_wet_mod.Next();
      if (playback_mode_ == PLAYBACK_MODE_KAMMERL) {
        dry_wet = 1.0f;
      }

      float fade_in = Interpolate(lut_xfade_in, dry_wet, 16.0f);
      float fade_out = Interpolate(lut_xfade_out, dry_wet, 16.0f);

      // Convert again from input, as in_ has feedback already applied
      float l = static_cast<float>(input[i].l) / 32768.0f;
      float r = static_cast<float>(input[i].r) / 32768.0f;

      // Since the data here has bypassed all the mute logic, reapply mutes
      ONE_POLE(mute_out_fade, mute_level_out, 0.01f);
      ONE_POLE(mute_in_fade, mute_level_in, 0.01f);
      fade_out *= (mute_in_fade * mute_out_fade);

      out_[i].l = (l * fade_out) + (out_[i].l * post_gain * fade_in);
      out_[i].r = (r * fade_out) + (out_[i].r * post_gain * fade_in);
    }
  }

  // Apply the simple post-processing reverb.
  if (reverb_dry_signal_ &&
      playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL) {
    float reverb_amount = parameters_.reverb;

    reverb_.set_amount(reverb_amount * 0.54f);
    reverb_.set_diffusion(0.7f);
    reverb_.set_time(0.35f + 0.63f * reverb_amount);
    reverb_.set_input_gain(0.2f);
    reverb_.set_lp(0.6f + 0.37f * feedback);

    reverb_.Process(out_, size);
  }

  for (size_t i = 0; i < size; ++i) {
    if (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
	    WarmDistortion(&out_[i].l, parameters_.kammerl.pitch_mode);
	    WarmDistortion(&out_[i].r, parameters_.kammerl.pitch_mode);
    }

    output[i].l = SoftConvert(out_[i].l);
    output[i].r = SoftConvert(out_[i].r);
  }

  // TOC
}

void GranularProcessor::PreparePersistentData() {
  persistent_state_.write_head[0] = low_fidelity_ ?
      buffer_8_[0].head() : buffer_16_[0].head();
  persistent_state_.write_head[1] = low_fidelity_ ?
      buffer_8_[1].head() : buffer_16_[1].head();
  persistent_state_.quality = quality();
  if (playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
      playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD)
    persistent_state_.spectral = playback_mode_;
  else
    persistent_state_.spectral = 0;
}

void GranularProcessor::GetPersistentData(
      PersistentBlock* block, size_t *num_blocks) {
  PersistentBlock* first_block = block;

  block->tag = FourCC<'S', 't', 'a', 't'>::value; // NOTE modified for Überclouds ('S' vs 's')
  block->data = &persistent_state_;
  block->size = sizeof(PersistentState);
  ++block;

  // Create save block holding the audio buffers.
  for (int32_t i = 0; i < num_channels_; ++i) {
    block->tag = FourCC<'b', 'u', 'f', 'f'>::value;
    block->data = buffer_[i];
    block->size = buffer_size_[num_channels_ - 1];
    ++block;
  }
  *num_blocks = block - first_block;
}

bool GranularProcessor::LoadPersistentData(const uint32_t* data) {
  // Force a silent output while the swapping of buffers takes place.
  silence_ = true;

  PersistentBlock block[4];
  size_t num_blocks;
  GetPersistentData(block, &num_blocks);

  for (size_t i = 0; i < num_blocks; ++i) {
    // Check that the format is correct.
    if (block[i].tag != data[0] || block[i].size != data[1]) {
      silence_ = false;
      return false;
    }

    // All good. Load the data. 2 words have already been used for the block tag
    // and the block size.
    data += 2;
    memcpy(block[i].data, data, block[i].size);
    data += block[i].size / sizeof(uint32_t);

    if (i == 0) {
      // We now know from which mode the data was saved.
      uint8_t currently_spectral =
          playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
          playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD
          ? playback_mode_ : 0;
      uint8_t requires_spectral = persistent_state_.spectral;
      if (currently_spectral ^ requires_spectral) {
        set_playback_mode(requires_spectral
            ? static_cast<PlaybackMode>(requires_spectral)
            : PLAYBACK_MODE_GRANULAR);
      }
      set_quality(persistent_state_.quality);

      // We can force a switch to this mode, and once everything has been
      // initialized for this mode, we continue with the loop to copy the
      // actual buffer data - with all state variables correctly initialized.
      Prepare();
      GetPersistentData(block, &num_blocks);
    }
  }

  // We can finally reset the position of the write heads.
  if (low_fidelity_) {
    buffer_8_[0].Resync(persistent_state_.write_head[0]);
    buffer_8_[1].Resync(persistent_state_.write_head[1]);
  } else {
    buffer_16_[0].Resync(persistent_state_.write_head[0]);
    buffer_16_[1].Resync(persistent_state_.write_head[1]);
  }
  parameters_.freeze = true;
  silence_ = false;
  return true;
}

void GranularProcessor::Prepare() {
  bool playback_mode_changed = previous_playback_mode_ != playback_mode_;
  bool benign_change = playback_mode_ != PLAYBACK_MODE_SPECTRAL
    && previous_playback_mode_ != PLAYBACK_MODE_SPECTRAL
    && playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD
    && previous_playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD
    && playback_mode_ != PLAYBACK_MODE_RESONESTOR
    && previous_playback_mode_ != PLAYBACK_MODE_RESONESTOR
    && playback_mode_ != PLAYBACK_MODE_OLIVERB
    && previous_playback_mode_ != PLAYBACK_MODE_OLIVERB
    && previous_playback_mode_ != PLAYBACK_MODE_LAST;

  if (!reset_buffers_ && playback_mode_changed && benign_change) {
    ResetFilters();
    pitch_shifter_.Clear();
    previous_playback_mode_ = playback_mode_;
  }

  if ((playback_mode_changed && !benign_change) || reset_buffers_) {
    parameters_.freeze = false;
  }

  if (reset_buffers_ || (playback_mode_changed && !benign_change)) {
    void* buffer[2];
    size_t buffer_size[2];
    void* workspace;
    size_t workspace_size;
    if (num_channels_ == 1) {
      // Large buffer: 120k of sample memory.
      // small buffer: fully allocated to FX workspace.
      buffer[0] = buffer_[0];
      buffer_size[0] = buffer_size_[0];
      buffer[1] = NULL;
      buffer_size[1] = 0;
      workspace = buffer_[1];
      workspace_size = buffer_size_[1];
    } else {
      // Large buffer: 64k of sample memory + FX workspace.
      // small buffer: 64k of sample memory.
      buffer_size[0] = buffer_size[1] = buffer_size_[1];
      buffer[0] = buffer_[0];
      buffer[1] = buffer_[1];

      workspace_size = buffer_size_[0] - buffer_size_[1];
      workspace = static_cast<uint8_t*>(buffer[0]) + buffer_size[0];
    }
    float sr = sample_rate();

    BufferAllocator allocator(workspace, workspace_size);
    diffuser_.Init(allocator.Allocate<float>(2048));

    uint16_t* reverb_buffer = allocator.Allocate<uint16_t>(16384);
    if (playback_mode_ == PLAYBACK_MODE_OLIVERB) {
      oliverb_.Init(reverb_buffer);
    } else {
      reverb_.Init(reverb_buffer);
    }

    size_t correlator_block_size = (kMaxWSOLASize / 32) + 2;
    uint32_t* correlator_data = allocator.Allocate<uint32_t>(
        correlator_block_size * 3);
    correlator_.Init(
        &correlator_data[0],
        &correlator_data[correlator_block_size]);
    pitch_shifter_.Init((uint16_t*)correlator_data);

    if (playback_mode_ == PLAYBACK_MODE_SPECTRAL) {
      phase_vocoder_.Init(
          PhaseVocoder::TRANSFORMATION_TYPE_FRAME,
          buffer, buffer_size,
          lut_sine_window_4096, 4096,
          num_channels_, resolution(), sr);
    } else if (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
      phase_vocoder_.Init(
          PhaseVocoder::TRANSFORMATION_TYPE_SPECTRAL_CLOUD,
          buffer, buffer_size,
          lut_sine_window_4096, 4096,
          num_channels_, resolution(), sr);
    } else if (playback_mode_ == PLAYBACK_MODE_RESONESTOR) {
      float* buf = (float*)buffer[0];
      resonestor_.Init(buf);
    } else {
      for (int32_t i = 0; i < num_channels_; ++i) {
        if (resolution() == 8) {
          buffer_8_[i].Init(
              buffer[i],
              (buffer_size[i]),
              tail_buffer_[i]);
        } else {
          buffer_16_[i].Init(
              buffer[i],
              ((buffer_size[i]) >> 1),
              tail_buffer_[i]);
        }
      }

      int32_t num_grains = (num_channels_ == 1 ? 40 : 32) *
         (low_fidelity_ ? 23 : 16) >> 4;
      player_.Init(num_channels_, num_grains);
      ws_player_.Init(&correlator_, num_channels_);
      looper_.Init(num_channels_);
      kammerl_.Init(num_channels_);
    }
    reset_buffers_ = false;
    previous_playback_mode_ = playback_mode_;
  }

  if (playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
      playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
    phase_vocoder_.Buffer();
  } else if (playback_mode_ == PLAYBACK_MODE_STRETCH ||
             playback_mode_ == PLAYBACK_MODE_OLIVERB) {
    if (resolution() == 8) {
      ws_player_.LoadCorrelator(buffer_8_);
    } else {
      ws_player_.LoadCorrelator(buffer_16_);
    }
    correlator_.EvaluateSomeCandidates();
  }
}

}  // namespace clouds
