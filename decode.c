/* Copyright (c) 2016-2017, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* This file implements the decode side of NvPipe.  It expects a valid h264
 * stream, particularly one generated by 'encode.c'.
 * Most of the involved parts of the logic come from sizing: we have multiple
 * sizes of relevance.  1) The size we expected images to be when creating the
 * decoder; 2) the size the user wanted when creating the decoder; 3) the size
 * of the image coming from the stream, and 4) the size that the user wants
 * /now/.  Because windows might be resized, (1) is not always == (3) and
 * (2) is not always == (4).
 * Worse, we may have a frame or more of latency.  So a resize operation
 * will change (4) in frame N and then (3) in N+x. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cuda_runtime_api.h>
#include <nvcuvid.h>
#include <nvToolsExt.h>
#include "config.nvp.h"
#include "debug.h"
#include "internal-api.h"
#include "nvpipe.h"
#include "yuv.h"

/* NvDec can actually do 8kx8k for HEVC, but this library does not yet
 * support that codec anyway. */
const size_t MAX_WIDTH = 4096;
const size_t MAX_HEIGHT = 4096;

DECLARE_CHANNEL(dec);

struct nvp_decoder {
	nvp_impl_t impl;
	bool initialized;
	CUvideodecoder decoder;
	CUvideoparser parser;
	cudaEvent_t ready;
	/** Source data may be on the device or the host.  However, cuvid only
	 * accepts host data.  If data come in on the device, we'll use this
	 * as a staging buffer for cuvid's input. */
	void* hbuf;
	size_t hbuf_sz;
	/** Most of the the logic in this file is related to sizing.  There are
	 * multiple sizes: 1) The size we expected images to be when creating the
	 * decoder; 2) the size the user wanted when creating the decoder; 3) the size
	 * of the image coming from the stream, and 4) the size that the user wants
	 * /now/.  (1) is not always (3) and (2) is not always (4): for one, windows
	 * can be resized, and the encode side "sees" that sooner than we do, but
	 * also h264 only works in 16x16 blocks, so it's plausible that the stream's
	 * dimensions may not match the output dimensions for the entire session. */
	struct {
		size_t wi; /**< what input/source dims Decoder was created with (1) */
		size_t hi;
		size_t wdst; /**< what *target* dims Decoder was created with (2) */
		size_t hdst;
		size_t wsrc; /**< "source" width/height: what DecodePicture says. (3) */
		size_t hsrc;
		/* 4 is not stored here: it will be the argument to _decode. */
	} d; /**< for "dims" */
	CUdeviceptr rgb; /**< temporary buffer to hold converted data. */
	bool empty;
	nv_fut_t* reorg; /**< reorganizes data from nv12 to RGB form. */
	/** NvCodec has an internal queue of finished frames, and fires off callbacks
	 * we give it when a frame is added to that queue.  We use 'idx' to
	 * communicate (essentially) which NvCodec-internal-buffer ID has just
	 * finished, set in our callback and read in our main code. */
	unsigned idx;
};

static int dec_sequence(void* cdc, CUVIDEOFORMAT* fmt);
static int dec_ode(void* cdc, CUVIDPICPARAMS* pic);

/** Initialize or reinitialize the decoder.
 * @param iwidth the input width of images
 * @param iheight the input height of images
 * @param dstwidth user width; width of image the user requested
 * @param dstheight user height; height of image the user requested */
static bool
dec_initialize(struct nvp_decoder* nvp, size_t iwidth, size_t iheight,
               size_t dstwidth, size_t dstheight) {
	assert(iwidth > 0 && iheight > 0);
	assert(dstwidth > 0 && dstheight > 0);
	assert(nvp->decoder == NULL);
	CUVIDDECODECREATEINFO crt = {0};
	crt.CodecType = cudaVideoCodec_H264;
	crt.ulWidth = iwidth;
	crt.ulHeight = iheight;
	nvp->d.wi = iwidth;
	nvp->d.hi = iheight;
	crt.ulNumDecodeSurfaces = 2;
	crt.ChromaFormat = cudaVideoChromaFormat_420;
	crt.OutputFormat = cudaVideoSurfaceFormat_NV12;
	crt.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;
	crt.ulTargetWidth = dstwidth;
	crt.ulTargetHeight = dstheight;
	crt.display_area.left = crt.display_area.top = 0;
	crt.display_area.right = iwidth;
	crt.display_area.bottom = iheight;
	crt.ulNumOutputSurfaces = 1;
	crt.ulCreationFlags = cudaVideoCreate_PreferCUVID;
	crt.vidLock = NULL;

	if(cuvidCreateDecoder(&nvp->decoder, &crt) != CUDA_SUCCESS) {
		ERR(dec, "decoder creation failed");
		return false;
	}

	if(dstwidth != nvp->d.wdst || dstheight != nvp->d.hdst) {
		if(nvp->rgb != 0) {
			const cudaError_t frerr = cudaFree((void*)nvp->rgb);
			if(cudaSuccess != frerr) {
				ERR(dec, "Could not free internal RGB buffer: %d", frerr);
				return false;
			}
			nvp->rgb = 0;
		}
		/* after decode, the buffer is in NV12 format.  A CUDA kernel
		 * reorganizes/converts to RGB, outputting into this buffer.  We will then
		 * do a standard CUDA copy to put it in the output buffer, since our API
		 * works completely on host memory for now. */
		const size_t nb_rgb = dstwidth*dstheight*3;
		const cudaError_t merr = cudaMalloc((void**)&nvp->rgb, nb_rgb);
		if(cudaSuccess != merr) {
			ERR(dec, "could not allocate temporary RGB buffer: %d", merr);
			nvp->rgb = 0;
			return false;
		}
		nvp->d.wdst = dstwidth;
		nvp->d.hdst = dstheight;
	}

	nvp->initialized = true;
	return true;
}

/* Resizes an existing decoder. */
void
resize(struct nvp_decoder* nvp, size_t width, size_t height, size_t dstwidth,
       size_t dstheight) {
	if(nvp->decoder && cuvidDestroyDecoder(nvp->decoder) != CUDA_SUCCESS) {
		ERR(dec, "Error destroying decoder");
	}
	nvp->decoder = NULL;
	dec_initialize(nvp, width, height, dstwidth, dstheight);
}

static void
nvp_cuvid_destroy(nvpipe* const __restrict cdc) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	assert(nvp->impl.type == DECODER);

	if(nvp->decoder && cuvidDestroyDecoder(nvp->decoder) != CUDA_SUCCESS) {
		WARN(dec, "Error destroying decoder");
	}
	if(nvp->parser && cuvidDestroyVideoParser(nvp->parser) != CUDA_SUCCESS) {
		WARN(dec, "Error destroying parser.");
	}
	if(cudaSuccess != cudaFree((void*)nvp->rgb)) {
		WARN(dec, "Error freeing decode temporary buffer.");
	}
	if(nvp->reorg) {
		nvp->reorg->destroy(nvp->reorg);
		nvp->reorg = NULL;
	}
	if(cudaSuccess != cudaEventDestroy(nvp->ready)) {
		WARN(dec, "Error destroying sync event.");
	}

	free(nvp->hbuf);
	nvp->hbuf_sz = 0;
}

static int
dec_sequence(void* cdc, CUVIDEOFORMAT* fmt) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	/* warn the user if the image is too large, but try it anyway. */
	if((size_t)fmt->display_area.right > MAX_WIDTH ||
	   (size_t)fmt->display_area.bottom > MAX_HEIGHT) {
		WARN(dec, "Video stream exceeds (%zux%zu) limits.", MAX_WIDTH, MAX_HEIGHT);
	}
	if(fmt->bit_depth_luma_minus8) {
		WARN(dec, "Unhandled bit depth (%d).  Was the frame compressed by "
		     "a different version of this library?", fmt->bit_depth_luma_minus8);
		return 0;
	}

	/* We could read the format from 'fmt' and then use that to
	 * (cuvid)Create-a-Decoder.  But since we know we're getting the results of
	 * NvPipe, we already know the stream type, and so we just specify
	 * explicitly. */
	assert(fmt->chroma_format == cudaVideoChromaFormat_420);
	assert(fmt->codec == cudaVideoCodec_H264);
	assert(fmt->progressive_sequence == 1);
	const size_t w = fmt->display_area.right - fmt->display_area.left;
	const size_t h = fmt->display_area.bottom - fmt->display_area.top;
	/* This appears to happen sometimes, which height are we supposed to use? */
	if(fmt->coded_height != h) {
		TRACE(dec, "coded height (%u) does not correspond to height (%zu).",
		      fmt->coded_height, h);
	}
	/* If this is our first sequence, both the decoder and our internal buffer
	 * need initializing. */
	if(!nvp->initialized) {
		if(!dec_initialize(nvp, w,h, w,h)) {
			return 0;
		}
	}
	return 1;
}

static int
dec_ode(void* cdc, CUVIDPICPARAMS* pic) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	nvtxRangePush("cuvid DecodePicture");
	const CUresult dec = cuvidDecodePicture(nvp->decoder, pic);
	nvtxRangePop();
	if(CUDA_SUCCESS != dec) {
		WARN(dec, "Error %d decoding frame", dec);
		return 0;
	}
	/* Make sure this is after the decode+error check: we use it to figure out if
	 * this function executed successfully. */
	nvp->d.wsrc = pic->PicWidthInMbs*16;
	nvp->d.hsrc = pic->FrameHeightInMbs*16;
	return 1;
}

/* We'll use this as cuvid's "display" callback.  The intent is that the
 * callback would e.g. copy the data into a texture and blit it to the screen.
 * We just use it inform our main code which frame it should map next. */
static int
dec_display(void* cdc, CUVIDPARSERDISPINFO *dinfo) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	nvp->idx = dinfo->picture_index;
	return 1;
}

static nvp_err_t
initialize_parser(struct nvp_decoder* nvp) {
	CUVIDPARSERPARAMS prs = {0};
	prs.CodecType = cudaVideoCodec_H264;
	prs.ulMaxNumDecodeSurfaces = 2;
	prs.ulErrorThreshold = 100;
	/* when MaxDisplayDelay > 0, we can't assure that each input frame will be
	 * ready immediately.  If your application can tolerate frame latency, you
	 * might consider increasing this and introducing an EINTR-esque interface.
	 * Diminishing returns beyond 4. */
	prs.ulMaxDisplayDelay = 0;
	prs.pUserData = nvp;
	prs.pfnSequenceCallback = dec_sequence;
	prs.pfnDecodePicture = dec_ode;
	prs.pfnDisplayPicture = dec_display;
	if(cuvidCreateVideoParser(&nvp->parser, &prs) != CUDA_SUCCESS) {
		ERR(dec, "failed creating video parser.");
		return NVPIPE_EDECODE;
	}
	return NVPIPE_SUCCESS;
}

/** @return true if the given pointer was allocated on the device. */
static bool
is_device_ptr(const void* ptr) {
	struct cudaPointerAttributes attr;
	const cudaError_t perr = cudaPointerGetAttributes(&attr, ptr);
	return perr == cudaSuccess && attr.devicePointer != NULL;
}

/* Our decoder accepts input data from either the device or the host.  However,
 * nvcuvid only accepts host data.  Thus we copy the data to a host buffer to
 * satisfy the cuvid API.
 * To avoid an allocation every time the user submits a frame, we keep a small
 * host buffer in the decode object and reuse it for every submission.
 * @returns the host pointer to use for source data, which will be one of the
 *          user's host pointer OR our internal buffer. */
static const void*
source_data(struct nvp_decoder* nvp, const void* ibuf, const size_t ibuf_sz) {
	const void* srcbuf = ibuf;
	if(is_device_ptr(ibuf)) {
		if(ibuf_sz > nvp->hbuf_sz) {
			void* buf = realloc(nvp->hbuf, ibuf_sz);
			if(buf == NULL) {
				ERR(dec, "allocation failure of %zu-byte temp host buffer", ibuf_sz);
				return NULL;
			}
			nvp->hbuf = buf;
			nvp->hbuf_sz = ibuf_sz;
		}
		assert(nvp->hbuf_sz >= ibuf_sz);
		const cudaError_t hcpy = cudaMemcpy(nvp->hbuf, ibuf, ibuf_sz,
		                                    cudaMemcpyDeviceToHost);
		if(hcpy != cudaSuccess) {
			ERR(dec, "copy to temp host buffer failed: %d", hcpy);
			return NULL;
		}
		srcbuf = nvp->hbuf;
	}

	return srcbuf;
}

/* reorganize the data from 'nv12' into 'obuf'. */
static nvp_err_t
reorganize(struct nvp_decoder* nvp, CUdeviceptr nv12,
           const size_t width, const size_t height,
           void* const __restrict obuf, unsigned pitch) {
	/* is 'obuf' a device pointer?  if so, we can reorganize directly into
	 * that instead of staging through 'nvp->rgb' first. */
	CUdeviceptr dstbuf = nvp->rgb;
	if(is_device_ptr(obuf)) {
		dstbuf = (CUdeviceptr)obuf;
	}
	assert(nvp->reorg);
	const cudaError_t sub = nvp->reorg->submit(nvp->reorg, nv12, width, height,
	                                           (CUdeviceptr)dstbuf, pitch);
	if(cudaSuccess != sub) {
		ERR(dec, "reorganization kernel failed: %d", sub);
		return sub;
	}

	/* If 'obuf' was *not* the user's buffer, we need to copy from 'dstbuf' into
	 * 'obuf'. */
	if(!is_device_ptr(obuf)) {
		const size_t nb_rgb = nvp->d.wdst*nvp->d.hdst*3;
		const cudaError_t hcopy = cudaMemcpyAsync(obuf, (void*)dstbuf, nb_rgb,
		                                          cudaMemcpyDeviceToHost,
		                                          nvp->reorg->strm);
		if(cudaSuccess != hcopy) {
			ERR(dec, "async DtoH failed: %d", hcopy);
			return hcopy;
		}
	}

	const cudaError_t synch = nvp->reorg->sync(nvp->reorg);
	if(cudaSuccess != synch) {
		ERR(dec, "reorganization sync failed: %d", synch);
		return synch;
	}

	return NVPIPE_SUCCESS;
}

/** decode/decompress packets
 *
 * Decode a frame into the given buffer.
 *
 * @param[in] codec instance variable
 * @param[in] ibuf the compressed frame
 * @param[in] ibuf_sz  the size in bytes of the compressed data
 * @param[out] obuf where the output frame will be written. must be w*h*3 bytes.
 * @param[in] width width of output image
 * @param[in] height height of output image
 *
 * @return NVPIPE_SUCCESS on success, nonzero on error.
 */
nvp_err_t
nvp_cuvid_decode(nvpipe* const cdc,
                 const void* const __restrict ibuf,
                 const size_t ibuf_sz,
                 void* const __restrict obuf,
                 size_t width, size_t height) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	if(nvp->impl.type != DECODER) {
		ERR(dec, "backend implementation configuration error");
		return NVPIPE_EINVAL;
	}
	if(ibuf_sz == 0) {
		ERR(dec, "input buffer size is 0.");
		return NVPIPE_EINVAL;
	}
	if(width == 0 || height == 0 || (height&0x1) == 1) {
		ERR(dec, "invalid width or height");
		return NVPIPE_EINVAL;
	}
	assert(ibuf);
	assert(obuf);

	/* dynamically init our parser: it can be quite slow and eat a lot of
	 * resources, so it's preferable to allocate it lazily. */
	if(NULL == nvp->parser) {
		const nvp_err_t perr = initialize_parser(nvp);
		if(NVPIPE_SUCCESS != perr) {
			return perr;
		}
	}

	/* cuvid needs host mem. if the input isn't host mem, use an internal staging
	 * buffer. */
	const void* srcbuf = source_data(nvp, ibuf, ibuf_sz);
	if(srcbuf == NULL) {
		return NVPIPE_ENOMEM;
	}

	CUVIDSOURCEDATAPACKET pkt = {0};
	pkt.payload_size = ibuf_sz;
	pkt.payload = srcbuf;
	nvtxRangePush("cuvid parse video data");
	const CUresult parse = cuvidParseVideoData(nvp->parser, &pkt);
	nvtxRangePop();
	if(CUDA_SUCCESS != parse) {
		ERR(dec, "parsing video data failed");
		return NVPIPE_EDECODE;
	}	/* That fired off all our dec_* callbacks. */

	if(nvp->d.wsrc == 0 || nvp->d.hsrc == 0) {
		/* A frame of latency means cuvid doesn't always fire our callbacks.  So,
		 * just resubmit the frame again, but do a quick check to make sure we do
		 * not recurse endlessly. */
		if(nvp->empty) {
			ERR(dec, "Input is just stream metadata!");
			return NVPIPE_EINVAL;
		}
		nvp->empty = true;
		return nvp_cuvid_decode(cdc, ibuf, ibuf_sz, obuf, width, height);
	}
	nvp->empty = false;

	/* 4 cases: sizes are unchanged; size to scale to changed; input image size
	 * changed; both input image size and size to scale to changed.  We check for
	 * buffer size differences in 'resize', though, so they all boil down to:
	 * resize(), and then call ourself again (i.e. resubmit the frame).
	 * One could optimize the scaling cases by potentially reusing the buffer,
	 * technically. */
	if(nvp->d.wsrc != nvp->d.wi || nvp->d.hsrc != nvp->d.hi ||
	   nvp->d.wdst != width || nvp->d.hdst != height) {
		resize(nvp, nvp->d.wsrc, nvp->d.hsrc, width, height);
		return nvp_cuvid_decode(cdc, ibuf, ibuf_sz, obuf, width, height);
	}

	CUVIDPROCPARAMS map = {0};
	map.progressive_frame = 1;
	unsigned pitch = 0;
	CUdeviceptr data = 0;
	assert(nvp->decoder);
	CUresult mrs = cuvidMapVideoFrame(nvp->decoder, nvp->idx,
	                                  (unsigned long long*)&data, &pitch, &map);
	if(CUDA_SUCCESS != mrs) {
		ERR(dec, "Failed mapping frame: %d", mrs);
		return mrs;
	}

	/* Record an event that will inform us when the mapping is ready. */
	const cudaError_t evt = cudaEventRecord(nvp->ready, 0);
	if(cudaSuccess != evt) {
		ERR(dec, "could not record synchronization event: %d", evt);
		return evt;
	}

	/* We'll submit work in the stream that nvp->reorg has, but since that work
	 * will use the Mapped frame and cuvid does its work in the default stream,
	 * make sure the 'reorg' work cannot start until the Map's work completes. */
	const cudaError_t evwait = cudaStreamWaitEvent(nvp->reorg->strm, nvp->ready,
	                                               0);
	if(cudaSuccess != evwait) {
		ERR(dec, "could not synchronize streams via event: %d", evwait);
		return evwait;
	}

	nvtxRangePush("reorganize");
	const nvp_err_t orgerr = reorganize(nvp, data, width, height, obuf,
	                                    pitch);
	nvtxRangePop();
	/* Unmap immediately. Even if reorganize() gave an error, we still need to
	 * do the unmap before we return, and the return is the only thing left. */
	const CUresult maperr = cuvidUnmapVideoFrame(nvp->decoder, data);
	if(CUDA_SUCCESS != maperr) {
		WARN(dec, "Unmapping frame failed: %d", maperr);
	}

	return orgerr;
}

/* The decoder can't encode.  Just error and bail. */
static nvp_err_t
nvp_cuvid_encode(nvpipe * const __restrict codec,
                 const void *const __restrict ibuf,
                 const size_t ibuf_sz,
                 void *const __restrict obuf,
                 size_t* const __restrict obuf_sz,
                 const size_t width, const size_t height,
                 nvp_fmt_t format) {
	(void)codec; (void)ibuf; (void)ibuf_sz;
	(void)obuf; (void)obuf_sz;
	(void)width; (void)height;
	(void)format;
	ERR(dec, "Decoder cannot encode; create an encoder instead.");
	assert(false); /* Such use always indicates a programmer error. */
	return NVPIPE_EINVAL;
}

static nvp_err_t
nvp_cuvid_bitrate(nvpipe* codec, uint64_t br) {
	(void)codec; (void)br;
	ERR(dec, "Bitrate is encoded into the stream; you can only change it"
	    " on the encode side.");
	assert(false); /* Such use always indicates a programmer error. */
	return NVPIPE_EINVAL;
}

nvp_impl_t*
nvp_create_decoder() {
	struct nvp_decoder* nvp = calloc(1, sizeof(struct nvp_decoder));
	nvp->impl.type = DECODER;
	nvp->impl.encode = nvp_cuvid_encode;
	nvp->impl.bitrate = nvp_cuvid_bitrate;
	nvp->impl.decode = nvp_cuvid_decode;
	nvp->impl.destroy = nvp_cuvid_destroy;

	/* Ensure the runtime API initializes its implicit context. */
	cudaDeviceSynchronize();

	const cudaError_t cuerr =
		cudaEventCreateWithFlags(&nvp->ready, cudaEventDisableTiming);
	if(cudaSuccess != cuerr) {
		ERR(dec, "could not create sync event: %d", cuerr);
		free(nvp);
		return NULL;
	}

	nvp->reorg = nv122rgb();
	if(NULL == nvp->reorg) {
		ERR(dec, "could not create internal reorganization object");
		free(nvp);
		return NULL;
	}

	return (nvp_impl_t*)nvp;
}
