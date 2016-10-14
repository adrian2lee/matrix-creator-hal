/*
 * Copyright 2016 <Admobilize>
 * All rights reserved.
 */

#include <wiringPi.h>
#include <string.h>
#include <fftw3.h>
#include <stdint.h>

#include <string>
#include <fstream>
#include <iostream>
#include <valarray>

#include "../cpp/driver/everloop_image.h"
#include "../cpp/driver/everloop.h"
#include "../cpp/driver/microphone_array.h"
#include "../cpp/driver/wishbone_bus.h"

/*
Cross-correlation between signals implemented in requency domain.
*/
class CrossCorrelation {
 public:
  CrossCorrelation(int N);
  ~CrossCorrelation();

  void Exec(int16_t* a, int16_t* b);
  float* Result();

 private:
  void Corr(float* out, float* x, float* y);

  int order_;
  float* in_;
  float* A_;
  float* B_;
  float* C_;
  float* c_;

  fftwf_plan forward_plan_a_;
  fftwf_plan forward_plan_b_;
  fftwf_plan inverse_plan_;
};

CrossCorrelation::CrossCorrelation(int N) : order_(N) {
  /*
  fftwf_malloc function that behave identically to malloc, except that they
  guarantee that the returned pointer obeys any special alignment restrictions
  imposed by any algorithm in FFTW (e.g. for SIMD acceleration).
  */
  in_ = (float*)fftwf_malloc(sizeof(float) * N);
  A_ = (float*)fftwf_malloc(sizeof(float) * N);
  B_ = (float*)fftwf_malloc(sizeof(float) * N);
  C_ = (float*)fftwf_malloc(sizeof(float) * N);
  c_ = (float*)fftwf_malloc(sizeof(float) * N);

  forward_plan_a_ = fftwf_plan_r2r_1d(N, in_, A_, FFTW_R2HC, FFTW_ESTIMATE);
  forward_plan_b_ = fftwf_plan_r2r_1d(N, in_, B_, FFTW_R2HC, FFTW_ESTIMATE);
  inverse_plan_ = fftwf_plan_r2r_1d(N, C_, c_, FFTW_HC2R, FFTW_ESTIMATE);
}

CrossCorrelation::~CrossCorrelation() {
  fftwf_destroy_plan(forward_plan_a_);
  fftwf_destroy_plan(forward_plan_b_);
  fftwf_destroy_plan(inverse_plan_);

  fftwf_free(in_);
  fftwf_free(A_);
  fftwf_free(B_);
  fftwf_free(C_);
  fftwf_free(c_);
}

float* CrossCorrelation::Result() { return c_; }

void CrossCorrelation::Exec(int16_t* a, int16_t* b) {
  for (int i = 0; i < order_; i++) {
    in_[i] = a[i];
  }

  fftwf_execute(forward_plan_a_);

  for (int i = 0; i < order_; i++) {
    in_[i] = b[i];
  }

  fftwf_execute(forward_plan_b_);

  Corr(C_, A_, B_);

  fftwf_execute(inverse_plan_);

  for (int i = 0; i < order_; i++) {
    c_[i] = c_[i] / order_;
  }
}

void CrossCorrelation::Corr(float* out, float* x, float* y) {
  memset(reinterpret_cast<void*>(out), 0, order_ * sizeof(float));

  out[0] = x[0] * y[0];                             // r0
  out[order_ / 2] = x[order_ / 2] * y[order_ / 2];  // r(n/2)

  for (int j = 1; j < order_ / 2; j++) {
    float a = x[j];
    float b = x[order_ - j];
    float c = y[j];
    float d = -y[order_ - j];
    out[j] += a * c - b * d;           // Re
    out[order_ - j] += b * c + a * d;  // Im
  }
}

namespace hal = matrix_hal;

const int N = 128;

int main() {
  hal::WishboneBus bus;
  bus.SpiInit();

  hal::MicrophoneArray mics;
  mics.Setup(&bus);

  hal::Everloop everloop;
  everloop.Setup(&bus);

  hal::EverloopImage image1d;

  for (auto& led : image1d.leds) led.red = 10;

  everloop.Write(&image1d);

  int16_t buffer[mics.Channels()][mics.SamplingRate()];

  CrossCorrelation corr(N);
  std::valarray<float> current_mag(4);
  std::valarray<float> current_index(4);

  while (true) {
    mics.Read(); /* Reading 8-mics buffer from de FPGA */

    for (uint32_t s = 0; s < mics.NumberOfSamples(); s++) {
      for (uint16_t c = 0; c < mics.Channels(); c++) { /* mics.Channels()=8 */
        buffer[c][s] = mics.At(s, c);
      }
    }

    for (int channel = 0; channel < 4; channel++) {
      corr.Exec(buffer[channel], buffer[channel + 4]);

      float* c = corr.Result();

      int index = 0;
      float m = c[0];
      int max_tof = 6;
      for (int i = 1; i < max_tof; i++)
        if (c[i] > m) {
          index = i;
          m = c[i];
        }

      for (int i = N-max_tof; i < N; i++)
        if (c[i] > m) {
          index = i;
          m = c[i];
        }

      current_mag[channel] = m;
      current_index[channel] = index;
    }

    int dir = 0;
    int index = current_index[0];
    float mag = current_mag[0];
    for (int channel = 1; channel < 4; channel++) {
      if (mag < current_mag[channel]) {
        dir = channel;
        mag = current_mag[channel];
        index = current_index[channel];
      }
    }
    if (mag > 2e8)
      std::cout << dir << "\t" << index << "\t" << mag << std::endl;
  }

  return 0;
}
