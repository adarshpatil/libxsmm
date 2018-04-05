/******************************************************************************
** Copyright (c) 2016-2018, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Alexander Heinecke, Rajkishore Barik,
 ** Ankush Mandal, Evangelos Georganas (Intel Corp.)
 ******************************************************************************/
#include "libxsmm_dnn_handle.h"
#include "libxsmm_main.h"
#include <libxsmm.h>
#include "libxsmm_dnn_dryruns.h"
#include "libxsmm_dnn_setup.h"

#if !defined(LIBXSMM_DNN_HANDLE_DEBUG) && 0
# define LIBXSMM_DNN_HANDLE_DEBUG
#endif

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <stdlib.h>
#include <string.h>
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
# include <stdio.h>
#endif
#if defined(_OPENMP)
# include <omp.h>
#endif
#if !defined(NDEBUG)
# include <stdio.h>
#endif
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif


LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_internal_create_conv_handle_direct( libxsmm_dnn_layer* handle ) {
  /* flag to test if we found an architecture which is supported */
  int noarch = 1;
  int internal_format_type;
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
  const char *const env = getenv("LIBXSMM_DNN_INTERNAL_FORMAT");
  LIBXSMM_ASSERT(0 != handle);

  if ( 0 == env || 0 == *env) {
    /* Default internal format type */
    handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
  } else {
    internal_format_type = atoi(env);
    if (internal_format_type == 1) {
      handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
    } else if ( internal_format_type == 2) {
      handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_2;
    } else {
      status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
      free(handle);
      handle = 0;
      return status;
    }
  }

  /* let's enable generic code path by default */
  handle->use_fwd_generic = 1;
  handle->use_bwd_generic = 1;
  handle->use_upd_generic = 1;

  /* @FIXME, we should find a better knob */
#ifdef __AVX512F__
  handle->use_thread_private_jit = 1;
#else
  handle->use_thread_private_jit = 0;
#endif

  /* If we do not have AVX512 arch disable kernel streams  */
  if (libxsmm_target_archid != LIBXSMM_X86_AVX512_MIC  &&
      libxsmm_target_archid != LIBXSMM_X86_AVX512_CORE &&
      libxsmm_target_archid != LIBXSMM_X86_AVX512_KNM  &&
      libxsmm_target_archid != LIBXSMM_X86_AVX512_ICL     ) {
    handle->use_thread_private_jit = 0;
  }

  /* If we use any options/fuse ops, disable kernel streams */
  if ( ((handle->desc.fuse_ops & LIBXSMM_DNN_CONV_FUSE_BIAS) > 0) ) {
    handle->use_thread_private_jit = 0;
  }

  /* If we do not run on custom/custom format, disable kernel streams */
  if (handle->buffer_format != LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM || handle->filter_format != LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM ) {
    handle->use_thread_private_jit = 0;
  }

  /* now architecture specific */
  if ( (libxsmm_target_archid == LIBXSMM_X86_AVX512_MIC  ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_CORE ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM  ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_ICL    ) &&
      handle->use_thread_private_jit == 1 )
  {
    /* Calculate feature map blocking factors based on precision and datatypes */
    /* This is basically a decision pertaining for all three passes: FWD, BWD and UPD */
    status = libxsmm_dnn_setup_feature_map_blocks(handle, &noarch);
    if ( status ==  LIBXSMM_DNN_ERR_UNSUPPORTED_DATATYPE) {
      free(handle);
      handle = 0;
      return status;
    } 
  }

  if (noarch == 0) {
    /* Forward path setup */
    /* Backward path setup */
    /* Weight update path setup */
    {
      handle->barrier = libxsmm_barrier_create(handle->desc.threads, 1);
      /* backward transpose filters */
      handle->scratch1 = 0;
      handle->scratch1_size = handle->blocksifm_lp * handle->ifmblock * handle->blocksofm * handle->ofmblock
        * handle->desc.R * handle->desc.S * handle->fm_lp_block * libxsmm_dnn_typesize(handle->datatype_in);
      if (handle->fm_lp_block > 1) {
        /* If low precision, we need extra buffer to store intermediate weight tensor */
        handle->scratch1_size *= 2;
      }

      /* weight update transpose of minibatch */
      handle->scratch3 = 0;
      handle->scratch3_size = handle->desc.N * handle->blocksifm_lp * handle->ifmblock * handle->ifhp * (handle->ifwp+8)
        * handle->fm_lp_block * libxsmm_dnn_typesize(handle->datatype_in);

      /* minibatch parallel execution of weight update kernel */
      if ( ((handle->blocksifm * handle->blocksofm) < handle->desc.threads) || (handle->use_thread_private_jit) ) {
        handle->upd_use_thread_fil = 1;
        handle->scratch4 = 0;
        handle->scratch4_size = 2 * handle->desc.threads * handle->desc.C * handle->desc.K * handle->desc.R * handle->desc.S * libxsmm_dnn_typesize(handle->datatype_out);
#if 0
        handle->scratch4_size += 32*handle->desc.threads *  handle->blocksofm *  handle->blocksifm * handle->desc.R
          * handle->desc.S * handle->ifmblock * handle->ofmblock * libxsmm_dnn_typesize(handle->datatype_out);
#endif
        /* enable external reduce of filter scratch */
        if ( (handle->options & LIBXSMM_DNN_CONV_OPTION_UPD_NO_FILTER_REDUCE) > 0 ) {
          handle->upd_use_external_reduce = 1;
        }
      } else {
        handle->scratch4 = 0;
        handle->scratch4_size = 0;
        handle->upd_use_thread_fil = 0;
      }

      /* Allocate scratch for additional output transpose */
      if (handle->use_lp_kernel == 1) {
        handle->scratch6 = 0;
        handle->scratch6_size = handle->desc.N * handle->blocksofm * handle->ofmblock * (handle->ofhp+2*handle->desc.pad_h) * (handle->ofwp+8+2*handle->desc.pad_w) * libxsmm_dnn_typesize(handle->datatype_in);
      } else {
        handle->scratch6 = 0;
        handle->scratch6_size = 0;
      }
    }
  } else {
    /* @TODO let's make these env variable to study */
    int tmp_max_c_block = 16;
    int tmp_max_k_block = 16;
    int tmp_block = 0;

    if ( handle->desc.C < tmp_max_c_block ) {
      handle->ifmblock = handle->desc.C;
    } else {
      for ( tmp_block = 1; tmp_block <= tmp_max_c_block; tmp_block *= 2 ) {
        if ( handle->desc.C % tmp_block == 0 ) handle->ifmblock = tmp_block;
      }
    }
    handle->blocksifm = handle->desc.C / handle->ifmblock;

    if ( handle->desc.K < tmp_max_k_block ) {
      handle->ofmblock = handle->desc.K;
    } else {
      for ( tmp_block = 1; tmp_block <= tmp_max_k_block; tmp_block *= 2 ) {
        if ( handle->desc.K % tmp_block == 0 ) handle->ofmblock = tmp_block;
      }
    }
    handle->blocksofm = handle->desc.K / handle->ofmblock;

    handle->fwd_ofh_rb = 1;
    handle->fwd_ofw_rb = handle->ofw;
    handle->bwd_ofh_rb = 1;
    handle->bwd_ofw_rb = handle->ofw;
    handle->fm_lp_block = 1;
    handle->use_thread_private_jit = 0;

    /* Adjust blocking factors if custom_2 format is requested */
    if ((handle->buffer_format == LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM) && (handle->custom_format_type == LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_2)) {
      if (handle->datatype_in == LIBXSMM_DNN_DATATYPE_F32)  {
        /* In this case of custom_2 format, regardless of requested padding, all the pad_in/pad_out parameters should be 0 */
        if ( ((handle->desc.pad_h > 0) && ((handle->desc.pad_h_in != 0) || (handle->desc.pad_h_out != 0))) || ((handle->desc.pad_w > 0) && ((handle->desc.pad_w_in != 0) || (handle->desc.pad_w_out !=0))) ) {
          status = LIBXSMM_DNN_ERR_INVALID_PADDING;
          free(handle);
          handle = 0;
          return status;
        }
        if ( (handle->desc.N % 16 == 0) && (handle->desc.C % 16 == 0) && (handle->desc.K % 16 == 0) ) {
          handle->nbImg = 16;
          handle->ifmblock = 16;
          handle->ofmblock = 16;
          handle->fm_lp_block = 1;
        } else {
          /* Fallback to custom_1 format, when using custom_2 format N should be divisible by 16 */
          handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
        }
      } else {
        /* Fallback to custom_1 format, for now custom_2 format is supported only for float */
        handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
      }
    }

    /* use generic code path */
    handle->use_fwd_generic = 1;
    handle->use_bwd_generic = 1;
    handle->use_upd_generic = 1;

    handle->code_fwd[0].xconv.sconv = 0;
    handle->code_fwd[1].xconv.sconv = 0;
    handle->code_fwd[2].xconv.sconv = 0;
    /* Backward path */
    handle->code_bwd[0].xconv.sconv = 0;
    handle->code_bwd[1].xconv.sconv = 0;
    handle->code_bwd[2].xconv.sconv = 0;
    /* weight update path */
    handle->code_upd[0].xconv.sconv = 0;
    handle->code_upd[1].xconv.sconv = 0;

    /* prepare barrier */
    handle->barrier = libxsmm_barrier_create(handle->desc.threads, 1);

    /* backward transpose filters, as we want to call small GEMMs we need that scratch */
    handle->scratch1 = 0;
    handle->scratch1_size = handle->blocksifm * handle->ifmblock * handle->blocksofm * handle->ofmblock
      * handle->desc.R * handle->desc.S * libxsmm_dnn_typesize(handle->datatype_in);
    if (handle->fm_lp_block > 1) {
      /* If low precision, we need extra buffer to store intermediate weight tensor */
      handle->scratch1_size *= 2;
    }

    handle->scratch3 = 0;
    handle->scratch3_size = 0;
    handle->scratch4 = 0;
    handle->scratch4_size = 0;
  }

  if (handle->use_fwd_generic != 0 || handle->use_bwd_generic != 0 || handle->use_upd_generic != 0) {
    const int padded_h = handle->desc.H + (2 * handle->desc.pad_h);
    const int padded_w = handle->desc.W + (2 * handle->desc.pad_w);
    const size_t size7 = padded_h * padded_w * handle->ifmblock * libxsmm_dnn_typesize(handle->datatype_in);
    handle->scratch7_size = LIBXSMM_UP2(size7, LIBXSMM_CACHELINE) * handle->desc.threads;
    handle->scratch7 = 0;
  }
  else {
    handle->scratch7_size = 0;
    handle->scratch7 = 0;
  }
  if (handle->use_upd_generic != 0) {
    const size_t output_typesize = libxsmm_dnn_typesize(handle->datatype_out);
    /* FIXME: currently filter data-type is always smaller/equal output type */
    const size_t filter_typesize = output_typesize;
    const size_t size8 = handle->ofhp * handle->ofwp * handle->ofmblock * output_typesize;
    const size_t size9 = handle->desc.R * handle->desc.S * handle->ifmblock * handle->ofmblock * filter_typesize;
    handle->scratch8_size = LIBXSMM_UP2(size8, LIBXSMM_CACHELINE) * handle->desc.threads;
    handle->scratch8 = 0;
    handle->scratch9_size = LIBXSMM_UP2(size9, LIBXSMM_CACHELINE) * handle->desc.threads;
    handle->scratch9 = 0;
  }
  else {
    handle->scratch8_size = 0;
    handle->scratch8 = 0;
    handle->scratch9_size = 0;
    handle->scratch9 = 0;
  }

  return status;
}


/* This function finds the prime factors of a number */
LIBXSMM_API_INLINE void internal_dnn_handle_factors(
    unsigned int num,
    unsigned int num_factors[] )
{
  unsigned int primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
  int i;
  unsigned int total_primes = 10;
  unsigned int index = 0;

  for ( i = total_primes-1; i >= 0; i-- ) {
    while((num % primes[i]) == 0) {
      num_factors[index] = primes[i];
      index++;
      num = num/primes[i];
    }
  }
}


/**
 * This function finds the unroll factor for (itiles*jtiles*bimg)
 * such that ur <= max_acc
 * The following loop may not give an optimal solution (knapsack problem)
 * Eg, 12 = 3*2*2, MAX_ACC = 4, this algorithm: 3, best: 2*2
 */
LIBXSMM_API_INLINE void internal_dnn_handle_factors_all(
    unsigned int  product,
    unsigned int* ur,
    unsigned int  max_acc)
{
  unsigned int i;
  unsigned int fact[10];

  for ( i = 0; i < 10; i++ ) {
    fact[i] = 1;
  }
  internal_dnn_handle_factors(product, fact);

  *ur = 1;
  for ( i = 0; fact[i] != 1; i++ ) {
    if ( (fact[i] * (*ur)) <= max_acc ) {
      *ur = (*ur)*fact[i];
    }
  }
}


LIBXSMM_API_INTERN libxsmm_dnn_err_t libxsmm_dnn_internal_create_conv_handle_winograd_check( libxsmm_dnn_layer* handle ) {
  /* flag to test if we found an architecture which is supported */
  int noarch = 1;
  libxsmm_dnn_err_t status = LIBXSMM_DNN_SUCCESS;
  const char *const env = getenv("LIBXSMM_DNN_INTERNAL_FORMAT");
  int internal_format_type;
  if ( 0 == env || 0 == *env) {
    /* Default internal format type */
    handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
  } else {
    internal_format_type = atoi(env);
    if (internal_format_type == 1) {
      handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_1;
    } else if ( internal_format_type == 2) {
      handle->custom_format_type = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM_2;
    } else {
      status = LIBXSMM_DNN_ERR_INVALID_FORMAT_GENERAL;
      free(handle);
      handle = 0;
      return status;
    }
  }

  /* now architecture specific */
  if ((libxsmm_target_archid == LIBXSMM_X86_AVX512_MIC  ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_CORE ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM  ||
        libxsmm_target_archid == LIBXSMM_X86_AVX512_ICL    ) &&
      (handle->datatype_in == LIBXSMM_DNN_DATATYPE_F32) &&
      (handle->datatype_out == LIBXSMM_DNN_DATATYPE_F32) &&
      (0 == (handle->desc.C % 16) && 0 == (handle->desc.K % 16)) &&
      (3 == handle->desc.R && 3 == handle->desc.S) &&
      (1 == handle->desc.u && 1 == handle->desc.v))
  {
    noarch = 0;
    /* calculate blockings */
    handle->ifmblock = 16;
    handle->ofmblock = 16;
    handle->blocksifm = handle->desc.C / 16;
    handle->blocksofm = handle->desc.K / 16;
    handle->fm_lp_block = 1;

  } else {
    status = LIBXSMM_DNN_WARN_FALLBACK;
  }

  if (noarch == 0) {
    libxsmm_convolution_winograd_descriptor wino_desc_fp;
    libxsmm_convolution_winograd_descriptor wino_desc_bp;
    libxsmm_convolution_winograd_descriptor wino_desc_wu;
    const int alpha = 6;
    const int tileSize = alpha - 2;
    int allowed_unroll = 0;
    int max_acc = 0;
    int temp_ur;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
    int flagBenchmark = 0;
#endif
    /* Forward path */
    { wino_desc_fp.alpha = alpha;
      wino_desc_fp.jtiles = (handle->ofh + tileSize - 1) / tileSize;
      wino_desc_fp.itiles = (handle->ofw + tileSize - 1) / tileSize;

      /* LUT for DeepBench */
      if ((240 == handle->ofw) && (24 == handle->ofh) && (16 == handle->desc.N) && (16 == handle->desc.C) && (32 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((120 == handle->ofw) && (12 == handle->ofh) && (16 == handle->desc.N) && (32 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((60 == handle->ofw) && (6 == handle->ofh) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((54 == handle->ofw) && (54 == handle->ofh) && (8 == handle->desc.N) && (64 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((27 == handle->ofw) && (27 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 8;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 8;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->ofw) && (112 == handle->ofh) && (8 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 2;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 8;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 8;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->ofw) && (112 == handle->ofh) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (16 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (16 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 2;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 4;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 16;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for AlexNet */
      else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (384 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for GoogLenetV1 */
      else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (208 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (112 == handle->desc.C) && (224 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (144 == handle->desc.C) && (288 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for Overfeat */
      else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 4) ? 12 :
          (0 == wino_desc_fp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 4) ? 12 :
          (0 == wino_desc_fp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (1024 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 4) ? 12 :
          (0 == wino_desc_fp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for VGGA */
      else if ((112 == handle->ofw) && (112 == handle->ofh) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = 1;
        wino_desc_fp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = (0 == wino_desc_fp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_fp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_fp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* General scenario */
      else {
        if ((handle->desc.N % 4) == 0) {
          wino_desc_fp.bimg = 4;
        } else if ((handle->desc.N % 2) == 0) {
          wino_desc_fp.bimg = 2;
        } else {
          wino_desc_fp.bimg = 1;
        }
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM) {
          max_acc = 24;
        } else {
          max_acc = 26;
        }
        internal_dnn_handle_factors_all( wino_desc_fp.itiles*wino_desc_fp.jtiles*wino_desc_fp.bimg, &(wino_desc_fp.ur), max_acc );
        /* ur should be at least 14 to hide qfma latency */
        temp_ur = LIBXSMM_MIN(LIBXSMM_MAX(wino_desc_fp.ur, 14), wino_desc_fp.itiles*wino_desc_fp.jtiles*wino_desc_fp.bimg);
        if (0 == wino_desc_fp.itiles*wino_desc_fp.jtiles*wino_desc_fp.bimg % temp_ur) {
          wino_desc_fp.ur = temp_ur;
        }
      }

      /* The following condition checks whether we have encountered an input which is listed in our benchmark LUT */
#if defined(LIBXSMM_DNN_HANDLE_DEBUG) && 0
      if (flagBenchmark) printf("In benchmark\n");
#elif defined(LIBXSMM_DNN_HANDLE_DEBUG)
      LIBXSMM_UNUSED(flagBenchmark);
#endif
      /* ur_ifm = blocksifm so that we don't need to zero-initialize M and use streaming store */
      wino_desc_fp.ur_ifm = handle->blocksifm;
      wino_desc_fp.blocks_ifm = handle->blocksifm;

      handle->cwino_fwd = wino_desc_fp;

      /* TODO check JIT errors */
      if (libxsmm_target_archid == LIBXSMM_X86_AVX512_MIC  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_CORE ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_ICL    )
      {
        wino_desc_fp.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_NONE;
        handle->code_fwd[0].pmm = libxsmm_create_xconv_wino_forward(&wino_desc_fp);
        wino_desc_fp.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L1;
        handle->code_fwd[1].pmm = libxsmm_create_xconv_wino_forward(&wino_desc_fp);
        wino_desc_fp.prefetch = (libxsmm_convolution_prefetch_type)(LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L1 | LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L2);
        handle->code_fwd[2].pmm = libxsmm_create_xconv_wino_forward(&wino_desc_fp);
      } else {
        assert(0/*should not happen*/);
      }
    }
    /* Backward path */
    { wino_desc_bp.alpha = alpha;
      wino_desc_bp.jtiles = (handle->desc.H + tileSize - 1) / tileSize;
      wino_desc_bp.itiles = (handle->desc.W + tileSize - 1) / tileSize;

      /* LUT for DeepBench */
      if ((240 == handle->desc.W) && (24 == handle->desc.H) && (16 == handle->desc.N) && (16 == handle->desc.C) && (32 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((120 == handle->desc.W) && (12 == handle->desc.H) && (16 == handle->desc.N) && (32 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((60 == handle->desc.W) && (6 == handle->desc.H) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 6;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((54 == handle->desc.W) && (54 == handle->desc.H) && (8 == handle->desc.N) && (64 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((27 == handle->desc.W) && (27 == handle->desc.H) && (8 == handle->desc.N) && (128 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 8;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->desc.W) && (7 == handle->desc.H) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 8;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->desc.W) && (112 == handle->desc.H) && (8 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->desc.W) && (56 == handle->desc.H) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 2;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 8;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->desc.W) && (7 == handle->desc.H) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 8;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->desc.W) && (112 == handle->desc.H) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->desc.W) && (56 == handle->desc.H) && (16 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (16 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 2;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 4;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->desc.W) && (7 == handle->desc.H) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 16;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for AlexNet */
      else if ((13 == handle->desc.W) && (13 == handle->desc.H) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->desc.W) && (13 == handle->desc.H) && (64 <= handle->desc.N) && (384 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->desc.W) && (13 == handle->desc.H) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for GoogLenetV1 */
      else if ((56 == handle->desc.W) && (56 == handle->desc.H) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (208 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (112 == handle->desc.C) && (224 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (144 == handle->desc.C) && (288 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 16;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->desc.W) && (7 == handle->desc.H) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->desc.W) && (7 == handle->desc.H) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for Overfeat */
      else if ((12 == handle->desc.W) && (12 == handle->desc.H) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 4) ? 12 :
          (0 == wino_desc_bp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->desc.W) && (12 == handle->desc.H) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 4) ? 12 :
          (0 == wino_desc_bp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->desc.W) && (12 == handle->desc.H) && (64 <= handle->desc.N) && (1024 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 4) ? 12 :
          (0 == wino_desc_bp.bimg % 2) ? 6 : 3;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for VGGA */
      else if ((112 == handle->desc.W) && (112 == handle->desc.H) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->desc.W) && (56 == handle->desc.H) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->desc.W) && (56 == handle->desc.H) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 14;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->desc.W) && (28 == handle->desc.H) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = (0 == wino_desc_bp.bimg % 2) ? 14 : 7;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->desc.W) && (14 == handle->desc.H) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_bp.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_bp.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* General scenario */
      else {
        wino_desc_bp.bimg = wino_desc_fp.bimg;
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM) {
          max_acc = 24;
        } else {
          max_acc = 26;
        }
        internal_dnn_handle_factors_all( wino_desc_bp.itiles*wino_desc_bp.jtiles*wino_desc_bp.bimg, &(wino_desc_bp.ur), max_acc );
        temp_ur = LIBXSMM_MIN(LIBXSMM_MAX(wino_desc_bp.ur, 14), wino_desc_bp.itiles*wino_desc_bp.jtiles*wino_desc_bp.bimg);
        if (0 == wino_desc_bp.itiles*wino_desc_bp.jtiles*wino_desc_bp.bimg % temp_ur) {
          wino_desc_bp.ur = temp_ur;
        }
      }

      wino_desc_bp.ur_ifm = handle->blocksofm;
      wino_desc_bp.blocks_ifm = handle->blocksofm;

      handle->cwino_bwd = wino_desc_bp;

      /* TODO check JIT errors */
      if (libxsmm_target_archid == LIBXSMM_X86_AVX512_MIC  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_CORE ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_ICL    )
      {
        wino_desc_bp.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_NONE;
        handle->code_bwd[0].pmm = libxsmm_create_xconv_wino_backward(&wino_desc_bp);
        wino_desc_bp.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L1;
        handle->code_bwd[1].pmm = libxsmm_create_xconv_wino_backward(&wino_desc_bp);
        wino_desc_bp.prefetch = (libxsmm_convolution_prefetch_type)(LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L1 | LIBXSMM_CONVOLUTION_PREFETCH_INPUT_L2);
        handle->code_bwd[2].pmm = libxsmm_create_xconv_wino_backward(&wino_desc_bp);
      } else {
        assert(0/*should not happen*/);
      }
    } /* End of backward */
    /* Weight update path */
    { wino_desc_wu.alpha = alpha;
      wino_desc_wu.jtiles = wino_desc_fp.jtiles;
      wino_desc_wu.itiles = wino_desc_fp.itiles;

      /* LUT for DeepBench */
      if ((240 == handle->ofw) && (24 == handle->ofh) && (16 == handle->desc.N) && (16 == handle->desc.C) && (32 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((120 == handle->ofw) && (12 == handle->ofh) && (16 == handle->desc.N) && (32 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((60 == handle->ofw) && (6 == handle->ofh) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((54 == handle->ofw) && (54 == handle->ofh) && (8 == handle->desc.N) && (64 == handle->desc.C) && (64 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM) {
          wino_desc_wu.ur = 1;
        } else {
          wino_desc_wu.ur = 2;
        }
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((27 == handle->ofw) && (27 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1; /*8;*/
        wino_desc_wu.ur = 1; /*2;*/
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 8;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 8;
        wino_desc_wu.ur = 4;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->ofw) && (112 == handle->ofh) && (8 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (8 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1; /*2;*/
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM) {
          wino_desc_wu.ur = 1;
        } else {
          wino_desc_wu.ur = 2;
        }
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (8 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 2; /*4;*/
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 8;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (8 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 8;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((112 == handle->ofw) && (112 == handle->ofh) && (16 == handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (16 == handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM) {
          wino_desc_wu.ur = 1;
        } else {
          wino_desc_wu.ur = 2;
        }
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (16 == handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 2; /*16;*/
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 4; /*16;*/
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (16 == handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 16;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for AlexNet */
      else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (384 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((13 == handle->ofw) && (13 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for GoogLenetV1 */
      else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (192 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (96 == handle->desc.C) && (208 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (112 == handle->desc.C) && (224 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (144 == handle->desc.C) && (288 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (64 <= handle->desc.N) && (160 == handle->desc.C) && (320 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 32) ? 32 :
          (0 == handle->desc.N % 16) ? 16 :
          (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((7 == handle->ofw) && (7 == handle->ofh) && (64 <= handle->desc.N) && (192 == handle->desc.C) && (384 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 32) ? 32 :
          (0 == handle->desc.N % 16) ? 16 :
          (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for Overfeat */
      else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 32) ? 32 :
          (0 == handle->desc.N % 16) ? 16 :
          (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 32) ? 32 :
          (0 == handle->desc.N % 16) ? 16 :
          (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((12 == handle->ofw) && (12 == handle->ofh) && (64 <= handle->desc.N) && (1024 == handle->desc.C) && (1024 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 32) ? 32 :
          (0 == handle->desc.N % 16) ? 16 :
          (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* LUT for VGGA */
      else if ((112 == handle->ofw) && (112 == handle->ofh) && (64 <= handle->desc.N) && (64 == handle->desc.C) && (128 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (128 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((56 == handle->ofw) && (56 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (256 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (256 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((28 == handle->ofw) && (28 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = (0 == wino_desc_wu.bimg % 2) ? 2 : 1;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      } else if ((14 == handle->ofw) && (14 == handle->ofh) && (64 <= handle->desc.N) && (512 == handle->desc.C) && (512 == handle->desc.K) && (6 == alpha)) {
        wino_desc_wu.bimg = (0 == handle->desc.N % 8) ? 8 :
          (0 == handle->desc.N % 4) ? 4 :
          (0 == handle->desc.N % 2) ? 2 : 1;
        wino_desc_wu.ur = 2;
#if defined(LIBXSMM_DNN_HANDLE_DEBUG)
        flagBenchmark = 1;
#endif
      }

      /* General scenario */
      else {
        if ((handle->desc.N % 4) == 0) {
          wino_desc_wu.bimg = 4;
        } else if ((handle->desc.N % 2) == 0) {
          wino_desc_wu.bimg = 2;
        } else {
          wino_desc_wu.bimg = 1;
        }
        allowed_unroll = 512 / (wino_desc_wu.bimg*wino_desc_wu.itiles*wino_desc_wu.jtiles);
        allowed_unroll = (allowed_unroll > 26) ? 26 : allowed_unroll;
        if (libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM && (wino_desc_wu.itiles*wino_desc_wu.jtiles*wino_desc_wu.bimg % 4) == 0) {
          internal_dnn_handle_factors_all( wino_desc_wu.itiles*wino_desc_wu.jtiles*wino_desc_wu.bimg/4, &(wino_desc_wu.ur), allowed_unroll );
        } else {
          internal_dnn_handle_factors_all( wino_desc_wu.itiles*wino_desc_wu.jtiles*wino_desc_wu.bimg,   &(wino_desc_wu.ur), allowed_unroll );
        }
      }

      wino_desc_wu.ur_ifm = 1;

      handle->cwino_upd = wino_desc_wu;
      /* TODO check JIT errors */
      if (libxsmm_target_archid == LIBXSMM_X86_AVX512_MIC  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_CORE ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_KNM  ||
          libxsmm_target_archid == LIBXSMM_X86_AVX512_ICL    )
      {
        /* NONE */
        wino_desc_wu.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_NONE;
        handle->code_upd[0].pmm = libxsmm_create_xconv_wino_update_weights(&wino_desc_wu);
        /* ALL */
        wino_desc_wu.prefetch = LIBXSMM_CONVOLUTION_PREFETCH_ALL;
        handle->code_upd[1].pmm = libxsmm_create_xconv_wino_update_weights(&wino_desc_wu);
      } else {
        assert(0/*should not happen*/);
      }
    } /* end of weight-update handle */
    {
      /* Populating scratch registers for U V and M */
      int ijtiles;
      if (wino_desc_bp.itiles * wino_desc_bp.jtiles >= wino_desc_fp.itiles * wino_desc_fp.jtiles) {
        ijtiles = wino_desc_bp.itiles * wino_desc_bp.jtiles;
      } else {
        ijtiles = wino_desc_fp.itiles * wino_desc_fp.jtiles;
      }

      handle->scratch1 = 0;
      handle->scratch1_size = alpha*alpha*handle->desc.C*handle->desc.K*libxsmm_dnn_typesize(handle->datatype_in);
      handle->scratch3 = 0;
      handle->scratch3_size = alpha*alpha*ijtiles*handle->desc.N * handle->desc.C * libxsmm_dnn_typesize(handle->datatype_in);
      handle->scratch4 = 0;
      handle->scratch4_size = alpha*alpha*ijtiles*handle->desc.N * handle->desc.K * libxsmm_dnn_typesize(handle->datatype_out);
      handle->scratch6 = 0;
      handle->scratch6_size = 0;
      handle->scratchIw = 0;
      handle->scratchIw_size = ijtiles*alpha*alpha*16*libxsmm_dnn_typesize(handle->datatype_in)*handle->desc.threads;
      handle->scratchOw = 0;
      handle->scratchOw_size = ijtiles*alpha*alpha*16*libxsmm_dnn_typesize(handle->datatype_out)*handle->desc.threads;
      handle->scratchVk = 0;
      handle->scratchVk_size = handle->scratch3_size;
      handle->barrier = libxsmm_barrier_create(handle->desc.threads, 1);
    }
  } else {
    handle->code_fwd[0].xconv.sconv = 0;
    handle->code_fwd[1].xconv.sconv = 0;
    handle->code_fwd[2].xconv.sconv = 0;
    /* Backward path */
    handle->code_bwd[0].xconv.sconv = 0;
    handle->code_bwd[1].xconv.sconv = 0;
    handle->code_bwd[2].xconv.sconv = 0;
    /* weight update path */
    handle->code_upd[0].xconv.sconv = 0;
    handle->code_upd[1].xconv.sconv = 0;
    handle->barrier  = 0;
    handle->scratch1 = 0;
    handle->scratch1_size = 0;
    handle->scratch3 = 0;
    handle->scratch3_size = 0;
    handle->scratch4 = 0;
    handle->scratch4_size = 0;
    handle->scratch6 = 0;
    handle->scratch6_size = 0;
  }

  return status;
}

