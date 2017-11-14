/******************************************************************************
 ** Copyright (c) 2016-2017, Intel Corporation                                **
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
/* Evangelos Georganas (Intel Corp.)
 ******************************************************************************/

/* computing first logical thread */
const int ltid = tid-start_thread;

/* FIXME assignemnts here  */
int BLOCKSIFM = handle->blocksifm;
int BLOCKSOFM = handle->blocksofm;
int OFWP = handle->ofwp+handle->output_lp_padding;

/* Auxiliary integer variables   */
int img, ofm1, ifm1, ifm2, num_ofw_strips, num_ofh_strips, oi_, oj_, oi__, oj__,ii_, ij_, kh, kw, ofm1ifm1, ki, kj, imgifm1,ii, ij, i, j, ofm1ifm1img;

/* traspose, copy and reduce work-related variables  */
const int transpose_work = handle->desc.N*BLOCKSIFM;
const int transpose_chunksize = (transpose_work % handle->desc.threads == 0) ? (transpose_work / handle->desc.threads) : (transpose_work / handle->desc.threads) + 1;
const int transpose_thr_begin = (ltid * transpose_chunksize < transpose_work) ? (ltid * transpose_chunksize) : transpose_work;
const int transpose_thr_end = ((ltid + 1) * transpose_chunksize < transpose_work) ? ((ltid + 1) * transpose_chunksize) : transpose_work;

const int reduce_work = BLOCKSOFM*BLOCKSIFM*handle->desc.R*handle->desc.S*handle->ifmblock;
const int reduce_chunksize = (reduce_work % handle->desc.threads == 0) ? (reduce_work / handle->desc.threads) : (reduce_work / handle->desc.threads) + 1;
const int reduce_thr_begin = (ltid * reduce_chunksize < reduce_work) ? (ltid * reduce_chunksize) : reduce_work;
const int reduce_thr_end = ((ltid + 1) * reduce_chunksize < reduce_work) ? ((ltid + 1) * reduce_chunksize) : reduce_work;
const int copywork = handle->desc.N*BLOCKSIFM;
const int copychunksize = (copywork % handle->desc.threads == 0) ? (copywork / handle->desc.threads) : (copywork / handle->desc.threads) + 1;
const int copy_thr_begin = (ltid * copychunksize < copywork) ? (ltid * copychunksize) : copywork;
const int copy_thr_end = ((ltid + 1) * copychunksize < copywork) ? ((ltid + 1) * copychunksize) : copywork;


const int work = BLOCKSIFM*BLOCKSOFM;
/* compute chunck size */
const int chunksize = (work % handle->desc.threads == 0) ? (work / handle->desc.threads) : (work / handle->desc.threads) + 1;
/* compute thr_begin and thr_end */
const int thr_begin = (ltid * chunksize < work) ? (ltid * chunksize) : work;
const int thr_end = ((ltid + 1) * chunksize < work) ? ((ltid + 1) * chunksize) : work;

/* Pointer related variables for output and weight */
element_output_type *out = ((element_output_type*)handle->grad_output->data) + (handle->desc.pad_h_out * handle->ofwp + handle->desc.pad_w_out) * handle->ofmblock * handle->fm_lp_block;
LIBXSMM_VLA_DECL(6, element_output_type, tr_output,  (element_output_type*)handle->scratch6 , BLOCKSOFM, handle->ofhp, OFWP/2, handle->ofmblock, 2);
LIBXSMM_VLA_DECL(6, element_output_type, output, out, handle->blocksofm_lp, handle->ofhp, handle->ofwp, handle->ofmblock, handle->fm_lp_block);
LIBXSMM_VLA_DECL(6, element_filter_type, weight, (element_filter_type*)handle->grad_filter->data, BLOCKSIFM, handle->desc.R, handle->desc.S, handle->ifmblock, handle->ofmblock);
element_filter_type* remote_weight_ptr = 0;
element_filter_type* weight_ptr = (element_filter_type*)handle->grad_filter->data;
element_filter_type* per_thread_weight_ptr = ((element_filter_type*)handle->scratch4) + (handle->weight_copies*BLOCKSOFM*BLOCKSIFM*handle->desc.R*handle->desc.S*handle->ifmblock*handle->ofmblock);
LIBXSMM_VLA_DECL(6, element_filter_type, per_thread_weight, per_thread_weight_ptr, BLOCKSIFM, handle->desc.R, handle->desc.S, handle->ifmblock, handle->ofmblock);
/* Declare both variables for weights (private and global)  */
LIBXSMM_VLA_DECL(6, element_filter_type, opt_weight_ptr_per_thread, per_thread_weight, BLOCKSIFM, handle->desc.R, handle->desc.S, handle->ifmblock, handle->ofmblock);
LIBXSMM_VLA_DECL(6, element_filter_type, opt_weight_ptr, weight, BLOCKSIFM, handle->desc.R, handle->desc.S, handle->ifmblock, handle->ofmblock);
/* Pointer related variables for input */

/*LIBXSMM_VLA_DECL(2, element_filter_type, per_thread_weight, per_thread_weight_ptr, handle->ofmblock);*/
element_filter_type* reduction_weight_ptr = ((element_filter_type*)handle->scratch4) + (handle->weight_copies * BLOCKSOFM * BLOCKSIFM * handle->desc.R*handle->desc.S*handle->ifmblock*handle->ofmblock);
LIBXSMM_VLA_DECL(3, element_filter_type, reduction_weight, reduction_weight_ptr, handle->weight_copies, handle->ofmblock);

element_input_type (* LIBXSMM_RESTRICT input_ptr);
element_input_type (* LIBXSMM_RESTRICT copy_ptr);
element_input_type *prefetch_ptr;
int padded_h = (handle->padding_flag == 1) ? handle->ifhp + 2 * handle->desc.pad_h : handle->ifhp;
int padded_w = (handle->padding_flag == 1) ? handle->ifwp + 2 * handle->desc.pad_w : handle->ifwp;
int ifwp_extended = padded_w + handle->qfma_input_pad;
int dst_ifhp, k;
if (handle->resize_input == 1) {
  ifwp_extended = handle->ifwp_resized + handle->qfma_input_pad;
  dst_ifhp = handle->ifhp_resized;
} else {
  dst_ifhp = handle->ifhp;
}

LIBXSMM_VLA_DECL(6, element_input_type, input_nopad, (element_input_type*)handle->reg_input->data, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
LIBXSMM_VLA_DECL(5, element_input_type, tr_input_padded, (element_input_type*)handle->scratch5, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended);
LIBXSMM_VLA_DECL(5, element_input_type, input_padded, (element_input_type*)handle->scratch5, BLOCKSIFM, padded_h, padded_w, handle->ifmblock);
LIBXSMM_VLA_DECL(5, element_input_type, tr_input_nopad, (element_input_type*)handle->scratch3, BLOCKSIFM, dst_ifhp, handle->ifmblock, ifwp_extended);

/* Stream related variables  */
int *stream = handle->compute_upd_indices_ptrs[ltid];
int instr, offset_i, offset_o, offset_w, pi, po, pw, pc;

/* Base pointers  */
element_input_type *input_base;
element_filter_type *weight_base;
element_output_type *output_base;
element_input_type *input_zero;

/* Kernel related variables  */
libxsmm_xmatcopyfunction jitted_matcopy = handle->matcopy_upd[0].xmatcopy;
libxsmm_xmatcopyfunction jitted_matzero = handle->matcopy_upd[1].xmatcopy;
libxsmm_convfunction kernel = (handle->trans_ofw_ifm == 0 ) ? (libxsmm_convfunction)handle->code_upd[1].xconv.sconv : (libxsmm_convfunction)handle->code_upd[4].xconv.sconv;

transposer tp_func;
tp_func = get_transposer(handle->ifmblock, handle->ifwp, ifwp_extended, handle->ifmblock);

/* lazy barrier init */
libxsmm_barrier_init(handle->barrier, ltid);

#if 0
if (handle->padding_flag == 1) {
  input_zero = &LIBXSMM_VLA_ACCESS(5, tr_input_padded, ltid, 0, 0, 0, 0, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended);
  memset( input_zero, 0, BLOCKSIFM * padded_h * ifwp_extended * handle->ifmblock * sizeof(element_input_type) );
  for (imgifm1 = transpose_thr_begin; imgifm1 < transpose_thr_end; ++imgifm1) {
    img = imgifm1/BLOCKSIFM;
    ifm1 = imgifm1%BLOCKSIFM;
    for (ij=0; ij < handle->ifhp; ++ij) {
      float *dst = &(LIBXSMM_VLA_ACCESS(5, tr_input_padded, img, ifm1, ij + handle->desc.pad_h, 0, 0 + handle->desc.pad_w, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended));
      float *src = &(LIBXSMM_VLA_ACCESS(5, input_nopad, img, ifm1, ij, 0, 0, BLOCKSIFM, handle->ifhp, handle->ifwp, handle->ifmblock));
      tp_func(handle->ifmblock, handle->ifwp, dst, ifwp_extended, src, handle->ifmblock);
    }
  }
} else {
  if (handle->resize_input == 0) {
    for (imgifm1 = transpose_thr_begin; imgifm1 < transpose_thr_end; ++imgifm1) {
      img = imgifm1/BLOCKSIFM;
      ifm1 = imgifm1%BLOCKSIFM;
      for (ij=0; ij < handle->ifhp; ++ij) {
        float *dst = &(LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, ifm1, ij, 0, 0, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended));
        float *src = &(LIBXSMM_VLA_ACCESS(5, input_nopad, img, ifm1, ij, 0, 0, BLOCKSIFM, handle->ifhp, handle->ifwp, handle->ifmblock));
        tp_func(handle->ifmblock, handle->ifwp, dst, ifwp_extended, src, handle->ifmblock);
      }
    }
  } else {
    int dst_i, dst_j, src_i, src_j, fm;
    for (imgifm1 = transpose_thr_begin; imgifm1 < transpose_thr_end; ++imgifm1) {
      img = imgifm1/BLOCKSIFM;
      ifm1 = imgifm1%BLOCKSIFM;
      for (dst_j=0; dst_j < handle->ifhp_resized; dst_j++) {
        src_j = dst_j * handle->desc.v;
        for (dst_i=0; dst_i < handle->ifwp_resized; dst_i++) {
          src_i = dst_i * handle->desc.u;
          for (fm = 0; fm < handle->ifmblock; fm++){
            LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, ifm1, dst_j, fm, dst_i, BLOCKSIFM, handle->ifhp_resized, handle->ifmblock, ifwp_extended) =
              LIBXSMM_VLA_ACCESS(5, input_nopad, img, ifm1, src_j, src_i, fm, BLOCKSIFM, handle->ifhp, handle->ifwp, handle->ifmblock);
          }
        }
      }
    }
  }
}
#endif

/* Initialize base pointers */
if (handle->padding_flag == 1) {
  input_base = &LIBXSMM_VLA_ACCESS(5, tr_input_padded, 0, 0, 0, 0, 0, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended);
  input_zero = &LIBXSMM_VLA_ACCESS(5, tr_input_padded, ltid, 0, 0, 0, 0, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended);
  memset( input_zero, 0, BLOCKSIFM * padded_h * ifwp_extended * handle->ifmblock * sizeof(element_input_type) );
} else {
  input_base = &LIBXSMM_VLA_ACCESS(5, tr_input_nopad, 0, 0, 0, 0, 0, BLOCKSIFM, dst_ifhp, handle->ifmblock, ifwp_extended);
}


{
  int img = ltid, ifm1, ij, ifm2, ii;
  int ofm1, ofm2, k, lp;
  int FM;
  int W;

  if (handle->padding_flag == 1) {
    for (ifm1 = 0; ifm1 < handle->blocksifm_lp; ++ifm1) {
      for (ij = 0; ij < handle->ifhp; ++ij) {
        for (ii = 0; ii < handle->ifwp; ++ii) {
          for (ifm2 = 0; ifm2 < handle->ifmblock; ++ifm2) {
            for (lp = 0; lp < handle->fm_lp_block; ++lp) {
              FM = ifm1 * handle->ifmblock * handle->fm_lp_block + ifm2 * handle->fm_lp_block + lp;
              LIBXSMM_VLA_ACCESS(5, tr_input_padded, img, FM/handle->ifmblock, ij+handle->desc.pad_h, FM%handle->ifmblock, ii+handle->desc.pad_w, BLOCKSIFM, padded_h, handle->ifmblock, ifwp_extended) =
                LIBXSMM_VLA_ACCESS(6, input_nopad, img, ifm1, ij, ii, ifm2, lp, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
            }
          }
        }
      }
    }  
  } else {
    if (handle->resize_input == 0) {
#if 0
      for (ifm1 = 0; ifm1 < handle->blocksifm_lp; ++ifm1) {
        for (ij = 0; ij < handle->ifhp; ++ij) {
          for (ii = 0; ii < handle->ifwp; ++ii) {
            for (ifm2 = 0; ifm2 < handle->ifmblock; ++ifm2) {
              for (lp = 0; lp < handle->fm_lp_block; ++lp) {
                FM = ifm1 * handle->ifmblock * handle->fm_lp_block + ifm2 * handle->fm_lp_block + lp;
                LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, ij, FM%handle->ifmblock, ii, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended) =
                  LIBXSMM_VLA_ACCESS(6, input_nopad, img, ifm1, ij, ii, ifm2, lp, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
              }
            }
          }
        }
      }
#endif
      int w_chunks = handle->ifwp/16;
      int w_remainder = handle->ifwp%16;
      element_input_type gather_buffer[32];
      element_input_type compressed_gather_buffer[32];
      int w_i, w;
      int c_i;
      element_input_type *base_addr;
      const __m512i vgindex = _mm512_set_epi32(960,896,832,768,704,640,576,512,448,384,320,256,192,128,64,0);
      const __mmask16 gmask = ((uint32_t)1 << w_remainder)-1;

      int mask_remainder = (w_remainder+1)/2;
      unsigned int mask[8];
      for (c_i=0; c_i<mask_remainder; c_i++) {
        mask[c_i] = (1<<31);
      }
      for (c_i=mask_remainder; c_i<8; c_i++) {
        mask[c_i] = 0;
      }
      __m256i mask_reg = _mm256_loadu_si256((const union __m256i *) mask);

      for (ifm1 = 0; ifm1 < handle->blocksifm_lp; ++ifm1) {
        for (ij = 0; ij < handle->ifhp; ++ij) {

          /* Handle full chunks  */
          for (w=0; w<w_chunks; w++) {
            for (ifm2 = 0; ifm2 < handle->ifmblock; ++ifm2) {
              FM = ifm1 * handle->ifmblock * handle->fm_lp_block + ifm2 * handle->fm_lp_block;
              base_addr = &LIBXSMM_VLA_ACCESS(6, input_nopad, img, ifm1, ij, w*16, ifm2, 0, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
              __m512i gather_reg = _mm512_i32gather_epi32(vgindex, base_addr, 1);
              _mm512_store_epi32(gather_buffer, gather_reg);
              /* Emulated compress...  */
#if 1
              int lo_ind = 0;
              int hi_ind = 16;
              for (c_i=0; c_i<32; c_i+=2) {
                compressed_gather_buffer[lo_ind] = gather_buffer[c_i];
                compressed_gather_buffer[hi_ind] = gather_buffer[c_i+1];
                lo_ind++;
                hi_ind++;
              }
#else
              compressed_gather_buffer[0] = gather_buffer[0];
              compressed_gather_buffer[16] = gather_buffer[1];
              compressed_gather_buffer[1] = gather_buffer[2];
              compressed_gather_buffer[17] = gather_buffer[3];
              compressed_gather_buffer[2] = gather_buffer[4];
              compressed_gather_buffer[18] = gather_buffer[5];
              compressed_gather_buffer[3] = gather_buffer[6];
              compressed_gather_buffer[19] = gather_buffer[7];
              compressed_gather_buffer[4] = gather_buffer[8];
              compressed_gather_buffer[20] = gather_buffer[9];
              compressed_gather_buffer[5] = gather_buffer[10];
              compressed_gather_buffer[21] = gather_buffer[11];
              compressed_gather_buffer[6] = gather_buffer[12];
              compressed_gather_buffer[22] = gather_buffer[13];
              compressed_gather_buffer[7] = gather_buffer[14];
              compressed_gather_buffer[23] = gather_buffer[15];
              compressed_gather_buffer[8] = gather_buffer[16];
              compressed_gather_buffer[24] = gather_buffer[17];
              compressed_gather_buffer[9] = gather_buffer[18];
              compressed_gather_buffer[25] = gather_buffer[19];
              compressed_gather_buffer[10] = gather_buffer[20];
              compressed_gather_buffer[26] = gather_buffer[21];
              compressed_gather_buffer[11] = gather_buffer[22];
              compressed_gather_buffer[27] = gather_buffer[23];
              compressed_gather_buffer[12] = gather_buffer[24];
              compressed_gather_buffer[28] = gather_buffer[25];
              compressed_gather_buffer[13] = gather_buffer[26];
              compressed_gather_buffer[29] = gather_buffer[27];
              compressed_gather_buffer[14] = gather_buffer[28];
              compressed_gather_buffer[30] = gather_buffer[29];
              compressed_gather_buffer[15] = gather_buffer[30];
              compressed_gather_buffer[31] = gather_buffer[31];
#endif
              /* Store  */
              _mm256_storeu_si256((union __m256i *) &LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, ij, FM%handle->ifmblock, w*16, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended) , _mm256_loadu_si256((const union __m256i *) &compressed_gather_buffer[0]));
              _mm256_storeu_si256((union __m256i *) &LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, ij, FM%handle->ifmblock+1, w*16, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended) , _mm256_loadu_si256((const union __m256i *) &compressed_gather_buffer[16]));
            }
          }

          /* Handle remainder */
          if ( w_remainder > 0) {
            for (ifm2 = 0; ifm2 < handle->ifmblock; ++ifm2) {
              FM = ifm1 * handle->ifmblock * handle->fm_lp_block + ifm2 * handle->fm_lp_block;
              base_addr = &LIBXSMM_VLA_ACCESS(6, input_nopad, img, ifm1, ij, w_chunks*16, ifm2, 0, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
              __m512i gather_reg = _mm512_mask_i32gather_epi32(gather_reg, gmask, vgindex, base_addr, 1);
              _mm512_store_epi32(gather_buffer, gather_reg);

              /* Emulated compress...  */
#if 1
              int lo_ind = 0;
              int hi_ind = 16;
              for (c_i=0; c_i<32; c_i+=2) {
                compressed_gather_buffer[lo_ind] = gather_buffer[c_i];
                compressed_gather_buffer[hi_ind] = gather_buffer[c_i+1];
                lo_ind++;
                hi_ind++;
              }
#else
              compressed_gather_buffer[0] = gather_buffer[0];
              compressed_gather_buffer[16] = gather_buffer[1];
              compressed_gather_buffer[1] = gather_buffer[2];
              compressed_gather_buffer[17] = gather_buffer[3];
              compressed_gather_buffer[2] = gather_buffer[4];
              compressed_gather_buffer[18] = gather_buffer[5];
              compressed_gather_buffer[3] = gather_buffer[6];
              compressed_gather_buffer[19] = gather_buffer[7];
              compressed_gather_buffer[4] = gather_buffer[8];
              compressed_gather_buffer[20] = gather_buffer[9];
              compressed_gather_buffer[5] = gather_buffer[10];
              compressed_gather_buffer[21] = gather_buffer[11];
              compressed_gather_buffer[6] = gather_buffer[12];
              compressed_gather_buffer[22] = gather_buffer[13];
              compressed_gather_buffer[7] = gather_buffer[14];
              compressed_gather_buffer[23] = gather_buffer[15];
              compressed_gather_buffer[8] = gather_buffer[16];
              compressed_gather_buffer[24] = gather_buffer[17];
              compressed_gather_buffer[9] = gather_buffer[18];
              compressed_gather_buffer[25] = gather_buffer[19];
              compressed_gather_buffer[10] = gather_buffer[20];
              compressed_gather_buffer[26] = gather_buffer[21];
              compressed_gather_buffer[11] = gather_buffer[22];
              compressed_gather_buffer[27] = gather_buffer[23];
              compressed_gather_buffer[12] = gather_buffer[24];
              compressed_gather_buffer[28] = gather_buffer[25];
              compressed_gather_buffer[13] = gather_buffer[26];
              compressed_gather_buffer[29] = gather_buffer[27];
              compressed_gather_buffer[14] = gather_buffer[28];
              compressed_gather_buffer[30] = gather_buffer[29];
              compressed_gather_buffer[15] = gather_buffer[30];
              compressed_gather_buffer[31] = gather_buffer[31];
#endif
              /* Store  */
              _mm256_maskstore_epi32((int*) &LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, ij, FM%handle->ifmblock, w*16, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended), mask_reg, _mm256_loadu_si256((const union __m256i *) &compressed_gather_buffer[0]));
              _mm256_maskstore_epi32((int*) &LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, ij, FM%handle->ifmblock+1, w*16, BLOCKSIFM, handle->ifhp, handle->ifmblock, ifwp_extended), mask_reg, _mm256_loadu_si256((const union __m256i *) &compressed_gather_buffer[16]));  
            }
          }

        }
      }

    } else {
      int dst_i, dst_j, src_i, src_j;
      for (ifm1 = 0; ifm1 < handle->blocksifm_lp; ++ifm1) {
        for (dst_j=0; dst_j < handle->ifhp_resized; dst_j++) {
          src_j = dst_j * handle->desc.v;
          for (dst_i=0; dst_i < handle->ifwp_resized; dst_i++) {
            src_i = dst_i * handle->desc.u;
            for (ifm2 = 0; ifm2 < handle->ifmblock; ++ifm2) {
              for (lp = 0; lp < handle->fm_lp_block; ++lp) {
                FM = ifm1 * handle->ifmblock * handle->fm_lp_block + ifm2 * handle->fm_lp_block + lp;
                LIBXSMM_VLA_ACCESS(5, tr_input_nopad, img, FM/handle->ifmblock, dst_j, FM%handle->ifmblock, dst_i, BLOCKSIFM, handle->ifhp_resized, handle->ifmblock, ifwp_extended) =
                  LIBXSMM_VLA_ACCESS(6, input_nopad, img, ifm1, src_j, src_i, ifm2, lp, handle->blocksifm_lp, handle->ifhp, handle->ifwp, handle->ifmblock, handle->fm_lp_block);
              }
            }
          }
        }
      }
    }
  }

  for (ofm1 = 0; ofm1 < handle->blocksofm_lp; ++ofm1) {
    for (ij = 0; ij < handle->ofhp; ++ij) {
      for (ii = 0; ii < handle->ofwp; ++ii) {
        for (ofm2 = 0; ofm2 < handle->ofmblock; ++ofm2) {
          for (lp = 0; lp < handle->fm_lp_block; ++lp) {
            FM = ofm1 * handle->ofmblock * handle->fm_lp_block + ofm2  * handle->fm_lp_block + lp;
            LIBXSMM_VLA_ACCESS(6,  tr_output, img, FM/handle->ofmblock, ij, ii/2, FM%handle->ofmblock, ii%2, BLOCKSOFM, handle->ofhp, OFWP/2, handle->ofmblock, 2) = 
              LIBXSMM_VLA_ACCESS(6,   output, img, ofm1, ij, ii, ofm2, lp,  handle->blocksofm_lp, handle->ofhp, handle->ofwp, handle->ofmblock, handle->fm_lp_block);
          }
        }
      }
    }
  }

  /* Zero out the "output padding pixel" */
  if (handle->output_lp_padding != 0) {
    ii = handle->ofwp-1;
    for (ofm1 = 0; ofm1 < handle->blocksofm_lp; ++ofm1) {
      for (ij = 0; ij < handle->ofhp; ++ij) {
        for (ofm2 = 0; ofm2 < handle->ofmblock; ++ofm2) {
          for (lp = 0; lp < handle->fm_lp_block; ++lp) {
            FM = ofm1 * handle->ofmblock * handle->fm_lp_block + ofm2  * handle->fm_lp_block + lp;
            LIBXSMM_VLA_ACCESS(6,  tr_output, img, FM/handle->ofmblock, ij, ii/2, FM%handle->ofmblock, 1, BLOCKSOFM, handle->ofhp, OFWP/2, handle->ofmblock, 2) = (element_output_type)0; 
          }
        }
      }
    }
  }
}

libxsmm_barrier_wait(handle->barrier, ltid);

weight_base = &LIBXSMM_VLA_ACCESS(3, reduction_weight, 0, ltid/(handle->desc.threads/handle->weight_copies), 0, handle->weight_copies, handle->ofmblock); 
output_base = &LIBXSMM_VLA_ACCESS(6, tr_output, 0, 0, 0, 0, 0, 0, BLOCKSOFM, handle->ofhp, OFWP/2, handle->ofmblock, 2);

i = 0;
instr = handle->n_entries_upd[ltid];

float scale_factor __attribute__((aligned(64)));
if (handle->use_lp_kernel == 1) {
  scale_factor = 1.0; // (float) pow(2.0, -1.0*(handle->reg_filter->exp + handle->reg_input->exp));
}

float *max_vals __attribute__((aligned(64)));
__m512 max_abs = _mm512_setzero_ps();
if ((handle->fuse_ops & LIBXSMM_DNN_CONV_FUSE_MAX_STATS) > 0) {
  LIBXSMM_VLA_DECL(2, float, maxstats, handle->maxstats_upd->data, 16);
  max_vals = (float*) &LIBXSMM_VLA_ACCESS(2, maxstats, ltid, 0, 16);
}

for (pc = 0; pc < instr; pc++) {
  offset_i = stream[i];
  offset_w = stream[i+1];
  offset_o = stream[i+2];
  pi = stream[i+3];
  pw = stream[i+4];
  po = stream[i+5];
  kernel( input_base + offset_i, weight_base + offset_w, output_base + offset_o, input_base + pi, weight_base + pw, output_base + po, &scale_factor);
  i+=3;
}

libxsmm_barrier_wait(handle->barrier, ltid);

#define __AVX512F__
for ( j = reduce_thr_begin; j < reduce_thr_end; j++ ) {
#ifdef __AVX512F__
  __m512 weight_sum = _mm512_setzero_ps();
  for ( i = 0; i < handle->weight_copies; i++ ) {
    weight_sum = _mm512_add_ps(weight_sum, _mm512_load_ps(&LIBXSMM_VLA_ACCESS(3, reduction_weight, j, i, 0, handle->weight_copies, 16)));
  }
  _mm512_stream_ps(&weight_ptr[j*16], weight_sum);
  max_abs = _mm512_max_ps(max_abs, _mm512_abs_ps(weight_sum));
#else
  element_filter_type weight_sum[16] LIBXSMM_ATTRIBUTE(aligned(64));
  LIBXSMM_PRAGMA_VALIGNED
    LIBXSMM_PRAGMA_SIMD
    for ( k = 0; k < 16; k++ ) {
      weight_sum[k] = (element_filter_type) 0;
    }
  for ( i = 0; i < handle->weight_copies; i++ ) {
    LIBXSMM_PRAGMA_VALIGNED
      LIBXSMM_PRAGMA_SIMD
      for ( k = 0; k < 16; k++ ) {
        weight_sum[k] += LIBXSMM_VLA_ACCESS(3, reduction_weight, j, i, k, handle->weight_copies, 16);
      }
  }
  LIBXSMM_PRAGMA_NONTEMPORAL
    LIBXSMM_PRAGMA_VALIGNED
    LIBXSMM_PRAGMA_SIMD
    for ( k = 0; k < 16; k++ ) {
      weight_ptr[j*16 + k] = weight_sum[k];
    }
#endif
}
#ifdef __AVX512F__
_mm512_store_ps(max_vals, max_abs);
#endif
libxsmm_barrier_wait(handle->barrier, ltid);
#undef __AVX512F__

