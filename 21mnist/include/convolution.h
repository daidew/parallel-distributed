/**
   @file convolution.h
   @brief convolution layer
 */
#pragma once

#include "mnist_util.h"
#include "tensor.h"
#include "ada_delta.h"
#include "grad_check.h"
#include <stdio.h>

/**
   @brief configuration data for Convolution2D
   @details no configuration currently exist
*/
struct Convolution2DCfg { };

/**
   @brief convolution of images

   @param (maxB) the maximum number of images it can handle at a time (batch size)
   @param (IC) the number of channels per input image (the 
               original input image for MNIST has is grey scale
               and therefore has a single channel. 
               hidden layers have 32 or 64 channels.
   @param (H) height of an image (28 for an input image, 26 after the first 
              convolution layer and 24 after the second)
   @param (W) width of an image (same as H)
   @param (K) convolution kernel size (3 for MNIST). filter is K x K
   @param (OC) the number of channels per an output image

   @details this layer converts each ICxWxH image to
   OCx(W-K+1)x(H-K+1) image, applying ICxKxK stencil to each pixel
 */
template<idx_t maxB,idx_t IC,idx_t H,idx_t W,idx_t K,idx_t OC>
struct Convolution2D {
#if __CUDACC__
  Convolution2D<maxB,IC,H,W,K,OC> * dev; /**< device shadow */
  int T_b = 1024, W_b = 32, N_b_fw = 2304;
  int N_b_bw_L1=1024, N_b_bw_L2=1, N_b_bw_L3=3000;
  
#endif
  cmdline_opt opt;                 /**< command line option  */
  logger * lgr;                    /**< logger */
  tensor<real,maxB,IC,H,W>* x_ptr;    /**< pointer to the input to forward (x) */
  tensor<real,OC,IC,K,K> w;           /**< weight (y = w ＊ x + b) */ 
  tensor<real,OC> b;                  /**< bias (y = w ＊ x + b) */ 
  tensor<real,maxB,OC,H-K+1,W-K+1> y; /**< layer output */
  tensor<real,OC,IC,K,K> gw;          /**< ∂L/∂w */
  tensor<real,OC> gb;                 /**< ∂L/∂b */
  tensor<real,maxB,IC,H,W> gx;        /**< ∂L/∂x */
  AdaDelta<OC,IC,K,K> opt_w;          /**< optimizer for w */
  AdaDelta<OC> opt_b;                 /**< optimizer for b */
  /**
     @brief initialize the layer
     @param (opt) command line options
     @param (lgr) logger
     @param (rg) random number generator for initializing weights
     @param (cfg) configuration parameters
  */
  void init(cmdline_opt opt, logger * lgr, rnd_gen_t& rg, Convolution2DCfg cfg) {
    this->opt = opt;
    this->lgr = lgr;
    (void)cfg;
    real bound = 1.0 / sqrt(IC * K * K);
    /* init weight and bias */
    w.init_uniform(OC, rg, -bound, bound);
    b.init_uniform(OC, rg, -bound, bound);
    /* init optimizers */
    opt_w.init(opt.lr);
    opt_b.init(opt.lr);
  }
  /**
     @brief set the device pointer for this and all subobjects
     @param (dev) a device memory or null

     @details if dev is not null, dev fields of all subojects 
     point to the corresponding subjects in the device memory.
     if dev is not null, all dev fields become null.
  */
  void set_dev(Convolution2D<maxB,IC,H,W,K,OC>* dev) {
#if __CUDACC__
    this->dev = dev;
    w.set_dev(dev ? &dev->w : 0);
    b.set_dev(dev ? &dev->b : 0);
    y.set_dev(dev ? &dev->y : 0);
    gw.set_dev(dev ? &dev->gw : 0);
    gb.set_dev(dev ? &dev->gb : 0);
    gx.set_dev(dev ? &dev->gx : 0);
    opt_w.set_dev(dev ? &dev->opt_w : 0);
    opt_b.set_dev(dev ? &dev->opt_b : 0);
#else
    (void)dev;
#endif
  }
  /**
     @brief the baseline (serial) implementation of update

     @details called both by cpu implementation (update_cpu_base) and
     cuda implementation (update_cuda_base). the call sequence update
     -> update_cpu_base -> update_base on cpu and and is update ->
     update_cuda_base -> update_cuda_base_global ->
     update_cuda_base_device -> update_base

     @sa update
     @sa update_cpu_base
     @sa update_cuda_base
     @sa update_cuda_base_global
     @sa update_cuda_base_device
  */
  __device__ __host__
  void update_base() {
    /* let the optimizer update w and b based on their gradients */
    opt_w.update(w, gw);
    opt_b.update(b, gb);
  }
  /**
     @brief the device function of update called from the 
     global (non-member) function
     @sa update
     @sa update_cuda_base
     @sa update_cuda_base_global
     @sa update_base
  */
  __device__
  void update_cuda_base_device() {
    update_base();
  }
  /**
     @brief a cuda version of baseline code called from the 
     entry function (update)
     @sa update
     @sa update_cuda_base_device
     @sa update_cuda_base_global
     @sa update_base
  */
  void update_cuda_base() {
#if __CUDACC__
    assert(dev);
    launch_and_sync((update_cuda_base_global<<<1,1>>>(dev)));
#else
    err_cuda_code_non_cuda_compiler(opt.algo_s);
#endif
  }
  /**
     @brief a cpu version of baseline code called from the 
     entry function (update)
     @sa update
     @sa update_base
  */
  void update_cpu_base() {
    update_base();
  }
  /**
     @brief update weights of all sublayers with gradients
     that must have been computed
     @sa update_cpu_base
     @sa update_cuda_base
     @sa update_cuda_base_global
     @sa update_cuda_base_device
     @sa update_base
     @sa forward
     @sa backward
  */
  void update() {
    log_start_fun(lgr);
    tsc_t t0 = get_tsc();
    switch (opt.algo) {
      /* add case for your implementations here */
    case algo_cpu_base:
      update_cpu_base(); break;
    case algo_cuda_base:
      update_cuda_base(); break;
    default:
      /* fallback to base */
      if (opt.cuda_algo) {
        update_cuda_base();
      } else {
        update_cpu_base();
      }
      break;
    }
    tsc_t t1 = get_tsc();
    log_end_fun(lgr, t0, t1);
  }
  /**
     @brief the baseline (serial) implementation of forward
     @param (x) input images
     @param (training) 1 if it is called in training not testing

     @details called both by cpu implementation (forward_cpu_base) and
     cuda implementation (forward_cuda_base). the call sequence
     forward -> forward_cpu_base -> forward_base on cpu and and is
     forward -> forward_cuda_base -> forward_cuda_base_global ->
     forward_cuda_base_device -> forward_base

     @sa forward
     @sa forward_cpu_base
     @sa forward_cuda_base
     @sa forward_cuda_base_global
     @sa forward_cuda_base_device
  */
  __device__ __host__ 
  void forward_base(tensor<real,maxB,IC,H,W>& x, int training) {
    (void)training;
    idx_t B = x.n0;             // batch size
    y.set_n0(B);
    x_ptr = &x;                 // save pointer to input for backward
    for (idx_t s = 0; s < B; s++) {       // for each sample
      for (idx_t oc = 0; oc < OC; oc++) { // for each output channel
        for (idx_t i = 0; i < H - K + 1; i++) {   // for each output pixel
          for (idx_t j = 0; j < W - K + 1; j++) { // for each output pixel
            // calculate a single output pixel
            real v = 0.0;
            for (idx_t ic = 0; ic < IC; ic++) { // input channel
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj++) {
                  v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj);
                }
              }
            }
            y(s,oc,i,j) = v + b(oc);
          }
        }
      }
    }
  }
  void forward_cuda_fast(tensor<real,maxB,IC,H,W>& x, int training) {
#if __CUDACC__
    launch_and_sync((forward_cuda_fast_global<<<N_b_fw,T_b>>>(dev, x.dev, training)));
#else
    (void)x;
    (void)training;
    err_cuda_code_non_cuda_compiler(opt.algo_s);
#endif
}
  __device__  
  void forward_cuda_fast_device(tensor<real,maxB,IC,H,W>& x, int training) {
    (void)training;
    idx_t B = x.n0;             // batch size
    y.set_n0(B);
    x_ptr = &x;                 // save pointer to input for backward

    int n = blockDim.x * blockIdx.x + threadIdx.x, s, oc, i, j;
    int H2 = H-K+1, W2 = W-K+1;
    s=(((n/W2)/H2)/OC)%B;
    oc=((n/W2)/H2)%OC;
    i=(n/W2)%H2;
    j=n%W2;
    // printf("hello I am CUDA thread %d out of %d: (s,oc,i,j)=(%d,%d,%d,%d), (B,OC,H2,W2)=(%d,%d,%d,%d)\n", n, blockDim.x*gridDim.x, s,oc,i,j, B,OC,H2,W2);

    if (n < B*OC*H2*W2) {
      // calculate a single output pixel
      real v = 0.0;
      for (idx_t ic = 0; ic < IC; ic++) { // input channel
        for (idx_t di = 0; di < K; di++) {
          for (idx_t dj = 0; dj < K; dj++) {
            v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj);
          }
        }
      }
      y(s,oc,i,j) = v + b(oc);
    }        
  }

  
  void forward_cpu_omp(tensor<real,maxB,IC,H,W>& x, int training) {
    (void)training;
    idx_t B = x.n0;             // batch size
    y.set_n0(B);
    x_ptr = &x;                 // save pointer to input for backward
    // printf("B=%d, OC=%d, H-K+1: %d, W-K+1: %d, IC=%d, K=%d\n", B, OC, H-K+1, W-K+1, IC, K);
    #pragma omp parallel for collapse(4)
    for (idx_t s = 0; s < B; s++) {       // for each sample
      for (idx_t oc = 0; oc < OC; oc++) { // for each output channel
        for (idx_t i = 0; i < H - K + 1; i++) {   // for each output pixel
          for (idx_t j = 0; j < W - K + 1; j++) { // for each output pixel
            // calculate a single output pixel
            real v = 0.0;
            for (idx_t ic = 0; ic < IC; ic++) { // input channel
              for (idx_t di = 0; di < K; di+=2) {
                for (idx_t dj = 0; dj < K; dj+=3) {
                  // v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj);
                  v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj) 
                      +w(oc,ic,di,dj+1) * x(s,ic,i+di,j+dj+1) 
                      +w(oc,ic,di,dj+2) * x(s,ic,i+di,j+dj+2) 
                      +w(oc,ic,di+1,dj) * x(s,ic,i+di+1,j+dj) 
                      +w(oc,ic,di+1,dj+1) * x(s,ic,i+di+1,j+dj+1) 
                      +w(oc,ic,di+1,dj+2) * x(s,ic,i+di+1,j+dj+2); 
                }
              }
            }
            y(s,oc,i,j) = v + b(oc);
          }
        }
      }
    }
  }
  void forward_cpu_simd(tensor<real,maxB,IC,H,W>& x, int training) {
    (void)training;
    idx_t B = x.n0;             // batch size
                     // save pointer to input for backward
    for (idx_t s = 0; s < B; s++) {       // for each sample
      for (idx_t oc = 0; oc < OC; oc++) { // for each output channel
        for (idx_t i = 0; i < H - K + 1; i++) {   // for each output pixel
          for (idx_t j = 0; j < W - K + 1; j++) { // for each output pixel
            // calculate a single output pixel
            real v = 0.0;
            #pragma omp simd reduction(+:v)
            for (idx_t ic = 0; ic < IC; ic++) { // input channel
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj++) {
                  v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj);
                }
              }
            }
            y(s,oc,i,j) = v + b(oc);
          }
        }
      }
    }
  }
  void forward_cpu_omp_simd(tensor<real,maxB,IC,H,W>& x, int training) {
    (void)training;
    idx_t B = x.n0;             // batch size
    y.set_n0(B);
    x_ptr = &x;                 // save pointer to input for backward
    real v = 0.0;
    #pragma omp parallel for collapse(4)
    for (idx_t s = 0; s < B; s++) {       // for each sample
      for (idx_t oc = 0; oc < OC; oc++) { // for each output channel
        for (idx_t i = 0; i < H - K + 1; i++) {   // for each output pixel
          for (idx_t j = 0; j < W - K + 1; j+=3) { // for each output pixel
            // calculate a single output pixel
            // real v0 = 0.0, v1=0.0, v2=0.0;
            // #pragma omp simd
            // for (idx_t ic = 0; ic < IC; ic++) { // input channel
            //   v0 +=  w(oc,ic,0,0) * x(s,ic,i,j)
            //         +w(oc,ic,0,1) * x(s,ic,i,j+1)
            //         +w(oc,ic,0,2) * x(s,ic,i,j+2)
            //         +w(oc,ic,1,0) * x(s,ic,i+1,j)
            //         +w(oc,ic,1,1) * x(s,ic,i+1,j+1)
            //         +w(oc,ic,1,2) * x(s,ic,i+1,j+2)
            //         +w(oc,ic,2,0) * x(s,ic,i+2,j)
            //         +w(oc,ic,2,1) * x(s,ic,i+2,j+1)
            //         +w(oc,ic,2,2) * x(s,ic,i+2,j+2);
            //   v1 +=  w(oc,ic,0,0) * x(s,ic,i,j+1)
            //         +w(oc,ic,0,1) * x(s,ic,i,j+2)
            //         +w(oc,ic,0,2) * x(s,ic,i,j+3)
            //         +w(oc,ic,1,0) * x(s,ic,i+1,j+1)
            //         +w(oc,ic,1,1) * x(s,ic,i+1,j+2)
            //         +w(oc,ic,1,2) * x(s,ic,i+1,j+3)
            //         +w(oc,ic,2,0) * x(s,ic,i+2,j+1)
            //         +w(oc,ic,2,1) * x(s,ic,i+2,j+2)
            //         +w(oc,ic,2,2) * x(s,ic,i+2,j+3);
            //   v2 +=  w(oc,ic,0,0) * x(s,ic,i,j+2)
            //         +w(oc,ic,0,1) * x(s,ic,i,j+3)
            //         +w(oc,ic,0,2) * x(s,ic,i,j+4)
            //         +w(oc,ic,1,0) * x(s,ic,i+1,j+2)
            //         +w(oc,ic,1,1) * x(s,ic,i+1,j+3)
            //         +w(oc,ic,1,2) * x(s,ic,i+1,j+4)
            //         +w(oc,ic,2,0) * x(s,ic,i+2,j+2)
            //         +w(oc,ic,2,1) * x(s,ic,i+2,j+3)
            //         +w(oc,ic,2,2) * x(s,ic,i+2,j+4);
            // }
            // y(s,oc,i,j) = v0 + b(oc);
            // y(s,oc,i,j+1) = v1 + b(oc);
            // y(s,oc,i,j+2) = v2 + b(oc);
            real v = 0.0;
            #pragma omp simd reduction(+: v)
            for (idx_t ic = 0; ic < IC; ic++) { // input channel
              for (idx_t di = 0; di < K; di+=2) {
                for (idx_t dj = 0; dj < K; dj+=3) {
                  // v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj);
                  v += w(oc,ic,di,dj) * x(s,ic,i+di,j+dj) 
                      +w(oc,ic,di,dj+1) * x(s,ic,i+di,j+dj+1) 
                      +w(oc,ic,di,dj+2) * x(s,ic,i+di,j+dj+2) 
                      +w(oc,ic,di+1,dj) * x(s,ic,i+di+1,j+dj) 
                      +w(oc,ic,di+1,dj+1) * x(s,ic,i+di+1,j+dj+1) 
                      +w(oc,ic,di+1,dj+2) * x(s,ic,i+di+1,j+dj+2); 
                }
              }
            }
            y(s,oc,i,j) = v + b(oc);
          }
        }
      }
    }
  }
  /**
     @brief the device function of forward called from the 
     global (non-member) function
     @param (x) input images
     @param (training) 1 if it is called in training not testing
     @sa forward
     @sa forward_cuda_base
     @sa forward_cuda_base_global
     @sa forward_base
  */
  __device__
  void forward_cuda_base_device(tensor<real,maxB,IC,H,W>& x, int training) {
    forward_base(x, training);
  }
  /**
     @brief a cuda version of baseline code called from the 
     entry function (forward)
     @param (x) input images
     @param (training) 1 if it is called in training not testing
     @sa forward
     @sa forward_cuda_base_global
     @sa forward_cuda_base_device
     @sa forward_base
  */
  void forward_cuda_base(tensor<real,maxB,IC,H,W>& x, int training) {
#if __CUDACC__
    launch_and_sync((forward_cuda_base_global<<<1,1>>>(dev, x.dev, training)));
#else
    (void)x;
    (void)training;
    err_cuda_code_non_cuda_compiler(opt.algo_s);
#endif
  }
  /**
     @brief a cpu version of baseline code called from the 
     entry function (forward)
     @param (x) input images
     @param (training) 1 if it is called in training not testing
     @sa forward
     @sa forward_base
  */
  void forward_cpu_base(tensor<real,maxB,IC,H,W>& x, int training) {
    forward_base(x, training);
  }
  
  /**
     @brief forward phase of the layer
     @param (x) input images
     @param (training) 1 if it is called in training not testing
     @sa forward_base
     @sa forward_cpu_base
     @sa forward_cuda_base
     @sa forward_cuda_base_global
     @sa forward_cuda_base_device
     @sa backward
     @sa update
  */
  tensor<real,maxB,OC,H-K+1,W-K+1>& forward(tensor<real,maxB,IC,H,W>& x, int training) {
    log_start_fun(lgr);
    tsc_t t0 = get_tsc();
    switch (opt.algo) {
      /* add case for your implementations here */
    case algo_cpu_omp_simd:
      forward_cpu_omp_simd(x, training); break;
    case algo_cpu_simd:
      forward_cpu_simd(x, training); break;
    case algo_cpu_omp:
      forward_cpu_omp(x, training); break;
    case algo_cpu_base:
      forward_cpu_base(x, training); break;
    case algo_cuda_base:
      forward_cuda_base(x, training); break;
    case algo_cuda_fast:
      forward_cuda_fast(x, training); break;
    default:
      if (opt.cuda_algo) {
        forward_cuda_base(x, training);
      } else {
        forward_cpu_base(x, training);
      }        
    }
    tsc_t t1 = get_tsc();
    log_end_fun(lgr, t0, t1);
    return y;
  }
  /**
     @brief the baseline (serial) implementation of backward
     @param (gy) gradient of loss with respect to the output
     @details called both by cpu implementation (backward_cpu_base)
     and cuda implementation (backward_cuda_base). the call sequence
     backward -> backward_cpu_base -> backward_base on cpu and and is
     backward -> backward_cuda_base -> backward_cuda_base_global ->
     backward_cuda_base_device -> backward_base
     @sa backward
     @sa backward_cpu_base
     @sa backward_cuda_base
     @sa backward_cuda_base_global
     @sa backward_cuda_base_device
     @sa backward_base
  */
  __device__ __host__ 
  void backward_base(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    idx_t B = gy.n0;
    gw.set_n0(OC);
    gb.set_n0(OC);
    gx.set_n0(B);
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    for (idx_t oc = 0; oc < OC; oc++) {   // output channel
      for (idx_t ic = 0; ic < IC; ic++) { // input channel
        for (idx_t di = 0; di < K; di++) { // kernel pixel
          for (idx_t dj = 0; dj < K; dj++) { // kernel pixel
            real v = 0.0;
            for (idx_t s = 0; s < B; s++) { // training samples
              for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
                for (idx_t j = 0; j < W - K + 1; j++) { // sample pixel
                  v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
                }
              }
            }
            gw(oc,ic,di,dj) = v;
          }
        }
      }
    }
    for (idx_t oc = 0; oc < OC; oc++) {
      real v = 0.0;
      for (idx_t s = 0; s < B; s++) {
        for (idx_t i = 0; i < H - K + 1; i++) {
          for (idx_t j = 0; j < W - K + 1; j++) {
            v += gy(s,oc,i,j);
          }
        }
      }
      gb(oc) = v;
    }
    for (idx_t s = 0; s < B; s++) {
      for (idx_t ic = 0; ic < IC; ic++) {
        for (idx_t i = 0; i < H; i++) {
          for (idx_t j = 0; j < W; j++) {
            real v = 0.0;
            for (idx_t oc = 0; oc < OC; oc++) {
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj++) {
                  if (0 <= i - di && i - di < H - K + 1
                      && 0 <= j - dj && j - dj < W - K + 1) {
                    v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj);
                  }
                }
              }
            }
            gx(s,ic,i,j) = v;
          }
        }
      }
    }
  }

  void backward_cuda_fast(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
#if __CUDACC__
    idx_t B = gy.n0;
    printf("%d %d %d\n", gy.n0, gy.n0, gy.n0);
    gw.set_n0(OC);
    gb.set_n0(OC);
    gx.set_n0(B);
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    launch_and_sync((__L1__backward_cuda_fast_global<<<N_b_bw_L1,T_b>>>(dev, gy.dev)));
    launch_and_sync((__L2__backward_cuda_fast_global<<<N_b_bw_L2,T_b>>>(dev, gy.dev)));
    launch_and_sync((__L3__backward_cuda_fast_global<<<N_b_bw_L3,T_b>>>(dev, gy.dev)));

#else
    (void)x;
    (void)training;
    err_cuda_code_non_cuda_compiler(opt.algo_s);
#endif
  }
  __device__  
  void __L1__backward_cuda_fast_device(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    int n = blockDim.x * blockIdx.x + threadIdx.x, oc, ic, di, dj;
    idx_t B = gy.n0;
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    oc=(((n/K)/K)/OC)%B;
    ic=((n/K)/K)%OC;
    di=(n/K)%K;
    dj=n%K;
    
    if(n < OC*IC*K*K){
      real v = 0.0;
      for (idx_t s = 0; s < B; s++) { // training samples
        for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
          for (idx_t j = 0; j < W - K + 1; j++) { // sample pixel
            v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
          }
        }
      }
      gw(oc,ic,di,dj) = v;
    }
  }
  __device__  
  void __L2__backward_cuda_fast_device(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    int n = blockDim.x * blockIdx.x + threadIdx.x, oc;
    idx_t B = gy.n0;
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    oc = n % OC;
    if(n < OC){
      real v = 0.0;
      for (idx_t s = 0; s < B; s++) {
        for (idx_t i = 0; i < H - K + 1; i++) {
          for (idx_t j = 0; j < W - K + 1; j++) {
            v += gy(s,oc,i,j);
          }
        }
      }
      gb(oc) = v;
    }
  }
  __device__  
  void __L3__backward_cuda_fast_device(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    int n = blockDim.x * blockIdx.x + threadIdx.x, s, ic, i, j;
    idx_t B = gy.n0;
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    s=(((n/W)/H)/IC)%B;
    ic=((n/W)/H)%IC;
    i=(n/W)%H;
    j=n%W;

    if(n < B*IC*H*W){
      real v = 0.0;
      for (idx_t oc = 0; oc < OC; oc++) {
        for (idx_t di = 0; di < K; di++) {
          for (idx_t dj = 0; dj < K; dj++) {
            if (0 <= i - di && i - di < H - K + 1
                && 0 <= j - dj && j - dj < W - K + 1) {
              v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj);
            }
          }
        }
      }
      gx(s,ic,i,j) = v;
    }
  }
  void backward_cpu_omp(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    idx_t B = gy.n0;
    gw.set_n0(OC);
    gb.set_n0(OC);
    gx.set_n0(B);
    tensor<real,maxB,IC,H,W>& x = *x_ptr;

    #pragma omp parallel for collapse(4)
    for (idx_t oc = 0; oc < OC; oc++) {   // output channel
      for (idx_t ic = 0; ic < IC; ic++) { // input channel
        for (idx_t di = 0; di < K; di++) { // kernel pixel
          for (idx_t dj = 0; dj < K; dj++) { // kernel pixel
            // real v = 0.0;
            // for (idx_t s = 0; s < B; s++) { // training samples
            //   for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
            //     for (idx_t j = 0; j < W - K + 1; j++) { // sample pixel
            //       v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
            //     }
            //   }
            // }
            real v = 0.0;
            for (idx_t s = 0; s < B; s++) { // training samples
              for (idx_t i = 0; i < H - K + 1; i+=2) { // sample pixel
                for (idx_t j = 0; j < W - K + 1; j+=4) { // sample pixel
                  // v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
                  v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj)
                      +gy(s,oc,i,j+1) * x(s,ic,i+di,j+1+dj)
                      +gy(s,oc,i,j+2) * x(s,ic,i+di,j+2+dj)
                      +gy(s,oc,i,j+3) * x(s,ic,i+di,j+3+dj)
                      +gy(s,oc,i+1,j) * x(s,ic,i+1+di,j+dj)
                      +gy(s,oc,i+1,j+1) * x(s,ic,i+1+di,j+1+dj)
                      +gy(s,oc,i+1,j+2) * x(s,ic,i+1+di,j+2+dj)
                      +gy(s,oc,i+1,j+3) * x(s,ic,i+1+di,j+3+dj);
                }
              }
            }
            gw(oc,ic,di,dj) = v;

          }
        }
      }
    }
    #pragma omp parallel for
    for (idx_t oc = 0; oc < OC; oc++) {
      real v = 0.0;
      for (idx_t s = 0; s < B; s++) {
        for (idx_t i = 0; i < H - K + 1; i+=2) {
          for (idx_t j = 0; j < W - K + 1; j+=2) {
            // v += gy(s,oc,i,j);
            v += gy(s,oc,i,j) + gy(s,oc,i,j+1) + gy(s,oc,i+1,j) + gy(s,oc,i+1,j+1);
          }
        }
      }
      gb(oc) = v;
    }
    #pragma omp parallel for collapse(4)
    for (idx_t s = 0; s < B; s++) {
      for (idx_t ic = 0; ic < IC; ic++) {
        for (idx_t i = 0; i < H; i++) {
          for (idx_t j = 0; j < W; j++) {
            real v = 0.0;
            for (idx_t oc = 0; oc < OC; oc++) {
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj+=3) {
                  if (0 <= i - di && i - di < H - K + 1
                      && 0 <= j - dj && j - dj < W - K + 1) {
                    // v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj);
                    v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj)
                        +gy(s,oc,i-di,j-dj+1) * w(oc,ic,di,dj+1)
                        +gy(s,oc,i-di,j-dj+2) * w(oc,ic,di,dj+2);
                  }
                }
              }
            }
            gx(s,ic,i,j) = v;
          }
        }
      }
    }
  }
  void backward_cpu_simd(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    idx_t B = gy.n0;
    gw.set_n0(OC);
    gb.set_n0(OC);
    gx.set_n0(B);
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    for (idx_t oc = 0; oc < OC; oc++) {   // output channel
      for (idx_t ic = 0; ic < IC; ic++) { // input channel
        for (idx_t di = 0; di < K; di++) { // kernel pixel
          for (idx_t dj = 0; dj < K; dj++) { // kernel pixel
            real v = 0.0;
            
            for (idx_t s = 0; s < B; s++) { // training samples
            #pragma omp simd reduction(+:v)
              for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
                for (idx_t j = 0; j < W - K + 1; j++) { // sample pixel
                  v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
                }
              }
            }
            gw(oc,ic,di,dj) = v;
          }
        }
      }
    }
    for (idx_t oc = 0; oc < OC; oc++) {
      real v = 0.0;
      for (idx_t s = 0; s < B; s++) {
        #pragma omp simd reduction(+:v)
        for (idx_t i = 0; i < H - K + 1; i++) {
          for (idx_t j = 0; j < W - K + 1; j++) {
            v += gy(s,oc,i,j);
          }
        }
      }
      gb(oc) = v;
    }
    for (idx_t s = 0; s < B; s++) {
      for (idx_t ic = 0; ic < IC; ic++) {
        for (idx_t i = 0; i < H; i++) {
          for (idx_t j = 0; j < W; j++) {
            real v = 0.0;
            #pragma omp simd reduction(+:v)
            for (idx_t oc = 0; oc < OC; oc++) {
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj++) {
                  if (0 <= i - di && i - di < H - K + 1
                      && 0 <= j - dj && j - dj < W - K + 1) {
                    v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj);
                  }
                }
              }
            }
            gx(s,ic,i,j) = v;
          }
        }
      }
    }
  }
  void backward_cpu_omp_simd(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    idx_t B = gy.n0;
    gw.set_n0(OC);
    gb.set_n0(OC);
    gx.set_n0(B);
    tensor<real,maxB,IC,H,W>& x = *x_ptr;
    
    // #pragma omp parallel for collapse(4)
    // for (idx_t oc = 0; oc < OC; oc++) {   // output channel
    //   for (idx_t ic = 0; ic < IC; ic++) { // input channel
    //     for (idx_t di = 0; di < K; di++){
    //       for (idx_t dj = 0; dj < K; dj++){
    //         gw(oc,ic,di,dj) = 0.0;
    //       }
    //     }
    //   }
    // }
    // real s00=0.0,s01=0.0,s02=0.0,s10=0.0,s11=0.0,s12=0.0,s20=0.0,s21=0.0,s22=0.0;
    // #pragma omp parallel for collapse(5) private(s00,s01,s02,s10,s11,s12,s20,s21,s22)
    // for (idx_t oc = 0; oc < OC; oc++) {   // output channel
    //   for (idx_t ic = 0; ic < IC; ic++) { // input channel
    //     for (idx_t s = 0; s < B; s++) { // training samples
    //       for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
    //         for (idx_t j = 0; j < W - K + 1; j+=4) { // sample pixel
    //           s00 = gy(s,oc,i,j) * x(s,ic,i,j)     +gy(s,oc,i,j+1) * x(s,ic,i,j+1)   +gy(s,oc,i,j+2) * x(s,ic,i,j+2)   +gy(s,oc,i,j+3) * x(s,ic,i,j+3);
    //           s01 = gy(s,oc,i,j) * x(s,ic,i,j+1)   +gy(s,oc,i,j+1) * x(s,ic,i,j+2)   +gy(s,oc,i,j+2) * x(s,ic,i,j+3)   +gy(s,oc,i,j+3) * x(s,ic,i,j+4);
    //           s02 = gy(s,oc,i,j) * x(s,ic,i,j+2)   +gy(s,oc,i,j+1) * x(s,ic,i,j+3)   +gy(s,oc,i,j+2) * x(s,ic,i,j+4)   +gy(s,oc,i,j+3) * x(s,ic,i,j+5);
    //           s10 = gy(s,oc,i,j) * x(s,ic,i+1,j)   +gy(s,oc,i,j+1) * x(s,ic,i+1,j+1) +gy(s,oc,i,j+2) * x(s,ic,i+1,j+2) +gy(s,oc,i,j+3) * x(s,ic,i+1,j+3);
    //           s11 = gy(s,oc,i,j) * x(s,ic,i+1,j+1) +gy(s,oc,i,j+1) * x(s,ic,i+1,j+2) +gy(s,oc,i,j+2) * x(s,ic,i+1,j+3) +gy(s,oc,i,j+3) * x(s,ic,i+1,j+4);
    //           s12 = gy(s,oc,i,j) * x(s,ic,i+1,j+2) +gy(s,oc,i,j+1) * x(s,ic,i+1,j+3) +gy(s,oc,i,j+2) * x(s,ic,i+1,j+4) +gy(s,oc,i,j+3) * x(s,ic,i+1,j+5);
    //           s20 = gy(s,oc,i,j) * x(s,ic,i+2,j)   +gy(s,oc,i,j+1) * x(s,ic,i+2,j+1) +gy(s,oc,i,j+2) * x(s,ic,i+2,j+2) +gy(s,oc,i,j+3) * x(s,ic,i+2,j+3);
    //           s21 = gy(s,oc,i,j) * x(s,ic,i+2,j+1) +gy(s,oc,i,j+1) * x(s,ic,i+2,j+2) +gy(s,oc,i,j+2) * x(s,ic,i+2,j+3) +gy(s,oc,i,j+3) * x(s,ic,i+2,j+4);
    //           s22 = gy(s,oc,i,j) * x(s,ic,i+2,j+2) +gy(s,oc,i,j+1) * x(s,ic,i+2,j+3) +gy(s,oc,i,j+2) * x(s,ic,i+2,j+4) +gy(s,oc,i,j+3) * x(s,ic,i+2,j+5);

    //           #pragma omp atomic
    //             gw(oc,ic,0,0) += s00;
    //           #pragma omp atomic
    //             gw(oc,ic,0,1) += s01;
    //           #pragma omp atomic
    //             gw(oc,ic,0,2) += s02;
    //           #pragma omp atomic
    //             gw(oc,ic,1,0) += s10;
    //           #pragma omp atomic
    //             gw(oc,ic,1,1) += s11;
    //           #pragma omp atomic
    //             gw(oc,ic,1,2) += s12;
    //           #pragma omp atomic
    //             gw(oc,ic,2,0) += s20;
    //           #pragma omp atomic
    //             gw(oc,ic,2,1) += s21;
    //           #pragma omp atomic
    //             gw(oc,ic,2,2) += s22;
              
    //         }
    //       }
    //     }
    //   }
    // }


    // #pragma omp parallel for
    // for (idx_t oc = 0; oc < OC; oc++) {
    //   gy(oc) = 0.0;
    // }
    // real v = 0.0;
    // #pragma omp parallel for collapse(4) private(v)
    // for (idx_t oc = 0; oc < OC; oc++) {
    //   for (idx_t s = 0; s < B; s++) {
    //     for (idx_t i = 0; i < H - K + 1; i+=2) {
    //       for (idx_t j = 0; j < W - K + 1; j+=4) {
    //         v =  gy(s,oc,i,j) 
    //             +gy(s,oc,i,j+1) 
    //             +gy(s,oc,i,j+2) 
    //             +gy(s,oc,i,j+3) 
    //             +gy(s,oc,i+1,j) 
    //             +gy(s,oc,i+1,j+1)
    //             +gy(s,oc,i+1,j+2)
    //             +gy(s,oc,i+1,j+3)
    //             ;
    //         // #pragma omp atomic
    //         gy(oc) += v;
    //       }
    //     }
    //   }
    // }
    // #pragma omp parallel for collapse(4)
    // for (idx_t s = 0; s < B; s++) {
    //   for (idx_t ic = 0; ic < IC; ic++) {
    //     for (idx_t i = 0; i < H; i++) {
    //       for (idx_t j = 0; j < W; j++) {
    //         gx(s,ic,i,j) = 0.0;
    //       }
    //     }
    //   }
    // }
    // #pragma omp parallel for collapse(5)
    // for (idx_t s = 0; s < B; s++) {
    //   for (idx_t ic = 0; ic < IC; ic++) {
    //     for (idx_t i = 2; i < H; i+=3) {
    //       for (idx_t j = 2; j < W; j+=3) {
    //         for (idx_t oc = 0; oc < OC; oc++) {
    //           real v = gy(s,oc,i,j) * w(oc,ic,0,0)
    //               +gy(s,oc,i,j-1)   * w(oc,ic,0,1)
    //               +gy(s,oc,i,j-2)   * w(oc,ic,0,2)
    //               +gy(s,oc,i-1,j)   * w(oc,ic,1,0)
    //               +gy(s,oc,i-1,j-1) * w(oc,ic,1,1)
    //               +gy(s,oc,i-1,j-2) * w(oc,ic,1,2)
    //               +gy(s,oc,i-2,j)   * w(oc,ic,2,0)
    //               +gy(s,oc,i-2,j-1) * w(oc,ic,2,1)
    //               +gy(s,oc,i-2,j-2) * w(oc,ic,2,2)
    //               ;
    //           #pragma omp atomic
    //           gx(s,ic,i,j) += v;
    //         }
            
    //       }
    //     }
    //   }
    // }
    #pragma omp parallel for collapse(4)
    for (idx_t oc = 0; oc < OC; oc++) {   // output channel
      for (idx_t ic = 0; ic < IC; ic++) { // input channel
        for (idx_t di = 0; di < K; di++) { // kernel pixel
          for (idx_t dj = 0; dj < K; dj++) { // kernel pixel
            // real v = 0.0;
            // for (idx_t s = 0; s < B; s++) { // training samples
            //   for (idx_t i = 0; i < H - K + 1; i++) { // sample pixel
            //     for (idx_t j = 0; j < W - K + 1; j++) { // sample pixel
            //       v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
            //     }
            //   }
            // }
            real v = 0.0;
            #pragma omp simd reduction(+: v)
            for (idx_t s = 0; s < B; s++) { // training samples
              for (idx_t i = 0; i < H - K + 1; i+=2) { // sample pixel
                for (idx_t j = 0; j < W - K + 1; j+=4) { // sample pixel
                  // v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj);
                  v += gy(s,oc,i,j) * x(s,ic,i+di,j+dj)
                      +gy(s,oc,i,j+1) * x(s,ic,i+di,j+1+dj)
                      +gy(s,oc,i,j+2) * x(s,ic,i+di,j+2+dj)
                      +gy(s,oc,i,j+3) * x(s,ic,i+di,j+3+dj)
                      +gy(s,oc,i+1,j) * x(s,ic,i+1+di,j+dj)
                      +gy(s,oc,i+1,j+1) * x(s,ic,i+1+di,j+1+dj)
                      +gy(s,oc,i+1,j+2) * x(s,ic,i+1+di,j+2+dj)
                      +gy(s,oc,i+1,j+3) * x(s,ic,i+1+di,j+3+dj);
                }
              }
            }
            gw(oc,ic,di,dj) = v;

          }
        }
      }
    }
    #pragma omp parallel for
    for (idx_t oc = 0; oc < OC; oc++) {
      real v = 0.0;
      
      for (idx_t s = 0; s < B; s++) {
        for (idx_t i = 0; i < H - K + 1; i+=2) {
          for (idx_t j = 0; j < W - K + 1; j+=2) {
            // v += gy(s,oc,i,j);
            v += gy(s,oc,i,j) + gy(s,oc,i,j+1) + gy(s,oc,i+1,j) + gy(s,oc,i+1,j+1);
          }
        }
      }
      gb(oc) = v;
    }
    #pragma omp parallel for collapse(4)
    for (idx_t s = 0; s < B; s++) {
      for (idx_t ic = 0; ic < IC; ic++) {
        for (idx_t i = 0; i < H; i++) {
          for (idx_t j = 0; j < W; j++) {
            real v = 0.0;
            #pragma omp simd reduction(+: v)
            for (idx_t oc = 0; oc < OC; oc++) {
              for (idx_t di = 0; di < K; di++) {
                for (idx_t dj = 0; dj < K; dj+=3) {
                  if (0 <= i - di && i - di < H - K + 1
                      && 0 <= j - dj && j - dj < W - K + 1) {
                    // v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj);
                    v += gy(s,oc,i-di,j-dj) * w(oc,ic,di,dj)
                        +gy(s,oc,i-di,j-dj+1) * w(oc,ic,di,dj+1)
                        +gy(s,oc,i-di,j-dj+2) * w(oc,ic,di,dj+2);
                  }
                }
              }
            }
            gx(s,ic,i,j) = v;
          }
        }
      }
    }
  }
  /**
     @brief the device function of backward called from the 
     global (non-member) function
     @param (gy) gradient of loss with respect to the output
     @sa backward
     @sa backward_cuda_base
     @sa backward_cuda_base_global
     @sa backward_base
  */
  __device__
  void backward_cuda_base_device(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    backward_base(gy);
  }
  /**
     @brief a cuda version of baseline code called from the 
     entry function (backward)
     @param (gy) gradient of loss with respect to the output
     @sa backward
     @sa backward_cuda_base_global
     @sa backward_cuda_base_device
     @sa backward_base
  */
  void backward_cuda_base(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
#if __CUDACC__
    launch_and_sync((backward_cuda_base_global<<<1,1>>>(dev, gy.dev)));
#else
    (void)gy;
    err_cuda_code_non_cuda_compiler(opt.algo_s);
#endif
  }
  /**
     @brief a cpu version of baseline code called from the 
     entry function (backward)
     @param (gy) gradient of loss with respect to the output
     @sa backward
     @sa backward_base
  */
  void backward_cpu_base(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    backward_base(gy);
  }
  /**
     @brief calc the gradient of loss wrt the input (x)
     @param (gy) gradient of loss with respect to the output
     @details calc the gradient of loss wrt the input. along the way,
     it also calculates the gradient of loss wrt weights for
     all sublayers that have weights. since this is the entire
     network, gy is actually a vector whose components are all 1.
     (loss = sum of losses of each data).
     @sa backward_cpu_base
     @sa backward_cuda_base
     @sa backward_cuda_base_global
     @sa backward_cuda_base_device
     @sa backward_base
     @sa forward
     @sa update
  */
  tensor<real,maxB,IC,H,W>& backward(tensor<real,maxB,OC,H-K+1,W-K+1>& gy) {
    log_start_fun(lgr);
    tsc_t t0 = get_tsc();
    switch (opt.algo) {
      /* add case for your implementations here */
    case algo_cpu_omp_simd:
      backward_cpu_omp_simd(gy); break;
    case algo_cpu_simd:
      backward_cpu_simd(gy); break;
    case algo_cpu_omp:
      backward_cpu_omp(gy); break;
    case algo_cpu_base:
      backward_cpu_base(gy); break;
    case algo_cuda_base:
      backward_cuda_base(gy); break;
    case algo_cuda_fast:
      backward_cuda_fast(gy); break;
    default:
      if (opt.cuda_algo) {
        backward_cuda_base(gy);
      } else {
        backward_cpu_base(gy);
      }        
    }
    tsc_t t1 = get_tsc();
    log_end_fun(lgr, t0, t1);
    return gx;
  }
  /* member functions below assume data are on the host.
     they are only for checking (debugging) implementations */
  /**
     @brief randomly set all gradients to values between p and q
     @param (rg) random number generator
     @param (p) minimum value of a component
     @param (q) maximum value of a component
     @details only used for checking gradient computation
  */
  void rand_grad(rnd_gen_t& rg, real p, real q) {
    gw.init_uniform(OC, rg, p, q);
    gb.init_uniform(OC, rg, p, q);
  }
  /**
     @brief set all gradients to gradients of another object o
     @param (o) the object from which gradients get copied
     @details only used for checking gradient computation
  */
  void copy_grad(Convolution2D<maxB,IC,H,W,K,OC>& o) {
    gw = o.gw;
    gb = o.gb;
  }
  /**
     @brief w += alpha * gw
     @param (alpha) alpha of w += alpha * gw
  */
  void add_grad(real alpha) {
    w.add_(alpha, gw);
    b.add_(alpha, gb);
  }
  /**
     @brief take the inner product of gradients
     @param (o) the object to take the inner product with
     @details take the inner product of this object's gradient and b's
     gradient. only used for checking gradient computation
  */
  double grad_dot_grad(Convolution2D<maxB,IC,H,W,K,OC>& o) {
    return gw.dot(o.gw) + gb.dot(o.gb);
  }
};

/**
   @brief entry point of this header file
   @param (argc) the number of command line args
   @param (argv) command line args
   @sa convolution_grad_check_rand
   @details if this header file is included from
   a main C++ file and define convolution_main to be main
   (e.g., with -Dconvolution_main=main), then this
   function becomes th main function of the executable.
   it calls grad_check repeatedly to test
   the implementation of backward of convolution.
*/
int convolution_main(int argc, char ** argv) {
  cmdline_opt opt = parse_args(argc, argv);
  if (opt.error || opt.help) usage(argv[0]);
  const idx_t maxB = MAX_BATCH_SIZE;
  const idx_t B = min_i(maxB, opt.batch_size);
  const idx_t IC = 1;
  const idx_t H = 28;
  const idx_t W = 28;
  const idx_t K = 3;
  const idx_t OC = 32;
  const int n_checks = opt.epochs;
  /* logger */
  logger lgr;
  lgr.start_log(opt);
  /* initialize random number generator */
  rnd_gen_t rg;
  rg.seed(opt.weight_seed);
  /* check errors */
  double max_e = 0.0;
  double sum_e = 0.0;
  Convolution2DCfg cfg;
  for (int iter = 0; iter < n_checks; iter++) {
    printf("==== %d ====\n", iter);
    double e = grad_check<Convolution2D<maxB,IC,H,W,K,OC>,
                          tensor<real,maxB,IC,H,W>,
                          tensor<real,maxB,OC,H-K+1,W-K+1>,
                          Convolution2DCfg>(opt, &lgr, rg, cfg, B);
    max_e = max_r(max_e, e);
    sum_e += e;
  }
  printf("max relative error = %.9f\n", max_e);
  printf("avg relative error = %.9f\n", sum_e / n_checks);
  lgr.end_log();
  return 0;
}
